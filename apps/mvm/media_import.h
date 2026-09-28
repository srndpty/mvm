#ifndef MVM_APPS_MVM_MEDIA_IMPORT_H
#define MVM_APPS_MVM_MEDIA_IMPORT_H

#include "media/mlt/mvm_mlt_probe.h"
#include "media/still_image/media_stream_facts.h"
#include "media/still_image/still_image_decoder.h"
#include "project/project.h"

#include <filesystem>
#include <string>

namespace mvm::app {

struct MediaImportResult {
    bool success = false;
    project::MediaItem item; // id / name / folderId は呼び出し側が埋める
    // Video のときだけ意味を持つ。MediaItem には無いが clip の配置に要る値。
    bool hasAudio = false;
    int sarNum = 1;
    int sarDen = 1;
    // Video / Audio の尺 (秒)。MLT の probe の値。
    double durationSec = 0.0;
    std::string error;
};

// 素材を「静止画」「時間を持つ素材」「拒否」のどれとして扱うかを、FFmpeg で調べた
// stream の事実だけで決める。拡張子は見ない。
//   アニメーション画像 (GIF / WebP / APNG) と HDR 画像 (EXR / HDR) は理由を添えて拒否する。
//   それ以外の静止画 codec -> StillImage
//   映像か音声の stream がある -> TimeBased (種別は classifyMediaProbe が決める)
enum class MediaRoute { StillImage, TimeBased, Rejected };
struct MediaRouteDecision {
    MediaRoute route = MediaRoute::Rejected;
    std::string error;
};
MediaRouteDecision routeMedia(const media::MediaStreamFacts& facts);

// 時間を持つ素材の種別と値を決める。値が欠けている素材は、推測で埋めずに失敗させる。
//   本物の映像 stream がある (facts)  -> Video (MLT の尺と fps が必要)
//   カバーアートか音声だけ (facts)    -> Audio (MLT が映像ありと返しても音声として扱う)
//   1 frame だけの映像は動画として扱えないので拒否する。
MediaImportResult classifyMediaProbe(const MvmMltProbeResult& probe,
                                     const media::MediaStreamFacts& facts,
                                     const std::filesystem::path& mediaPath);

// 静止画の decode 結果から Image の MediaItem を作る。寸法は向きを反映した後の値。
MediaImportResult classifyStillImage(const media::StillImageDecodeResult& decoded,
                                     const std::filesystem::path& mediaPath);

// ファイルを調べて素材の種別と値を決める。判定はここだけで行う。
// 静止画は実際に decode し、読めることと向きを反映した寸法を確かめる。
MediaImportResult probeMediaFile(const std::filesystem::path& mediaPath);

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_IMPORT_H
