// V1 の隙間 (黒の背景) を含む書き出し (P3-5A.2)。
//
// MLT 7.36.1 の color producer は、cache を作り直す間に来た別の worker へ、埋まる前の画素を
// 返すことがある (docs/mlt-color-race-p35a2.md)。書き出しは、背景の形式を rgba に固定し、
// consumer を始める前に 1 thread で cache を作り、以後の画像の要求ごとに cache が変わっていない
// ことを確かめる (src/media/mlt/mvm_mlt_v1_background.c)。ここでは実物の書き出しの関数で、
//   - 隙間だけ・overlay と混ざる・V1 の clip をはさんで隙間が 2 つ、の 3 つの形が書き出せること
//   - encoder 直前の RGBA で、隙間だけの frame が全画素 (0, 0, 0, 255) であること
//   - 全 frame を 1 回以上観測し、cache の作り直しが 0 回であること
//   - cache の作り直しが起きたら (故障の注入)、書き出しが失敗して出力を残さないこと
// を確かめる。
//
// 引数なし: 上の検査。
// export <gap|mixed|multi> <validator 0|1> <worker の数> <出力.mp4> <故障>:
//   1 回書き出すだけ (修正の前後で出力を比べるため)。validator が 1 なら、frame ごとの RGBA の
//   digest を標準出力へ書く。

#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr int kFrames = 120;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

struct Scenario {
    std::string name;
    std::vector<MvmExportClip> clips;
    std::vector<MvmExportOpacityKeyframe> keys; // overlay の両端
    std::vector<bool> gapOnly;                  // frame ごと: 背景だけが見える
};

MvmExportClip still(int track, long long start, long long frames) {
    MvmExportClip clip{};
    clip.path = MVM_V1_GAP_TEST_IMAGE;
    clip.is_still_image = 1;
    clip.source_fps_num = 60;
    clip.source_fps_den = 1;
    clip.source_frame_count = frames;
    clip.source_out_frame = frames;
    clip.producer_out_frame = frames;
    clip.speed_num = 1;
    clip.speed_den = 1;
    clip.video_track = track;
    clip.timeline_start_frame = start;
    clip.timeline_duration_frames = frames;
    clip.rect_width = kWidth;
    clip.rect_height = kHeight;
    return clip;
}

// gap: clip なし。mixed: V2 の静止画が 30〜89。multi: V1 の静止画が 40〜79 (隙間が 2 つ)、
// V2 の静止画が 20〜99。
Scenario makeScenario(const std::string& name) {
    Scenario scenario;
    scenario.name = name;
    scenario.gapOnly.assign(kFrames, true);
    long long overlayStart = -1, overlayFrames = 0;
    if (name == "mixed") {
        overlayStart = 30;
        overlayFrames = 60;
    } else if (name == "multi") {
        scenario.clips.push_back(still(0, 40, 40));
        overlayStart = 20;
        overlayFrames = 80;
        for (int frame = 40; frame < 80; ++frame)
            scenario.gapOnly[static_cast<std::size_t>(frame)] = false;
    } else if (name != "gap") {
        std::fprintf(stderr, "不明な scenario: %s\n", name.c_str());
        std::exit(2);
    }
    if (overlayStart >= 0) {
        scenario.keys = {{0, 1.0}, {overlayFrames - 1, 1.0}};
        scenario.clips.push_back(still(1, overlayStart, overlayFrames));
        for (long long frame = overlayStart; frame < overlayStart + overlayFrames; ++frame)
            scenario.gapOnly[static_cast<std::size_t>(frame)] = false;
    }
    return scenario;
}

struct Observed {
    std::mutex mutex;
    std::vector<int> calls = std::vector<int>(kFrames, 0);
    std::vector<std::uint64_t> digest = std::vector<std::uint64_t>(kFrames, 0);
    std::vector<bool> black = std::vector<bool>(kFrames, false);
    long long wrongSize = 0, outside = 0, repeatDiffers = 0;
};

int validator(long long frame, const unsigned char* rgba, int width, int height, void* opaque) {
    auto* observed = static_cast<Observed*>(opaque);
    std::uint64_t value = 0xcbf29ce484222325ull;
    bool black = width == kWidth && height == kHeight;
    if (black) {
        const std::size_t bytes =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
        for (std::size_t index = 0; index < bytes; ++index) {
            value = (value ^ rgba[index]) * 0x100000001b3ull;
            if (rgba[index] != (index % 4 == 3 ? 255 : 0))
                black = false;
        }
    }
    std::lock_guard lock(observed->mutex);
    if (width != kWidth || height != kHeight) {
        ++observed->wrongSize;
    } else if (frame < 0 || frame >= kFrames) {
        ++observed->outside;
    } else {
        const auto index = static_cast<std::size_t>(frame);
        if (observed->calls[index] > 0 && observed->digest[index] != value)
            ++observed->repeatDiffers;
        ++observed->calls[index];
        observed->digest[index] = value;
        observed->black[index] = black;
    }
    return 0;
}

