#ifndef MVM_PROJECT_PROJECT_H
#define MVM_PROJECT_PROJECT_H

#include "core/checked_output_timebase.h"
#include "project/clip_effects.h"
#include "project/math_clip.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

enum class TimelineClipKind { Video, Manim, Audio, Text, Image, Math };

// 素材の時間軸を持たない clip (文字・静止画・数式)。尺は timeline 上で自由に伸縮し、
// 素材 frame domain は in = 0・out = 尺 の合成値にする (fps は置いたときの timeline の値)。
// 速度・リンク・スリップ・レート調整を持たない。
bool isStillClipKind(TimelineClipKind kind);

// プロジェクトパネルの素材から作る clip か (動画・音声・画像)。これらは mediaItemId が必須。
bool clipUsesMediaItem(TimelineClipKind kind);

// mediaPath (実ファイル) を持つ clip か。文字と数式は Project のデータだけから描くので持たない。
// 検証・JSON の読み書き・path の解決はすべてこれで判定する。
bool clipKindHasMediaPath(TimelineClipKind kind);

// "#AARRGGBB" (16 進 8 桁) を 0xAARRGGBB にする。文字・数式の色の形式はこれだけで判定する。
bool parseArgbColor(const std::string& text, std::uint32_t& argb);

// 文字・静止画を置いたときの既定の尺 (5 秒、最低 1 frame)。
std::int64_t defaultStillClipFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen);
std::int64_t defaultFrameHoldFrames(std::int64_t timelineFpsNum, std::int64_t timelineFpsDen);

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
    // video では非表示 (layer から外す)、audio では消音。
    bool muted = false;
    // audio だけが持つ。どれか 1 つでも solo なら、solo の track だけが鳴る。
    // video track では常に false (JSON の読み込みと setTrackSolo が拒否する)。
    bool solo = false;
    std::string mixerName{};
    double mixerGainDb = 0.0; // -96 は無音。上限は +15 dB。
    double mixerPan = 0.0;    // -1 (左) 〜 +1 (右)、中央は減衰なし。
    bool operator==(const Track&) const = default;
};

// clip がどの track に載っているか。kind と index を必ず組で扱い、
// 「V1 と A1 が同じ 0」という取り違えを型で防ぐ。
struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0;
    bool operator==(const TrackRef&) const = default;
};

struct FrameHold {
    std::int64_t sourceFrame = 0;
    std::int64_t sourceFpsNum = 0;
    std::int64_t sourceFpsDen = 1;
    std::int64_t sourceFrameCount = 0;
    // 保持元 clip の速度。書き出しは同じ速度の timewarp の位置で保持する。高 fps 素材を
    // slow motion にした clip では、素材 fps のままでは timeline へ出せない frame があるため。
    std::int64_t speedNum = 1;
    std::int64_t speedDen = 1;
    bool operator==(const FrameHold&) const = default;
};

struct TimelineClip {
    TimelineClipKind kind = TimelineClipKind::Video;
    std::filesystem::path mediaPath; // 解決済みの実ファイル
    std::string name;                // UI 表示名
    std::string id;                  // Project 内で一意な永続 ID
    // 素材の出どころ (プロジェクトパネルの MediaItem::id)。動画・音声・画像の clip は必須で、
    // mediaPath はその素材と同じファイルを指す。文字・数式・Manim の clip は空。
    std::string mediaItemId;
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
    bool preservePitch = false;
    std::optional<FrameHold> frameHold{};
    TextClipData text{};
    // kind が Math のときだけ意味を持つ。それ以外の kind では既定値のまま。
    MathClipData math{};
    // kind が Math のときだけ意味を持つ時間の振る舞い (Write など)。それ以外の kind では既定値。
    MathClipAnimation mathAnimation{};
    // 無効にした clip は timeline に残るが、preview・書き出し・音声に出さない (Shift+E)。
    bool enabled = true;
    bool operator==(const TimelineClip&) const = default;
};

// 同じ track で接している 2 clip の編集点に置くトランジション。種類は track の種別で
// 決まる (映像: クロスディゾルブ、音声: 等パワーのクロスフェード)。区間は timeline frame で
// [cut - framesBeforeCut, cut + framesAfterCut)。cut は outgoing の終端 = incoming の先頭。
// cut より前は incoming の先頭より前の素材 (頭の余白)、cut より後は outgoing の終端より後の
// 素材 (尻の余白) を使う。前後を別々に持つので、中央・cut 始まり・cut 終わりを表せる。
struct TimelineTransition {
    std::string id;
    std::string outgoingClipId;
    std::string incomingClipId;
    std::int64_t framesBeforeCut = 0;
    std::int64_t framesAfterCut = 0;
    bool operator==(const TimelineTransition&) const = default;
};

