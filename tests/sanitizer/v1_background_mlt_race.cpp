// P3-5A.2: V1 の背景の保護 (src/media/mlt/mvm_mlt_v1_background.c) を、検出器の下で MLT 7.36.1 と
// 一緒に動かす隔離 harness。Linux の gcc で、MLT の framework と core module を同じ検出器付きで
// build したものへ link する (scripts/mlt-color-race-p35a2-sanitizers.sh)。
//
// 対象: 実物の mvm_mlt_v1_background.c。MLT の color producer・playlist・tractor・consumer の中の
// 読み書きも検出器が追う。
// 対象外: mvm_mlt_export.c (Windows の API と Qt・FFmpeg の MLT module に依存する)。graph は
// この file が同じ形で書き直している: V1 = 背景 (0〜79)・別の clip (80〜159)・背景 (160〜239)。
//
// 使い方: v1_background_mlt_race <MLT の module の directory> <scenario> [worker の数]
//   protected        保護あり。consumer は yuv422 を要求する (検査の無い製品の書き出しと同じ)。
//                    同じ process で 3 回 (背景は毎回作り直す)。隙間の全 frame が黒 (alpha 255)
//   protected-rgba   保護あり。consumer は rgba を要求する
//   seek             保護あり。consumer を使わず、worker が順不同の位置へ seek して画像を要求する。
//                    要求の形式は位置ごとに rgba と yuv422 が混ざる
//   cancel           保護あり。途中で consumer を止める
//   fault-rebuild    対照。prewarm の後で cache を作り直させる → 検査が見つける
//   fault-prewarm    対照。prewarm の検査の失敗 → 作る時点で失敗する
//   invalidate-format    対照。形式を固定しない → MLT へ渡す前に断る
//   invalidate-size      対照。consumer が別の大きさを要求する → MLT へ渡す前に断る
//   invalidate-rgba64    対照。consumer が rgba64 を要求する → MLT へ渡す前に断る
//   invalidate-resource  対照。consumer を始める前に色の文字列を変える → MLT へ渡す前に断る
//   unprotected      対照。保護なし。順不同・形式が混ざる要求 → 検出器が MLT の中の data race
//   を報告 unprotected-pixels   対照。保護なし。最初の frame を全 worker
//   が同時に要求する。黒でない画素を
//                    受け取ったら終了コード 3 (再現した)、受け取らなければ 0
// 終了コード: 0 = 期待どおり、1 = 不成立、3 = unprotected-pixels で誤った画素を観測。

extern "C" {
#include "media/mlt/mvm_mlt_v1_background.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFrames = 240;
constexpr int kClipStart = 80; // V1 の clip (隙間ではない区間)
constexpr int kClipEnd = 160;

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

bool isGap(long long position) {
    return position < kClipStart || position >= kClipEnd;
}

mlt_profile makeProfile() {
    mlt_profile profile = mlt_profile_init(nullptr);
    profile->width = kWidth;
    profile->height = kHeight;
    profile->frame_rate_num = 60;
    profile->frame_rate_den = 1;
    profile->sample_aspect_num = profile->sample_aspect_den = 1;
    profile->display_aspect_num = kWidth;
    profile->display_aspect_den = kHeight;
    profile->progressive = 1;
    return profile;
}

// 出力の直前の画像の検査。隙間の frame は、rgba で全画素が黒 (alpha 255) でなければならない。
struct Audit {
    std::atomic<long long> observed{0};
    std::atomic<long long> gapObserved{0};
    std::atomic<long long> gapNotRgba{0};
    std::atomic<long long> gapNotBlack{0};
    std::vector<std::atomic<int>> seen;

    Audit() : seen(static_cast<std::size_t>(kFrames)) {}
};

void inspect(Audit& audit, long long position, const uint8_t* image, mlt_image_format format,
             int width, int height) {
    audit.observed.fetch_add(1);
    if (position < 0 || position >= kFrames)
        return;
    audit.seen[static_cast<std::size_t>(position)].fetch_add(1);
    if (!isGap(position))
        return;
    audit.gapObserved.fetch_add(1);
    if (format != mlt_image_rgba || width != kWidth || height != kHeight) {
        audit.gapNotRgba.fetch_add(1);
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    for (std::size_t index = 0; index < bytes; ++index)
        if (image[index] != (index % 4 == 3 ? 255 : 0)) {
            audit.gapNotBlack.fetch_add(1);
            return;
        }
}

int auditImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height,
               int writable) {
    auto* audit = static_cast<Audit*>(mlt_frame_pop_service(frame));
    const auto position = static_cast<long long>(mlt_frame_get_position(frame));
    const int status = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (status == 0 && *image)
        inspect(*audit, position, *image, *format, *width, *height);
    return status;
}

mlt_frame auditProcess(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter->child);
    mlt_frame_push_get_image(frame, auditImage);
    return frame;
}

