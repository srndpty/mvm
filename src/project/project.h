#ifndef MVM_PROJECT_PROJECT_H
#define MVM_PROJECT_PROJECT_H

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

// M4: timeline に並ぶ 1 clip。順序だけを持ち、開始時刻・トラック・トリムは持たない。
enum class TimelineClipKind { Video, Manim };

struct TimelineClip {
    TimelineClipKind kind = TimelineClipKind::Video;
    std::filesystem::path mediaPath; // 解決済みの実ファイル
    std::string name;                // UI 表示名
};

struct Project {
    int schemaVersion = 1;
    std::vector<ManimAsset> manimAssets;
    // 並び順がそのまま再生順になる。M4 では [通常 video][Manim] の 2 clip を想定する。
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
