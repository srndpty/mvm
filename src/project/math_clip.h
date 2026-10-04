#ifndef MVM_PROJECT_MATH_CLIP_H
#define MVM_PROJECT_MATH_CLIP_H

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

} // namespace mvm::project

#endif // MVM_PROJECT_MATH_CLIP_H
