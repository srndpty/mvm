#ifndef MVM_MEDIA_MATH_MATH_TRANSFORM_H
#define MVM_MEDIA_MATH_MATH_TRANSFORM_H

// 式から式への変形 (P2) の backend 中立な契約: 変形の描画を変える入力、cache key、
// 端点の配置、文字色の補間。
//
// Manim の型・記法、Project の型 (clip・トランジションの ID、timeline の位置・fps) は含めない。
// 隣接・Write との重なり・ClipEffects の条件は Project の検証 (validateTimelineTransitions) が
// 決め、ここでは見ない。

#include "media/math/math_render.h"
#include "media/math/math_tex_segments.h"

#include <cstdint>
#include <string>

namespace mvm::math {

// 変形の描画結果を変える入力。frames 枚を描き、frame i は進み具合 i / frames
// (frame 0 は source の静止と同じ、最後の frame は (frames - 1) / frames)。進み具合 1 は
// target の静止と同じなので連番には含めない (Write の連番と同じ数え方)。
// 部分の分け方と照合は source・target から決まる派生の値で、ここには持たない
// (規則の版は key に入る)。色・背景・ClipEffects は合成の側の値なので含めない。
struct MathTransformSpec {
    MathRenderSpec source;
    MathRenderSpec target;
    std::int64_t frames = 0;
    bool operator==(const MathTransformSpec&) const = default;
};

// 分け方と照合の規則の版。既定は現在の版。key の材料で、試験では版を変えた key を作る。
struct MathTransformAlgorithms {
    std::string segmenter = kMathTexSegmenterVersion;
    std::string matching = kMathTexMatchingVersion;
};

// 変形の cache key (小文字 16 進 64 桁)。静止・連番の key と別の名前空間で、次の正準形の SHA-256。
// 失敗 (CNG の失敗) なら空文字列。
//
//   mvm-math-transform/1\n
//   segmenter=<byte 数>:<分け方の版>\n
//   matching=<byte 数>:<照合の版>\n
//   frames=<10 進>\n
//   source_syntax=<byte 数>:<syntax>\n
//   source_source=<byte 数>:<source>\n
//   source_font_size=<10 進>\n
//   target_syntax=<byte 数>:<syntax>\n
//   target_source=<byte 数>:<source>\n
//   target_font_size=<10 進>\n
//   backend=<byte 数>:<backendId>\n
//   toolchain=<byte 数>:<canonical>\n
//   transform_template=<byte 数>:<transformTemplate>\n
//
// transformTemplate は backend が渡す変形の描画 script の識別 (例 "manim-transform/1")。
// 色・背景・ClipEffects・clip / トランジションの ID・timeline の位置と fps は含めない。
std::string mathTransformKey(const MathTransformSpec& spec,
                             const MathToolchainFingerprint& toolchain,
                             const std::string& transformTemplate,
                             const MathTransformAlgorithms& algorithms = {});

// 変形の canvas の中の端点 (source・target の静止の mask) の配置。
//
// backend は式の bbox の中心を canvas の中心 (canvasWidth / 2, canvasHeight / 2) に置き、
// (shiftX, shiftY) px だけ動かして描く (x は右、y は下が正)。すると静止の mask の画素 (x, y) は
// canvas の画素 (left + x, top + y) にちょうど重なる (半画素の位相の補正。P2-0 で、補正なしは
// 2603 画素ずれた)。
//
// - left・top は静止の配置 (mathRasterPlacement) と同じ「中央、余りは左上寄せ」の整数。
// - left + maskWidth / 2 == canvasWidth / 2 + shiftX (top も同様) が実数で厳密に成り立つ。
// - shift は 0 (大きさの偶奇が canvas と同じ) か -0.5 (違う)。
struct MathEndpointPlacement {
    int left = 0;
    int top = 0;
    double shiftX = 0.0;
    double shiftY = 0.0;
    bool operator==(const MathEndpointPlacement&) const = default;
};

// mask が canvas より大きい・大きさが不正なら false (はみ出した端点を黙って切らない)。
// canvas の大きさ自体は決めない (全 frame の alpha の bbox の実測から決めるのは後の段階)。
bool mathEndpointPlacement(int maskWidth, int maskHeight, int canvasWidth, int canvasHeight,
                           MathEndpointPlacement& placement);

struct MathTransformPlacement {
    MathEndpointPlacement source;
    MathEndpointPlacement target;
    bool operator==(const MathTransformPlacement&) const = default;
};

// source・target を同じ canvas に置く。各端点の配置は相手の大きさに依らない。
bool mathTransformPlacement(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight,
                            int canvasWidth, int canvasHeight, MathTransformPlacement& placement);

// 変形の frame (frame / frames) での文字色 (0xAARRGGBB)。A・R・G・B の各成分を
// straight alpha のまま (premultiply せずに) 線形に補間し、0.5 は切り上げて整数に丸める。
// frame 0 は from、frame == frames (連番に含めない終状態) は to にちょうど一致する。
// frames <= 0、frame が [0, frames] の外、または frames が大きすぎる (2^40 超) なら false。
// 背景は補間しない (P2-1 で両端の背景は透明に限っている)。
bool mathTransformColorAt(std::uint32_t fromArgb, std::uint32_t toArgb, std::int64_t frame,
                          std::int64_t frames, std::uint32_t& argb);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_TRANSFORM_H
