#ifndef MVM_PROJECT_PROJECT_H
#define MVM_PROJECT_PROJECT_H

#include "project/clip_effects.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::project {

enum class ManimGenerationState { NotGenerated, Ready, SourceChanged, GenerationFailed };

struct ManimAsset {
    std::filesystem::path scriptPath;
    std::string sceneName;
    std::filesystem::path generatedVideoPath;
    ManimGenerationState generationState = ManimGenerationState::NotGenerated;
    std::string sourceFingerprint;
};

enum class TimelineClipKind { Video, Manim };

struct TimelineClip {
    TimelineClipKind kind = TimelineClipKind::Video;
    std::filesystem::path mediaPath; // 解決済みの実ファイル
    std::string name;                // UI 表示名
    std::string id;                  // Project 内で一意な永続 ID
    std::int64_t sourceFpsNum = 0;
    std::int64_t sourceFpsDen = 1;
    std::int64_t sourceFrameCount = 0;
    std::int64_t sourceInFrame = 0;      // inclusive、素材固有 frame domain
    std::int64_t sourceOutFrame = 0;     // exclusive、素材固有 frame domain
    std::int64_t timelineStartFrame = 0; // Project timebase
    ClipEffects effects;
    bool operator==(const TimelineClip&) const = default;
};

struct Project {
    int schemaVersion = 2;
    std::int64_t timelineFpsNum = 60;
    std::int64_t timelineFpsDen = 1;
    std::vector<ManimAsset> manimAssets;
    std::vector<TimelineClip> timelineClips;
};

struct ManimAssetResult {
    bool success = false;
    ManimAsset asset;
    std::string error;
};

ManimAssetResult createReadyManimAsset(std::filesystem::path scriptPath, std::string sceneName,
                                       std::filesystem::path generatedVideoPath,
                                       std::string sourceFingerprint);

// Ready / SourceChanged の asset を現在の fingerprint で再評価する。
// NotGenerated / GenerationFailed は render 操作まで現在の state を維持する。
bool refreshManimGenerationState(ManimAsset& asset, const std::string& currentFingerprint,
                                 std::string& error);

const char* manimGenerationStateName(ManimGenerationState state);

const char* timelineClipKindName(TimelineClipKind kind);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_H
