#ifndef MVM_PROJECT_PROJECT_H
#define MVM_PROJECT_PROJECT_H

#include "core/checked_output_timebase.h"
#include "project/clip_effects.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
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

enum class TimelineClipKind { Video, Manim, Audio };

enum class TrackKind { Video, Audio };

// track は video / audio それぞれ独立した vector で持つ。
// 片方へ track を足しても、もう片方の clip の index を振り直さずに済む。
struct Track {
    std::string name; // UI 表示名。"V1" / "A1" など
    bool muted = false;
    bool operator==(const Track&) const = default;
};

// clip がどの track に載っているか。kind と index を必ず組で扱い、
// 「V1 と A1 が同じ 0」という取り違えを型で防ぐ。
struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0;
    bool operator==(const TrackRef&) const = default;
};

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
    TrackRef track;
    // 同じ値を持つ video/audio clip はリンクされている。空文字列は未リンク。
    // リンクは横移動と削除だけを同期し、track と trim は各 clip 固有に保つ。
    std::string linkGroupId;
    bool operator==(const TimelineClip&) const = default;
};

struct Project {
    int schemaVersion = 3;
    std::int64_t timelineFpsNum = 60;
    std::int64_t timelineFpsDen = 1;
    // Project の出力 raster。preview と export が共有する基準寸法。
    int outputWidth = 1920;
    int outputHeight = 1080;
    // index 0 が最下層 (V1)。合成順は index 昇順で bottom -> top。
    std::vector<Track> videoTracks;
    std::vector<Track> audioTracks;
    std::vector<ManimAsset> manimAssets;
    std::vector<TimelineClip> timelineClips;
};

// 新規 Project の初期構成。track が 0 本の Project を作らせない。
Project createDefaultProject();

// timeline fps として設定できる rate かどうか。core の configurable 表を参照する。
// ここに別表を持たない。
bool isConfigurableTimelineFrameRate(std::int64_t fpsNum, std::int64_t fpsDen);
const std::vector<core::SupportedFrameRate>& configurableTimelineFrameRates();
// 実測して qualify した rate かどうか。UI はこれを使って
// 「設定はできるが未計測」を利用者へ出す。受理できること = 計測済み ではない。
bool isMeasuredTimelineFrameRate(std::int64_t fpsNum, std::int64_t fpsDen);
// 約分済みの pair かどうか。Project へ永続化する fps は canonical だけを認める。
bool isCanonicalFrameRate(std::int64_t fpsNum, std::int64_t fpsDen);

const std::vector<Track>& tracksOfKind(const Project& project, TrackKind kind);
std::vector<Track>& tracksOfKind(Project& project, TrackKind kind);
bool isValidTrackRef(const Project& project, TrackRef track);
// clip の kind がその track に載ってよいか。audio clip を video track へ置かせない。
bool clipKindFitsTrackKind(TimelineClipKind clipKind, TrackKind trackKind);
std::string defaultTrackName(TrackKind kind, int index);

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
const char* trackKindName(TrackKind kind);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_H
