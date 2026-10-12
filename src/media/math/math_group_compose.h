#ifndef MVM_MEDIA_MATH_GROUP_COMPOSE_H
#define MVM_MEDIA_MATH_GROUP_COMPOSE_H

// 重なった数式 layer を、下の RGBA へ下から順に合成する (CPU、字形の範囲だけ)。
//
// 目的は、MLT 7.36.1 の affine transition が全画面の透過画像を 1 枚ずつ重ねた結果と、byte 単位で
// 同じ画素を作ること (docs/math-compositor-p3-design.md、docs/math-compositor-p31.md)。
// MLT にも Qt にも依存しない。製品の書き出しからはまだ呼ばれていない。
//
// 画素の規則:
//   - destination・layer とも straight alpha の RGBA8 (byte の並びは R, G, B, A)。premultiply
//   しない。
//   - layers[0] が一番下。順に destination へ重ね、1 枚ごとに 8 bit へ切り捨てる。
//   - layer は出力と同じ大きさの画像で、等倍のまま左上を (rectX, rectY) へ置く。端数の位置は
//     双線形補間になる。素材座標は倍精度で求めて binary32 へ落とし、以降は binary32 で計算する。
//   - 素材座標が 0 以上 (幅 - 1) 以下、0 以上 (高さ - 1) 以下に入る出力画素だけを書く
//     (外へはみ出した分は描かない)。
//   - 不透明度は 0〜1。0 でも式どおりに計算する (結果は下の画素のまま)。
//
// 対応していないもの (拒否する): 拡縮、回転、shear、crop、出力と大きさの違う layer。
// これらは MLT 側の画素の振る舞いをまだ確かめていない。

#include <cstddef>
#include <cstdint>
#include <string>

namespace mvm::math {

struct MathGroupLayer {
    const std::uint8_t* rgba = nullptr; // width * height * 4 byte、行間の余白なし
    int width = 0;
    int height = 0;
    double rectX = 0.0;
    double rectY = 0.0;
    double opacity = 1.0;
    // alpha が 0 でない texel を含む範囲 (texel 座標)。この外の texel は alpha 0
    // でなければならない。 走査をこの範囲の近くに限るためだけに使う。roiWidth が 0
    // なら全体を走査する。
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
};

struct MathGroupStats {
    std::uint64_t visitedPixels = 0; // 補間と合成を行った出力画素の数 (全 layer の合計)
};

// destination は width * height の RGBA8、行の間隔が stride byte。失敗したら destination は
// 途中まで書かれていることがある (呼び出し側は失敗した frame を使わない)。
bool composeMathGroup(std::uint8_t* destination, int width, int height, std::size_t stride,
                      const MathGroupLayer* layers, std::size_t count, std::string& error,
                      MathGroupStats* stats = nullptr);

// 合成しない画素を飛ばしてよいか。MLT は alpha 0 の texel でも下の alpha を 255 * (a / 255) で
// 書き直す。これが全ての a (0〜255) で a に戻るなら、字形の無い画素は飛ばしても結果が同じになる。
bool mathGroupAlphaRoundTripIsIdentity();

} // namespace mvm::math

#endif
