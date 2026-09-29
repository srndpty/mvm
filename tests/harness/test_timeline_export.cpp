// M6a: timeline order と source-native trim を逐次 export する focused test。
//
// 検査するのは次の経路に絞る。
//   1. 実素材 2 本を順に並べて 1 本の再生可能な MP4 になる
//   2. 29.97fps 素材の trim が MLT producer 位置と内容の両方で一致する
//   3. clip が 0 本なら失敗する
//   4. 素材が存在しなければ失敗する
//
// 期待フレーム数は実装の式を再利用せず、入力を probe して独立に足し合わせる。

#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"
#include "project/timeline_edit.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string toUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "NG: %s\n", message);
    ++gFailures;
}

// 入力素材のフレーム数。実装ではなく probe から取る。
long long probeFrameCount(const std::filesystem::path& path) {
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(toUtf8(path).c_str(), &probe) != 0 || !probe.ok) {
        std::fprintf(stderr, "NG: 入力を probe できません: %s (%s)\n", toUtf8(path).c_str(),
                     probe.error);
        ++gFailures;
        return 0;
    }
    return probe.frame_count;
}

mvm::project::Project makeProject(const std::filesystem::path& first,
                                  const std::filesystem::path& second, long long firstFrames,
                                  long long secondFrames) {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Video,
                                     first,
                                     "normal",
                                     "normal-id",
                                     {},
                                     60,
                                     1,
                                     firstFrames,
                                     0,
                                     firstFrames,
                                     0,
                                     {},
                                     {},
                                     {}});
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Manim,
                                     second,
                                     "manim",
                                     "manim-id",
                                     {},
                                     60,
                                     1,
                                     secondFrames,
                                     0,
                                     secondFrames,
                                     firstFrames,
                                     {},
                                     {},
                                     {}});
    return project;
}

bool generateFractionalFixture(const std::filesystem::path& ffmpeg,
                               const std::filesystem::path& output) {
    const intptr_t exitCode =
        _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                 L"lavfi", L"-i", L"color=c=red:s=64x64:r=30000/1001:d=1", L"-f", L"lavfi", L"-i",
                 L"color=c=blue:s=64x64:r=30000/1001:d=1", L"-filter_complex",
                 L"[0:v][1:v]concat=n=2:v=1:a=0", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p",
                 output.c_str(), static_cast<wchar_t*>(nullptr));
    return exitCode == 0 && std::filesystem::is_regular_file(output);
}

bool frameIsBlue(const std::filesystem::path& path, long long frame) {
    MvmMltImage image{};
    char error[512] = {};
    if (mvm_mlt_decode_frame(toUtf8(path).c_str(), frame, &image, error, sizeof(error)) != 0 ||
        !image.rgba || image.width <= 0 || image.height <= 0) {
        std::fprintf(stderr, "NG: 色検査frameをdecodeできません: %s\n", error);
        return false;
    }
    const std::size_t center =
        (static_cast<std::size_t>(image.height / 2) * static_cast<std::size_t>(image.width) +
         static_cast<std::size_t>(image.width / 2)) *
        4;
    const int red = image.rgba[center];
    const int green = image.rgba[center + 1];
    const int blue = image.rgba[center + 2];
    mvm_mlt_image_free(&image);
    if (!(blue > 180 && red < 80))
        std::fprintf(stderr, "  frame %lld RGBA先頭=%d,%d,%d\n", frame, red, green, blue);
    return blue > 180 && red < 80;
}

bool generateEffectsFixture(const std::filesystem::path& ffmpeg,
                            const std::filesystem::path& output) {
    const wchar_t* filter = L"drawbox=x=0:y=0:w=iw:h=20:color=red:t=fill,"
                            L"drawbox=x=0:y=ih-20:w=iw:h=20:color=blue:t=fill,"
                            L"drawbox=x=0:y=20:w=20:h=ih-40:color=green:t=fill,"
                            L"drawbox=x=iw-20:y=20:w=20:h=ih-40:color=yellow:t=fill,"
                            L"drawbox=x=150:y=110:w=20:h=20:color=magenta:t=fill";
    return _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                    L"lavfi", L"-i", L"color=c=gray:s=320x240:r=60:d=2", L"-vf", filter, L"-c:v",
                    L"libx264", L"-preset", L"ultrafast", L"-crf", L"8", L"-pix_fmt", L"yuv420p",
                    output.c_str(), static_cast<wchar_t*>(nullptr)) == 0;
}

bool generateSolidFixture(const std::filesystem::path& ffmpeg, const std::filesystem::path& output,
                          const wchar_t* color) {
    const std::wstring source = std::wstring(L"color=c=") + color + L":s=320x240:r=60:d=2";
    return _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error", L"-f",
                    L"lavfi", L"-i", source.c_str(), L"-c:v", L"libx264", L"-preset", L"ultrafast",
                    L"-crf", L"8", L"-pix_fmt", L"yuv420p", output.c_str(),
                    static_cast<wchar_t*>(nullptr)) == 0;
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixelAt(const std::filesystem::path& path, long long frame, int x, int y) {
    MvmMltImage image{};
    char error[512] = {};
    if (mvm_mlt_decode_frame(toUtf8(path).c_str(), frame, &image, error, sizeof(error)) != 0 ||
        !image.rgba || x < 0 || y < 0 || x >= image.width || y >= image.height) {
        check(false, "overlay出力frameをdecodeできません");
        return {};
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
         static_cast<std::size_t>(x)) *
        4;
    const Rgb result{image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2]};
    mvm_mlt_image_free(&image);
    return result;
}

bool blue(const Rgb& value) {
    return value.b > 180 && value.r < 70 && value.g < 70;
}

struct EffectFrameMetrics {
    double mean = 0.0;
    int minX = 100000;
    int minY = 100000;
    int maxX = -1;
    int maxY = -1;
};

