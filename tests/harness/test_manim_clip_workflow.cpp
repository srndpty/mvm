#include "app/manim_clip_workflow.h"
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

bool require(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}

bool writeScript(const std::filesystem::path& path, const std::string& marker) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "from manim import *\n# " << marker << '\n';
    return output.good();
}

bool sameAsset(const mvm::project::ManimAsset& left, const mvm::project::ManimAsset& right) {
    return left.scriptPath == right.scriptPath && left.sceneName == right.sceneName &&
           left.generatedVideoPath == right.generatedVideoPath &&
           left.generationState == right.generationState &&
           left.sourceFingerprint == right.sourceFingerprint;
}

} // namespace

int main(int argc, char** argv) {
    mvm_enable_utf8_console();
    if (argc != 3) {
        std::fprintf(stderr, "使い方: test_manim_clip_workflow <fake-manim> <test-dir>\n");
        return 2;
    }

    const auto fakeManim = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto testDirectory = std::filesystem::absolute(fromUtf8(argv[2]));
    std::error_code error;
    std::filesystem::remove_all(testDirectory, error);
    error.clear();
    std::filesystem::create_directories(testDirectory / L"source", error);
    if (!require(!error, "test directoryを作成できません"))
        return 1;

    const auto projectPath = testDirectory / L"project.mvm.json";
    const auto scriptPath = testDirectory / L"source" / L"scene.py";
    if (!require(writeScript(scriptPath, "first"), "sample scriptを書けません"))
        return 1;

    mvm::project::Project project;
    mvm::app::ManimClipGenerationRequest request{
        .manimExecutablePath = fakeManim,
        .projectPath = projectPath,
        .scriptPath = scriptPath,
        .sceneName = "MvmM0Scene",
    };
    const auto first = mvm::app::generateManimClip(project, request);
    if (!require(first.success, first.error.c_str()) ||
        !require(project.manimAssets.size() == 1, "Ready assetが1件作成されません") ||
        !require(project.manimAssets.front().generationState ==
                     mvm::project::ManimGenerationState::Ready,
                 "生成assetがReadyではありません") ||
        !require(first.outputVideoPath.parent_path().parent_path().parent_path().parent_path() ==
                     testDirectory / L"cache" / L"manim",
                 "generated MP4がproject cache/manim配下ではありません")) {
        return 1;
    }

    const auto loaded = mvm::project::loadProjectJson(projectPath);
    if (!require(loaded.success, loaded.error.c_str()) ||
        !require(loaded.project.manimAssets.size() == 1,
                 "保存・reload後にManim relationが残りません") ||
        !require(sameAsset(project.manimAssets.front(), loaded.project.manimAssets.front()),
                 "保存・reload後のManim relationが一致しません")) {
        return 1;
    }

    const std::string oldFingerprint = project.manimAssets.front().sourceFingerprint;
    const auto oldVideoPath = project.manimAssets.front().generatedVideoPath;
    if (!require(writeScript(scriptPath, "second"), "sample scriptを更新できません"))
        return 1;
    const auto second = mvm::app::generateManimClip(project, request);
    if (!require(second.success, second.error.c_str()) ||
        !require(project.manimAssets.size() == 1, "同じscriptとSceneでassetが増えました") ||
        !require(project.manimAssets.front().sourceFingerprint != oldFingerprint,
                 "再生成でfingerprintが更新されません") ||
        !require(project.manimAssets.front().generatedVideoPath != oldVideoPath,
                 "再生成でgenerated video pathが更新されません")) {
        return 1;
    }

    const mvm::project::Project beforeFailure = project;
    request.sceneName = "NonZeroScene";
    const auto failed = mvm::app::generateManimClip(project, request);
    if (!require(!failed.success && failed.exitCode == 17,
                 "fake CLIの非ゼロ終了を失敗として返しません") ||
        !require(project.manimAssets.size() == beforeFailure.manimAssets.size() &&
                     sameAsset(project.manimAssets.front(), beforeFailure.manimAssets.front()),
                 "render失敗時にProjectが変更されました")) {
        return 1;
    }

    std::puts("M2 Manim clip workflow focused test: PASS");
    return 0;
}