struct Graph {
    mlt_profile profile = nullptr;
    MvmV1Background* background = nullptr;
    mlt_producer clip = nullptr;
    mlt_playlist base = nullptr;
    mlt_tractor tractor = nullptr;
    std::string error;
};

// mvm_mlt_export.c と同じ形: V1 の clip の前後を、同じ背景の producer で埋める。
bool buildGraph(Graph& graph, int fault, Audit& audit) {
    graph.profile = makeProfile();
    char reason[256] = {};
    graph.background = mvm_mlt_v1_background_new(graph.profile, kFrames, kWidth, kHeight, fault,
                                                 reason, sizeof(reason));
    graph.error = reason;
    if (!graph.background)
        return false;
    mlt_producer background = mvm_mlt_v1_background_producer(graph.background);
    // V1 の clip の代わり (別の色の producer)。この harness の足場なので、cache は先に 1 thread で
    // 作っておく (製品の V1 の clip は静止画・動画で、color producer ではない)。
    graph.clip = mlt_factory_producer(graph.profile, "color", "#804020");
    mlt_properties_set(MLT_PRODUCER_PROPERTIES(graph.clip), "mlt_image_format", "rgba");
    {
        mlt_frame warm = nullptr;
        mlt_service_get_frame(MLT_PRODUCER_SERVICE(graph.clip), &warm, 0);
        uint8_t* image = nullptr;
        mlt_image_format format = mlt_image_rgba;
        int width = kWidth, height = kHeight;
        mlt_frame_get_image(warm, &image, &format, &width, &height, 0);
        mlt_frame_close(warm);
        mlt_producer_seek(graph.clip, 0);
    }
    graph.base = mlt_playlist_new(graph.profile);
    graph.tractor = mlt_tractor_new();
    mlt_playlist_append_io(graph.base, background, 0, kClipStart - 1);
    mlt_playlist_append_io(graph.base, graph.clip, 0, kClipEnd - kClipStart - 1);
    mlt_playlist_append_io(graph.base, background, 0, kFrames - kClipEnd - 1);
    mlt_tractor_set_track(graph.tractor, MLT_PLAYLIST_PRODUCER(graph.base), 0);
    mlt_producer output = MLT_TRACTOR_PRODUCER(graph.tractor);
    mlt_producer_set_in_and_out(output, 0, kFrames - 1);
    mlt_producer_seek(output, 0);
    mlt_filter filter = mlt_filter_new();
    filter->child = &audit;
    filter->process = auditProcess;
    mlt_service_attach(MLT_PRODUCER_SERVICE(output), filter);
    mlt_filter_close(filter);
    return true;
}

// 閉じる順は mvm_mlt_export.c と同じ: tractor → playlist → clip → 背景。
void closeGraph(Graph& graph) {
    if (graph.tractor)
        mlt_tractor_close(graph.tractor);
    if (graph.base)
        mlt_playlist_close(graph.base);
    if (graph.clip)
        mlt_producer_close(graph.clip);
    mvm_mlt_v1_background_close(graph.background);
    if (graph.profile)
        mlt_profile_close(graph.profile);
    graph = {};
}

struct Outcome {
    bool built = false;
    bool failedGate = false; // 背景の検査が違反を報告した
    std::string gateError;
    long long checked = 0, violations = 0, rejected = 0;
};

