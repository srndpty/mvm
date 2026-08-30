#ifndef MVM_MEDIA_MANIM_MANIM_RENDERER_H
#define MVM_MEDIA_MANIM_MANIM_RENDERER_H

#include <filesystem>
#include <string>

namespace mvm::manim {

struct ManimRenderRequest {
    std::filesystem::path manimExecutablePath;
    std::filesystem::path scriptPath;
    std::string sceneName;
    std::filesystem::path outputDirectory;
    int width = 0;
    int height = 0;
    int fps = 0;
};

struct ManimRenderResult {
    bool success = false;
    std::filesystem::path outputVideoPath;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

// 明示された Manim executable を同期実行する。
// PATH や Python module から別の executable を推測することはしない。
ManimRenderResult renderManim(const ManimRenderRequest& request);

} // namespace mvm::manim

#endif // MVM_MEDIA_MANIM_MANIM_RENDERER_H
