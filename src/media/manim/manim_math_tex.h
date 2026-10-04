#ifndef MVM_MEDIA_MANIM_MANIM_MATH_TEX_H
#define MVM_MEDIA_MANIM_MANIM_MATH_TEX_H

// 数式 clip の backend "manim-mathtex"。Manim の MathTex で数式を透過 PNG に描く。
//
// Manim 固有の設定と処理はここに閉じる。外へは math::MathRenderBackend (中立な契約) だけを出す。

#include "media/math/math_render.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace mvm::manim {

inline constexpr char kMathTexBackendId[] = "manim-mathtex";
// script template を変えたら上げる (fingerprint に入り、古い cache を引かなくなる)。
inline constexpr int kMathTexTemplateVersion = 1;

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

// script が読む request.json の内容。式は Python の source へ埋め込まず、この JSON で渡す。
std::string manimMathTexRequestJson(const math::MathRenderSpec& spec);

// mvm の fontSize (1 em の px) を Manim の font_size へ換算する。
// 1080p 相当の 135 px/unit では 1 em = font_size x 17/12 px (docs/math-clips.md P0-0)。
double manimFontSizeFor(int emPixels);

} // namespace mvm::manim

#endif // MVM_MEDIA_MANIM_MANIM_MATH_TEX_H
