#include "project/clip_effects.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

mvm::project::Project projectWithClip() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Manim,
                                     "clip.mp4",
                                     "clip",
                                     "clip-1",
                                     60,
                                     1,
                                     120,
                                     10,
                                     110,
                                     0,
                                     {},
                                     {},
                                     {}});
    return project;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    const std::filesystem::path directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);

    using namespace mvm::project;
    ClipEffects effects;
    check(clipEffectsAreDefault(effects), "既定effectをdefaultと判定する");
    {
        // preview / export は default の clip で effect 処理ごと省く。キーだけの clip
        // を落とさない。
        ClipEffects keyed;
        keyed.opacityKeys = {{0, 100.0}, {10, 0.0}};
        check(!clipEffectsAreDefault(keyed), "不透明度キーだけのeffectをdefaultにしない");
        keyed = ClipEffects{};
        keyed.volumeKeys = {{5, 100.0}};
        check(!clipEffectsAreDefault(keyed), "音量キーだけのeffectをdefaultにしない");
        keyed = ClipEffects{};
        keyed.volumePercent = 50.0;
        check(!clipEffectsAreDefault(keyed), "音量だけのeffectをdefaultにしない");
    }
    std::string error;
    check(validateClipEffects(effects, 100, error), "既定effectが有効");
    effects.cropLeftPercent = 10;
    effects.cropTopPercent = 20;
    effects.cropRightPercent = 30;
    effects.cropBottomPercent = 10;
    effects.scalePercent = 60;
    effects.positionXPercent = 15;
    effects.positionYPercent = -5;
    effects.rotationDegrees = 25;
    effects.opacityPercent = 55;
    effects.fadeInFrames = 12;
    effects.fadeOutFrames = 18;
    check(validateClipEffects(effects, 100, error), "組合せeffectが有効");
    const auto mapping = mapClipEffects(effects);
    check(std::abs(mapping.sourceRect.x - 0.1) < 1e-9 &&
              std::abs(mapping.sourceRect.width - 0.6) < 1e-9,
          "asymmetric cropをsource UVへ写す");
    check(std::abs(mapping.destinationRect.width - 0.36) < 1e-9 &&
              std::abs(mapping.destinationRect.x - 0.37) < 1e-9,
          "post-crop中心を維持してscaleしpositionを加える");
    check(std::abs(mapping.baseOpacity - 0.55) < 1e-9 && mapping.fadeInFrames == 12,
          "opacityとsource-native fadeを写す");

    ClipEffects invalid = effects;
    invalid.cropRightPercent = 90;
    check(!validateClipEffects(invalid, 100, error), "左右crop合計100%以上を拒否する");
    invalid = effects;
    invalid.fadeInFrames = 90;
    invalid.fadeOutFrames = 20;
    check(!validateClipEffects(invalid, 100, error), "source-native fade重複を拒否する");
    invalid = effects;
    invalid.scalePercent = std::numeric_limits<double>::quiet_NaN();
    check(!validateClipEffects(invalid, 100, error), "非有限値を拒否する");

    Project project = projectWithClip();
    effects.opacityKeys = {{20, 80.0}, {80, 20.123456789123}};
    project.timelineClips.front().effects = effects;
    const auto path = directory / "effects.mvm";
    check(saveProjectJson(project, path).success, "effects付きProjectを保存する");
    const auto loaded = loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips.front().effects == effects,
          "effectsをJSON round-tripする");
    {
        std::ifstream saved(path, std::ios::binary);
        const std::string originalJson((std::istreambuf_iterator<char>(saved)),
                                       std::istreambuf_iterator<char>());
        auto malformedJson = originalJson;
        const auto secondKey = malformedJson.find("\"frame\":80");
        check(secondKey != std::string::npos, "負例のキー位置が保存されていません");
        if (secondKey != std::string::npos) {
            malformedJson.replace(secondKey, std::string("\"frame\":80").size(), "\"frame\":20");
            const auto duplicatePath = directory / "duplicate-key.mvm";
            std::ofstream output(duplicatePath, std::ios::binary);
            output << malformedJson;
            output.close();
            check(!loadProjectJson(duplicatePath).success, "JSONの同一frameキーを拒否する");
        }
    }

    const auto legacy = directory / "legacy.mvm";
    std::ofstream legacyFile(legacy);
    legacyFile
        << R"({"schema_version":4,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"track_kind":"video","track_index":0}]})";
    legacyFile.close();
    const auto legacyLoaded = loadProjectJson(legacy);
    check(!legacyLoaded.success, "schema 4を拒否する");

    const auto partial = directory / "partial.mvm";
    std::ofstream partialFile(partial);
    partialFile
        << R"({"schema_version":9,"timeline_markers":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"speed_num":1,"speed_den":1,"track_kind":"video","track_index":0,"effects":{"scale_percent":60}}]})";
    partialFile.close();
    check(!loadProjectJson(partial).success, "部分effects objectをfail-closedで拒否する");

    // schema 7 以降の clip は速度 (speed_num / speed_den) を必須にする。どの負例も対照群から
    // 1 か所だけ変えて作るので、壊した箇所で落ちていることが分かる。
    const auto speedProject = [&](const std::string& version, const std::string& speed) {
        return R"({"schema_version":)" + version +
               R"(,"timeline_markers":[],"in_frame":null,"out_frame":null,"format":"mvm-project","media_folders":[],"media_items":[],"timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,)" +
               speed + R"("track_kind":"video","track_index":0}]})";
    };
    const auto loadText = [&](const char* name, const std::string& text) {
        const auto projectPath = directory / name;
        std::ofstream file(projectPath);
        file << text;
        file.close();
        return loadProjectJson(projectPath);
    };
    const auto halfSpeed =
        loadText("speed-half.mvm", speedProject("9", R"("speed_num":1,"speed_den":2,)"));
    check(halfSpeed.success && halfSpeed.project.timelineClips.size() == 1 &&
              halfSpeed.project.timelineClips[0].speedNum == 1 &&
              halfSpeed.project.timelineClips[0].speedDen == 2,
          "対照群: 50%のclipを読めない");
    check(!loadText("schema5.mvm", speedProject("5", R"("speed_num":1,"speed_den":2,)")).success,
          "schema 5を拒否する");
    // schema 8 で image clip を足した。版を上げたので旧版は読まない (互換分岐を持たない)。
    check(!loadText("schema7.mvm", speedProject("7", R"("speed_num":1,"speed_den":2,)")).success,
          "schema 7を拒否する");
    // 旧ファイルは速度の field を持たない。後続の field で落ちる前に、版の違いとして報告する。
    const auto legacyClip = loadText("schema5-legacy.mvm", speedProject("5", ""));
    check(!legacyClip.success &&
              legacyClip.error.find("対応していない schema_version です: 5") != std::string::npos,
          (std::string("schema 5の旧ファイルを版の違いとして報告しない: ") + legacyClip.error)
              .c_str());
    check(!loadText("speed-missing.mvm", speedProject("9", "")).success,
          "速度の無いclipを既定値で受理しない");
    check(
        !loadText("speed-slow.mvm", speedProject("9", R"("speed_num":1,"speed_den":11,)")).success,
        "10%未満の速度を拒否する");
    check(
        !loadText("speed-fast.mvm", speedProject("9", R"("speed_num":11,"speed_den":1,)")).success,
        "1000%を超える速度を拒否する");
    check(
        loadText("speed-edge.mvm", speedProject("9", R"("speed_num":10,"speed_den":1,)")).success &&
            loadText("speed-edge-slow.mvm", speedProject("9", R"("speed_num":1,"speed_den":10,)"))
                .success,
        "10%と1000%ちょうどを受理する");
    check(!loadText("speed-unreduced.mvm", speedProject("9", R"("speed_num":2,"speed_den":4,)"))
               .success,
          "約分されていない速度を拒否する");

    return failures == 0 ? 0 : 1;
}
