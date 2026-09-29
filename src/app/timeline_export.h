#ifndef MVM_APP_TIMELINE_EXPORT_H
#define MVM_APP_TIMELINE_EXPORT_H

#include "project/project.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mvm::app {

// M4 の書き出し要求。Project の output size を呼び出し側が渡す。
struct TimelineExportRequest {
    std::filesystem::path outputPath;
    int width = 1920;
    int height = 1080;
    int fpsNum = 60;
    int fpsDen = 1;
    // libx264 の constant rate factor。UI が提示する3段階の実値を明示して渡す。
    int videoCrf = 23;
    int timeoutMs = 600000;
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

struct TimelineExportClipMapping {
    int projectClipIndex = -1;
    bool audio = false;
    bool still = false; // 文字・画像。全画面の透過 PNG を stage して qimage で開く
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
    std::vector<TimelineExportOpacityKey> opacityKeys;
    std::vector<TimelineExportGainKey> gainKeys;
};

struct TimelineExportPlan {
    bool success = false;
    TimelineExportResult::Backend backend = TimelineExportResult::Backend::Sequential;
    std::int64_t totalDurationFrames = 0;
    std::vector<TimelineExportClipMapping> clips;
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
