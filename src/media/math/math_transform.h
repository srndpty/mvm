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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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
// shiftX・shiftY は raster の座標の px (+X は右、+Y は下)。backend は式の bbox の中心を
// canvas の中心 (canvasWidth / 2, canvasHeight / 2) に置き、(shiftX, shiftY) だけ動かして描く。
// すると静止の mask の画素 (x, y) は canvas の画素 (left + x, top + y) にちょうど重なる
// (半画素の位相の補正。P2-0 で、補正なしは 2603 画素ずれた)。
// +Y が上の座標系 (Manim など) の backend は、縦に -shiftY だけ動かす。
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

// 1 枚の被覆率 (alpha) の画像。1 画素 1 byte、行間の余白なし (width * height byte)。
struct MathCoverage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
    bool operator==(const MathCoverage&) const = default;
};

// 大きさが正で、alpha の byte 数が width * height と一致するか。
bool mathCoverageValid(const MathCoverage& coverage);

// 画像の file (backend の出力の PNG など) を被覆率として読む。色は捨て、alpha だけを使う。
// 読めなければ false と error。src/media/math は decoder を持たないので、呼び出し側が渡す。
using MathCoverageLoader = std::function<bool(const std::filesystem::path& file,
                                              MathCoverage& coverage, std::string& error)>;

// 画素の矩形。width または height が 0 なら空。
struct MathRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0; }

    bool operator==(const MathRect&) const = default;
};

// alpha > 0 の画素の外接矩形。alpha が全く無ければ空。
MathRect mathCoverageBounds(const MathCoverage& coverage);

// 2 つの矩形を含む最小の矩形。空の矩形は無視する (両方空なら空)。
MathRect mathRectUnion(const MathRect& a, const MathRect& b);

// 空でない bounds が width x height の画像の縁 (最外周の 1 画素) に触れるか。
bool mathRectTouchesEdge(const MathRect& bounds, int width, int height);

// frame の (left, top) に mask を置いたとき、alpha が違う画素の数。mask の外は 0 と比べる。
// 大きさが不正、または mask が frame からはみ出すなら -1。
std::int64_t mathEndpointDifference(const MathCoverage& frame, const MathCoverage& mask, int left,
                                    int top);

// 変形の連番の描画要求。端点の静止の mask は、同じ backend の静止の描画 (renderer の正) を
// 呼び出し側が decode して渡す。backend は frame 0 と終状態をこれと画素で照合する。
struct MathTransformRenderRequest {
    MathTransformSpec spec;
    MathCoverage sourceStatic;
    MathCoverage targetStatic;
    // backend が自由に使ってよい作業 directory。呼び出し側が作り、後で消す。
    std::filesystem::path jobDirectory;
    std::chrono::milliseconds timeout{120000};
};

// 変形の連番。色は含まない (白の glyph を透過背景に描いた PNG で、alpha が被覆率)。
// 文字色の補間 (mathTransformColorAt) と配置は合成の側で行う。
struct MathTransformRenderResult {
    MathRenderStatus status = MathRenderStatus::Failed;
    // spec.frames 枚 (frame i は進み具合 i / frames)。各 frame は canvas の大きさ。
    // 終状態の照合に使った 1 枚は含めない。
    std::vector<std::filesystem::path> frames;
    int canvasWidth = 0;
    int canvasHeight = 0;
    // canvas の座標で、全 frame (照合の 1 枚を含む) の alpha の外接矩形と、
    // 両端の静止の矩形の和。artifact として切り出す範囲。
    MathRect artifact;
    // canvas の中の端点の配置 (P2-2 の契約)。
    MathTransformPlacement placement;
    std::string message;
    std::string log;
};

// 変形の frame (frame / frames) での文字色 (0xAARRGGBB)。A・R・G・B の各成分を
// straight alpha のまま (premultiply せずに) 線形に補間し、0.5 は切り上げて整数に丸める。
// frame 0 は from、frame == frames (連番に含めない終状態) は to にちょうど一致する。
// frames <= 0、frame が [0, frames] の外、または frames が大きすぎる (2^40 超) なら false。
// 背景は補間しない (P2-1 で両端の背景は透明に限っている)。
bool mathTransformColorAt(std::uint32_t fromArgb, std::uint32_t toArgb, std::int64_t frame,
                          std::int64_t frames, std::uint32_t& argb);

// 変形の artifact (切り出した被覆) を出力 raster に置く左上 (P2-5)。preview と書き出しが共有する。
//
// 端点 (frame 0 と、連番に含めない終状態) では、artifact の中の端点の静止が、静止の配置
// (mathRasterPlacement) とちょうど同じ画素に来なければならない。したがって
//   source = mathRasterPlacement(source の静止) - artifact の中の source の位置
//   target = mathRasterPlacement(target の静止) - artifact の中の target の位置
// 両端の静止の大きさの偶奇が違うと、2 つは 1 画素違いうる (中央の余りを左上へ寄せるため)。
struct MathTransformRasterPlacement {
    int sourceLeft = 0;
    int sourceTop = 0;
    int targetLeft = 0;
    int targetTop = 0;
    bool operator==(const MathTransformRasterPlacement&) const = default;
};

// artifact の大きさと、その中の両端の静止の位置・大きさから、出力 raster での artifact の
// 左上を決める。大きさが不正、端点の静止が artifact からはみ出す、静止が出力より大きい、
// または artifact がどちらの端の位置でも出力に収まらなければ false
// (はみ出した画素を黙って切らない)。
bool mathTransformRasterPlacement(int artifactWidth, int artifactHeight, int sourceX, int sourceY,
                                  int sourceWidth, int sourceHeight, int targetX, int targetY,
                                  int targetWidth, int targetHeight, int outputWidth,
                                  int outputHeight, MathTransformRasterPlacement& placement);

// 変形の frame (0 <= frame < frames) での artifact の左上。各軸、source の位置から target の
// 位置へ frame / frames で線形に動かし、最も近い整数に丸める。ちょうど半分は target の側へ
// 丸める (動く向きに依らず、進み具合 1/2 で同じように切り替わる)。
// frame 0 は source の位置。P2-2 の配置 (mathTransformPlacement) で描いた artifact では
// 端点の差は各軸 1 画素以内なので、frames >= 2 なら最後の frame (進み具合 (frames-1)/frames
// >= 1/2) は target の位置になり、その次の target の静止へ段差なく続く。
// frames <= 0、frames が 2^28 超、または frame が [0, frames) の外なら false。
bool mathTransformArtifactOriginAt(const MathTransformRasterPlacement& placement,
                                   std::int64_t frame, std::int64_t frames, int& left, int& top);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_TRANSFORM_H