// 速度の範囲 (10%〜1000%)。rbpitch の pitchscale 0.1〜10 に収め、後から音程保持を
// 足しても範囲を変えずに済むようにしている。
inline constexpr std::int64_t kMinClipSpeedPercent = 10;
inline constexpr std::int64_t kMaxClipSpeedPercent = 1000;

// Project JSON の schema。timeline 検証と JSON の読み書きが同じ値を参照する。
// 18: 数式 clip (kind "math" と "math" object)。
// 19: 数式 clip の時間の振る舞い ("math_animation" object、省略は intro 無し)。
// 18・17・16 の file は読み込み時に 19 へ上げる。
inline constexpr int kProjectSchemaVersion = 19;

struct SubtitleCue {
    std::string id;
    std::int64_t startFrame = 0;
    std::int64_t endFrame = 0;
    std::string content;
    // 文字起こし元の timeline clip。空は未リンク。clip を横へ動かすと同じ量だけ追従し、
    // clip を削除・カットすると一緒に消える。字幕だけを動かしてもリンクは保つ。
    std::string linkClipId;
    bool operator==(const SubtitleCue&) const = default;
};

struct SubtitleStyle {
    std::string fontFamily = "Meiryo";
    int fontSize = 64;
    bool bold = false;
    std::string color = "#FFFFFFFF";
    std::string outlineColor = "#FF000000";
    int outlineWidth = 3;
    std::string backgroundColor = "#00000000";
    std::string alignment = "center";
    double sideMargin = 0.05;
    double bottomMargin = 0.15;
    bool operator==(const SubtitleStyle&) const = default;
};

struct SubtitleTrack {
    bool visible = true;
    SubtitleStyle style;
    std::vector<SubtitleCue> cues;
    bool operator==(const SubtitleTrack&) const = default;
};

bool hasSyntheticSourceDomain(const TimelineClip& clip);

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
    std::vector<TimelineTransition> timelineTransitions;
    std::vector<std::int64_t> timelineMarkers;
    std::optional<std::int64_t> inFrame;
    std::optional<std::int64_t> outFrame;
    std::vector<MediaFolder> mediaFolders;
    std::vector<MediaItem> mediaItems;
    std::optional<SubtitleTrack> subtitles;
    // 自動音量調整ダイアログが次に開くときへ出す、最後に適用した設定。
    // clip ごとの記録は適用対象の履歴であり、ダイアログの authority はこちら。
    std::string lastAudioAdjustmentSettings;
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
// track の clip を preview へ出すか。video は mute (非表示) でなければ出す。audio は mute で
// なく、かつ solo の track が 1 つも無いか自分が solo なら鳴らす。描画・音声の経路はすべて
// これで判定する (mute だけを見ると solo が効かない経路ができる)。
bool isTrackOutputEnabled(const Project& project, TrackRef track);
bool isValidAudioMix(double gainDb, double pan);
std::pair<double, double> audioMixGains(double gainDb, double pan);
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

// Project を 1 つ複製したときに確保されるおおよその byte 数 (struct の大きさと、文字列・
// 配列の中身)。Undo 履歴のように Project の複製を多数持つ場所が、件数ではなく memory で
// 上限を決めるために使う。allocator の管理領域や短い文字列の最適化は見ないので概算である。
std::size_t approximateProjectBytes(const Project& project);

// Undo / Redo 履歴から捨てる件数。どちらの配列も「現在の状態から遠い順」に並べた各世代の
// approximateProjectBytes (Undo は古い順、Redo は最後にやり直す編集から順)。
// 両方の合計の件数が maxEntries を超えるか、合計の byte 数が maxBytes を超える間、現在の
// 状態から最も遠い世代から捨てる (距離が同じなら Undo 側)。ただし現在の状態に隣り合う
// Undo と Redo の 1 件ずつは予算を超えても残す (直前の編集を必ず元に戻し、やり直せる)。
struct EditHistoryDrop {
    std::size_t undo = 0;
    std::size_t redo = 0;
};

EditHistoryDrop editHistoryEntriesToDrop(const std::vector<std::size_t>& undoFarthestFirst,
                                         const std::vector<std::size_t>& redoFarthestFirst,
                                         std::size_t maxEntries, std::size_t maxBytes);

} // namespace mvm::project

#endif // MVM_PROJECT_PROJECT_H
