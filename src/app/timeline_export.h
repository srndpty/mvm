#ifndef MVM_APP_TIMELINE_EXPORT_H
#define MVM_APP_TIMELINE_EXPORT_H

#include "project/project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace mvm::app {

// M4 の書き出し要求。設定 UI は作らないので固定 profile を既定値として持つ。
struct TimelineExportRequest {
    std::filesystem::path outputPath;
    int width = 640;
    int height = 360;
    int fpsNum = 60;
    int fpsDen = 1;
    int timeoutMs = 600000;
};

struct TimelineExportResult {
    bool success = false;
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

struct TimelineExportClipMapping {
    int projectClipIndex = -1;
    int videoTrackIndex = 0;
    std::int64_t timelineStartFrame = 0;
    std::int64_t timelineDurationFrames = 0;
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
    std::vector<TimelineExportOpacityKey> opacityKeys;
};

// MLT 側の書き出し経路が持つ playlist は V1 / V2 の 2 本だけである。
// track を増やせるのは編集 model の話であり、書き出しはここで fail-closed にする。
inline constexpr int kMaxExportVideoTracks = 2;

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