EffectFrameMetrics effectMetrics(const std::filesystem::path& path, long long frame) {
    EffectFrameMetrics result;
    MvmMltImage image{};
    char error[512] = {};
    if (mvm_mlt_decode_frame(toUtf8(path).c_str(), frame, &image, error, sizeof(error)) != 0 ||
        !image.rgba) {
        check(false, "effect出力frameをdecodeできません");
        return result;
    }
    double sum = 0.0;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                 static_cast<std::size_t>(x)) *
                4;
            const int brightness =
                image.rgba[offset] + image.rgba[offset + 1] + image.rgba[offset + 2];
            sum += brightness;
            if (brightness > 45) {
                result.minX = std::min(result.minX, x);
                result.minY = std::min(result.minY, y);
                result.maxX = std::max(result.maxX, x);
                result.maxY = std::max(result.maxY, y);
            }
        }
    }
    result.mean = sum / static_cast<double>(image.width * image.height * 3);
    mvm_mlt_image_free(&image);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    mvm_enable_utf8_console();
    if (argc != 5) {
        std::fprintf(stderr, "使い方: test_timeline_export <test-dir> <clip1> <clip2> <ffmpeg>\n");
        return 2;
    }

    const auto testDirectory = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto firstClip = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto secondClip = std::filesystem::absolute(fromUtf8(argv[3]));
    const auto ffmpeg = std::filesystem::absolute(fromUtf8(argv[4]));

    std::error_code error;
    std::filesystem::remove_all(testDirectory, error);
    error.clear();
    std::filesystem::create_directories(testDirectory, error);
    if (error) {
        std::fprintf(stderr, "test directory を作成できません\n");
        return 1;
    }

    // cut の範囲は Project の境界 ceil(s R) で決める。期待値は手で計算した値である。
    {
        mvm::project::TimelineClip ntsc;
        ntsc.sourceFpsNum = 30000;
        ntsc.sourceFpsDen = 1001;
        ntsc.sourceFrameCount = 300;
        ntsc.sourceInFrame = 30;  // 30 * 60 * 1001 / 30000 = 60.06
        ntsc.sourceOutFrame = 60; // 120.12
        const auto range = mvm::project::clipProducerRange(ntsc, 60, 1);
        check(range.success && range.begin == 61 && range.end == 121 && range.tailFrames == 0,
              "29.97fps素材のcut範囲がProject境界のceilと一致しません");

        // 25fps 素材の末尾 (N = 100) を 60fps へ置く。位置 239 は 239 / 2.4 = 99.58 で
        // 存在しない frame 100 に丸まるので cut に含めず、最終 frame で 1 frame 埋める。
        mvm::project::TimelineClip pal;
        pal.sourceFpsNum = 25;
        pal.sourceFpsDen = 1;
        pal.sourceFrameCount = 100;
        pal.sourceInFrame = 90;
        pal.sourceOutFrame = 100;
        const auto terminal = mvm::project::clipProducerRange(pal, 60, 1);
        check(terminal.success && terminal.begin == 216 && terminal.end == 239 &&
                  terminal.tailFrames == 1,
              "素材末尾で存在しないframeへ丸まる位置をcutから外していません");

        pal.sourceInFrame = 40; // ceil(96) = 96
        pal.sourceOutFrame = 50;
        const auto middle = mvm::project::clipProducerRange(pal, 60, 1);
        check(middle.success && middle.end == 120 && middle.tailFrames == 0,
              "素材の途中で終わるclipに末尾の補完を付けました");
    }
    if (!std::filesystem::is_regular_file(firstClip) ||
        !std::filesystem::is_regular_file(secondClip) ||
        !std::filesystem::is_regular_file(ffmpeg)) {
        std::fprintf(stderr, "テスト素材がありません。pwsh scripts/make-testmedia.ps1 -Mode Smoke "
                             "を実行してください\n");
        return 2;
    }

    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT を初期化できません\n");
        return 1;
    }

    // --- 1. 正: 2 本を順に並べて 1 本の MP4 にする --------------------------
    const long long firstFrames = probeFrameCount(firstClip);
    const long long secondFrames = probeFrameCount(secondClip);
    const long long expectedFrames = firstFrames + secondFrames;
    check(expectedFrames > 0, "入力素材のフレーム数を取得できません");

    mvm::app::TimelineExportRequest invalidQualityRequest;
    invalidQualityRequest.videoCrf = 52;
    check(!mvm::app::mapTimelineExportPlan(
               makeProject(firstClip, secondClip, firstFrames, secondFrames), invalidQualityRequest)
               .success,
          "範囲外の映像CRFを拒否しません");

    const auto outputPath = testDirectory / L"m4-export.mp4";
    mvm::app::TimelineExportRequest request;
    request.outputPath = outputPath;
    long long reportedCompleted = -1;
    long long reportedTotal = -1;
    request.progress = [&](long long completed, long long total) {
        reportedCompleted = completed;
        reportedTotal = total;
        return false;
    };

    auto result = mvm::app::exportTimeline(
        makeProject(firstClip, secondClip, firstFrames, secondFrames), request);
    check(result.success, "2 clip の書き出しに失敗しました");
    if (!result.success)
        std::fprintf(stderr, "  error: %s\n", result.error.c_str());
    check(std::filesystem::exists(outputPath), "出力 MP4 がありません");
    check(!std::filesystem::exists(std::filesystem::path(outputPath).concat(".mvmtmp")),
          "一時ファイルが残っています");
    check(result.backend == mvm::app::TimelineExportResult::Backend::Sequential,
          "contiguous V1-onlyが既存sequential fast pathを外れました");
    check(reportedTotal == expectedFrames && reportedCompleted == reportedTotal,
          "書き出し進捗がtotal frameまで通知されません");

    // cancellation callbackがconsumerを停止し、一時/最終ファイルを残さないこと。
    const auto cancelledPath = testDirectory / L"m4-cancelled.mp4";
    mvm::app::TimelineExportRequest cancelledRequest;
    cancelledRequest.outputPath = cancelledPath;
    int cancelCallbackCount = 0;
    cancelledRequest.progress = [&](long long, long long) {
        ++cancelCallbackCount;
        return true;
    };
    const auto cancelled = mvm::app::exportTimeline(
        makeProject(firstClip, secondClip, firstFrames, secondFrames), cancelledRequest);
    check(!cancelled.success && cancelled.cancelled && cancelCallbackCount > 0,
          "書き出しキャンセルを失敗として識別できません");
    check(!std::filesystem::exists(cancelledPath) &&
              !std::filesystem::exists(std::filesystem::path(cancelledPath).concat(".mvmtmp")),
          "キャンセルした書き出しファイルが残っています");

    if (result.success) {
        MvmMltProbeResult probe{};
        const bool probed = mvm_mlt_probe_file(toUtf8(outputPath).c_str(), &probe) == 0 && probe.ok;
        check(probed, "出力 MP4 を probe できません");
        if (probed) {
            check(probe.has_video == 1, "出力 MP4 に映像がありません");
            check(probe.width == request.width && probe.height == request.height,
                  "出力 MP4 の解像度が要求と違います");
            check(probe.fps_num == request.fpsNum && probe.fps_den == request.fpsDen,
                  "出力 MP4 の fps が要求と違います");
            // 連結の境界で 1 フレームずれ得るため許容幅を 2 フレームとする。
            const long long difference = probe.frame_count - expectedFrames;
            check(difference <= 2 && difference >= -2,
                  "出力 MP4 のフレーム数が入力の合計と一致しません");
            if (difference > 2 || difference < -2) {
                std::fprintf(stderr, "  期待 %lld / 実際 %lld\n", expectedFrames,
                             probe.frame_count);
            }
        }
    }

    // producerの実尺より素材末尾境界だけが1 frame長い場合は、実尺へ限定して書き出す。
    // 2 frame以上の超過まで黙って切り詰めないnegative testも対にする。
    {
        auto terminalRounding = mvm::project::createDefaultProject();
        terminalRounding.timelineClips.push_back({mvm::project::TimelineClipKind::Video,
                                                  firstClip,
                                                  "terminal-rounding",
                                                  "terminal-rounding-id",
                                                  {},
                                                  60,
                                                  1,
                                                  firstFrames + 1,
                                                  0,
                                                  firstFrames + 1,
                                                  0,
                                                  {},
                                                  {},
                                                  {}});
        mvm::app::TimelineExportRequest roundingRequest;
        roundingRequest.outputPath = testDirectory / L"terminal-rounding.mp4";
        const auto rounded = mvm::app::exportTimeline(terminalRounding, roundingRequest);
        check(rounded.success && std::filesystem::is_regular_file(roundingRequest.outputPath),
              "素材末尾の1 frame丸め差を書き出し実尺へ合わせられません");
        if (!rounded.success)
            std::fprintf(stderr, "  error=%s\n", rounded.error.c_str());

        terminalRounding.timelineClips[0].sourceFrameCount = firstFrames + 2;
        terminalRounding.timelineClips[0].sourceOutFrame = firstFrames + 2;
        roundingRequest.outputPath = testDirectory / L"terminal-overrun.mp4";
        const auto overrun = mvm::app::exportTimeline(terminalRounding, roundingRequest);
        check(!overrun.success, "素材末尾の2 frame超過を黙って切り詰めました");
    }

    // --- 2. 29.97fps: source-native trim -> MLT producer位置の内容検査 ------
    {
        const auto fractional = testDirectory / L"fractional-source.mp4";
        check(generateFractionalFixture(ffmpeg, fractional), "29.97fps fixtureを生成できません");
        MvmMltProbeResult sourceProbe{};
        const bool sourceOk = mvm_mlt_probe_file(toUtf8(fractional).c_str(), &sourceProbe) == 0 &&
                              sourceProbe.ok && sourceProbe.fps_num == 30000 &&
                              sourceProbe.fps_den == 1001 && sourceProbe.frame_count >= 60;
        check(sourceOk, "29.97fps fixtureのFPSまたはframe countが不正です");
        if (sourceOk) {
            mvm::project::Project trimmed = mvm::project::createDefaultProject();
            trimmed.timelineClips.push_back({mvm::project::TimelineClipKind::Video,
                                             fractional,
                                             "fractional",
                                             "fractional-id",
                                             {},
                                             sourceProbe.fps_num,
                                             sourceProbe.fps_den,
                                             sourceProbe.frame_count,
                                             30,
                                             sourceProbe.frame_count,
                                             0,
                                             {},
                                             {},
                                             {}});
            const auto output = testDirectory / L"fractional-trimmed.mp4";
            mvm::app::TimelineExportRequest trimmedRequest;
            trimmedRequest.outputPath = output;
            const auto exported = mvm::app::exportTimeline(trimmed, trimmedRequest);
            check(exported.success, "29.97fps trimを書き出せません");
            if (!exported.success)
                std::fprintf(stderr, "  error: %s\n", exported.error.c_str());
            if (exported.success) {
                check(frameIsBlue(output, 0), "trim出力の先頭が選択した青区間ではありません");
                check(frameIsBlue(output, exported.frameCount - 1),
                      "trim出力の末尾が選択した青区間ではありません");
            }
        }
    }

    // --- 3. 負: clip が 0 本 ----------------------------------------------
    // --- M7b-3: 製品tractor経路の実MP4 overlay -----------------------------
    {
        const auto bottomPath = testDirectory / L"m7b-bottom.mp4";
        const auto topPath = testDirectory / L"m7b-top.mp4";
        check(generateSolidFixture(ffmpeg, bottomPath, L"blue"), "M7b V1 fixtureを生成できません");
        check(generateSolidFixture(ffmpeg, topPath, L"red"), "M7b V2 fixtureを生成できません");
        {
            const std::string topUtf8 = toUtf8(topPath);
            MvmExportClip invalid{};
            invalid.path = topUtf8.c_str();
            invalid.source_fps_num = 60;
            invalid.source_fps_den = 1;
            invalid.source_frame_count = 10;
            invalid.source_in_frame = 0;
            invalid.source_out_frame = 10;
            invalid.producer_in_frame = 0;
            invalid.producer_out_frame = 10;
            invalid.speed_num = 1;
            invalid.speed_den = 1;
            invalid.video_track = 1;
            invalid.timeline_start_frame = 0;
            invalid.timeline_duration_frames = 10;
            invalid.rect_width = 320;
            invalid.rect_height = 240;
            invalid.opacity_keyframe_count = 2;
            const MvmExportOpacityKeyframe invalidKeys[] = {{1, 1.0}, // local 0を意図的に欠落させる
                                                            {9, 1.0}};
            invalid.opacity_keyframes = invalidKeys;
            const MvmExportSpec invalidSpec{320, 240, 60, 1, 23, 10000, 4, 0, nullptr, nullptr};
            char invalidError[512] = {};
            const auto invalidOutput = testDirectory / L"m7b-invalid-key.mp4";
            check(mvm_mlt_export_two_track(&invalid, 1, 10, &invalidSpec,
                                           toUtf8(invalidOutput).c_str(), nullptr, invalidError,
                                           sizeof(invalidError)) != 0,
                  "transition-local端key欠落を拒否しません");
            check(!std::filesystem::exists(invalidOutput),
                  "invalid transition mappingで出力を生成しました");
        }
        mvm::project::Project overlaid = mvm::project::createDefaultProject();
        mvm::project::TimelineClip bottom{mvm::project::TimelineClipKind::Video,
                                          bottomPath,
                                          "bottom",
                                          "bottom-id",
                                          {},
                                          60,
                                          1,
                                          120,
                                          0,
                                          120,
                                          0,
                                          {},
                                          {},
                                          {}};
        bottom.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, 0};
        mvm::project::TimelineClip top{mvm::project::TimelineClipKind::Manim,
                                       topPath,
                                       "top",
                                       "top-id",
                                       {},
                                       60,
                                       1,
                                       120,
                                       10,
                                       70,
                                       20,
                                       {},
                                       {},
                                       {}};
        top.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, 1};
        top.effects.scaleXPercent = top.effects.scaleYPercent = 60;
        top.effects.positionXPercent = 10;
        top.effects.cropLeftPercent = 15;
        top.effects.opacityPercent = 50;
        top.effects.fadeInFrames = 10;
        top.effects.fadeOutFrames = 10;
        mvm::project::TimelineClip secondTop{mvm::project::TimelineClipKind::Manim,
                                             topPath,
                                             "top-2",
                                             "top-2-id",
                                             {},
                                             60,
                                             1,
                                             120,
                                             80,
                                             100,
                                             90,
                                             {},
                                             {},
                                             {}};
        secondTop.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, 1};
        // vector順をtimeline authorityにしないことも実経路で踏む。
        overlaid.timelineClips = {secondTop, top, bottom};
        mvm::app::TimelineExportRequest overlayRequest;
        overlayRequest.outputPath = testDirectory / L"m7b-product-overlay.mp4";
        overlayRequest.width = 320;
        overlayRequest.height = 240;
        const auto exported = mvm::app::exportTimeline(overlaid, overlayRequest);
        check(exported.success, "M7b product tractor overlayを書き出せません");
        if (!exported.success)
            std::fprintf(stderr, "  error: %s\n", exported.error.c_str());
        check(exported.backend == mvm::app::TimelineExportResult::Backend::Tractor,
              "M7b overlayがtractor経路を使用していません");
        check(exported.playlistBlankCount == 3 && exported.transitionCount == 2,
              "V2 playlist blankまたはclip単位transition数が不正です");
        check(exported.opaqueBlackAffineFilterCount == 0,
              "V2がopaque-black affine filterを使用しました");
        if (exported.success) {
            check(blue(pixelAt(overlayRequest.outputPath, 0, 160, 120)),
                  "V2開始前にV1が表示されません");
            check(blue(pixelAt(overlayRequest.outputPath, 20, 160, 120)),
                  "V2 fade-in先頭がV1を透過しません");
            const auto center = pixelAt(overlayRequest.outputPath, 45, 190, 120);
            const auto corner = pixelAt(overlayRequest.outputPath, 45, 10, 10);
            check(center.r > 70 && center.b > 70, "V2 opacity 50%がV1を透過する混色になりません");
            check(blue(corner), "V2 scale/crop外側がV1を露出しません");
            check(blue(pixelAt(overlayRequest.outputPath, 85, 160, 120)),
                  "2本のV2 clip間gapへtransition/effectが漏れています");
            const auto second = pixelAt(overlayRequest.outputPath, 95, 160, 120);
            check(second.r > 180 && second.b < 80,
                  "default V2 clipがoverlay transitionで表示されません");
        }
    }

    // --- 3. 負: clip が 0 本 ----------------------------------------------
    // --- M7a-2: 製品exportの固定effect chain -------------------------------
    {
        const auto source = testDirectory / L"m7a-effects-source.mp4";
        check(generateEffectsFixture(ffmpeg, source), "M7a effect fixtureを生成できません");
        mvm::project::Project effected = mvm::project::createDefaultProject();
        effected.timelineClips.push_back({mvm::project::TimelineClipKind::Manim,
                                          source,
                                          "effects",
                                          "effects-id",
                                          {},
                                          60,
                                          1,
                                          120,
                                          10,
                                          70,
                                          0,
                                          {},
                                          {},
                                          {}});
        auto& effects = effected.timelineClips.front().effects;
        effects.cropLeftPercent = 10;
        effects.cropTopPercent = 10;
        effects.cropRightPercent = 20;
        effects.cropBottomPercent = 5;
        effects.scaleXPercent = effects.scaleYPercent = 60;
        effects.positionXPercent = 12;
        effects.positionYPercent = -8;
        effects.opacityPercent = 50;
        effects.fadeInFrames = 10;
        effects.fadeOutFrames = 10;
        // 外接矩形の期待値は mapClipEffects の式を呼ばずに手で計算する (320x240)。
        //   crop 後の範囲: 幅 70% / 高さ 85%。60% に縮めて 134.4 x 122.4 px。
        //   中心は crop 範囲の中心 (45%, 52.5%) に位置 (+12%, -8%) を足した (182.4, 106.8)。
        //   -> x 115..249 / y 46..168
        // 以前は回転 25 度も同時に掛けて「230 x 210 未満」だけを見ていた。V1 の回転は
        // fix_rotate_z で書き出されておらず、この緩い上限では回転の有無を判別できなかった。
        const auto expectBox = [&](const EffectFrameMetrics& metrics, int minX, int maxX, int minY,
                                   int maxY, const char* what) {
            // 縮小と半透明で縁がぼけるので ±5 px。回転の有無の差は数十 px ある。
            const bool ok =
                std::abs(metrics.minX - minX) <= 5 && std::abs(metrics.maxX - maxX) <= 5 &&
                std::abs(metrics.minY - minY) <= 5 && std::abs(metrics.maxY - maxY) <= 5;
            if (!ok)
                std::fprintf(stderr, "  %s: 実測 x %d..%d / y %d..%d、期待 x %d..%d / y %d..%d\n",
                             what, metrics.minX, metrics.maxX, metrics.minY, metrics.maxY, minX,
                             maxX, minY, maxY);
            check(ok, what);
        };
        mvm::app::TimelineExportRequest effectRequest;
        effectRequest.outputPath = testDirectory / L"m7a-effects.mp4";
        effectRequest.width = 320;
        effectRequest.height = 240;
        const auto exported = mvm::app::exportTimeline(effected, effectRequest);
        check(exported.success, "M7a effect付きclipを書き出せません");
        if (!exported.success)
            std::fprintf(stderr, "  error: %s\n", exported.error.c_str());
        if (exported.success) {
            const auto first = effectMetrics(effectRequest.outputPath, 0);
            const auto middle = effectMetrics(effectRequest.outputPath, 30);
            const auto last = effectMetrics(effectRequest.outputPath, exported.frameCount - 1);
            check(middle.mean > 5.0, "effect出力の中間frameが空です");
            check(first.mean < middle.mean * 0.2 && last.mean < middle.mean * 0.2,
                  "source-native Fade In/Outが実画素へ反映されません");
            expectBox(middle, 115, 249, 46, 168, "crop/scale/position の外接矩形が期待と違います");
        }

        // crop と回転を同時に掛ける。crop 範囲 134.4 x 122.4 px を中心 (182.4, 106.8) で 25 度
        // 回すと、外接矩形は 134.4cos25+122.4sin25 = 173.5 x 134.4sin25+122.4cos25 = 167.7 px。
        //   -> x 96..269 / y 23..191
        // 以前は crop filter が frame の寸法を変え、平行四辺形 (x 58..305 / y 41..171)
        // になっていた。
        {
            auto both = effected;
            both.timelineClips.front().effects.rotationDegrees = 25;
            effectRequest.outputPath = testDirectory / L"m7a-effects-crop-rotated.mp4";
            const auto bothExport = mvm::app::exportTimeline(both, effectRequest);
            check(bothExport.success, "crop + 回転の clip を書き出せません");
            if (bothExport.success)
                expectBox(effectMetrics(effectRequest.outputPath, 30), 96, 269, 23, 191,
                          "crop + 回転の外接矩形が回転した矩形になりません (shear)");
        }

        // 回転は crop 無しでも見る。192 x 144 px (60%) を中心 (198.4, 100.8) で 25 度回すと、
        // 外接矩形は 192cos25+144sin25 = 234.9 x 192sin25+144cos25 = 211.6 px。
        //   -> x 81..316 / y -5..207 (上は画面で切れて 0)
        // crop と回転を同時に使うと MLT の書き出しは shear になる (docs/premiere-like-editing.md
        // §18.4 の [事実])。ここでは組み合わせない。
        auto rotated = effected;
        auto& rotation = rotated.timelineClips.front().effects;
        rotation.cropLeftPercent = rotation.cropTopPercent = 0;
        rotation.cropRightPercent = rotation.cropBottomPercent = 0;
        rotation.rotationDegrees = 25;
        effectRequest.outputPath = testDirectory / L"m7a-effects-rotated.mp4";
        const auto rotatedExport = mvm::app::exportTimeline(rotated, effectRequest);
        check(rotatedExport.success, "回転付きclipを書き出せません");
        if (rotatedExport.success)
            expectBox(effectMetrics(effectRequest.outputPath, 30), 81, 316, 0, 207,
                      "V1 の回転が外接矩形に反映されません");
    }

    // --- tractor末尾補完frameにもcrop/effectが掛かる ------------------------
    // 素材末尾の+1丸めでproducer実尺がtimeline配置尺より1 frame短くなるclipを作り、
    // 補完frame（出力末尾）が直前の本体frameと同じ変換を受けていることを実画素で確かめる。
    // tailが無変換なら、V1はscale 60%の黒枠が消え、V2はcropした左端の緑帯が戻る。
    {
        const auto source = testDirectory / L"padding-effects-source.mp4";
        const auto background = testDirectory / L"padding-background.mp4";
        check(generateEffectsFixture(ffmpeg, source), "padding effect fixtureを生成できません");
        check(generateSolidFixture(ffmpeg, background, L"blue"),
              "padding V1 fixtureを生成できません");
        const long long sourceFrames = probeFrameCount(source);
        check(sourceFrames == 120, "padding fixtureのframe数が前提と違います");
        const std::string sourceUtf8 = toUtf8(source);
        const std::string backgroundUtf8 = toUtf8(background);
        constexpr long long kDuration = 61;

        MvmExportClip padded{};
        padded.path = sourceUtf8.c_str();
        padded.source_fps_num = 60;
        padded.source_fps_den = 1;
        // probe上の素材尺をproducer実尺+1とし、末尾丸めclampで1 frame不足させる。
        padded.source_frame_count = sourceFrames + 1;
        padded.source_in_frame = sourceFrames + 1 - kDuration;
        padded.source_out_frame = sourceFrames + 1;
        // 60fps 素材を 60fps へ置くので producer 位置は素材 frame と同じ。
        padded.producer_in_frame = padded.source_in_frame;
        padded.producer_out_frame = padded.source_out_frame;
        padded.speed_num = 1;
        padded.speed_den = 1;
        padded.timeline_start_frame = 0;
        padded.timeline_duration_frames = kDuration;
        padded.opacity_keyframe_count = 2;
        const MvmExportOpacityKeyframe paddedKeys[] = {{0, 1.0}, {kDuration - 1, 1.0}};
        padded.opacity_keyframes = paddedKeys;

        MvmExportClip v1Effect = padded;
        v1Effect.video_track = 0;
        v1Effect.effects_enabled = 1;
        v1Effect.crop_left = 32;
        v1Effect.rect_x = 64;
        v1Effect.rect_y = 48;
        v1Effect.rect_width = 192;
        v1Effect.rect_height = 144;

        MvmExportClip v1Plain{};
        v1Plain.path = backgroundUtf8.c_str();
        v1Plain.source_fps_num = 60;
        v1Plain.source_fps_den = 1;
        v1Plain.source_frame_count = sourceFrames;
        v1Plain.source_in_frame = 0;
        v1Plain.source_out_frame = kDuration;
        v1Plain.producer_in_frame = 0;
        v1Plain.producer_out_frame = kDuration;
        v1Plain.speed_num = 1;
        v1Plain.speed_den = 1;
        v1Plain.timeline_duration_frames = kDuration;
        MvmExportClip v2Crop = padded;
        v2Crop.video_track = 1;
        v2Crop.crop_left = 96;
        v2Crop.rect_width = 320;
        v2Crop.rect_height = 240;

        struct PaddingCase {
            const wchar_t* name;
            std::vector<MvmExportClip> clips;
        };

        const PaddingCase cases[] = {{L"padding-v1-effects.mp4", {v1Effect}},
                                     {L"padding-v2-crop.mp4", {v1Plain, v2Crop}}};
        const MvmExportSpec paddingSpec{320, 240, 60, 1, 23, 60000, 4, 0, nullptr, nullptr};
        for (const auto& paddingCase : cases) {
            const auto output = testDirectory / paddingCase.name;
            char paddingError[512] = {};
            const int status = mvm_mlt_export_two_track(
                paddingCase.clips.data(), static_cast<int>(paddingCase.clips.size()), kDuration,
                &paddingSpec, toUtf8(output).c_str(), nullptr, paddingError, sizeof(paddingError));
            check(status == 0, "末尾補完が必要なclipをtractorで書き出せません");
            if (status != 0) {
                std::fprintf(stderr, "  %ls: %s\n", paddingCase.name, paddingError);
                continue;
            }
            check(probeFrameCount(output) == kDuration, "末尾補完後の出力尺がtimeline尺と違います");
            // 本体の最終frame(kDuration-2)と補完frame(kDuration-1)を格子状に比較する。
            int mismatches = 0;
            for (int y = 8; y < 240; y += 16) {
                for (int x = 8; x < 320; x += 16) {
                    const auto body = pixelAt(output, kDuration - 2, x, y);
                    const auto tail = pixelAt(output, kDuration - 1, x, y);
                    if (std::abs(body.r - tail.r) + std::abs(body.g - tail.g) +
                            std::abs(body.b - tail.b) >
                        60)
                        ++mismatches;
                }
            }
            check(mismatches == 0, "末尾補完frameが本体と同じcrop/effectを受けていません");
            if (mismatches != 0)
                std::fprintf(stderr, "  %ls: 不一致画素 %d\n", paddingCase.name, mismatches);
        }
    }

    // --- 非timeline fps素材で、書き出しとpreviewが同じ素材frameを出す -------------
    // frame n の輝度を Y = 40 + 8n にした 48fps 素材を in = 3 から 60fps timeline へ置く。
    // MLT は位置 p (素材の 0 frame から数えた 60fps 位置) に frame floor(p / 1.25 + 1/2) を出す。
    // cut は ceil(3 * 1.25) = 4 から始まり、位置 4..11 は手計算で次の frame になる。
    //   4:3.2 5:4.0 6:4.8 7:5.6 8:6.4 9:7.2 10:8.0 11:8.8
    // 48fps -> 60fps では p / 1.25 がちょうど 1/2 にならないので、MLT の double 誤差に依存しない。
    // 以前は書き出しの cut を floor(3.75) = 3 から始め (先頭で in より前の frame 2 が出た)、
    // preview は floor で 3,3,4,... を出していた。どちらもこの検査で落ちる。
    //
    // 書き出しは色変換で輝度の絶対値が変わるので、同じ輝度式の 60fps 素材を 1:1 で後ろに置き、
    // 同じ経路を通した「素材 frame -> 画素値」の表と照合する。
    {
        const auto generateRamp = [&](const std::filesystem::path& output, const wchar_t* rate) {
            const std::wstring lavfi =
                std::wstring(L"color=c=black:s=320x240:r=") + rate + L":d=0.5";
            return _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                            L"-f", L"lavfi", L"-i", lavfi.c_str(), L"-vf",
                            L"geq=lum='40+8*N':cb=128:cr=128", L"-c:v", L"libx264", L"-preset",
                            L"ultrafast", L"-qp", L"0", L"-pix_fmt", L"yuv420p", output.c_str(),
                            static_cast<wchar_t*>(nullptr)) == 0;
        };
        const auto source = testDirectory / L"frame-mapping-48fps.mp4";
        const auto calibration = testDirectory / L"frame-mapping-60fps.mp4";
        check(generateRamp(source, L"48") && generateRamp(calibration, L"60"),
              "frame対応検査用のfixtureを生成できません");
        const long long sourceFrames = probeFrameCount(source);
        const long long calibrationFrames = probeFrameCount(calibration);
        check(sourceFrames == 24 && calibrationFrames == 30,
              "frame対応検査用fixtureのframe数が前提と違います");

        const auto rampClip = [](const std::filesystem::path& path, const char* id,
                                 std::int64_t fps, std::int64_t frames, std::int64_t in,
                                 std::int64_t out, std::int64_t start) {
            mvm::project::TimelineClip clip;
            clip.kind = mvm::project::TimelineClipKind::Video;
            clip.mediaPath = path;
            clip.name = id;
            clip.id = id;
            clip.sourceFpsNum = fps;
            clip.sourceFpsDen = 1;
            clip.sourceFrameCount = frames;
            clip.sourceInFrame = in;
            clip.sourceOutFrame = out;
            clip.timelineStartFrame = start;
            clip.track = {mvm::project::TrackKind::Video, 0};
            return clip;
        };
        // 続けて 60fps 素材を 40% (速度 2/5) で in = 2..8 に置く。実効 24fps で R = 2.5、
        // 原点は ceil(5) = 5 で尺は ceil(20) - 5 = 15。位置 5..19 を 2.5 で割って四捨五入する。
        //   5:2.0 6:2.4 7:2.8 8:3.2 9:3.6 10:4.0 11:4.4 12:4.8 13:5.2 14:5.6 15:6.0 16:6.4
        //   17:6.8 18:7.2 19:7.6
        // 最後の frame 8 は trim の外側 (out = 8) で、丸めで出るのは MLT の timewarp と同じ。
        // p / 2.5 はちょうど 1/2 にならない (4p = 5(2k + 1) は偶奇が合わない)。
        std::vector<long long> expected = {3, 4, 5, 6, 6, 7, 8, 9};
        const std::vector<long long> slowed = {2, 2, 3, 3, 4, 4, 4, 5, 5, 6, 6, 6, 7, 7, 8};
        const auto mixedLength = static_cast<std::int64_t>(expected.size());
        expected.insert(expected.end(), slowed.begin(), slowed.end());
        constexpr long long kCalibrationFrames = 12;
        const auto mappedLength = static_cast<std::int64_t>(expected.size());
        auto slowedClip =
            rampClip(calibration, "frame-mapping-slowed", 60, calibrationFrames, 2, 8, mixedLength);
        slowedClip.speedNum = 2;
        slowedClip.speedDen = 5;
        mvm::project::Project mapped = mvm::project::createDefaultProject();
        mapped.timelineClips = {rampClip(source, "frame-mapping", 48, sourceFrames, 3, 9, 0),
                                slowedClip,
                                rampClip(calibration, "calibration", 60, calibrationFrames, 0,
                                         kCalibrationFrames, mappedLength)};

        for (std::int64_t frame = 0; frame < mappedLength; ++frame) {
            const auto preview = mvm::app::mapTimelinePreviewFrame(mapped, frame);
            check(preview.success && preview.layers.size() == 1 &&
                      preview.layers[0].sourceFrameNumber ==
                          expected[static_cast<std::size_t>(frame)],
                  "previewの素材frameが手計算の対応と違います");
        }

        mvm::app::TimelineExportRequest mappedRequest;
        mappedRequest.outputPath = testDirectory / L"frame-mapping-export.mp4";
        mappedRequest.width = 320;
        mappedRequest.height = 240;
        const auto exported = mvm::app::exportTimeline(mapped, mappedRequest);
        check(exported.success, "frame対応検査のclipを書き出せません");
        if (exported.success) {
            check(probeFrameCount(mappedRequest.outputPath) == mappedLength + kCalibrationFrames,
                  "frame対応検査の出力尺が違います");
            std::vector<int> table;
            for (long long n = 0; n < kCalibrationFrames; ++n)
                table.push_back(pixelAt(mappedRequest.outputPath, mappedLength + n, 160, 120).r);
            // 表が素材 frame ごとに区別できること (隣の frame と 4 以上離れていること)。
            bool distinct = true;
            for (std::size_t n = 1; n < table.size(); ++n)
                distinct = distinct && table[n] - table[n - 1] >= 4;
            check(distinct, "校正表で素材frameを区別できません");
            int compared = 0;
            for (std::int64_t frame = 0; frame < mappedLength; ++frame) {
                const int r = pixelAt(mappedRequest.outputPath, frame, 160, 120).r;
                long long nearest = 0;
                for (long long n = 1; n < kCalibrationFrames; ++n) {
                    if (std::abs(table[static_cast<std::size_t>(n)] - r) <
                        std::abs(table[static_cast<std::size_t>(nearest)] - r))
                        nearest = n;
                }
                ++compared;
                if (nearest != expected[static_cast<std::size_t>(frame)]) {
                    check(false, "書き出しの素材frameが手計算の対応と違います");
                    std::fprintf(stderr, "  timeline %lld: 素材 frame %lld (期待 %lld, r=%d)\n",
                                 static_cast<long long>(frame), nearest,
                                 expected[static_cast<std::size_t>(frame)], r);
                }
            }
            check(compared == static_cast<int>(mappedLength),
                  "書き出しのframe対応を全frame比較していません");
        }
    }

    // --- 速度を変えた clip の音声は速度に連動する (timewarp warp_pitch=0) --------------
    // 440Hz・1 秒の WAV を 50% で置くと、尺が 2 秒になり 220Hz になる。
    {
        const auto wave = testDirectory / L"speed-source.wav";
        check(_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                       L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=1:sample_rate=48000",
                       L"-c:a", L"pcm_s16le", wave.c_str(), static_cast<wchar_t*>(nullptr)) == 0,
              "速度検証用WAVを生成できません");
        auto project = mvm::project::createDefaultProject();
        mvm::project::TimelineClip video;
        video.kind = mvm::project::TimelineClipKind::Video;
        video.mediaPath = firstClip;
        video.name = "speed-video";
        video.id = "speed-video";
        video.sourceFpsNum = 60;
        video.sourceFpsDen = 1;
        video.sourceFrameCount = firstFrames;
        video.sourceOutFrame = 60;
        video.speedNum = 1;
        video.speedDen = 2;
        video.track = {mvm::project::TrackKind::Video, 0};
        auto sound = video;
        sound.kind = mvm::project::TimelineClipKind::Audio;
        sound.track = {mvm::project::TrackKind::Audio, 0};
        sound.mediaPath = wave;
        sound.id = "speed-audio";
        sound.sourceFrameCount = 60;
        project.timelineClips = {video, sound};
        mvm::app::TimelineExportRequest speedRequest;
        speedRequest.outputPath = testDirectory / L"speed-half.mp4";
        speedRequest.width = 320;
        speedRequest.height = 240;
        const auto exported = mvm::app::exportTimeline(project, speedRequest);
        check(exported.success, "50%のclipを書き出せません");
        if (!exported.success)
            std::fprintf(stderr, "  %s\n", exported.error.c_str());
        if (exported.success) {
            check(probeFrameCount(speedRequest.outputPath) == 120,
                  "50%のclipの出力尺が2倍になりません");
            const auto raw = testDirectory / L"speed-half.f32";
            check(_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                           L"-i", speedRequest.outputPath.c_str(), L"-map", L"0:a:0", L"-ac", L"1",
                           L"-ar", L"48000", L"-c:a", L"pcm_f32le", L"-f", L"f32le", raw.c_str(),
                           static_cast<wchar_t*>(nullptr)) == 0,
                  "50%書き出しのPCMを抽出できません");
            std::ifstream stream(raw, std::ios::binary | std::ios::ate);
            std::vector<float> samples;
            if (stream) {
                samples.resize(static_cast<std::size_t>(stream.tellg()) / sizeof(float));
                stream.seekg(0);
                stream.read(reinterpret_cast<char*>(samples.data()),
                            static_cast<std::streamsize>(samples.size() * sizeof(float)));
            }
            // 中央の 1 秒 (0.5〜1.5 秒) の負 -> 正のゼロ交差を数える。
            int crossings = 0;
            if (samples.size() >= 72000) {
                for (std::size_t i = 24001; i < 72000; ++i)
                    crossings += samples[i - 1] < 0.0F && samples[i] >= 0.0F ? 1 : 0;
            }
            check(samples.size() >= 95000 && std::abs(crossings - 220) <= 3,
                  "50%の音声が2倍の尺・半分の周波数になりません");
            std::fprintf(stderr, "  50%%音声: %zu sample, 中央1秒のゼロ交差 %d\n", samples.size(),
                         crossings);
        }
    }

    // --- 3. 負: clip が 0 本 ----------------------------------------------
    {
        mvm::project::Project empty = mvm::project::createDefaultProject();
        mvm::app::TimelineExportRequest emptyRequest;
        emptyRequest.outputPath = testDirectory / L"m4-empty.mp4";
        const auto emptyResult = mvm::app::exportTimeline(empty, emptyRequest);
        check(!emptyResult.success, "clip 0 本の書き出しが成功してしまいました");
        check(!std::filesystem::exists(emptyRequest.outputPath),
              "clip 0 本なのに出力ファイルが作られました");
    }

    // --- 4. 負: 素材が存在しない ------------------------------------------
    {
        const auto missing = testDirectory / L"missing.mp4";
        mvm::app::TimelineExportRequest missingRequest;
        missingRequest.outputPath = testDirectory / L"m4-missing.mp4";
        const auto missingResult = mvm::app::exportTimeline(
            makeProject(firstClip, missing, firstFrames, secondFrames), missingRequest);
        check(!missingResult.success, "存在しない素材の書き出しが成功してしまいました");
        check(!std::filesystem::exists(missingRequest.outputPath),
              "失敗したのに出力ファイルが残っています");
        check(!std::filesystem::exists(
                  std::filesystem::path(missingRequest.outputPath).concat(".mvmtmp")),
              "失敗したのに一時ファイルが残っています");
    }

    // 音量filterの実出力。設定の読み戻しだけでは無音・増幅を証明できない。
    {
        const auto wave = testDirectory / L"gain-source.wav";
        check(_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                       L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=1:sample_rate=48000",
                       L"-c:a", L"pcm_s16le", wave.c_str(), static_cast<wchar_t*>(nullptr)) == 0,
              "gain検証用WAVを生成できません");
        auto renderGain = [&](const char* name, double base,
                              std::vector<mvm::project::ClipKeyframe> keys) {
            auto project = mvm::project::createDefaultProject();
            mvm::project::TimelineClip video;
            video.kind = mvm::project::TimelineClipKind::Video;
            video.mediaPath = firstClip;
            video.name = "video";
            video.id = "gain-video";
            video.sourceFpsNum = 60;
            video.sourceFpsDen = 1;
            video.sourceFrameCount = firstFrames;
            video.sourceOutFrame = 60;
            video.track = {mvm::project::TrackKind::Video, 0};
            project.timelineClips.push_back(video);
            auto sound = video;
            sound.kind = mvm::project::TimelineClipKind::Audio;
            sound.track = {mvm::project::TrackKind::Audio, 0};
            sound.mediaPath = wave;
            sound.id = "gain-audio";
            sound.effects.volumePercent = base;
            sound.effects.volumeKeys = std::move(keys);
            project.timelineClips.push_back(sound);
            mvm::app::TimelineExportRequest gainRequest;
            gainRequest.outputPath = testDirectory / (std::string(name) + ".mp4");
            gainRequest.width = 320;
            gainRequest.height = 240;
            const auto exported = mvm::app::exportTimeline(project, gainRequest);
            check(exported.success, "gain付きMLT書き出しに失敗しました");
            if (!exported.success) {
                std::fprintf(stderr, "  %s: %s\n", name, exported.error.c_str());
                return std::vector<float>{};
            }
            const auto raw = testDirectory / (std::string(name) + ".f32");
            check(_wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                           L"-i", gainRequest.outputPath.c_str(), L"-map", L"0:a:0", L"-ac", L"1",
                           L"-c:a", L"pcm_f32le", L"-f", L"f32le", raw.c_str(),
                           static_cast<wchar_t*>(nullptr)) == 0,
                  "gain書き出しのPCMを抽出できません");
            std::ifstream stream(raw, std::ios::binary | std::ios::ate);
            if (!stream)
                return std::vector<float>{};
            const auto bytes = stream.tellg();
            std::vector<float> samples(static_cast<std::size_t>(bytes) / sizeof(float));
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(samples.data()),
                        static_cast<std::streamsize>(samples.size() * sizeof(float)));
            return samples;
        };
        const auto unity = renderGain("gain-unity", 100.0, {});
        const auto silent = renderGain("gain-silent", 0.0, {});
        const auto boosted = renderGain("gain-boost", 200.0, {});
        const auto ramp = renderGain("gain-ramp", 100.0, {{0, 0.0}, {59, 200.0}});
        const auto rms = [](const std::vector<float>& samples, std::size_t begin,
                            std::size_t count) {
            if (samples.size() < begin + count)
                return 0.0;
            double power = 0.0;
            for (std::size_t i = begin; i < begin + count; ++i)
                power += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
            return std::sqrt(power / static_cast<double>(count));
        };
        const double reference = rms(unity, 20000, 8000);
        check(reference > 0.02, "基準音声が実際に出力されていません");
        check(rms(silent, 20000, 8000) < reference * 0.03, "音量0%が実際のPCMを無音にしていません");
        check(rms(boosted, 20000, 8000) > reference * 1.6, "音量200%が実際のPCMを増幅していません");
        check(rms(ramp, 4000, 4000) < rms(ramp, 38000, 4000) * 0.3,
              "時間変化する音量が実際のPCMへ反映されていません");
    }

    mvm_mlt_runtime_shutdown();

    if (gFailures != 0) {
        std::fprintf(stderr, "m4 timeline export: FAIL (%d 件)\n", gFailures);
        return 1;
    }
    std::puts("m4 timeline export: PASS");
    return 0;
}