struct Run {
    int status = -1;
    std::string error;
    MvmExportResult result{};
    MvmExportObserver observer{};
    Observed observed;
    bool outputExists = false;
};

// progressLimit >= 0 なら、その frame 数に達した時点で取消を返す。
void runExport(Scenario& scenario, bool useValidator, int workers, int fault,
               const std::filesystem::path& output, Run& run, long long cancelAt = -1) {
    for (auto& clip : scenario.clips)
        if (clip.video_track > 0) {
            clip.opacity_keyframes = scenario.keys.data();
            clip.opacity_keyframe_count = static_cast<int>(scenario.keys.size());
        }
    std::filesystem::remove(output);
    MvmExportSpec spec{kWidth, kHeight, 60,      1, 23,      120000,  workers,
                       0,      nullptr, nullptr, 0, nullptr, nullptr, nullptr};
    run.observer.diagnostic_v1_background_fault = fault;
    spec.observer = &run.observer;
    if (useValidator) {
        spec.rgba_callback = &validator;
        spec.rgba_opaque = &run.observed;
    }
    static long long cancelFrame = -1;
    cancelFrame = cancelAt;
    if (cancelAt >= 0)
        spec.progress_callback = [](long long completed, long long, void*) {
            return completed >= cancelFrame ? 1 : 0;
        };
    char error[512] = {};
    const auto outputUtf8 = output.u8string();
    run.status = mvm_mlt_export_two_track(scenario.clips.empty() ? nullptr : scenario.clips.data(),
                                          static_cast<int>(scenario.clips.size()), kFrames, &spec,
                                          reinterpret_cast<const char*>(outputUtf8.c_str()),
                                          &run.result, error, sizeof(error));
    run.error = error;
    run.outputExists = std::filesystem::exists(output);
}

std::filesystem::path scratch(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("mvm-v1-gap-" + name + ".mp4");
}

void checkSuccess(const std::string& label, Scenario& scenario, bool useValidator, int workers) {
    Run run;
    const auto output = scratch(label);
    runExport(scenario, useValidator, workers, 0, output, run);
    if (run.status != 0)
        std::fprintf(stderr, "%s: status=%d error=%s\n", label.c_str(), run.status,
                     run.error.c_str());
    require(run.status == 0 && run.result.frame_count == kFrames,
            label + ": 書き出しが完了すること");
    require(run.outputExists, label + ": 出力ができること");
    require(run.observer.v1_background_violations == 0, label + ": cache の作り直しが無いこと");
    require(run.observer.v1_background_rejected == 0, label + ": 断った要求が無いこと");
    // 背景へ届く要求の形式は、rgba (overlay の affine、RGBA の検査) と yuv422 (consumer) だけ。
    constexpr int kRgba = 1 << 2, kYuv422 = 1 << 3; // mlt_image_rgba = 2、mlt_image_yuv422 = 3
    const int formats = run.observer.v1_background_requested_formats;
    require(formats != 0 && (formats & ~(kRgba | kYuv422)) == 0,
            label + ": 背景への要求の形式が rgba・yuv422 だけであること");
    if (useValidator)
        require(formats == kRgba, label + ": RGBA の検査があるときは、要求が rgba だけであること");
    long long gapFrames = 0;
    for (const bool gap : scenario.gapOnly)
        gapFrames += gap;
    // 背景は、隙間だけの frame と、overlay が隙間に重なる frame で要求される。
    require(run.observer.v1_background_frames >= gapFrames,
            label + ": 隙間の frame の数だけ背景の検査を通ること");
    if (useValidator) {
        int missing = 0, notBlack = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto index = static_cast<std::size_t>(frame);
            missing += run.observed.calls[index] == 0;
            notBlack += scenario.gapOnly[index] && !run.observed.black[index];
        }
        require(missing == 0, label + ": 全 frame を観測すること");
        require(notBlack == 0, label + ": 隙間だけの frame が全画素 (0, 0, 0, 255) であること");
        require(run.observed.wrongSize == 0 && run.observed.outside == 0 &&
                    run.observed.repeatDiffers == 0,
                label + ": 大きさ・frame 番号・再要求の画素が正しいこと");
    }
    std::printf("%s: 背景の検査 %lld 回、違反 %lld、要求の形式%s%s\n", label.c_str(),
                static_cast<long long>(run.observer.v1_background_frames),
                static_cast<long long>(run.observer.v1_background_violations),
                (formats & kRgba) ? " rgba" : "", (formats & kYuv422) ? " yuv422" : "");
    std::filesystem::remove(output);
}

