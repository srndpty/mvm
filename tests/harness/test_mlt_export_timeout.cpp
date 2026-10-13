// 書き出しの timeout の数え方 (P3-5B)。
//
// 契約 (src/media/mlt/mvm_mlt_export.h の MvmExportSpec::timeout_ms):
//   - 監視するのは、consumer を開始してから consumer が止まるまでの待ちだけ。準備・graph の構築・
//     consumer の close (encoder の flush)・出力の検証は含まない。
//   - 経過時間は実時間 (単調な時計) で数える。待ち loop の回数では数えない。
//   - 待ち loop は 1 回ごとに、(1) consumer が止まっていれば完了、(2) 取消、(3) 合成・背景の失敗、
//     (4) timeout、の順に見る。同じ回で重なったら、先のものが勝つ。
//   - timeout を見つけるのは待ち loop の 1 回の中なので、進行 callback が長くかかれば、その分だけ
//     遅れる (callback を途中で打ち切らない)。
//
// P3-5B より前の実装は Sleep(20) の回数で数えていた。進行 callback に時間がかかると、実時間では
// timeout_ms よりずっと遅く timeout した (timeout_ms = 1000 で 3418 ms)。
//
// ここでは実物の書き出しの関数 (待ち loop を含む) を、遅い consumer (RGBA の検査で frame
// ごとに眠る) と 遅い進行 callback で動かし、試験の側の時計で実時間を測る。
//
// 引数なし: 契約の検査。
// legacy: 古い数え方 (observer の診断用の入力) で同じ測定をする。実時間で timeout_ms の 1.5 倍より
//         遅ければ終了コード 3 (古い数え方の記録)、1.5 倍以内なら 0。

#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_runtime.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
// timeout を見つけるまでの遅れの許容 (待ちの 1 回 20 ms と、Windows の sleep の粒度・thread
// の切り替え)。 1 秒の timeout が秒の単位で遅れることは許さない。
constexpr double kToleranceMs = 400.0;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

double nowMs() {
    return static_cast<double>(mvm_mlt_export_observer_now_ns()) / 1e6;
}

MvmExportClip videoClip(long long frames) {
    MvmExportClip clip{};
    clip.path = MVM_TIMEOUT_TEST_VIDEO;
    clip.source_fps_num = 60;
    clip.source_fps_den = 1;
    clip.source_frame_count = 300;
    clip.source_out_frame = frames;
    clip.producer_out_frame = frames;
    clip.speed_num = 1;
    clip.speed_den = 1;
    clip.timeline_duration_frames = frames;
    return clip;
}

struct Controls {
    int timeoutMs = 1000;
    int workers = 4;
    long long frames = 300;
    int validatorSleepMs = 0;  // RGBA の検査で frame ごとに眠る (遅い consumer)
    int pollSleepMs = 0;       // 進行 callback が毎回眠る (待ち loop の 1 回が長い)
    double stallAfterMs = -1;  // 待ちの開始からこの時間が過ぎた最初の進行 callback で...
    int stallMs = 0;           // ...これだけ眠る (待ち loop の thread が止められた状態)
    double cancelAfterMs = -1; // 待ちの開始からこの時間が過ぎたら、進行 callback が取消を返す
    bool legacyAccounting = false;
    bool sequenceApi = false;
    std::filesystem::path output;
};

struct Probe {
    Controls controls;
    double firstPollMs = -1, lastPollReturnMs = -1;
    long long polls = 0;
    bool stalled = false;
    std::atomic<long long> validatorCalls{0};
};

