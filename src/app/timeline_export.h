#ifndef MVM_APP_TIMELINE_EXPORT_H
#define MVM_APP_TIMELINE_EXPORT_H

#include "app/equation_sequence_export.h"
#include "media/math/math_transform.h"
#include "project/project.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mvm::app {

// 現在の key の disk artifact を検査した呼び出し側が渡す。preview の常駐状態は含めない。
struct TimelineMathTransformArtifact {
    math::MathTransformSpec spec;
    int width = 0, height = 0;
    int sourceX = 0, sourceY = 0, targetX = 0, targetY = 0;
    std::int64_t frames = 0;
    std::function<bool(std::size_t, std::vector<std::uint8_t>&, std::string&)> loadFrame;
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
    // 検証用: PNG へ保存する直前の内部合成の RGBA。外側 effects は既存 MLT 合成で掛ける。
    std::function<void(const std::string&, std::int64_t, const std::vector<std::uint8_t>&)>
        equationFrameObserver;
    int renderThreads = 4;
    int encoderThreads = 0;
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
    enum class Backend { Sequential, Tractor } backend = Backend::Sequential;
    int playlistBlankCount = 0;
    int transitionCount = 0;
    int opaqueBlackAffineFilterCount = 0;
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
