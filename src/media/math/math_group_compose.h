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
//   - 合成を始める前の destination は、全画素の alpha が 1 以上であること
//     (kMathGroupMinimumDestinationAlpha)。alpha 1〜255 のすべての値について、実際の MLT の出力と
//     全画素が一致することを確かめている (docs/math-transition-p33b.md)。
//   - alpha 0 の画素がある destination は対象外。MLT は、上下とも alpha 0 の画素の色を 0 に
//     書き換える (0 での割り算の結果を 8 bit へ落とすため)。これは字形の無い画素でも起きるので、
//     字形の範囲だけを走査するこの合成器では再現しない。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mvm::math {

// 準備済みの画像が画素をどう持つか。
//   RoiOnly    中身のある範囲 (どれかの byte が 0 でない texel の外接矩形) の画素だけを持つ。
//   FullFrame  元の画像の全画素を持つ (P3-3B までの形。P3-3C の間、比較の基準として残す)。
// どちらも合成の結果は同じでなければならない (docs/math-transition-p33c.md)。
enum class MathRasterStorage { RoiOnly, FullFrame };

// 合成器へ渡せる layer の画像。prepare だけが中身を作り、作った後は変えられない
// (複製は同じ画素を共有する)。呼び出し側が元の buffer を書き換えても変わらない。
//
// 座標は、いつも元の画像 (width x height、左上が原点) のものである。RoiOnly でも、保存した範囲を
// 「左上が (0, 0) の小さい画像」としては扱わない: 保存した範囲の位置 (storedX, storedY) を持ち、
// 元の画像の texel (x, y) は「範囲の中なら保存した画素、外なら (0, 0, 0, 0)」として読む。
// 範囲の外の texel が 4 byte とも 0 であることは、prepare が全画素を走査して範囲を決めることで
// 成り立つ (alpha が 0 でも色が 0 でない texel は範囲に入る。MLT は色を alpha
// と独立に補間するので、 そういう texel を捨てると縁の画素の色が変わる)。外から範囲を渡す口は無い。
class PreparedMathRaster {
public:
    PreparedMathRaster() = default; // 空。合成器は拒否する

    // rgba は width * height の RGBA8 (straight alpha)、行の間隔が stride byte。
    // provenance は、画像の出どころ (成果物と前処理) を呼び出し側が記録するための文字列で、
    // 合成には使わない。
    static bool prepare(const std::uint8_t* rgba, int width, int height, std::size_t stride,
                        PreparedMathRaster& out, std::string& error,
                        MathRasterStorage storage = MathRasterStorage::RoiOnly,
                        std::string provenance = {});

    bool valid() const { return width_ > 0; }

    // 元の画像の大きさ。
    int width() const { return width_; }

    int height() const { return height_; }

    // 字形の範囲 (元の画像の texel 座標): alpha が 0 でない texel の外接矩形。alpha が全部 0 なら
    // 幅・高さとも 0。合成器が走査する出力画素を決めるのに使う。
    int roiX() const { return roiX_; }

    int roiY() const { return roiY_; }

    int roiWidth() const { return roiWidth_; }

    int roiHeight() const { return roiHeight_; }

    // 保存している範囲 (元の画像の texel 座標) と、その画素 (行間の余白なし、storedWidth * 4 byte
    // ごと)。FullFrame なら (0, 0, width, height)。RoiOnly なら中身のある範囲で、全 byte が 0 の
    // 画像は幅・高さとも 0 (画素を持たない)。字形の範囲は必ずこの中にある。
    MathRasterStorage storage() const { return storage_; }

    int storedX() const { return storedX_; }

    int storedY() const { return storedY_; }

    int storedWidth() const { return storedWidth_; }

    int storedHeight() const { return storedHeight_; }

    const std::uint8_t* storedRgba() const;

    std::size_t storedBytes() const;

    // 元の画像の texel (x, y) の 4 byte。保存していない所は 0。範囲外の座標も 0 を返す。
    void texel(int x, int y, std::uint8_t out[4]) const;

    // 元の画像の全画素 (大きさを含む) から求めた照合用の値。同じ画像なら、持ち方が違っても同じ。
    std::uint64_t contentChecksum() const { return checksum_; }

    const std::string& provenance() const { return provenance_; }

    // 元の画像として読める内容が other と完全に同じか (大きさ・持ち方・範囲・保存した全 byte)。
    // 照合用の値だけでは決めない (値が同じでも byte を比べる)。
    bool sameContent(const PreparedMathRaster& other) const;

private:
    struct Pixels;