int progress(long long, long long, void* opaque) {
    auto* probe = static_cast<Probe*>(opaque);
    const double now = nowMs();
    if (probe->firstPollMs < 0)
        probe->firstPollMs = now;
    ++probe->polls;
    const double sinceFirst = now - probe->firstPollMs;
    if (probe->controls.stallAfterMs >= 0 && !probe->stalled &&
        sinceFirst >= probe->controls.stallAfterMs) {
        probe->stalled = true;
        Sleep(static_cast<DWORD>(probe->controls.stallMs));
    }
    if (probe->controls.pollSleepMs > 0)
        Sleep(static_cast<DWORD>(probe->controls.pollSleepMs));
    const bool cancel =
        probe->controls.cancelAfterMs >= 0 && sinceFirst >= probe->controls.cancelAfterMs;
    probe->lastPollReturnMs = nowMs();
    return cancel ? 1 : 0;
}

int validator(long long, const unsigned char*, int, int, void* opaque) {
    auto* probe = static_cast<Probe*>(opaque);
    probe->validatorCalls.fetch_add(1);
    if (probe->controls.validatorSleepMs > 0)
        Sleep(static_cast<DWORD>(probe->controls.validatorSleepMs));
    return 0;
}

struct Outcome {
    int status = -1;
    std::string error;
    long long frameCount = 0;
    double totalMs = 0;         // 関数の呼び出しから戻りまで (試験の側の時計)
    double waitStartMs = 0;     // 待ちの開始 (observer の RENDER の段階)
    double detectedAfterMs = 0; // 待ちの開始から、最後の進行 callback が戻るまで
    double waitedMs = 0;        // 待ちの開始から、待ちの終わり (close か失敗の段階) まで
    bool reachedClose = false;
    bool outputExists = false;
    long long validatorCallsAtReturn = 0, validatorCallsLater = 0;

    bool timedOut() const {
        return status == MVM_EXPORT_FAILED && error.find("timeout") != std::string::npos;
    }
};

Outcome run(const Controls& controls) {
    Probe probe;
    probe.controls = controls;
    const MvmExportClip clip = videoClip(controls.frames);
    MvmExportObserver observer{};
    observer.diagnostic_legacy_timeout_accounting = controls.legacyAccounting ? 1 : 0;
    MvmExportSpec spec{kWidth,           kHeight, 60,      1,       23, controls.timeoutMs,
                       controls.workers, 0,       nullptr, nullptr, 0,  nullptr,
                       nullptr,          nullptr};
    spec.observer = &observer;
    spec.progress_callback = &progress;
    spec.progress_opaque = &probe;
    spec.rgba_callback = &validator;
    spec.rgba_opaque = &probe;
    std::error_code ignored;
    std::filesystem::remove(controls.output, ignored);
    const auto outputUtf8 = controls.output.u8string();
    const char* path = reinterpret_cast<const char*>(outputUtf8.c_str());
    char error[512] = {};
    MvmExportResult result{};
    Outcome outcome;
    const double began = nowMs();
    outcome.status =
        controls.sequenceApi
            ? mvm_mlt_export_sequence(&clip, 1, &spec, path, &result, error, sizeof(error))
            : mvm_mlt_export_two_track(&clip, 1, controls.frames, &spec, path, &result, error,
                                       sizeof(error));
    outcome.totalMs = nowMs() - began;
    outcome.validatorCallsAtReturn = probe.validatorCalls.load();
    outcome.error = error;
    outcome.frameCount = result.frame_count;
    outcome.waitStartMs = static_cast<double>(observer.stage_ns[MVM_EXPORT_STAGE_RENDER]) / 1e6;
    const long long closeNs = observer.stage_ns[MVM_EXPORT_STAGE_CONSUMER_CLOSE];
    const long long failedNs = observer.stage_ns[MVM_EXPORT_STAGE_FAILED];
    outcome.reachedClose = closeNs != 0;
    if (outcome.waitStartMs > 0) {
        outcome.detectedAfterMs = probe.lastPollReturnMs - outcome.waitStartMs;
        outcome.waitedMs =
            static_cast<double>(closeNs ? closeNs : failedNs) / 1e6 - outcome.waitStartMs;
    }
    outcome.outputExists = std::filesystem::exists(controls.output, ignored);
    // 戻った後に、render worker が検査の callback を呼び続けていないこと。
    Sleep(300);
    outcome.validatorCallsLater = probe.validatorCalls.load();
    std::filesystem::remove(controls.output, ignored);
    return outcome;
}

