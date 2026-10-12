#ifndef MVM_MEDIA_MATH_GROUP_COMPOSE_H
#define MVM_MEDIA_MATH_GROUP_COMPOSE_H

// 重なった数式 layer を、下の RGBA へ下から順に合成する (CPU、字形の範囲だけ)。
//
// 目的は、MLT 7.36.1 の affine transition が全画面の透過画像を 1 枚ずつ重ねた結果と、byte 単位で
// 同じ画素を作ること (docs/math-compositor-p3-design.md、docs/math-compositor-p31.md、
// docs/math-compositor-p312.md)。
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
//   - 下の alpha は 8 bit の値に binary32 の 1/255 を掛けて求める (255 で割らない)。MLT の実 binary
//     (libmltplus の interpBL_b32) がそうしている。下の alpha が 254 などのとき、割り算とは
//     binary32 の末尾の桁が違い、色が 1 階調ずれる画素が出る (P3-1.2 の反例)。
//
// 対象にしてよい入力 (これ以外は拒否する。MLT 側の画素の振る舞いを確かめていない):
//   - 等倍の layer だけ。拡縮、回転、shear、crop、出力と大きさの違う layer は対象外。
//   - 合成を始める前の destination は、全画素が不透明 (alpha 255) であること。透明・半透明の
//     下地は実際の MLT と比べていない。合成の途中で alpha が 254 になるのは MLT と同じ結果で、
//     これは対象の中にある (実際の MLT の出力と全画素を比べている)。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mvm::math {

// 合成器へ渡せる layer の画像。prepare だけが中身を作り、作った後は変えられない。
// 画素は複製して持つ (呼び出し側が元の buffer を書き換えても変わらない)。字形の範囲
// (alpha が 0 でない texel の外接矩形) は prepare が画素から求める。外から範囲を渡す口は無い。
class PreparedMathRaster {
public:
    PreparedMathRaster() = default; // 空。合成器は拒否する

    // rgba は width * height の RGBA8 (straight alpha)、行の間隔が stride byte。
    static bool prepare(const std::uint8_t* rgba, int width, int height, std::size_t stride,
                        PreparedMathRaster& out, std::string& error);

    bool valid() const { return width_ > 0; }

    int width() const { return width_; }

    int height() const { return height_; }

    const std::uint8_t* rgba() const { return rgba_.data(); } // 行間の余白なし

    // 字形の範囲 (texel 座標)。alpha が全部 0 の画像は幅・高さとも 0。
    int roiX() const { return roiX_; }

    int roiY() const { return roiY_; }

    int roiWidth() const { return roiWidth_; }

    int roiHeight() const { return roiHeight_; }

private:
    std::vector<std::uint8_t> rgba_;
    int width_ = 0, height_ = 0;
    int roiX_ = 0, roiY_ = 0, roiWidth_ = 0, roiHeight_ = 0;
};

struct MathGroupPlacement {
    const PreparedMathRaster* raster = nullptr;
    double rectX = 0.0;
    double rectY = 0.0;
    double opacity = 1.0;
};

struct MathGroupStats {
    std::uint64_t visitedPixels = 0; // 補間と合成を行った出力画素の数 (全 layer の合計)
};

// destination が対象の下地か (全画素の alpha が 255)。
bool mathGroupDestinationEligible(const std::uint8_t* destination, int width, int height,
                                  std::size_t stride);

// destination は width * height の RGBA8、行の間隔が stride byte。
// 入力が対象外 (上の「対象にしてよい入力」) なら、destination へ何も書かずに false を返す。
// 対象外の入力を別の計算で代わりに合成することはしない。
bool composeMathGroup(std::uint8_t* destination, int width, int height, std::size_t stride,
                      const MathGroupPlacement* layers, std::size_t count, std::string& error,
                      MathGroupStats* stats = nullptr);

// 合成しない画素を飛ばしてよいか。MLT は alpha 0 の texel でも下の alpha を 255 * (a * (1/255))
// で書き直す。これが全ての a (0〜255) で a に戻るなら、字形の無い画素は飛ばしても結果が同じになる。
bool mathGroupAlphaRoundTripIsIdentity();

// 検査を通さない入口。単体試験と、検証 harness が画素の途中の値を追うためだけに使う。
// 字形の範囲と下地の alpha を確かめない。製品の code から呼ばない。
namespace detail {

struct UncheckedMathGroupLayer {
    const std::uint8_t* rgba = nullptr; // width * height * 4 byte、行間の余白なし
    int width = 0;
    int height = 0;
    double rectX = 0.0;
    double rectY = 0.0;
    double opacity = 1.0;
    // alpha が 0 でない texel を含む範囲 (texel 座標)。この外の texel は alpha 0 でなければ
    // ならない (確かめない)。roiWidth が 0 なら全体を走査する。
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
};

// 失敗したら destination は途中まで書かれていることがある。
bool composeMathGroupUnchecked(std::uint8_t* destination, int width, int height, std::size_t stride,
                               const UncheckedMathGroupLayer* layers, std::size_t count,
                               std::string& error, MathGroupStats* stats = nullptr);

} // namespace detail

} // namespace mvm::math

#endif