// consumer で全 frame を書き出す。stopAfter >= 0 なら、その数を観測した時点で止める。
// mutateAt >= 0 なら、その数を観測した時点で色の文字列を変える (cache の作り直しの対照)。
Outcome runConsumer(Graph& graph, Audit& audit, int workers, const char* consumerFormat,
                    int requestWidth, long long stopAfter, long long mutateAt) {
    Outcome outcome;
    mlt_consumer consumer = mlt_factory_consumer(graph.profile, "null", nullptr);
    if (!consumer)
        return outcome;
    mlt_properties properties = MLT_CONSUMER_PROPERTIES(consumer);
    mlt_properties_set_int(properties, "real_time", -workers);
    mlt_properties_set_int(properties, "terminate_on_pause", 1);
    if (consumerFormat)
        mlt_properties_set(properties, "mlt_image_format", consumerFormat);
    if (requestWidth > 0) {
        mlt_properties_set_int(properties, "width", requestWidth);
        mlt_properties_set_int(properties, "height", requestWidth * kHeight / kWidth);
    }
    outcome.built = mlt_consumer_connect(
                        consumer, MLT_PRODUCER_SERVICE(MLT_TRACTOR_PRODUCER(graph.tractor))) == 0 &&
                    mlt_consumer_start(consumer) == 0;
    bool mutated = false;
    char reason[256] = {};
    if (mutateAt == 0) {
        // consumer を始める前に (1 thread で) 変える。worker が動いている間に property を書き換える
        // のは、MLT 自身の読み方 (lock なしで文字列を読む) と競合するので、対照として使わない。
        mutated = true;
        mlt_properties_set(
            MLT_PRODUCER_PROPERTIES(mvm_mlt_v1_background_producer(graph.background)), "resource",
            "0x000000ff");
    }
    while (outcome.built && !mlt_consumer_is_stopped(consumer)) {
        // mvm_mlt_export.c の待ち loop と同じ: 違反を見たら止める。
        if (mvm_mlt_v1_background_failed(graph.background, reason, sizeof(reason))) {
            outcome.failedGate = true;
            break;
        }
        if (stopAfter >= 0 && audit.observed.load() >= stopAfter)
            break;
        if (mutateAt >= 0 && !mutated && audit.observed.load() >= mutateAt) {
            mutated = true;
            mlt_properties_set(
                MLT_PRODUCER_PROPERTIES(mvm_mlt_v1_background_producer(graph.background)),
                "resource", "0x000000ff");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    if (mvm_mlt_v1_background_failed(graph.background, reason, sizeof(reason)))
        outcome.failedGate = true;
    outcome.gateError = reason;
    mvm_mlt_v1_background_stats(graph.background, &outcome.checked, &outcome.violations,
                                &outcome.rejected, nullptr);
    return outcome;
}

void requireAllGapFramesBlack(const Audit& audit, bool complete) {
    require(audit.gapNotRgba.load() == 0, "隙間の frame が出力と同じ大きさの rgba であること");
    require(audit.gapNotBlack.load() == 0, "隙間の frame の全画素が黒 (alpha 255) であること");
    if (!complete)
        return;
    int missing = 0;
    for (const auto& seen : audit.seen)
        missing += seen.load() == 0;
    require(missing == 0, "全 frame を観測すること");
    // 隙間の最初の frame (0 と kClipEnd) を含む。
    require(audit.seen[0].load() > 0 && audit.seen[kClipEnd].load() > 0,
            "隙間ごとの最初の frame を観測すること");
}

void scenarioProtected(int workers, const char* consumerFormat) {
    for (int repeat = 0; repeat < 3; ++repeat) { // 背景は毎回作り直す
        Graph graph;
        Audit audit;
        require(buildGraph(graph, MVM_V1_BACKGROUND_FAULT_NONE, audit), "graph を作れること");
        const Outcome outcome = runConsumer(graph, audit, workers, consumerFormat, 0, -1, -1);
        require(outcome.built, "consumer を始められること");
        require(!outcome.failedGate && outcome.violations == 0, "背景の違反が無いこと");
        require(outcome.checked >= kFrames - (kClipEnd - kClipStart),
                "隙間の frame の数だけ検査を通ること");
        requireAllGapFramesBlack(audit, true);
        std::printf("protected workers=%d repeat=%d 検査=%lld 違反=%lld 隙間の frame=%lld\n",
                    workers, repeat, outcome.checked, outcome.violations, audit.gapObserved.load());
        closeGraph(graph);
    }
}

// consumer を使わず、worker が順不同の位置の frame を取り、画像を同時に要求する。
// frame を作る所 (seek と get_frame) は 1 つずつ、画像の要求は並行。要求の形式は位置で変える。
void pullReordered(Graph& graph, Audit& audit, int workers, bool directBackground) {
    std::vector<int> order;
    std::uint32_t state = 12345;
    for (int index = 0; index < kFrames * 2; ++index) {
        state = state * 1664525u + 1013904223u;
        order.push_back(static_cast<int>((state >> 8) % kFrames));
    }
    order.push_back(0);
    order.push_back(kClipEnd);
    std::mutex frameMutex;
    std::atomic<std::size_t> next{0};
    mlt_producer output = directBackground ? mvm_mlt_v1_background_producer(graph.background)
                                           : MLT_TRACTOR_PRODUCER(graph.tractor);
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker)
        threads.emplace_back([&] {
            for (;;) {
                const std::size_t index = next.fetch_add(1);
                if (index >= order.size())
                    return;
                const int position = order[index];
                mlt_frame frame = nullptr;
                {
                    std::lock_guard lock(frameMutex);
                    mlt_producer_seek(output, position);
                    mlt_service_get_frame(MLT_PRODUCER_SERVICE(output), &frame, 0);
                }
                if (!frame)
                    continue;
                uint8_t* image = nullptr;
                mlt_image_format format = position % 2 ? mlt_image_yuv422 : mlt_image_rgba;
                int width = kWidth, height = kHeight;
                if (mlt_frame_get_image(frame, &image, &format, &width, &height, 0) == 0 && image &&
                    directBackground)
                    inspect(audit, 0, image, format, width, height);
                mlt_frame_close(frame);
            }
        });
    for (auto& thread : threads)
        thread.join();
}

void scenarioSeek(int workers) {
    Graph graph;
    Audit audit;
    require(buildGraph(graph, MVM_V1_BACKGROUND_FAULT_NONE, audit), "graph を作れること");
    pullReordered(graph, audit, workers, false);
    char reason[256] = {};
    require(!mvm_mlt_v1_background_failed(graph.background, reason, sizeof(reason)),
            "順不同・形式の混ざる要求で背景の違反が無いこと");
    long long checked = 0, violations = 0;
    mvm_mlt_v1_background_stats(graph.background, &checked, &violations, nullptr, nullptr);
    require(checked > 0 && violations == 0, "検査を通り、違反が無いこと");
    requireAllGapFramesBlack(audit, false);
    require(audit.seen[0].load() > 0 && audit.seen[kClipEnd].load() > 0,
            "隙間ごとの最初の frame を観測すること");
    std::printf("seek workers=%d 検査=%lld 違反=%lld 隙間の frame=%lld\n", workers, checked,
                violations, audit.gapObserved.load());
    closeGraph(graph);
}

void scenarioCancel(int workers) {
    for (const long long stopAfter : {0LL, 1LL, 8LL, 100LL}) {
        Graph graph;
        Audit audit;
        require(buildGraph(graph, MVM_V1_BACKGROUND_FAULT_NONE, audit), "graph を作れること");
        const Outcome outcome = runConsumer(graph, audit, workers, nullptr, 0, stopAfter, -1);
        require(outcome.built && !outcome.failedGate && outcome.violations == 0,
                "取消で背景の違反が無いこと");
        requireAllGapFramesBlack(audit, false);
        closeGraph(graph);
    }
    std::printf("cancel workers=%d ok\n", workers);
}

// 検査が違反を見つけること (cache の作り直しの対照)。
void scenarioDetected(const char* name, int workers, int fault, const char* consumerFormat,
                      int requestWidth, long long mutateAt, const char* expected,
                      bool rejectedBeforeMlt) {
    Graph graph;
    Audit audit;
    require(buildGraph(graph, fault, audit), "graph を作れること");
    const Outcome outcome =
        runConsumer(graph, audit, workers, consumerFormat, requestWidth, -1, mutateAt);
    std::printf("%s workers=%d 検査=%lld 違反=%lld: %s\n", name, workers, outcome.checked,
                outcome.violations, outcome.gateError.c_str());
    require(outcome.failedGate && outcome.violations > 0, "背景の検査が違反を見つけること");
    require(rejectedBeforeMlt ? outcome.rejected == outcome.violations : outcome.rejected == 0,
            "MLT へ渡す前に断ったかどうかが期待どおりであること");
    std::printf("  渡す前に断った要求: %lld\n", outcome.rejected);
    require(outcome.gateError.find(expected) != std::string::npos,
            "違反の理由が期待どおりであること");
    closeGraph(graph);
}

void scenarioPrewarmFails() {
    Graph graph;
    Audit audit;
    require(!buildGraph(graph, MVM_V1_BACKGROUND_FAULT_PREWARM_FAILS, audit),
            "prewarm が失敗したら背景を作らないこと");
    require(graph.error.find("prewarm") != std::string::npos, "理由が prewarm であること");
    if (graph.profile)
        mlt_profile_close(graph.profile);
    std::printf("fault-prewarm: %s\n", graph.error.c_str());
}

void scenarioUnprotected(int workers) {
    Graph graph;
    Audit audit;
    require(buildGraph(graph, MVM_V1_BACKGROUND_FAULT_NO_PREWARM, audit), "graph を作れること");
    pullReordered(graph, audit, workers, false);
    std::printf("unprotected workers=%d 隙間の frame=%lld 黒でない=%lld\n", workers,
                audit.gapObserved.load(), audit.gapNotBlack.load());
    closeGraph(graph);
}

// 保護なしの背景へ、最初の画像を全 worker が同時に要求する (cache が無い状態からの競争)。
int scenarioUnprotectedPixels(int workers) {
    long long wrong = 0, total = 0;
    for (int round = 0; round < 40; ++round) {
        mlt_profile profile = makeProfile();
        char reason[256] = {};
        MvmV1Background* background =
            mvm_mlt_v1_background_new(profile, kFrames, kWidth, kHeight,
                                      MVM_V1_BACKGROUND_FAULT_NO_PREWARM, reason, sizeof(reason));
        mlt_producer producer = mvm_mlt_v1_background_producer(background);
        std::vector<mlt_frame> frames(static_cast<std::size_t>(workers), nullptr);
        for (auto& frame : frames)
            mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0);
        std::atomic<bool> go{false};
        std::atomic<long long> notBlack{0};
        std::vector<std::thread> threads;
        for (int worker = 0; worker < workers; ++worker)
            threads.emplace_back([&, worker] {
                while (!go.load())
                    std::this_thread::yield();
                uint8_t* image = nullptr;
                mlt_image_format format = mlt_image_rgba;
                int width = kWidth, height = kHeight;
                mlt_frame frame = frames[static_cast<std::size_t>(worker)];
                if (mlt_frame_get_image(frame, &image, &format, &width, &height, 0) != 0 || !image)
                    return;
                const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
                for (std::size_t index = 0; index < bytes; ++index)
                    if (image[index] != (index % 4 == 3 ? 255 : 0)) {
                        notBlack.fetch_add(1);
                        return;
                    }
            });
        go.store(true);
        for (auto& thread : threads)
            thread.join();
        for (auto frame : frames)
            mlt_frame_close(frame);
        mvm_mlt_v1_background_close(background);
        mlt_profile_close(profile);
        wrong += notBlack.load();
        total += workers;
    }
    std::printf("unprotected-pixels workers=%d: 黒でない画像 %lld / %lld\n", workers, wrong, total);
    return wrong > 0 ? 3 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <MLT module directory> <scenario> [workers]\n", argv[0]);
        return 2;
    }
    const std::string scenario = argv[2];
    const int workers = argc > 3 ? std::atoi(argv[3]) : 4;
    if (!mlt_factory_init(argv[1])) {
        std::fprintf(stderr, "MLT を初期化できません: %s\n", argv[1]);
        return 2;
    }
    int code = 0;
    if (scenario == "protected")
        scenarioProtected(workers, nullptr);
    else if (scenario == "protected-rgba")
        scenarioProtected(workers, "rgba");
    else if (scenario == "seek")
        scenarioSeek(workers);
    else if (scenario == "cancel")
        scenarioCancel(workers);
    else if (scenario == "fault-rebuild")
        scenarioDetected("fault-rebuild", workers, MVM_V1_BACKGROUND_FAULT_REBUILD, nullptr, 0, -1,
                         "作り直されました", false);
    else if (scenario == "fault-prewarm")
        scenarioPrewarmFails();
    else if (scenario == "invalidate-format")
        scenarioDetected("invalidate-format", workers, MVM_V1_BACKGROUND_FAULT_NO_FORMAT, nullptr,
                         0, -1, "形式の固定が外れています", true);
    else if (scenario == "invalidate-size")
        scenarioDetected("invalidate-size", workers, MVM_V1_BACKGROUND_FAULT_NONE, nullptr, 320, -1,
                         "出力と違う大きさ", true);
    else if (scenario == "invalidate-rgba64")
        scenarioDetected("invalidate-rgba64", workers, MVM_V1_BACKGROUND_FAULT_NONE, "rgba64", 0,
                         -1, "rgba64", true);
    else if (scenario == "invalidate-resource")
        scenarioDetected("invalidate-resource", workers, MVM_V1_BACKGROUND_FAULT_NONE, nullptr, 0,
                         0, "色の指定が変わっています", true);
    else if (scenario == "unprotected")
        scenarioUnprotected(workers);
    else if (scenario == "unprotected-pixels")
        code = scenarioUnprotectedPixels(workers);
    else {
        std::fprintf(stderr, "不明な scenario: %s\n", scenario.c_str());
        return 2;
    }
    mlt_factory_close();
    if (failures) {
        std::fprintf(stderr, "不成立: %d\n", failures);
        return 1;
    }
    if (code == 0)
        std::printf("%s: 成立\n", scenario.c_str());
    return code;
}
