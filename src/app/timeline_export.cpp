#include "app/timeline_export.h"

#include "media/mlt/mvm_mlt_export.h"
#include "project/timeline_edit.h"

#include <system_error>
#include <vector>

namespace mvm::app {
namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

TimelineExportResult exportTimeline(const project::Project& project,
                                    const TimelineExportRequest& request) {
    TimelineExportResult result;

    if (request.outputPath.empty()) {
        result.error = "書き出し先が指定されていません";
        return result;
    }
    if (project.timelineClips.empty()) {
        result.error = "timeline に clip がありません";
        return result;
    }
    const auto timeline = project::validateTimeline(project);
    if (!timeline.success) {
        result.error = timeline.error;
        return result;
    }

    std::error_code pathError;
    const auto outputPath = std::filesystem::absolute(request.outputPath, pathError);
    if (pathError || outputPath.filename().empty()) {
        result.error = "書き出し先のパスが不正です";
        return result;
    }
    const auto outputDirectory = outputPath.parent_path();
    std::filesystem::create_directories(outputDirectory, pathError);
    if (pathError) {
        result.error = "書き出し先の directory を作成できません: " + pathError.message();
        return result;
    }

    // clip のパス文字列は C API へ const char* で渡すため、
    // 呼び出しが終わるまで生存させる。
    std::vector<std::string> clipPaths;
    clipPaths.reserve(project.timelineClips.size());
    for (const auto& clip : project.timelineClips) {
        if (clip.mediaPath.empty()) {
            result.error = "clip '" + clip.name + "' の media path が空です";
            return result;
        }
        clipPaths.push_back(pathToUtf8(clip.mediaPath));
    }

    std::vector<MvmExportClip> clips;
    clips.reserve(clipPaths.size());
    for (std::size_t index = 0; index < clipPaths.size(); ++index) {
        const auto& clip = project.timelineClips[index];
        clips.push_back(MvmExportClip{clipPaths[index].c_str(), clip.sourceFpsNum,
                                      clip.sourceFpsDen, clip.sourceInFrame,
                                      clip.sourceOutFrame});
    }

    const MvmExportSpec spec{
        .width = request.width,
        .height = request.height,
        .fps_num = request.fpsNum,
        .fps_den = request.fpsDen,
        .timeout_ms = request.timeoutMs,
    };

    // 一時ファイルへ書き、検証を通ってから正規名へ rename する。
    // 途中で失敗した出力を最終ファイル名で残さない。
    auto temporaryPath = outputPath;
    temporaryPath += ".mvmtmp";
    std::filesystem::remove(temporaryPath, pathError);

    const std::string temporaryUtf8 = pathToUtf8(temporaryPath);
    MvmExportResult exported{};
    char error[1024] = {0};
    if (mvm_mlt_export_sequence(clips.data(), static_cast<int>(clips.size()), &spec,
                                temporaryUtf8.c_str(), &exported, error, sizeof(error)) != 0) {
        std::filesystem::remove(temporaryPath, pathError);
        result.error = error[0] ? error : "書き出しに失敗しました";
        return result;
    }

    std::filesystem::rename(temporaryPath, outputPath, pathError);
    if (pathError) {
        std::filesystem::remove(temporaryPath, pathError);
        result.error = "書き出したファイルを正規名へ rename できません: " + pathError.message();
        return result;
    }

    result.outputPath = outputPath;
    result.frameCount = exported.frame_count;
    result.durationSec = exported.duration_sec;
    result.success = true;
    return result;
}

} // namespace mvm::app
