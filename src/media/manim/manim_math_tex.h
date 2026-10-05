#ifndef MVM_MEDIA_MANIM_MANIM_MATH_TEX_H
#define MVM_MEDIA_MANIM_MANIM_MATH_TEX_H

// 数式 clip の backend "manim-mathtex"。Manim の MathTex で数式を透過 PNG に描く。
//
// Manim 固有の設定と処理はここに閉じる。外へは math::MathRenderBackend (中立な契約) だけを出す。

#include "media/math/math_render.h"
#include "media/math/math_tex_segments.h"
#include "media/math/math_transform.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mvm::manim {

inline constexpr char kMathTexBackendId[] = "manim-mathtex";
// script template を変えたら上げる (fingerprint に入り、古い cache を引かなくなる)。
inline constexpr int kMathTexTemplateVersion = 1;
// Write の連番の script。変えたら版を上げる (連番の key だけに入り、静止の cache は変えない)。
inline constexpr char kMathWriteTemplateId[] = "manim-write";
inline constexpr int kMathWriteTemplateVersion = 1;
// Manim は連番を 4 桁 (<Scene>0000.png) で名付ける。名前の順を frame の順として使うので、
// 桁が増える枚数は描かない。
inline constexpr std::int64_t kMaximumMathWriteFrames = 9999;

struct ManimMathTexConfig {
    // 明示された Manim executable。PATH から探さない。
    std::filesystem::path manimExecutablePath;
    // preflight が版の出力を書く作業 directory (呼び出し側が所有する)。
    std::filesystem::path workDirectory;
    std::chrono::milliseconds toolTimeout{30000};
};

// Manim・latex・dvisvgm が使えるかを調べ、それぞれの版から fingerprint を作る。
// latex と dvisvgm は、Manim へ引き継ぐのと同じ環境変数 PATH の各 directory から探す
// (見つからなければ他の場所を探さず Unavailable)。Available なら render 関数を束ねて返す。
math::MathPreflightResult preflightManimMathTex(const ManimMathTexConfig& config,
                                                const std::atomic<bool>* cancel);

// 1 つの式を描く。preflight が backend に束ねる関数そのもの (試験から直接呼べるよう公開)。
math::MathStaticRenderResult renderManimMathTex(const std::filesystem::path& manimExecutablePath,
                                                const math::MathStaticRenderRequest& request,
                                                const std::atomic<bool>* cancel);

// 式を Write で書く連番を描く (spec.frames 枚、frame i は進み具合 i / frames)。
// preflight が backend に束ねる関数そのもの。
math::MathSequenceRenderResult
renderManimMathWrite(const std::filesystem::path& manimExecutablePath,
                     const math::MathSequenceRenderRequest& request,
                     const std::atomic<bool>* cancel);

// 式から式への変形 (P2) の script。変えたら版を上げる (変形の key だけに入る)。
inline constexpr char kMathTransformTemplateId[] = "manim-transform";
inline constexpr int kMathTransformTemplateVersion = 1;
// 変形は frames 枚に終状態の照合の 1 枚を足して描く。4 桁の連番に収まる枚数まで。
inline constexpr std::int64_t kMaximumMathTransformFrames = kMaximumMathWriteFrames - 1;
// 変形を描く一時的な canvas の、端点の大きい方の静止の矩形からの各辺の余白 (px)。
// 途中の frame がこの余白の縁に触れたら、切れたとみなして失敗にする。
inline constexpr int kMathTransformCanvasPadding = 200;

// 変形の描画の計画 (Manim を起動する前に決まる値)。
struct ManimTransformPlan {
    std::vector<math::MathTexSegment> sourceSegments;
    std::vector<math::MathTexSegment> targetSegments;
    math::MathSegmentMatching matching;
    // 両端の静止の mask の大きさ。
    int sourceWidth = 0;
    int sourceHeight = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    math::MathTransformPlacement placement; // raster の座標 (+Y は下)
};

// spec と両端の静止の mask の大きさから計画を作る。分け方 (segmentMathTex) と照合
// (matchMathTexSegments) は mvm の正で、Manim の TransformMatchingTex には任せない。
// 描画要求が不正なら false と error。
bool planManimMathTransform(const math::MathTransformSpec& spec, int sourceWidth, int sourceHeight,
                            int targetWidth, int targetHeight, ManimTransformPlan& plan,
                            std::string& error);

// raster の縦の shift (+Y は下) を、Manim の縦の shift (+Y は上) へ換算する (px のまま)。
double manimShiftUpFor(double rasterShiftY);

// 変形の request.json。部分の文字列・対応・canvas・各端点の Manim の向きの shift を渡す。
std::string manimMathTransformRequestJson(const math::MathTransformSpec& spec,
                                          const ManimTransformPlan& plan);

// script が書いた部分の構造の報告 (structure.txt) を、mvm が分けた部分と照らす。
// 部分の数・各部分の文字列・型 (MathTexPart) が一致し、Manim の代用の log が無ければ空文字列。
// そうでなければ利用者に見せる理由 (式全体の group での代用を黙って受け付けない)。
std::string checkManimTransformStructure(const std::string& report,
                                         const std::vector<math::MathTexSegment>& source,
                                         const std::vector<math::MathTexSegment>& target);

// 変形の連番を描く。frames 枚 + 終状態の照合の 1 枚を余白の広い canvas に描き、全 frame の
// alpha を loader で読んで検査する (縁に触れない・frame 0 が source の静止と一致・終状態が
// target の静止と一致)。artifact の矩形は全 frame の外接矩形と両端の静止の矩形の和。
math::MathTransformRenderResult
renderManimMathTransform(const std::filesystem::path& manimExecutablePath,
                         const math::MathTransformRenderRequest& request,
                         const math::MathCoverageLoader& loader, const std::atomic<bool>* cancel);

// script が読む request.json の内容。式は Python の source へ埋め込まず、この JSON で渡す。
std::string manimMathTexRequestJson(const math::MathRenderSpec& spec);
// Write の request.json。静止の内容に intro_frames (描く枚数) を足す。
std::string manimMathWriteRequestJson(const math::MathSequenceSpec& spec);

// mvm の fontSize (1 em の px) を Manim の font_size へ換算する。
// 1080p 相当の 135 px/unit では 1 em = font_size x 17/12 px (docs/math-clips.md P0-0)。
double manimFontSizeFor(int emPixels);

} // namespace mvm::manim

#endif // MVM_MEDIA_MANIM_MANIM_MATH_TEX_H
