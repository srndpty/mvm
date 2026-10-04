#include "project/project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <utility>

namespace mvm::project {

bool isValidAudioMix(double gainDb, double pan) {
    return std::isfinite(gainDb) && gainDb >= -96.0 && gainDb <= 15.0 && std::isfinite(pan) &&
           pan >= -1.0 && pan <= 1.0;
}

std::pair<double, double> audioMixGains(double gainDb, double pan) {
    const double gain = gainDb <= -96.0 ? 0.0 : std::pow(10.0, gainDb / 20.0);
    return {gain * (pan > 0.0 ? 1.0 - pan : 1.0), gain * (pan < 0.0 ? 1.0 + pan : 1.0)};
}

namespace {

bool isSha256(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isdigit(character) || (character >= 'a' && character <= 'f');
           });
}

} // namespace

bool isValidProjectOutputSize(int width, int height) {
    return width >= 2 && height >= 2 && width <= kMaximumProjectOutputDimension &&
           height <= kMaximumProjectOutputDimension && width % 2 == 0 && height % 2 == 0;
}

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
    case TimelineClipKind::Text:
        return "text";
    case TimelineClipKind::Image:
        return "image";
    }
    return "";
}

bool clipUsesMediaItem(TimelineClipKind kind) {
    return kind == TimelineClipKind::Video || kind == TimelineClipKind::Audio ||
           kind == TimelineClipKind::Image;
}

bool isStillClipKind(TimelineClipKind kind) {
    return kind == TimelineClipKind::Text || kind == TimelineClipKind::Image;
}

namespace {
std::int64_t roundedClipFrames(std::int64_t seconds, std::int64_t timelineFpsNum,
                               std::int64_t timelineFpsDen) {
    if (timelineFpsNum <= 0 || timelineFpsDen <= 0)
        return 1;
    return std::max<std::int64_t>(1,
                                  (seconds * timelineFpsNum + timelineFpsDen / 2) / timelineFpsDen);
}
} // namespace

std::int64_t defaultStillClipFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen) {
    return roundedClipFrames(5, timelineFpsNum, timelineFpsDen);
}

std::int64_t defaultFrameHoldFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen) {
    return roundedClipFrames(2, timelineFpsNum, timelineFpsDen);
}

