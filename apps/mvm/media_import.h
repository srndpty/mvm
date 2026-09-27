#ifndef MVM_APPS_MVM_MEDIA_IMPORT_H
#define MVM_APPS_MVM_MEDIA_IMPORT_H

#include "media/mlt/mvm_mlt_probe.h"
#include "project/project.h"

#include <filesystem>
#include <string>

namespace mvm::app {

struct MediaImportResult {
    bool success = false;
    project::MediaItem item; // id / name / folderId は呼び出し側が埋める
    std::string error;
};

// probe 結果から素材種別と表示用の値を決める。拡張子は見ない。
//   映像あり・有限尺  -> Video
//   映像あり・無限尺、または音声なしの 1 frame -> Image
//     (MLT は PNG の length を INT_MAX、JPEG を 1 frame で返す)
//   映像なし・音声あり -> Audio
// 値が欠けている素材は、推測で埋めずに失敗させる。
MediaImportResult classifyMediaProbe(const MvmMltProbeResult& probe,
                                     const std::filesystem::path& mediaPath);

// ファイルを MLT で probe して classifyMediaProbe へ渡す。
MediaImportResult probeMediaForBin(const std::filesystem::path& mediaPath);

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_IMPORT_H