void print(const char* name, const Controls& controls, const Outcome& outcome) {
    std::printf(
        "%-28s timeout=%d ms status=%d 待ちの開始から検出まで=%.0f ms 待ち=%.0f ms 全体=%.0f ms "
        "出力=%d error=%s\n",
        name, controls.timeoutMs, outcome.status, outcome.detectedAfterMs, outcome.waitedMs,
        outcome.totalMs, outcome.outputExists ? 1 : 0, outcome.error.c_str());
    std::fflush(stdout);
}

// timeout になり、待ちの開始から timeout_ms 以上・(timeout_ms + 余分 + 許容) 以内に見つけたこと。
void requireTimeout(const char* name, const Controls& controls, const Outcome& outcome,
                    double extraMs = 0.0) {
    const std::string label = name;
    require(outcome.timedOut(), label + ": timeout として失敗すること (成功・取消にしない)");
    require(!outcome.reachedClose, label + ": 完了の段階へ進まないこと");
    require(outcome.detectedAfterMs >= controls.timeoutMs - 25.0,
            label + ": timeout_ms より早く timeout しないこと");
    require(outcome.detectedAfterMs <= controls.timeoutMs + extraMs + kToleranceMs,
            label + ": 実時間で timeout_ms (+ 許容) までに timeout すること");
    require(outcome.validatorCallsLater == outcome.validatorCallsAtReturn,
            label + ": 戻った後に render worker の callback が走らないこと");
}

std::filesystem::path scratch(const char* name) {
    return std::filesystem::temp_directory_path() /
           (std::string("mvm-export-timeout-") + name + ".mp4");
}

