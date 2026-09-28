#ifndef MVM_MEDIA_STILL_IMAGE_STILL_IMAGE_DECODER_H
#define MVM_MEDIA_STILL_IMAGE_STILL_IMAGE_DECODER_H

#include "media/still_image/still_image_limits.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::media {

// 画像素材の decode は preview と書き出しの両方がこの module だけを通す。
// 両者で同じ画素を使うことで、decoder の違いによる見た目の食い違いを起こさない。

// straight alpha の RGBA8。行間に余白は無い (rgba.size() == width * height * 4)。
struct StillImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct StillImageDecodeResult {
    bool success = false;
    StillImage image;    // EXIF の向きを反映した後の画素
    int orientation = 1; // 素材が持っていた EXIF orientation (1..8)
    std::string sourcePixelFormat;
    std::string error;
};

// 先頭の 1 frame を decode し、向きを反映した RGBA8 を返す。
// アニメーションかどうかの判定は MediaStreamFacts が担う。ここでは見ない。
StillImageDecodeResult decodeStillImage(const std::filesystem::path& path,
                                        const StillImageLimits& limits = {});

// EXIF orientation (1..8) を画素へ反映する。それ以外の値は拒否する。
bool applyExifOrientation(StillImage& image, int orientation, std::string& error);

struct StillRasterResult {
    bool success = false;
    StillImage raster;
    std::string error;
};

// 出力解像度の raster の中央へ、縦横比を保って収まる最大の大きさで置く。余白は alpha 0。
// 文字 clip の全画面 PNG と同じ形にすることで、crop / 変形の座標系を映像・文字と揃える。
// 縮拡は premultiplied alpha で行い、透明な縁へ色がにじむのを防ぐ。
StillRasterResult fitStillImageToRaster(const StillImage& image, int outputWidth, int outputHeight);

} // namespace mvm::media

#endif
