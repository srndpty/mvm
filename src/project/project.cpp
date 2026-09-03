#include "project/project.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace mvm::project {
namespace {

bool isSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) || (character >= 'a' && character <= 'f');
           });
}

} // namespace

ManimAssetResult createReadyManimAsset(std::filesystem::path scriptPath, std::string sceneName,
                                       std::filesystem::path generatedVideoPath,
                                       std::string sourceFingerprint) {
    ManimAssetResult result;
    if (scriptPath.empty()) {
        result.error = "Manim script path が空です";
        return result;
    }
    if (sceneName.empty()) {
        result.error = "Manim Scene 名が空です";
        return result;
    }
    if (generatedVideoPath.empty()) {
        result.error = "生成済み Manim video path が空です";
        return result;
    }
    if (!isSha256(sourceFingerprint)) {
        result.error = "Manim source fingerprint が小文字64桁の SHA-256 ではありません";
        return result;
    }

    result.asset = {
        .scriptPath = std::move(scriptPath),
        .sceneName = std::move(sceneName),
        .generatedVideoPath = std::move(generatedVideoPath),
        .generationState = ManimGenerationState::Ready,
        .sourceFingerprint = std::move(sourceFingerprint),
    };
    result.success = true;
    return result;
}

bool refreshManimGenerationState(ManimAsset& asset, const std::string& currentFingerprint,
                                 std::string& error) {
    error.clear();
    if (asset.generationState == ManimGenerationState::NotGenerated ||
        asset.generationState == ManimGenerationState::GenerationFailed) {
        return true;
    }
    if (!isSha256(asset.sourceFingerprint) || !isSha256(currentFingerprint)) {
        error = "比較する Manim source fingerprint が正しい SHA-256 ではありません";
        return false;
    }
    asset.generationState = asset.sourceFingerprint == currentFingerprint
                                ? ManimGenerationState::Ready
                                : ManimGenerationState::SourceChanged;
    return true;
}

const char* timelineClipKindName(TimelineClipKind kind) {
    switch (kind) {
    case TimelineClipKind::Video:
        return "video";
    case TimelineClipKind::Manim:
        return "manim";
    case TimelineClipKind::Audio:
        return "audio";
    }
    return "";
}

const char* trackKindName(TrackKind kind) {
    switch (kind) {
    case TrackKind::Video:
        return "video";
    case TrackKind::Audio:
        return "audio";
    }
    return "";
}

const std::vector<core::SupportedFrameRate>& supportedTimelineFrameRates() {
    return core::supportedOutputFrameRates();
}

bool isSupportedTimelineFrameRate(std::int64_t fpsNum, std::int64_t fpsDen) {
    return core::isSupportedOutputFrameRate(fpsNum, fpsDen);
}

std::string defaultTrackName(TrackKind kind, int index) {
    return (kind == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
}

const std::vector<Track>& tracksOfKind(const Project& project, TrackKind kind) {
    return kind == TrackKind::Video ? project.videoTracks : project.audioTracks;
}

std::vector<Track>& tracksOfKind(Project& project, TrackKind kind) {
    return kind == TrackKind::Video ? project.videoTracks : project.audioTracks;
}

bool isValidTrackRef(const Project& project, TrackRef track) {
    const auto& tracks = tracksOfKind(project, track.kind);
    return track.index >= 0 && track.index < static_cast<int>(tracks.size());
}

bool clipKindFitsTrackKind(TimelineClipKind clipKind, TrackKind trackKind) {
    return (clipKind == TimelineClipKind::Audio) == (trackKind == TrackKind::Audio);
}

Project createDefaultProject() {
    Project project;
    project.videoTracks = {Track{defaultTrackName(TrackKind::Video, 0), false},
                           Track{defaultTrackName(TrackKind::Video, 1), false}};
    project.audioTracks = {Track{defaultTrackName(TrackKind::Audio, 0), false}};
    return project;
}

const char* manimGenerationStateName(ManimGenerationState state) {
    switch (state) {
    case ManimGenerationState::NotGenerated:
        return "NotGenerated";
    case ManimGenerationState::Ready:
        return "Ready";
    case ManimGenerationState::SourceChanged:
        return "SourceChanged";
    case ManimGenerationState::GenerationFailed:
        return "GenerationFailed";
    }
    return "";
}

} // namespace mvm::project
