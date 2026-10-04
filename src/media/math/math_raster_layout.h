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

// mask を出力の中央へ置いたときの左上 (composeMathRaster と同じ位置)。mask が出力より大きい・
// 大きさが不正なら false。
bool mathRasterPlacement(int maskWidth, int maskHeight, int outputWidth, int outputHeight,
                         int& left, int& top);

// mask の領域だけを合成する (Write の連番の 1 frame を preview の該当矩形へ書く)。
// coverage は 1 画素 1 byte (mask の alpha) で maskWidth * maskHeight byte。
// out は maskWidth * maskHeight * 4 byte の RGBA8 straight alpha。
// 結果は composeMathRaster の出力の mathRasterPlacement の矩形と同じ画素になる。
void composeMathPatch(const std::uint8_t* coverage, int maskWidth, int maskHeight,
                      const MathComposeStyle& style, std::uint8_t* out);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_RASTER_LAYOUT_H
