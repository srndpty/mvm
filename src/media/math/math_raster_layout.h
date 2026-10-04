#ifndef MVM_MEDIA_MATH_MATH_RASTER_LAYOUT_H
#define MVM_MEDIA_MATH_MATH_RASTER_LAYOUT_H

// 数式の mask (renderer の出力) を出力 raster へ置く。preview と書き出しが同じ関数を通し、
// 同じ画素を得る。位置・拡大・回転は ClipEffects が後段で掛けるので、ここでは常に中央へ置く。

#include <cstdint>
#include <string>
#include <vector>

namespace mvm::math {

struct MathComposeStyle {
    std::uint32_t colorArgb = 0xFFFFFFFFu;      // glyph の色 (0xAARRGGBB)
    std::uint32_t backgroundArgb = 0x00000000u; // mask 全体 (glyph の bbox + padding) の背景
};

struct MathComposeResult {
    bool success = false;
    std::string error;
    int width = 0;
    int height = 0;
    // RGBA8 straight alpha、行間の余白なし。
    std::vector<std::uint8_t> rgba;
};

// mask は RGBA8 straight alpha (width * height * 4 byte)。alpha だけを glyph の被覆率として使う。
// mask は出力の中央 (余りは左上寄せ) に置き、mask の外は透明にする。
// mask が出力より大きければ失敗 (はみ出した数式を黙って切らない)。
MathComposeResult composeMathRaster(const std::uint8_t* maskRgba, int maskWidth, int maskHeight,
                                    const MathComposeStyle& style, int outputWidth,
                                    int outputHeight);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_RASTER_LAYOUT_H
