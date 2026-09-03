#include "media/manim/manim_fingerprint.h"
#include "media/manim/manim_renderer.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"
#include "project/project_json.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string toUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
    return result;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 4) {
        std::fprintf(stderr,
                     "使い方: mvm_manim_project_smoke <manim.exe> <sample.py> <project-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    const auto manimExecutable = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto sourceFixture = std::filesystem::absolute(fromUtf8(argv[2]));
    const auto projectDirectory = std::filesystem::absolute(fromUtf8(argv[3]));
    const auto script = projectDirectory / L"source" / L"m0_scene.py";
    const auto projectPath = projectDirectory / L"project.mvm.json";

    std::error_code fileError;
    std::filesystem::create_directories(script.parent_path(), fileError);
    if (fileError) {
        mvm_win_free_utf8_args(argv, argc);
        return fail("project source directory を作成できません: " + fileError.message());
    }
    std::filesystem::copy_file(sourceFixture, script,
                               std::filesystem::copy_options::overwrite_existing, fileError);
    if (fileError) {
        mvm_win_free_utf8_args(argv, argc);
        return fail("sample Manim script を project directory へコピーできません: " +
                    fileError.message());
    }

    const mvm::manim::ManimRenderRequest request{
        .manimExecutablePath = manimExecutable,
        .scriptPath = script,
        .sceneName = "MvmM0Scene",
        .outputDirectory = projectDirectory / L"cache" / L"manim",
        .width = 320,
        .height = 180,
        .fps = 15,
    };
    const auto render = mvm::manim::renderManim(request);
    if (!render.success) {
        mvm_win_free_utf8_args(argv, argc);
        return fail("Manim render に失敗しました (exit=" + std::to_string(render.exitCode) + ")\n" +
                    render.stdoutText + "\n" + render.stderrText);
    }

    const auto fingerprint = mvm::manim::fingerprintManimSource(script);
    if (!fingerprint.success) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(fingerprint.error);
    }
    auto created = mvm::project::createReadyManimAsset(
        script, request.sceneName, render.outputVideoPath, fingerprint.fingerprint);
    if (!created.success) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(created.error);
    }

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.manimAssets.push_back(created.asset);
    const auto saved = mvm::project::saveProjectJson(project, projectPath);
    if (!saved.success) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(saved.error);
    }
    auto loaded = mvm::project::loadProjectJson(projectPath);
    if (!loaded.success || loaded.project.manimAssets.size() != 1) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(loaded.success ? "reload 後の Manim asset 数が1ではありません" : loaded.error);
    }

    auto& asset = loaded.project.manimAssets.front();
    if (asset.scriptPath != script.lexically_normal() || asset.sceneName != request.sceneName ||
        asset.generatedVideoPath != render.outputVideoPath.lexically_normal() ||
        asset.generationState != mvm::project::ManimGenerationState::Ready ||
        asset.sourceFingerprint != fingerprint.fingerprint) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(
            "reload 後に Manim script/Scene/video/state/fingerprint relation が変化しました");
    }
    std::printf("PROJECT_RELATION=ready,reloaded\n");
    std::printf("PROJECT_VIDEO=%s\n", toUtf8(asset.generatedVideoPath).c_str());

    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        mvm_win_free_utf8_args(argv, argc);
        return fail("MLT runtime を初期化できませんでした");
    }
    const std::string videoPath = toUtf8(asset.generatedVideoPath);
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(videoPath.c_str(), &probe) != 0 || !probe.ok || !probe.has_video ||
        probe.width != 320 || probe.height != 180 || probe.fps_num != 15 || probe.fps_den != 1) {
        mvm_mlt_runtime_shutdown();
        mvm_win_free_utf8_args(argv, argc);
        return fail("reload した generated MP4 を MLT probe で認識できません: " +
                    std::string(probe.error));
    }

    MvmMltImage frame{};
    char decodeError[512]{};
    if (mvm_mlt_decode_frame(videoPath.c_str(), 0, &frame, decodeError, sizeof(decodeError)) != 0) {
        mvm_mlt_runtime_shutdown();
        mvm_win_free_utf8_args(argv, argc);
        return fail("reload した generated MP4 の frame 0 を decode できません: " +
                    std::string(decodeError));
    }
    std::printf("MLT_PROJECT_VIDEO=video,320x180,15/1,frame0=%dx%d\n", frame.width, frame.height);
    mvm_mlt_image_free(&frame);
    mvm_mlt_runtime_shutdown();

    {
        std::ofstream edited(script, std::ios::binary | std::ios::app);
        edited << "\n# M1 source change\n";
        if (!edited.good()) {
            mvm_win_free_utf8_args(argv, argc);
            return fail("source change fixture を編集できません");
        }
    }
    const auto changedFingerprint = mvm::manim::fingerprintManimSource(script);
    std::string stateError;
    if (!changedFingerprint.success ||
        !mvm::project::refreshManimGenerationState(asset, changedFingerprint.fingerprint,
                                                   stateError) ||
        asset.generationState != mvm::project::ManimGenerationState::SourceChanged) {
        mvm_win_free_utf8_args(argv, argc);
        return fail(changedFingerprint.success ? stateError : changedFingerprint.error);
    }
    std::printf("SOURCE_STATE=SourceChanged\n");

    mvm_win_free_utf8_args(argv, argc);
    return 0;
}
