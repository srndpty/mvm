// M4: timeline export の focused test。
//
// 検査するのは 3 点だけに絞る (M4 のスコープ)。
//   1. 実素材 2 本を順に並べて 1 本の再生可能な MP4 になる
//   2. clip が 0 本なら失敗する
//   3. 素材が存在しなければ失敗する
//
// 期待フレーム数は実装の式を再利用せず、入力を probe して独立に足し合わせる。

#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
                                  const std::filesystem::path& second) {
    mvm::project::Project project;
    const auto firstFrames = probeFrameCount(first);
    const auto secondFrames = probeFrameCount(second);
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Video, first, "normal",
                                     "normal-id", 60, 1, firstFrames, 0, firstFrames, 0});
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Manim, second, "manim",
                                     "manim-id", 60, 1, secondFrames, 0, secondFrames,
                                     firstFrames});
    return project;
}

} // namespace

int main(int argc, char** argv) {
    mvm_enable_utf8_console();
    if (argc != 4) {
        std::fprintf(stderr, "使い方: test_timeline_export <test-dir> <clip1> <clip2>\n");
        return 2;
    }

    const auto testDirectory = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto firstClip = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto secondClip = std::filesystem::absolute(fromUtf8(argv[3]));

    std::error_code error;
    std::filesystem::remove_all(testDirectory, error);
    error.clear();
    std::filesystem::create_directories(testDirectory, error);
    if (error) {
        std::fprintf(stderr, "test directory を作成できません\n");
        return 1;
    }
    if (!std::filesystem::is_regular_file(firstClip) ||
        !std::filesystem::is_regular_file(secondClip)) {
        std::fprintf(stderr, "テスト素材がありません。pwsh scripts/make-testmedia.ps1 -Mode Smoke "
                             "を実行してください\n");
        return 2;
    }

    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT を初期化できません\n");
        return 1;
    }

    // --- 1. 正: 2 本を順に並べて 1 本の MP4 にする --------------------------
    const long long expectedFrames = probeFrameCount(firstClip) + probeFrameCount(secondClip);
    check(expectedFrames > 0, "入力素材のフレーム数を取得できません");

    const auto outputPath = testDirectory / L"m4-export.mp4";
    mvm::app::TimelineExportRequest request;
    request.outputPath = outputPath;

    auto result = mvm::app::exportTimeline(makeProject(firstClip, secondClip), request);
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

    // --- 2. 負: clip が 0 本 ----------------------------------------------
    {
        mvm::project::Project empty;
        mvm::app::TimelineExportRequest emptyRequest;
        emptyRequest.outputPath = testDirectory / L"m4-empty.mp4";
        const auto emptyResult = mvm::app::exportTimeline(empty, emptyRequest);
        check(!emptyResult.success, "clip 0 本の書き出しが成功してしまいました");
        check(!std::filesystem::exists(emptyRequest.outputPath),
              "clip 0 本なのに出力ファイルが作られました");
    }

    // --- 3. 負: 素材が存在しない ------------------------------------------
    {
        const auto missing = testDirectory / L"missing.mp4";
        mvm::app::TimelineExportRequest missingRequest;
        missingRequest.outputPath = testDirectory / L"m4-missing.mp4";
        const auto missingResult =
            mvm::app::exportTimeline(makeProject(firstClip, missing), missingRequest);
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
