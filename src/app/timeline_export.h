#ifndef MVM_APP_TIMELINE_EXPORT_H
#define MVM_APP_TIMELINE_EXPORT_H

#include "project/project.h"

#include <filesystem>
#include <string>

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
};

// Project の timelineClips を並び順のまま 1 本の MP4 へ書き出す。
//
// 出力は一時ファイルへ書き、probe 検証を通ってから正規名へ rename する。
// 失敗時は一時ファイルを残さない。Qt / GUI には依存しない。
TimelineExportResult exportTimeline(const project::Project& project,
                                    const TimelineExportRequest& request);

} // namespace mvm::app

#endif // MVM_APP_TIMELINE_EXPORT_H