// 古い数え方の記録 (mlt_export_timeout_accounting_deviation
// と同じ測定を、古い数え方を指定して行う)。
int legacyMeasurement() {
    Controls controls;
    controls.validatorSleepMs = 100;
    controls.pollSleepMs = 30;
    controls.legacyAccounting = true;
    controls.output = scratch("legacy");
    const Outcome outcome = run(controls);
    print("legacy-poll-30ms", controls, outcome);
    if (!outcome.timedOut()) {
        std::fprintf(stderr, "FAIL: timeout にならなかったので測定できません\n");
        return 1;
    }
    return outcome.detectedAfterMs <= controls.timeoutMs * 1.5 ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT runtime を初期化できません\n");
        return 2;
    }
    if (argc > 1 && std::string(argv[1]) == "legacy") {
        const int code = legacyMeasurement();
        mvm_mlt_runtime_shutdown();
        return code;
    }

    // 1. 遅い consumer (4 worker)。300 frame × 100 ms / 4 = 7.5 秒かかるので、1 秒で timeout する。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.output = scratch("slow");
        const Outcome outcome = run(controls);
        print("slow-consumer", controls, outcome);
        requireTimeout("slow-consumer", controls, outcome);
    }
    // 2. 進行 callback が毎回 30 ms かかる (P3-5B より前は、実時間 3418 ms で timeout していた形)。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.pollSleepMs = 30;
        controls.output = scratch("poll30");
        const Outcome outcome = run(controls);
        print("poll-30ms", controls, outcome);
        requireTimeout("poll-30ms", controls, outcome);
    }
    // 3. 進行 callback が毎回 400 ms かかる。timeout を見るのは callback の後なので、最大で
    //    callback 1 回分だけ遅れる (秒の単位では遅れない)。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.pollSleepMs = 400;
        controls.output = scratch("poll400");
        const Outcome outcome = run(controls);
        print("poll-400ms", controls, outcome);
        requireTimeout("poll-400ms", controls, outcome, 400.0);
    }
    // 4. worker 8 本が動いている間の timeout。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.workers = 8;
        controls.output = scratch("workers8");
        const Outcome outcome = run(controls);
        print("slow-consumer-8-workers", controls, outcome);
        requireTimeout("slow-consumer-8-workers", controls, outcome);
    }
    // 5. frame を順に書き出す方の関数 (mvm_mlt_export_sequence) も同じ数え方であること。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.pollSleepMs = 30;
        controls.sequenceApi = true;
        controls.output = scratch("sequence");
        const Outcome outcome = run(controls);
        print("sequence-poll-30ms", controls, outcome);
        requireTimeout("sequence-poll-30ms", controls, outcome);
    }
    // 6. 期限より十分に早く終わる書き出しは成功する。直前の timeout の状態を引きずらない。
    double plainMs = 0;
    {
        Controls controls;
        controls.timeoutMs = 60000;
        controls.frames = 60;
        controls.output = scratch("fast");
        const Outcome outcome = run(controls);
        print("completes-early", controls, outcome);
        require(outcome.status == MVM_EXPORT_OK && outcome.frameCount == 60,
                "completes-early: 成功すること");
        require(outcome.outputExists, "completes-early: 出力ができること");
        plainMs = outcome.waitedMs;
    }
    // 7. 期限の近く。まず十分に長い期限でかかる時間 D を測り、D + 600 ms の期限では成功、
    //    D - 600 ms の期限では timeout になること (ちょうどの時刻は API
    //    から分からないので、決めない)。
    {
        Controls controls;
        controls.timeoutMs = 60000;
        controls.frames = 120;
        controls.validatorSleepMs = 50; // 120 × 50 / 4 = 1.5 秒ほど
        controls.output = scratch("near");
        const Outcome measured = run(controls);
        print("near-deadline-measure", controls, measured);
        require(measured.status == MVM_EXPORT_OK, "near-deadline: 測定の書き出しが成功すること");
        const double duration = measured.waitedMs;
        require(duration >= 900.0, "near-deadline: 測定した時間が、前後を分けられる長さであること");
        controls.timeoutMs = static_cast<int>(duration + 600.0);
        const Outcome after = run(controls);
        print("near-deadline-above", controls, after);
        require(after.status == MVM_EXPORT_OK, "near-deadline: 期限が長ければ成功すること");
        controls.timeoutMs = static_cast<int>(duration - 600.0);
        const Outcome before = run(controls);
        print("near-deadline-below", controls, before);
        requireTimeout("near-deadline-below", controls, before);
    }
    // 8. 待ち loop の thread が期限をまたいで止められていたが、その間に consumer は終わっていた。
    //    完了を先に見るので、成功になる (終わった時刻は分からないので、timeout とは言わない)。
    {
        Controls controls;
        controls.timeoutMs = 1500;
        controls.frames = 60;
        controls.stallAfterMs = 0;
        controls.stallMs = static_cast<int>(plainMs) + 2500;
        controls.output = scratch("stall-done");
        const Outcome outcome = run(controls);
        print("stalled-poll-completed", controls, outcome);
        require(outcome.status == MVM_EXPORT_OK && outcome.frameCount == 60,
                "stalled-poll-completed: 止められている間に終わった書き出しは成功すること");
        require(outcome.detectedAfterMs > controls.timeoutMs,
                "stalled-poll-completed: 期限を過ぎてから確認していること (試験の前提)");
    }
    // 9. 待ち loop の thread が期限をまたいで止められ、consumer はまだ動いている。起きた時点で
    //    timeout になる (止められていた分だけ遅れる)。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.stallAfterMs = 100;
        controls.stallMs = 2500;
        controls.output = scratch("stall-running");
        const Outcome outcome = run(controls);
        print("stalled-poll-running", controls, outcome);
        requireTimeout("stalled-poll-running", controls, outcome, 100.0 + 2500.0 - 1000.0);
    }
    // 10. 期限より前の取消は、取消として終わる。
    {
        Controls controls;
        controls.timeoutMs = 60000;
        controls.validatorSleepMs = 100;
        controls.cancelAfterMs = 300;
        controls.output = scratch("cancel");
        const Outcome outcome = run(controls);
        print("cancel-before-deadline", controls, outcome);
        require(outcome.status == MVM_EXPORT_CANCELLED, "cancel-before-deadline: 取消で終わること");
        require(outcome.validatorCallsLater == outcome.validatorCallsAtReturn,
                "cancel-before-deadline: 戻った後に callback が走らないこと");
    }
    // 11. 取消と timeout が同じ回で重なる: 進行 callback が期限をまたいで止められ、取消を返す。
    //     取消を先に見るので、取消になる。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.stallAfterMs = 700;
        controls.stallMs = 600;
        controls.cancelAfterMs = 700;
        controls.output = scratch("cancel-race");
        const Outcome outcome = run(controls);
        print("cancel-races-timeout", controls, outcome);
        require(outcome.status == MVM_EXPORT_CANCELLED,
                "cancel-races-timeout: 同じ回で重なったら取消になること");
        require(outcome.detectedAfterMs > controls.timeoutMs,
                "cancel-races-timeout: 期限を過ぎてから取消を返していること (試験の前提)");
    }
    // 12. consumer の失敗 (出力先の directory が無い)。timeout でも成功でもなく、失敗になる。
    //     待ち loop の thread が期限をまたいで止められていても同じ。
    for (const bool stalled : {false, true}) {
        Controls controls;
        controls.frames = 60;
        controls.output = std::filesystem::temp_directory_path() /
                          "mvm-export-timeout-no-such-directory" / "out.mp4";
        if (stalled) {
            controls.stallAfterMs = 0;
            controls.stallMs = 2500;
        }
        const Outcome outcome = run(controls);
        print(stalled ? "consumer-failure-stalled" : "consumer-failure", controls, outcome);
        require(outcome.status == MVM_EXPORT_FAILED, "consumer-failure: 失敗で終わること");
        require(!outcome.timedOut(), "consumer-failure: timeout と報告しないこと");
        require(!outcome.outputExists, "consumer-failure: 出力が無いこと");
    }
    // 13. 同じ process で続けて: timeout → 成功 → timeout。前の書き出しの時間を引きずらない。
    for (int round = 0; round < 2; ++round) {
        Controls slow;
        slow.validatorSleepMs = 100;
        slow.pollSleepMs = 30;
        slow.output = scratch("repeat-slow");
        const Outcome timedOut = run(slow);
        print("repeat-timeout", slow, timedOut);
        requireTimeout("repeat-timeout", slow, timedOut);
        Controls fast;
        fast.timeoutMs = 60000;
        fast.frames = 60;
        fast.output = scratch("repeat-fast");
        const Outcome completed = run(fast);
        print("repeat-success", fast, completed);
        require(completed.status == MVM_EXPORT_OK,
                "repeat-success: timeout の後の書き出しが成功すること");
    }
    // 14. 対照: 古い数え方 (診断用の入力) では、進行 callback が 30 ms かかると秒の単位で遅れる。
    //     上の 2 の検査が、古い数え方を検出することの確認。
    {
        Controls controls;
        controls.validatorSleepMs = 100;
        controls.pollSleepMs = 30;
        controls.legacyAccounting = true;
        controls.output = scratch("legacy-control");
        const Outcome outcome = run(controls);
        print("legacy-accounting-control", controls, outcome);
        require(outcome.timedOut(), "legacy-accounting-control: timeout にはなること");
        require(outcome.detectedAfterMs > controls.timeoutMs + kToleranceMs,
                "legacy-accounting-control: 古い数え方は許容を超えて遅れること (検査が効いている)");
    }

    mvm_mlt_runtime_shutdown();
    if (failures) {
        std::fprintf(stderr, "不成立: %d\n", failures);
        return 1;
    }
    std::printf("mlt_export_timeout_real_time: 全部成立\n");
    return 0;
}
