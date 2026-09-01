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
#include "media/mlt/mvm_mlt_export.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <process.h>
#include <string>

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
    mvm::project::Project project;
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Video, first, "normal",
                                     "normal-id", 60, 1, firstFrames, 0, firstFrames, 0});
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Manim, second, "manim",
                                     "manim-id", 60, 1, secondFrames, 0, secondFrames,
                                     firstFrames});
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
                (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4;
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

    long long producerBoundary = -1;
    check(mvm_source_boundary_to_producer_boundary(30, 30000, 1001, 60, 1, &producerBoundary) ==
                  0 &&
              producerBoundary == 60,
          "29.97fps source境界をMLT producer位置へ変換できません");
    check(mvm_source_boundary_to_producer_boundary(60, 30000, 1001, 60, 1, &producerBoundary) ==
                  0 &&
              producerBoundary == 120,
          "MLT producer境界変換がProject timelineのceil意味論と分離されていません");
    check(mvm_source_boundary_to_producer_boundary(-1, 60, 1, 60, 1, &producerBoundary) != 0,
          "負のMLT producer境界を拒否しません");
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

    const auto outputPath = testDirectory / L"m4-export.mp4";
    mvm::app::TimelineExportRequest request;
    request.outputPath = outputPath;

    auto result = mvm::app::exportTimeline(
        makeProject(firstClip, secondClip, firstFrames, secondFrames), request);
    check(result.success, "2 clip の書き出しに失敗しました");
    if (!result.success)
        std::fprintf(stderr, "  error: %s\n", result.error.c_str());
    check(std::filesystem::exists(outputPath), "出力 MP4 がありません");
    check(!std::filesystem::exists(std::filesystem::path(outputPath).concat(".mvmtmp")),
          "一時ファイルが残っています");

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
            mvm::project::Project trimmed;
            trimmed.timelineClips.push_back({mvm::project::TimelineClipKind::Video, fractional,
                                             "fractional", "fractional-id", sourceProbe.fps_num,
                                             sourceProbe.fps_den, sourceProbe.frame_count, 30,
                                             sourceProbe.frame_count, 0});
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
    // --- M7a-2: 製品exportの固定effect chain -------------------------------
    {
        const auto source = testDirectory / L"m7a-effects-source.mp4";
        check(generateEffectsFixture(ffmpeg, source), "M7a effect fixtureを生成できません");
        mvm::project::Project effected;
        effected.timelineClips.push_back({mvm::project::TimelineClipKind::Manim, source, "effects",
                                          "effects-id", 60, 1, 120, 10, 70, 0});
        auto& effects = effected.timelineClips.front().effects;
        effects.cropLeftPercent = 10;
        effects.cropTopPercent = 10;
        effects.cropRightPercent = 20;
        effects.cropBottomPercent = 5;
        effects.scalePercent = 60;
        effects.positionXPercent = 12;
        effects.positionYPercent = -8;
        effects.rotationDegrees = 25;
        effects.opacityPercent = 50;
        effects.fadeInFrames = 10;
        effects.fadeOutFrames = 10;
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
            check(middle.maxX > middle.minX && middle.maxY > middle.minY &&
                      middle.maxX - middle.minX < 230 && middle.maxY - middle.minY < 210,
                  "非zoom crop/scale/rotationの外接矩形が反映されません");
        }
    }

    // --- 3. 負: clip が 0 本 ----------------------------------------------
    {
        mvm::project::Project empty;
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

    mvm_mlt_runtime_shutdown();

    if (gFailures != 0) {
        std::fprintf(stderr, "m4 timeline export: FAIL (%d 件)\n", gFailures);
        return 1;
    }
    std::puts("m4 timeline export: PASS");
    return 0;
}
