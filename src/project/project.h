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
    bool operator==(const ManimAsset&) const = default;
};

enum class TimelineClipKind { Video, Manim, Audio, Text, Image };

// 素材の時間軸を持たない clip (文字と静止画)。尺は timeline 上で自由に伸縮し、
// 素材 frame domain は in = 0・out = 尺 の合成値にする (fps は置いたときの timeline の値)。
// 速度・リンク・スリップ・レート調整を持たない。
bool isStillClipKind(TimelineClipKind kind);

// 文字・静止画を置いたときの既定の尺 (5 秒、最低 1 frame)。
std::int64_t defaultStillClipFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen);

struct TextClipData {
    std::string content;
    std::string fontFamily = "Meiryo";
    int fontSize = 64;
    int x = 0;
    int y = 0;
    std::string color = "#FFFFFFFF";
    bool bold = false;
    std::string alignment = "left";
    std::string outlineColor = "#FF000000";
    int outlineWidth = 0;
    std::string backgroundColor = "#00000000";
    bool operator==(const TextClipData&) const = default;
};

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
    // 再生速度 (約分済みの正の有理数)。1/1 が等速、1/2 が 50%。
    // 素材範囲 (in/out) は変えず、timeline 上の尺が 1/速度 倍になる。
    // timeline との換算は必ず clipTimebase (timeline_edit.h) を通す。
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    TextClipData text{};
    bool operator==(const TimelineClip&) const = default;
};

// 速度の範囲 (10%〜1000%)。rbpitch の pitchscale 0.1〜10 に収め、後から音程保持を
// 足しても範囲を変えずに済むようにしている。
inline constexpr std::int64_t kMinClipSpeedPercent = 10;
inline constexpr std::int64_t kMaxClipSpeedPercent = 1000;

// Project JSON の schema。timeline 検証と JSON の読み書きが同じ値を参照する。
inline constexpr int kProjectSchemaVersion = 8;

// プロジェクトパネルの素材。timeline clip とは独立に存在し、mediaPath で対応づく。
enum class MediaKind { Video, Audio, Image };

// parentId が空なら root 直下。
struct MediaFolder {
    std::string id; // folder と media item で共有する名前空間の中で一意
    std::string name;
    std::string parentId;
    bool operator==(const MediaFolder&) const = default;
};

// 種別ごとに意味を持つ field だけを埋め、それ以外は既定値のままにする。
//   Video: fps / frameCount / width / height
//   Audio: sampleRate / durationSamples
//   Image: width / height
struct MediaItem {
    std::string id;
    MediaKind kind = MediaKind::Video;
    std::filesystem::path mediaPath; // 解決済みの実ファイル。bin 内で一意
    std::string name;
    std::string folderId; // 空なら root 直下
    std::int64_t fpsNum = 0;
    std::int64_t fpsDen = 1;
    std::int64_t frameCount = 0;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    std::int64_t durationSamples = 0;
    bool operator==(const MediaItem&) const = default;
};

struct Project {
    int schemaVersion = kProjectSchemaVersion;
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
    std::vector<MediaFolder> mediaFolders;
    std::vector<MediaItem> mediaItems;
    bool operator==(const Project&) const = default;
};

inline constexpr int kMaximumProjectOutputDimension = 16384;
// MP4のyuv420p出力が扱える正の偶数rasterだけをProjectへ保存する。
bool isValidProjectOutputSize(int width, int height);

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