// 故障を入れた書き出しは失敗し、理由に expected を含むこと。
void checkFault(const std::string& label, Scenario& scenario, bool useValidator, int fault,
                const char* expected, bool beforeConsumer, bool rejectedBeforeMlt = false) {
    Run run;
    const auto output = scratch(label);
    runExport(scenario, useValidator, 8, fault, output, run);
    std::printf("%s: status=%d error=%s\n", label.c_str(), run.status, run.error.c_str());
    require(run.status != 0, label + ": 書き出しが失敗すること");
    require(run.error.find(expected) != std::string::npos, label + ": 理由が背景の検査であること");
    if (beforeConsumer) {
        require(run.observer.stage_ns[MVM_EXPORT_STAGE_CONSUMER_START] == 0,
                label + ": consumer を始める前に失敗すること");
        require(!run.outputExists, label + ": 出力を作らないこと");
    } else {
        require(run.observer.v1_background_violations > 0, label + ": 違反を数えること");
        require((run.observer.v1_background_rejected > 0) == rejectedBeforeMlt,
                label + ": 要求を MLT へ渡す前に断ったかどうかが期待どおりであること");
    }
    std::filesystem::remove(output);
}

int exportOnce(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: export <scenario> <validator> <workers> <output> <fault>\n");
        return 2;
    }
    Scenario scenario = makeScenario(argv[2]);
    Run run;
    runExport(scenario, std::atoi(argv[3]) != 0, std::atoi(argv[4]), std::atoi(argv[6]), argv[5],
              run);
    std::fprintf(stderr, "status=%d error=%s frames=%lld 背景の検査=%lld 違反=%lld\n", run.status,
                 run.error.c_str(), static_cast<long long>(run.result.frame_count),
                 static_cast<long long>(run.observer.v1_background_frames),
                 static_cast<long long>(run.observer.v1_background_violations));
    if (std::atoi(argv[3]) != 0)
        for (int frame = 0; frame < kFrames; ++frame) {
            const auto index = static_cast<std::size_t>(frame);
            std::printf("%d\t%d\t%016llx\t%d\n", frame, run.observed.calls[index],
                        static_cast<unsigned long long>(run.observed.digest[index]),
                        run.observed.black[index] ? 1 : 0);
        }
    return run.status == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT runtime を初期化できません\n");
        return 2;
    }
    if (argc > 1 && std::string(argv[1]) == "export") {
        const int code = exportOnce(argc, argv);
        mvm_mlt_runtime_shutdown();
        return code;
    }

    // 3 つの形 × 検査あり・なし × worker 1・8 本。
    for (const char* name : {"gap", "mixed", "multi"})
        for (const bool useValidator : {true, false})
            for (const int workers : {1, 8}) {
                Scenario scenario = makeScenario(name);
                checkSuccess(std::string(name) + (useValidator ? "-rgba" : "-product") + "-w" +
                                 std::to_string(workers),
                             scenario, useValidator, workers);
            }
    // 同じ process で続けて書き出す (背景は書き出しごとに作り直す)。
    for (int repeat = 0; repeat < 3; ++repeat) {
        Scenario scenario = makeScenario("multi");
        checkSuccess("repeat-" + std::to_string(repeat), scenario, false, 8);
    }
    // 取消: 途中で止めても、背景の違反にならないこと。
    {
        Scenario scenario = makeScenario("mixed");
        Run run;
        const auto output = scratch("cancel");
        runExport(scenario, false, 8, 0, output, run, 20);
        require(run.status == MVM_EXPORT_CANCELLED, "取消で終わること");
        require(run.observer.v1_background_violations == 0, "取消で背景の違反を記録しないこと");
        std::filesystem::remove(output);
    }
    // 対照: cache の作り直しが起きると、書き出しは失敗する。
    {
        // 形式を固定しない。overlay の無い frame は consumer の形式 (yuv) で要求されるので、
        // そのまま渡せば rgba で作った cache が作り直される (修正の前の製品の動き)。
        // 要求は MLT へ渡す前に断られる。
        Scenario scenario = makeScenario("mixed");
        checkFault("fault-no-format", scenario, false, 1, "形式の固定が外れています", false, true);
    }
    {
        // 渡す前の検査が見ない所で cache が作り直された状態 (形も画素も同じ。buffer だけが違う)。
        // 渡した後の検査が見つける。
        Scenario scenario = makeScenario("gap");
        checkFault("fault-rebuild", scenario, false, 2, "作り直されました", false);
    }
    {
        Scenario scenario = makeScenario("gap");
        checkFault("fault-prewarm", scenario, false, 3, "prewarm", true);
    }

    mvm_mlt_runtime_shutdown();
    if (failures) {
        std::fprintf(stderr, "不成立: %d\n", failures);
        return 1;
    }
    std::printf("mlt_export_v1_gap: 全部成立\n");
    return 0;
}