bool hasSyntheticSourceDomain(const TimelineClip& clip) {
    return isStillClipKind(clip.kind) || clip.frameHold.has_value();
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

const std::vector<core::SupportedFrameRate>& configurableTimelineFrameRates() {
    return core::configurableOutputFrameRates();
}

bool isConfigurableTimelineFrameRate(std::int64_t fpsNum, std::int64_t fpsDen) {
    return core::isConfigurableOutputFrameRate(fpsNum, fpsDen);
}

bool isMeasuredTimelineFrameRate(std::int64_t fpsNum, std::int64_t fpsDen) {
    return core::isMeasuredOutputFrameRate(fpsNum, fpsDen);
}

bool isCanonicalFrameRate(std::int64_t fpsNum, std::int64_t fpsDen) {
    return fpsNum > 0 && fpsDen > 0 && std::gcd(fpsNum, fpsDen) == 1;
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

bool isTrackOutputEnabled(const Project& project, TrackRef track) {
    if (!isValidTrackRef(project, track))
        return false;
    const auto& tracks = tracksOfKind(project, track.kind);
    const auto& target = tracks[static_cast<std::size_t>(track.index)];
    if (target.muted)
        return false;
    if (track.kind == TrackKind::Video || target.solo)
        return true;
    return std::none_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.solo; });
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

namespace {

std::size_t heapBytes(const std::string& text) {
    return text.size();
}

std::size_t heapBytes(const std::filesystem::path& path) {
    return path.native().size() * sizeof(std::filesystem::path::value_type);
}

} // namespace

std::size_t approximateProjectBytes(const Project& project) {
    std::size_t bytes = sizeof(Project);
    for (const auto* tracks : {&project.videoTracks, &project.audioTracks}) {
        bytes += tracks->size() * sizeof(Track);
        for (const auto& track : *tracks)
            bytes += heapBytes(track.name) + heapBytes(track.mixerName);
    }
    bytes += project.manimAssets.size() * sizeof(ManimAsset);
    for (const auto& asset : project.manimAssets)
        bytes += heapBytes(asset.scriptPath) + heapBytes(asset.sceneName) +
                 heapBytes(asset.generatedVideoPath) + heapBytes(asset.sourceFingerprint);
    bytes += project.timelineClips.size() * sizeof(TimelineClip);
    for (const auto& clip : project.timelineClips) {
        bytes += heapBytes(clip.mediaPath) + heapBytes(clip.name) + heapBytes(clip.id) +
                 heapBytes(clip.mediaItemId) + heapBytes(clip.linkGroupId);
        bytes += heapBytes(clip.text.content) + heapBytes(clip.text.fontFamily) +
                 heapBytes(clip.text.color) + heapBytes(clip.text.alignment) +
                 heapBytes(clip.text.outlineColor) + heapBytes(clip.text.backgroundColor);
        bytes += heapBytes(clip.effects.audioAdjustmentSettings) +
                 heapBytes(clip.effects.audioAdjustmentFingerprint);
        for (const auto& channel : effectChannels())
            bytes += (clip.effects.*channel.keys).size() * sizeof(ClipKeyframe);
    }
    bytes += heapBytes(project.lastAudioAdjustmentSettings);
    bytes += project.timelineTransitions.size() * sizeof(TimelineTransition);
    for (const auto& transition : project.timelineTransitions)
        bytes += heapBytes(transition.id) + heapBytes(transition.outgoingClipId) +
                 heapBytes(transition.incomingClipId);
    bytes += project.timelineMarkers.size() * sizeof(std::int64_t);
    bytes += project.mediaFolders.size() * sizeof(MediaFolder);
    for (const auto& folder : project.mediaFolders)
        bytes += heapBytes(folder.id) + heapBytes(folder.name) + heapBytes(folder.parentId);
    bytes += project.mediaItems.size() * sizeof(MediaItem);
    for (const auto& item : project.mediaItems)
        bytes += heapBytes(item.id) + heapBytes(item.mediaPath) + heapBytes(item.name) +
                 heapBytes(item.folderId);
    if (project.subtitles) {
        bytes += sizeof(SubtitleTrack) + project.subtitles->cues.size() * sizeof(SubtitleCue);
        const auto& style = project.subtitles->style;
        bytes += heapBytes(style.fontFamily) + heapBytes(style.color) +
                 heapBytes(style.outlineColor) + heapBytes(style.backgroundColor) +
                 heapBytes(style.alignment);
        for (const auto& cue : project.subtitles->cues)
            bytes += heapBytes(cue.id) + heapBytes(cue.content);
    }
    return bytes;
}

EditHistoryDrop editHistoryEntriesToDrop(const std::vector<std::size_t>& undoFarthestFirst,
                                         const std::vector<std::size_t>& redoFarthestFirst,
                                         std::size_t maxEntries, std::size_t maxBytes) {
    std::size_t total = 0;
    for (const auto* history : {&undoFarthestFirst, &redoFarthestFirst})
        for (const auto bytes : *history)
            total += bytes;
    EditHistoryDrop drop;
    std::size_t undoRemaining = undoFarthestFirst.size();
    std::size_t redoRemaining = redoFarthestFirst.size();
    while (undoRemaining + redoRemaining > maxEntries || total > maxBytes) {
        // 残っている中で最も遠い世代は、Undo 側なら undoRemaining 番目、Redo 側なら
        // redoRemaining 番目の距離にある。隣り合う 1 件 (残り 1 件) は捨てない。
        const bool undoDroppable = undoRemaining > 1;
        const bool redoDroppable = redoRemaining > 1;
        if (!undoDroppable && !redoDroppable)
            break;
        if (undoDroppable && (!redoDroppable || undoRemaining >= redoRemaining)) {
            total -= undoFarthestFirst[drop.undo];
            ++drop.undo;
            --undoRemaining;
        } else {
            total -= redoFarthestFirst[drop.redo];
            ++drop.redo;
            --redoRemaining;
        }
    }
    return drop;
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
