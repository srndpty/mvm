#ifndef MVM_MEDIA_STILL_IMAGE_STILL_IMAGE_LIMITS_H
#define MVM_MEDIA_STILL_IMAGE_STILL_IMAGE_LIMITS_H

#include <cstdint>

namespace mvm::media {

// 画素を確保する前に拒否する寸法の上限。巨大な寸法の画像でメモリを使い果たさないための
// 値であり、実在する素材の上限を測った値ではない。
//
// PNG などは avformat_find_stream_info が寸法を知るために先頭 frame を decode する (実測)。
// そのため stream 情報の取得にもこの上限を渡す。渡さないと、ここで拒否する前に
// 9000x9000 の PNG で 324MB を確保していた。
struct StillImageLimits {
    int maxDimension = 16384;
    std::int64_t maxPixels = std::int64_t{64} * 1024 * 1024;
};

} // namespace mvm::media

#endif
