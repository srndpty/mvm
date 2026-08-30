#ifndef MVM_APP_MANIM_CLIP_WORKFLOW_H
#define MVM_APP_MANIM_CLIP_WORKFLOW_H

#include "project/project.h"

#include <filesystem>
#include <string>

namespace mvm::app {

struct ManimClipGenerationRequest {
    std::filesystem::path manimExecutablePath;
    std::filesystem::path projectPath;
    std::filesystem::path scriptPath;
    std::string sceneName;
    int width = 640;
    int height = 360;
    int fps = 60;
};

struct ManimClipGenerationResult {
    bool success = false;
    std::filesystem::path outputVideoPath;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    std::string error;
};

// Manim renderからProject保存までを同期実行する。
// 途中で失敗した場合、projectは呼び出し前のまま維持する。
ManimClipGenerationResult generateManimClip(project::Project& project,
                                            const ManimClipGenerationRequest& request);

} // namespace mvm::app

#endif // MVM_APP_MANIM_CLIP_WORKFLOW_H