    std::shared_ptr<const Pixels> pixels_;
    MathRasterStorage storage_ = MathRasterStorage::RoiOnly;
    int width_ = 0, height_ = 0;
    int roiX_ = 0, roiY_ = 0, roiWidth_ = 0, roiHeight_ = 0;
    int storedX_ = 0, storedY_ = 0, storedWidth_ = 0, storedHeight_ = 0;
    std::uint64_t checksum_ = 0;
    std::string provenance_;
};

// 生きている準備済みの画像が持つ画素の byte 数の合計 (process 全体)。全部を解放すれば 0 になる。
long long preparedMathRasterLiveBytes();

// 同じ画像を 1 回だけ持つための置き場 (書き出し 1 回の間だけ使う)。
// 共有するのは、出どころ (provenance: 成果物・見た目・前処理を呼び出し側が書いた文字列) が同じで、
// かつ内容が完全に同じ (sameContent) ときだけである。出どころが同じでも内容が違えば別の画像として
// 持つ (同じ path の成果物が書き換わっていた場合など)。内容が同じでも出どころが違えば共有しない。
// intern が返す pointer は、この置き場が生きている間は動かない。thread からは読むだけにする
// (intern は 1 つの thread から呼ぶ)。
class PreparedMathRasterPool {
public:
    PreparedMathRasterPool();
    ~PreparedMathRasterPool();
    PreparedMathRasterPool(const PreparedMathRasterPool&) = delete;
    PreparedMathRasterPool& operator=(const PreparedMathRasterPool&) = delete;

    // raster を置き場へ入れ、使う画像を返す。同じ出どころ・同じ内容の画像が既にあればそれを返し、
    // raster は捨てる。valid でない raster は nullptr。
    const PreparedMathRaster* intern(PreparedMathRaster raster);

    std::size_t size() const;          // 持っている画像の数
    std::size_t storedBytes() const;   // 持っている画像の画素の byte 数の合計
    std::size_t sharedCount() const;   // 既にある画像を返した回数
    std::size_t conflictCount() const; // 出どころが同じで内容が違った回数 (別に持った)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct MathGroupPlacement {
    const PreparedMathRaster* raster = nullptr;
    double rectX = 0.0;
    double rectY = 0.0;
    double opacity = 1.0;
};

struct MathGroupStats {
    std::uint64_t visitedPixels = 0; // 補間と合成を行った出力画素の数 (全 layer の合計)
    // 素材座標の表 (layer ごとに幅 + 高さの分) を作る時間の合計 (ns)。計測用。
    std::uint64_t coordinateNs = 0;
};

// 合成を始める前の destination の alpha の下限 (これより小さい画素が 1 つでもあれば拒否する)。
// 値の根拠は docs/math-transition-p33b.md (P3-3B より前は 255)。
constexpr int kMathGroupMinimumDestinationAlpha = 1;

// destination の全画素の alpha の最小値。参照・大きさが不正なら -1。
int mathGroupMinimumDestinationAlpha(const std::uint8_t* destination, int width, int height,
                                     std::size_t stride);

// destination が対象の下地か (全画素の alpha が kMathGroupMinimumDestinationAlpha 以上)。
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
    // 非 0 なら、rgba は元の画像のうち [storedX, storedX + storedWidth) x
    // [storedY, storedY + storedHeight) だけを持つ (storedWidth * 4 byte ごと)。その外の texel は
    // (0, 0, 0, 0) として読む。storedWidth・storedHeight が 0 なら画素を持たない (rgba
    // は使わない)。 0 なら、rgba は元の画像の全画素 (上の width・height)。
    bool storedRegion = false;
    int storedX = 0;
    int storedY = 0;
    int storedWidth = 0;
    int storedHeight = 0;
};

// composeMathGroup と同じ検査をするが、destination の alpha の下限を呼び出し側が決める。
// 下限の根拠を調べる実験 (検証 harness、診断用の書き出し) だけが使う。対象外なら何も書かない。
bool composeMathGroupWithAlphaFloor(std::uint8_t* destination, int width, int height,
                                    std::size_t stride, const MathGroupPlacement* layers,
                                    std::size_t count, int minimumDestinationAlpha,
                                    std::string& error, MathGroupStats* stats = nullptr);

// 失敗したら destination は途中まで書かれていることがある。
bool composeMathGroupUnchecked(std::uint8_t* destination, int width, int height, std::size_t stride,
                               const UncheckedMathGroupLayer* layers, std::size_t count,
                               std::string& error, MathGroupStats* stats = nullptr);

} // namespace detail

} // namespace mvm::math

#endif
