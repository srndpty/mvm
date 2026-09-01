#include "app/manim_clip_workflow.h"

#include "media/manim/manim_fingerprint.h"
#include "media/manim/manim_renderer.h"
#include "project/project_json.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace mvm::app {
namespace {

bool sameScript(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    if (!error)
        return equivalent;
    return left.lexically_normal() == right.lexically_normal();
}

} // namespace

ManimClipGenerationResult generateManimClip(project::Project& project,
                                            const ManimClipGenerationRequest& request) {
    ManimClipGenerationResult result;
    if (request.projectPath.empty() || request.projectPath.filename().empty()) {
        result.error = "Project JSON pathが不正です";
        return result;
    }

    std::error_code pathError;
    const auto absoluteProjectPath = std::filesystem::absolute(request.projectPath, pathError);
    if (pathError) {
        result.error = "Project JSON pathを解決できません: " + pathError.message();
        return result;
    }

    manim::ManimRenderRequest renderRequest;
    renderRequest.manimExecutablePath = request.manimExecutablePath;
    renderRequest.scriptPath = request.scriptPath;
    renderRequest.sceneName = request.sceneName;
    renderRequest.outputDirectory = absoluteProjectPath.parent_path() / "cache" / "manim";
    renderRequest.width = request.width;
    renderRequest.height = request.height;
    renderRequest.fps = request.fps;

    const manim::ManimRenderResult rendered = manim::renderManim(renderRequest);
    result.outputVideoPath = rendered.outputVideoPath;
    result.exitCode = rendered.exitCode;
    result.stdoutText = rendered.stdoutText;
    result.stderrText = rendered.stderrText;
    if (!rendered.success) {
        result.error = "Manim renderに失敗しました";
        return result;
    }

    const manim::ManimFingerprintResult fingerprint =
        manim::fingerprintManimSource(request.scriptPath);
    if (!fingerprint.success) {
        result.error = fingerprint.error;
        return result;
    }

    project::ManimAssetResult created = project::createReadyManimAsset(
        request.scriptPath, request.sceneName, rendered.outputVideoPath, fingerprint.fingerprint);
    if (!created.success) {
        result.error = created.error;
        return result;
    }

    project::Project candidate = project;
    const auto existing = std::find_if(candidate.manimAssets.begin(), candidate.manimAssets.end(),
                                       [&](const auto& asset) {
                                           return asset.sceneName == request.sceneName &&
                                                  sameScript(asset.scriptPath, request.scriptPath);
                                       });
    if (existing == candidate.manimAssets.end())
        candidate.manimAssets.push_back(std::move(created.asset));
    else
        *existing = std::move(created.asset);

    const project::ProjectIoResult saved =
        project::saveProjectJsonTransaction(project, std::move(candidate), absoluteProjectPath);
    if (!saved.success) {
        result.error = saved.error;
        return result;
    }
    result.success = true;
    return result;
}

ManimClipRestoreResult restoreFirstManimClip(project::Project& project,
                                             const std::filesystem::path& projectPath) {
    ManimClipRestoreResult result;
    if (project.manimAssets.empty()) {
        result.success = true;
        return result;
    }

    result.hasAsset = true;
    const project::ManimAsset& current = project.manimAssets.front();
    // 未生成やfile削除はerror_codeを立てるが、ここでは「再生できない」だけの状態として扱う。
    std::error_code videoError;
    result.generatedVideoAvailable =
        std::filesystem::is_regular_file(current.generatedVideoPath, videoError);

    const manim::ManimFingerprintResult fingerprint =
        manim::fingerprintManimSource(current.scriptPath);
    if (!fingerprint.success) {
        result.error = fingerprint.error;
        return result;
    }

    project::Project candidate = project;
    project::ManimAsset& candidateAsset = candidate.manimAssets.front();
    const project::ManimGenerationState previousState = candidateAsset.generationState;
    if (!project::refreshManimGenerationState(candidateAsset, fingerprint.fingerprint,
                                              result.error)) {
        return result;
    }

    if (candidateAsset.generationState != previousState) {
        const project::ProjectIoResult saved =
            project::saveProjectJsonTransaction(project, std::move(candidate), projectPath);
        if (!saved.success) {
            result.error = saved.error;
            return result;
        }
    }

    result.success = true;
    return result;
}

} // namespace mvm::app
