#include "project/clip_effects.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    project.timelineClips.front().effects = effects;
    const auto path = directory / "effects.mvm";
    check(saveProjectJson(project, path).success, "effects付きProjectを保存する");
    const auto loaded = loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips.front().effects == effects,
          "effectsをJSON round-tripする");

    const auto legacy = directory / "legacy.mvm";
    std::ofstream legacyFile(legacy);
    legacyFile
        << R"({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"track_kind":"video","track_index":0}]})";
    legacyFile.close();
    const auto legacyLoaded = loadProjectJson(legacy);
    check(legacyLoaded.success &&
              clipEffectsAreDefault(legacyLoaded.project.timelineClips[0].effects),
          "effects欠損clipへ明示defaultを適用する");

    const auto partial = directory / "partial.mvm";
    std::ofstream partialFile(partial);
    partialFile
        << R"({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,"video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"a","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":10,"source_in_frame":0,"source_out_frame":10,"timeline_start_frame":0,"track_kind":"video","track_index":0,"effects":{"scale_percent":60}}]})";
    partialFile.close();
    check(!loadProjectJson(partial).success, "部分effects objectをfail-closedで拒否する");

    return failures == 0 ? 0 : 1;
}
