#ifndef MVM_PROJECT_MATH_CLIP_H
#define MVM_PROJECT_MATH_CLIP_H

#include <cstdint>
#include <string>

namespace mvm::project {

// 数式 clip の正 (Project に保存する値)。数式の意味と見た目だけを持つ。
// 何で描くか (backend) は描画方針でありここに持たない。どこに置くか (位置・拡大・回転) は
// 画像 clip と同じく ClipEffects が持つ。描いた画像は派生物で、Project には保存しない
// (docs/math-clips.md)。
struct MathClipData {
    std::string syntax = "latex"; // 記法。現在は latex だけを受理する
    std::string source;           // 利用者が書いた式
    int fontSize = 96;            // 1 em の px (拡大 100% のとき)。描く解像度を決める
    std::string color = "#FFFFFFFF";
    // glyph の bbox (と余白) を塗る色。既定は透明。
    std::string backgroundColor = "#00000000";
    bool operator==(const MathClipData&) const = default;
};

// 値の形だけを見る (空でない式、既知の記法、文字サイズの範囲、色の形式)。
// 式を描けるかどうかは見ない。描けない式も Project の正として保存できる。
bool validateMathClipData(const MathClipData& data, int outputHeight, std::string& error);

// clip の先頭で式を出す animation の種類。Write は Manim の Write で式を書いていく。
enum class MathIntroKind { None, Write };

// 数式 clip の時間の振る舞い (docs/math-clips.md の P1)。式の意味と見た目 (MathClipData) とは
// 分けて持つ。MathClipData から作る静止の描画 (と cache key) を変えないため。
// 位置・拡大・不透明度などの動きは ClipEffects が持ち、ここには持たない。
struct MathClipAnimation {
    MathIntroKind intro = MathIntroKind::None;
    // intro の尺。clip の素材 frame (fade と同じ domain) で数え、clip の見えている先頭から始まる。
    // None のときは 0。
    std::int64_t introFrames = 0;
    bool operator==(const MathClipAnimation&) const = default;
};

// 形と時間の意味だけを見る。None なら尺は 0、Write なら 1 から clip の尺 (素材 frame) まで。
// 描画の方式 (連番の枚数や memory) による上限は持たない。描けない長さは Project の値としては
// 正しく、描画 (backend・cache) が「未対応」として失敗する。
bool validateMathClipAnimation(const MathClipAnimation& animation, std::int64_t clipSourceFrames,
                               std::string& error);

const char* mathIntroKindName(MathIntroKind kind);
bool parseMathIntroKind(const std::string& name, MathIntroKind& kind);

} // namespace mvm::project

#endif // MVM_PROJECT_MATH_CLIP_H
