#ifndef MVM_MEDIA_STILL_IMAGE_STATIC_IMAGE_H
#define MVM_MEDIA_STILL_IMAGE_STATIC_IMAGE_H

#include "media/still_image/media_stream_facts.h"
#include "media/still_image/still_image_decoder.h"
#include "media/still_image/still_image_limits.h"

#include <filesystem>
#include <string>

namespace mvm::media {

// 素材を「静止画」「時間を持つ素材」「拒否」のどれとして扱うかを、FFmpeg で調べた
// stream の事実だけで決める。拡張子は見ない。
//   アニメーション画像 (GIF / WebP / APNG) と HDR 画像 (EXR / HDR) は理由を添えて拒否する。
//   それ以外の静止画 codec -> StillImage
//   映像か音声の stream がある -> TimeBased (種別は呼び出し側が MLT の probe で決める)
enum class MediaRoute { StillImage, TimeBased, Rejected };

struct MediaRouteDecision {
    MediaRoute route = MediaRoute::Rejected;
    std::string error;
};

MediaRouteDecision routeMedia(const MediaStreamFacts& facts);

// 静止画 1 枚の素材だけを decode する。画像素材の画素が要る経路 (取り込み・preview の
// raster cache・書き出し) は、decodeStillImage を直接呼ばずに必ずここを通す。
// 取り込んだ後に同じ path が動画・アニメーション画像・HDR 画像へ差し替えられても、
// 取り込み時と同じ routeMedia の判定で拒否する (経路によって受理する素材が変わらない)。
StillImageDecodeResult loadStaticImage(const std::filesystem::path& path,
                                       const StillImageLimits& limits = {});

} // namespace mvm::media

#endif
