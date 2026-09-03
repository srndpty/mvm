#include "project/project.h"
#include "project/project_json.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const std::string& description) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "FAIL %s\n", description.c_str());
    }
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void testRoundTrip(const std::filesystem::path& root) {
    const std::string fingerprint(64, 'a');
    const auto script = root / L"source" / L"scene.py";
    const auto video = root / L"cache" / L"manim" / L"generated.mp4";
    std::filesystem::create_directories(script.parent_path());
    std::filesystem::create_directories(video.parent_path());
    writeText(script, "from manim import Scene\n");
    writeText(video, "video");

    auto created = mvm::project::createReadyManimAsset(script, "ExampleScene", video, fingerprint);
    check(created.success, "Ready Manim asset を作成できる");

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.manimAssets.push_back(created.asset);
    const auto projectPath = root / L"project.mvm.json";
    const auto saved = mvm::project::saveProjectJson(project, projectPath);
    check(saved.success, "Project JSON を保存できる");

    const auto loaded = mvm::project::loadProjectJson(projectPath);
    check(loaded.success, "Project JSON を reload できる");
    check(loaded.project.manimAssets.size() == 1, "Manim asset が1件 reload される");
    if (!loaded.success || loaded.project.manimAssets.size() != 1)
        return;

    auto asset = loaded.project.manimAssets.front();
    check(asset.scriptPath == script.lexically_normal(), "script path が round-trip する");
    check(asset.sceneName == "ExampleScene", "Scene 名が round-trip する");
    check(asset.generatedVideoPath == video.lexically_normal(),
          "generated video path が round-trip する");
    check(asset.generationState == mvm::project::ManimGenerationState::Ready,
          "Ready state が round-trip する");
    check(asset.sourceFingerprint == fingerprint, "fingerprint が round-trip する");

    std::string error;
    check(mvm::project::refreshManimGenerationState(asset, fingerprint, error) &&
              asset.generationState == mvm::project::ManimGenerationState::Ready,
          "同じ fingerprint は Ready のまま");
    check(mvm::project::refreshManimGenerationState(asset, std::string(64, 'b'), error) &&
              asset.generationState == mvm::project::ManimGenerationState::SourceChanged,
          "異なる fingerprint は SourceChanged になる");
}

void testAbsoluteScriptFallback(const std::filesystem::path& root) {
    const auto generated = root / L"cache" / L"absolute-fallback.mp4";
    const std::filesystem::path otherDriveScript = L"Z:\\manim source\\scene.py";
    auto created = mvm::project::createReadyManimAsset(otherDriveScript, "OtherDriveScene",
                                                       generated, std::string(64, 'c'));
    check(created.success, "別 drive の script asset を作成できる");

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.manimAssets.push_back(created.asset);
    const auto projectPath = root / L"absolute-script.mvm.json";
    const auto saved = mvm::project::saveProjectJson(project, projectPath);
    check(saved.success, "別 drive の script path は絶対 path fallback で保存できる");
    const auto loaded = mvm::project::loadProjectJson(projectPath);
    check(loaded.success && loaded.project.manimAssets.size() == 1,
          "絶対 script path の Project を reload できる");
    if (loaded.success && loaded.project.manimAssets.size() == 1)
        check(loaded.project.manimAssets.front().scriptPath == otherDriveScript.lexically_normal(),
              "別 drive の絶対 script path が round-trip する");
}

void testInvalidJson(const std::filesystem::path& root) {
    const auto badSchema = root / L"bad-schema.json";
    writeText(badSchema, R"({"schema_version":1,"manim_assets":[]})");
    check(!mvm::project::loadProjectJson(badSchema).success, "未知の schema version を拒否する");

    const auto missingField = root / L"missing-field.json";
    writeText(
        missingField,
        R"({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[{"script_path":"scene.py"}],"timeline_clips":[]})");
    check(!mvm::project::loadProjectJson(missingField).success, "必須 field 欠損を拒否する");

    const auto unknownState = root / L"unknown-state.json";
    writeText(
        unknownState,
        R"({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[{"script_path":"scene.py","scene_name":"Scene","generated_video_path":"cache/out.mp4","generation_state":"Unknown","source_fingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}],"timeline_clips":[]})");
    check(!mvm::project::loadProjectJson(unknownState).success,
          "未知の generation state を拒否する");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_project_manim <test-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    const auto root = std::filesystem::absolute(fromUtf8(argv[1]));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testRoundTrip(root);
    testAbsoluteScriptFallback(root);
    testInvalidJson(root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", gChecks, gFailures);
    mvm_win_free_utf8_args(argv, argc);
    return gFailures == 0 ? 0 : 1;
}
