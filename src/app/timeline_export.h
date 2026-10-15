#ifndef MVM_APP_TIMELINE_EXPORT_H
#define MVM_APP_TIMELINE_EXPORT_H

#include "app/equation_sequence_export.h"
#include "app/graph_export.h"
#include "media/math/math_transform.h"
#include "project/project.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

// MLT 層の書き出しの観測 (media/mlt/mvm_mlt_export.h)。診断用。
struct MvmExportObserver;

namespace mvm::app {

// 現在の key の disk artifact を検査した呼び出し側が渡す。preview の常駐状態は含めない。
struct TimelineMathTransformArtifact {
    math::MathTransformSpec spec;
    int width = 0, height = 0;
    int sourceX = 0, sourceY = 0, targetX = 0, targetY = 0;
    std::int64_t frames = 0;
    std::function<bool(std::size_t, std::vector<std::uint8_t>&, std::string&)> loadFrame;
};

// 数式の静止 layer を MLT へ渡す形 (試作。製品は FullFrame のまま)。
//   FullFrame    出力と同じ大きさの透過 PNG。MLT は layer ごとに全画素を合成する
//   BoundingBox  字形のある範囲 (+ 余白 2 画素) だけを切り出し、配置の矩形をその分ずらす
//   AnchoredBox  左上を原点のまま、字形の右端・下端より先だけを落とす。素材座標が FullFrame と
//                同じになる (MLT は素材座標を binary32 で持つので、原点をずらすと端数が変わる)
// 切り出すのは、等倍・回転なし・crop なしで上位 track にある静止の数式だけ。それ以外の clip は
// どの指定でも FullFrame で渡す。
enum class MathLayerStaging { FullFrame, BoundingBox, AnchoredBox };

// 静止の数式 layer を 1 つの transition でまとめて合成する最適化 (数式 group) を使うか (P3-6A)。
//   Off   使わない (既定)。いつも従来の MLT の graph で書き出す
//   Auto  書き出しの前に判定し、ELIGIBLE のときだけ数式 group の graph を使う。INELIGIBLE・UNKNOWN
//   の
//         ときは、書き出しを始める前に従来の graph を選ぶ。ELIGIBLE で数式 group の合成が実行時に
//         失敗したら、書き出しを失敗にする (従来の graph でやり直さない)
// 判定は Project の中身 (V1 の画素の alpha の証明、clip の配置) だけで行う。Project の名前や clip
// の 数、試験用の fixture かどうかでは決めない。
enum class MathGroupOptimization { Off, Auto };

// 数式 group の選択の記録 (書き出しの結果に付く。成功・失敗・取消のどれでも)。
struct MathGroupExportReport {
    MathGroupOptimization requested = MathGroupOptimization::Off;
    bool evaluated = false;          // 書き出しの前の判定を行った
    std::string decision;            // "ELIGIBLE"・"INELIGIBLE"・"UNKNOWN"。判定していなければ空
    std::string reason;              // 判定の理由 (事前に決めた名前)
    bool mathGroupGraph = false;     // 実際に作った graph (false = 従来の graph)
    long long groups = 0;            // 数式 group の transition の数
    long long groupedClips = 0;      // まとめた clip の数
    long long retainedClips = 0;     // 対象として申告されたが、従来の経路に残した clip の数
    long long affineTransitions = 0; // 残った affine の transition の数
    long long composedFrames = 0;    // 数式 group が実際に合成した回数 (0 なら使われていない)
    long long callbacks = 0;
    long long errors = 0;                       // 数式 group の実行時の失敗の数
    long long rasterCount = 0, rasterBytes = 0; // 準備済みの画像
    int baseAlphaProven = -1;                   // 判定に使った V1 の alpha の最小値 (-1 は該当なし)
    // P3-6A.1: 数式 group 用の画像の準備を取りやめた (Auto のときだけ起きる)。判定は行わず、
    // 従来の graph で書き出す。preparationReason は事前に決めた名前
    // ("prepared-raster-budget-exceeded")。
    bool preparationRejected = false;
    std::string preparationReason;
    long long rasterBudgetBytes = 0; // 準備済みの画像の上限 (0 は上限なし・該当なし)
    // 書き出しの関数の中で、MLT の graph を閉じた後に残っていた数 (0 が正)。
    long long liveTransitionsAfter = 0, activeCallbacksAfter = 0;
};

// M4 の書き出し要求。Project の output size を呼び出し側が渡す。
struct TimelineExportRequest {
    std::filesystem::path outputPath;
    int width = 1920;
    int height = 1080;
    int fpsNum = 60;
    int fpsDen = 1;
    // libx264 の constant rate factor。UI が提示する3段階の実値を明示して渡す。
    int videoCrf = 23;
    bool burnSubtitles = true;
    // 音声の比較用に Matroska / PCM を明示選択できる。通常の製品出力は MP4 / AAC。
    bool losslessAudio = false;
    int timeoutMs = 600000;
    // 数式 clip の ID -> 描画済みの PNG (cache の <key>.png)。書き出しは数式を描かない。
    // 出力する数式 clip がここに無ければ失敗する (古い描画や描画中の式で書き出さない)。
    std::map<std::string, std::filesystem::path> mathArtifacts;
    // 数式 clip の ID -> Write の連番の PNG (cache の write/<key>/、frame 0 から順)。Write のある
    // 出力する数式 clip がここに無い・枚数が足りなければ失敗する。
    std::map<std::string, std::vector<std::filesystem::path>> mathWriteFrames;
    std::map<std::string, TimelineMathTransformArtifact> mathTransforms;
    std::map<std::string, EquationExportSnapshot> equationSequences;
    GraphExportEnvironment graphEnvironment;
    // Graph の層の RGBA。最終 encoder 合成の oracle とは別の検証境界。
    std::function<void(const std::string&, std::int64_t, const graph::Raster&)> graphFrameObserver;
    // 検証用: 実際に保存した Graph staging PNG。callback の間だけ一時パスが有効。
    std::function<void(const std::string&, std::int64_t, const std::filesystem::path&)>
        graphStagingObserver;
    // 最終 MLT 合成後、encoder の YUV 変換前。false は検査失敗として公開を拒否する。
    std::function<bool(std::int64_t, const std::uint8_t*, int, int)> encoderFrameValidator;
    // 検証用: PNG へ保存する直前の内部合成の RGBA。外側 effects は既存 MLT 合成で掛ける。
    std::function<void(const std::string&, std::int64_t, const std::vector<std::uint8_t>&)>
        equationFrameObserver;
    int renderThreads = 4;
    int encoderThreads = 0;
    // 試作: 既定 (FullFrame) 以外は検証 harness だけが指定する。
    MathLayerStaging mathLayerStaging = MathLayerStaging::FullFrame;
    // 試作の検証用: 切り出した数式 clip の ID と範囲 (x, y, 幅, 高さ) を受け取る。
    std::function<void(const std::string&, int, int, int, int)> mathStagingObserver;
    // 数式 group の最適化。既定は使わない。
    MathGroupOptimization mathGroupOptimization = MathGroupOptimization::Off;
    // 診断用: MLT 層の各境界の通過回数を記録する。nullptr なら観測しない (製品は nullptr)。
    // 故障の注入・以前の timeout の数え方などの診断用の入力は、この observer を渡したときだけ使える
    // (製品の要求は observer を渡さないので、使えない)。
    MvmExportObserver* exportObserver = nullptr;
    // 診断用: 段階の境界で呼ぶ ("plan-begin"・"plan-end"・"staging-end"・"encode-end"・
    // "published")。worker thread から呼ばれる。
    std::function<void(const char*)> stageObserver;
    // trueを返すとキャンセルする。worker threadから呼ばれる。
    std::function<bool(long long completedFrames, long long totalFrames)> progress;
};

struct TimelineExportResult {
    bool success = false;
    bool cancelled = false;
    std::filesystem::path outputPath;
    long long frameCount = 0;
    double durationSec = 0.0;
    std::string error;
    EquationExportReadiness equationReadiness;
    GraphExportReadiness graphReadiness;
    enum class Backend { Sequential, Tractor } backend = Backend::Sequential;
    int playlistBlankCount = 0;
    int transitionCount = 0;
    int opaqueBlackAffineFilterCount = 0;
    MathGroupExportReport mathGroup;
};

struct TimelineExportOpacityKey {
    std::int64_t localFrame = 0;
    double opacity = 1.0;
};

struct TimelineExportGainKey {
    std::int64_t localFrame = 0;
    double gain = 1.0;
};

struct TimelineExportMotionFrame {
    std::int64_t localFrame = 0;
    double cropLeft = 0, cropTop = 0, cropRight = 0, cropBottom = 0;
    double rectX = 0, rectY = 0, rectWidth = 0, rectHeight = 0;
    double rotationDegrees = 0, shearDegrees = 0;
};

struct TimelineExportClipMapping {
    int projectClipIndex = -1;
    // 書き出す区間に合わせた clip (project::TimelineRenderSegment::clip)。トランジションで
    // 延ばした分の素材範囲を含む。producer に渡す素材範囲・速度はこちらを使う。
    project::TimelineClip renderClip;
    std::optional<project::SubtitleCue> subtitle;
    double mixerPan = 0.0;
    bool audio = false;
    bool still = false; // 文字・画像。全画面の透過 PNG を stage して qimage で開く
    // 数式 clip の Write の区間。timeline の frame ごとの全画面の透過 PNG を連番で stage し、
    // qimage の連番で開く。Write の後は同じ clip の別の mapping (静止) が続く。
    bool mathWrite = false;
    std::string mathTransformId;
    bool equationSequence = false;
    bool graph = false;
    // MLT の映像 layer (下から 0, 1, ...)。track ごとに lane 0 と、トランジションがあれば
    // lane 1 (incoming を重ねる) を積む。トランジションが無ければ track の index と同じ。
    int videoTrackIndex = 0;
    std::int64_t timelineStartFrame = 0;
    std::int64_t timelineDurationFrames = 0;
    // producer の cut [producerInFrame, producerOutFrame) と、末尾を最終 frame で埋める数。
    std::int64_t producerInFrame = 0;
    std::int64_t producerOutFrame = 0;
    std::int64_t tailPaddingFrames = 0;
    bool effectsEnabled = false;
    int cropLeft = 0;
    int cropTop = 0;
    int cropRight = 0;
    int cropBottom = 0;
    double rectX = 0.0;
    double rectY = 0.0;
    double rectWidth = 0.0;
    double rectHeight = 0.0;
    double rotationDegrees = 0.0;
    double shearDegrees = 0.0;
    // クロスディゾルブの incoming。素材の余白を黒で埋めた全画面として重ねる (preview と同じ)。
    bool opaqueBackdrop = false;
    std::vector<TimelineExportMotionFrame> motionFrames;
    std::vector<TimelineExportOpacityKey> opacityKeys;
    std::vector<TimelineExportGainKey> gainKeys;
};

struct TimelineExportPlan {
    bool success = false;
    bool cancelled = false;
    TimelineExportResult::Backend backend = TimelineExportResult::Backend::Sequential;
    std::int64_t totalDurationFrames = 0;
    std::vector<TimelineExportClipMapping> clips;
    std::map<std::string, math::MathTransformRasterPlacement> mathTransformPlacements;
    std::map<std::string, EquationSequenceExportPlan> equationSequences;
    EquationExportReadiness equationReadiness;
    GraphExportLedger graphs;
    std::string error;
};

TimelineExportPlan mapTimelineExportPlan(const project::Project& project,
                                         const TimelineExportRequest& request);

// Project のtrack/start配置を解決して 1 本の MP4 へ書き出す。vector順は配置authorityにしない。
//
// 出力は一時ファイルへ書き、probe 検証を通ってから正規名へ rename する。
// 失敗時は一時ファイルを残さない。Qt / GUI には依存しない。
TimelineExportResult exportTimeline(const project::Project& project,
                                    const TimelineExportRequest& request);

} // namespace mvm::app

#endif // MVM_APP_TIMELINE_EXPORT_H
