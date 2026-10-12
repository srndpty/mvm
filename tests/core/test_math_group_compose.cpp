// 数式 layer の CPU 合成 (src/media/math/math_group_compose) の単体試験と計測。
//
// 期待値は既存の独立した oracle (tests/harness/mlt_rgba_oracle.h の mltSourceOver) で作る。
// 合成器の関数を期待値の計算に使わない。oracle は整数位置 (補間なし) の source-over なので、
// ここで確かめるのは整数位置だけである。端数位置は実際の MLT の出力との比較で確かめる
// (mvm_math_score_export_parity --reference-mode 9)。
//
// 引数 "bench" を付けると計測だけを行う (合否には使わない)。

#include "../harness/mlt_rgba_oracle.h"
#include "media/math/math_group_compose.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

// 既存の試験は、範囲と下地を確かめない入口 (detail) を直接使う: 誤った範囲を渡す対照と、
// 下の alpha が 254 の画素の検査は、検査を通す入口からは作れない。
using MathGroupLayer = mvm::math::detail::UncheckedMathGroupLayer;
constexpr auto composeUnchecked = mvm::math::detail::composeMathGroupUnchecked;
using Image = std::vector<std::uint8_t>;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct Placed {
    Image rgba;
    int x = 0, y = 0;
    double opacity = 1.0;
};

// oracle による期待値。layer を下から順に、整数だけずらして重ねる。範囲の外は書かない。
// MLT は素材座標が幅 - 1・高さ - 1 を超える画素を書かない。整数位置では全 texel が対象になる。
Image expected(Image destination, int width, int height, const std::vector<Placed>& layers) {
    for (const auto& layer : layers)
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column) {
                const int sx = column - layer.x, sy = row - layer.y;
                if (sx < 0 || sx >= width || sy < 0 || sy >= height)
                    continue;
                auto* to =
                    destination.data() + (static_cast<std::size_t>(row) * width + column) * 4;
                const auto* from =
                    layer.rgba.data() + (static_cast<std::size_t>(sy) * width + sx) * 4;
                const auto result =
                    mvm::test::mltSourceOver({to[0], to[1], to[2], to[3]},
                                             {from[0], from[1], from[2], from[3]}, layer.opacity);
                std::memcpy(to, result.data(), 4);
            }
    return destination;
}

bool compose(Image& destination, int width, int height, const std::vector<Placed>& layers,
             bool roi = false, int omitRightColumns = 0) {
    std::vector<MathGroupLayer> input;
    for (const auto& layer : layers) {
        MathGroupLayer item;
        item.rgba = layer.rgba.data();
        item.width = width;
        item.height = height;
        item.rectX = layer.x;
        item.rectY = layer.y;
        item.opacity = layer.opacity;
        if (roi) { // 中央の 1/2 だけに字形がある画像用
            item.roiX = width / 4;
            item.roiY = height / 4;
            // omitRightColumns > 0 は対照用: 字形のある右端の列を範囲から外す (誤った範囲)。
            item.roiWidth = width / 2 - omitRightColumns;
            item.roiHeight = height / 2;
        }
        input.push_back(item);
    }
    std::string error;
    return composeUnchecked(destination.data(), width, height, static_cast<std::size_t>(width) * 4,
                            input.data(), input.size(), error, nullptr);
}

long long differing(const Image& a, const Image& b) {
    long long count = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        count += a[i] != b[i];
    return count;
}

// 中央の 1/2 に、透明・半透明・不透明の texel が混ざった画像。外側は完全に透明 (0,0,0,0)。
Image glyph(std::mt19937& random, int width, int height) {
    Image image(static_cast<std::size_t>(width) * height * 4, 0);
    for (int row = height / 4; row < height / 4 + height / 2; ++row)
        for (int column = width / 4; column < width / 4 + width / 2; ++column) {
            auto* p = image.data() + (static_cast<std::size_t>(row) * width + column) * 4;
            const int kind = static_cast<int>(random() % 4);
            p[3] = kind == 0 ? 0 : kind == 1 ? 255 : static_cast<std::uint8_t>(random() % 256);
            for (int c = 0; c < 3; ++c)
                p[c] = p[3] == 0 ? 0 : static_cast<std::uint8_t>(random() % 256);
        }
    return image;
}

int runTests() {
    std::printf("alpha の書き直しは恒等: %s\n",
                mvm::math::mathGroupAlphaRoundTripIsIdentity() ? "はい" : "いいえ");
    constexpr int width = 64, height = 48;
    std::mt19937 random(20261012);
    long long compared = 0;
    for (int round = 0; round < 40; ++round) {
        // 下地は不透明 (alpha 255)。MLT の書き出しの最下層と同じ条件。
        Image base(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < base.size(); ++i)
            base[i] = i % 4 == 3 ? 255 : static_cast<std::uint8_t>(random() % 256);
        std::vector<Placed> layers;
        const double opacities[] = {0.0, 0.25, 0.5, 0.75, 1.0, 0.3333};
        for (int index = 0; index < 1 + round % 5; ++index) {
            Placed layer;
            layer.rgba = glyph(random, width, height);
            // 重なり、出力の端からのはみ出し (負と正) を含める。
            layer.x = static_cast<int>(random() % 41) - 20;
            layer.y = static_cast<int>(random() % 31) - 15;
            layer.opacity = opacities[random() % 6];
            layers.push_back(std::move(layer));
        }
        // 対照を行う回は、どの layer も画素へ寄与するようにする (不透明度 0 の layer を
        // 外しても結果は変わらないので、欠落の対照にならない)。
        if (round == 7)
            for (auto& layer : layers)
                if (layer.opacity == 0.0)
                    layer.opacity = 0.5;
        // 範囲の境界の対照用: 一番下の layer の、字形の範囲の四隅と右端の列を必ず不透明にする
        // (境界ちょうどに見える画素がある)。出力の中に収まる位置へ置く。
        if (round == 7) {
            auto& bottom = layers.front();
            bottom.x = 0;
            bottom.y = 0;
            bottom.opacity = 1.0;
            const int right = width / 4 + width / 2 - 1;
            for (int row = height / 4; row < height / 4 + height / 2; ++row) {
                auto* p = bottom.rgba.data() + (static_cast<std::size_t>(row) * width + right) * 4;
                p[0] = 200;
                p[1] = 40;
                p[2] = 40;
                p[3] = 255;
            }
        }
        const Image want = expected(base, width, height, layers);
        Image got = base;
        check(compose(got, width, height, layers), "合成が成功する");
        const long long wrong = differing(got, want);
        if (wrong != 0)
            std::fprintf(stderr, "round %d: %lld byte が oracle と違います\n", round, wrong);
        check(wrong == 0, "整数位置で oracle と全 byte 一致する");
        compared += static_cast<long long>(want.size());
        Image limited = base;
        check(compose(limited, width, height, layers, true) && limited == got,
              "字形の範囲だけを走査しても結果が同じ");

        // 対照: 入力を 1 点だけ誤らせると、oracle の期待値と一致しなくなる。
        if (round == 7) {
            const auto mutated = [&](const char* name, std::vector<Placed> changed) {
                Image out = base;
                compose(out, width, height, changed);
                if (differing(out, want) == 0)
                    std::fprintf(stderr, "対照 '%s' が検出されません\n", name);
                check(differing(out, want) != 0, "対照が oracle との不一致として検出される");
            };
            auto order = layers;
            std::swap(order.front(), order.back());
            mutated("layer の順序", order);
            auto shifted = layers;
            shifted[1].x += 1;
            mutated("位置の 1 画素ずれ", shifted);
            auto faded = layers;
            faded[0].opacity = faded[0].opacity > 0.5 ? 0.25 : 0.75;
            mutated("不透明度", faded);
            auto missing = layers;
            missing.pop_back();
            mutated("layer の欠落", missing);
            auto alpha = layers;
            for (std::size_t i = 3; i < alpha[0].rgba.size(); i += 4)
                if (alpha[0].rgba[i] > 8)
                    alpha[0].rgba[i] = static_cast<std::uint8_t>(alpha[0].rgba[i] - 8);
            mutated("texel の alpha", alpha);
            // 範囲の対照: 字形のある右端の 1 列を範囲から外すと、その列が描かれず、期待値と違う。
            // (正しい範囲では上の「字形の範囲だけを走査しても結果が同じ」が成り立っている。)
            Image clipped = base;
            compose(clipped, width, height, layers, true, 1);
            if (differing(clipped, want) == 0)
                std::fprintf(stderr, "対照 '範囲の右端の欠落' が検出されません\n");
            check(differing(clipped, want) != 0,
                  "範囲から外した境界の画素は不一致として検出される");
            // 端数の位置は整数位置の期待値と一致してはならない。
            std::vector<MathGroupLayer> fractional(1);
            fractional[0].rgba = layers[0].rgba.data();
            fractional[0].width = width;
            fractional[0].height = height;
            fractional[0].rectX = layers[0].x + 0.5;
            fractional[0].rectY = layers[0].y;
            fractional[0].opacity = 1.0;
            Image out = base;
            std::string error;
            composeUnchecked(out.data(), width, height, width * 4U, fractional.data(), 1, error,
                             nullptr);
            auto single = layers;
            single.resize(1);
            single[0].opacity = 1.0;
            check(differing(out, expected(base, width, height, single)) != 0,
                  "端数の位置は整数位置の結果と違う");
        }
    }
    check(compared > 0, "比較した byte がある (空振りでない)");
    // 対応していない入力は拒否する。
    Image base(static_cast<std::size_t>(width) * height * 4, 255);
    Image small(16 * 16 * 4, 255);
    MathGroupLayer bad;
    bad.rgba = small.data();
    bad.width = 16;
    bad.height = 16;
    std::string error;
    check(!composeUnchecked(base.data(), width, height, width * 4U, &bad, 1, error, nullptr),
          "出力と大きさの違う layer (拡縮) を拒否する");
    bad.rgba = base.data();
    bad.width = width;
    bad.height = height;
    bad.opacity = 1.5;
    check(!composeUnchecked(base.data(), width, height, width * 4U, &bad, 1, error, nullptr),
          "範囲外の不透明度を拒否する");

    // --- P3-1.2: 下の alpha が 254 のときの丸め (回帰) -----------------------------------
    // 実際の MLT の pre-YUV で観測した値 (cmp-layers-48 の frame 13 (866, 556) と frame 14
    // (911, 510)、最後に重なる layer)。下の alpha を 255 で割る実装は R が 1 階調ずれる。
    {
        struct Observed {
            std::uint8_t below[4], texel[4];
            double opacity;
            std::uint8_t mlt[4];
        };

        const Observed observed[] = {
            {{204, 194, 194, 254}, {204, 34, 34, 255}, 0.12049074074074075, {203, 174, 174, 254}},
            {{204, 204, 204, 254}, {204, 34, 34, 255}, 0.13792592592592592, {204, 180, 180, 254}},
        };
        for (const auto& item : observed) {
            Image destination(4 * 4 * 4), raster(4 * 4 * 4);
            for (std::size_t pixel = 0; pixel < 16; ++pixel) {
                std::memcpy(destination.data() + pixel * 4, item.below, 4);
                std::memcpy(raster.data() + pixel * 4, item.texel, 4);
            }
            MathGroupLayer layer;
            layer.rgba = raster.data();
            layer.width = layer.height = 4;
            layer.opacity = item.opacity;
            std::string composeError;
            check(composeUnchecked(destination.data(), 4, 4, 16, &layer, 1, composeError, nullptr),
                  "下の alpha が 254 の合成が成功する");
            if (std::memcmp(destination.data(), item.mlt, 4) != 0)
                std::fprintf(stderr, "MLT の観測値 %d,%d,%d,%d に対して %d,%d,%d,%d\n", item.mlt[0],
                             item.mlt[1], item.mlt[2], item.mlt[3], destination[0], destination[1],
                             destination[2], destination[3]);
            check(std::memcmp(destination.data(), item.mlt, 4) == 0,
                  "下の alpha が 254 の画素が MLT の観測値と一致する");
        }
    }
    // 下の alpha 1〜255 (8 値) について、色・texel の alpha・不透明度を総当たりして oracle
    // と比べる。 (254 は MLT が字形の縁で作る alpha。合成の途中の下地として実際に現れる。)
    {
        constexpr int sweepWidth = 256;
        const std::uint8_t colors[] = {0, 34, 128, 204, 255};
        const std::uint8_t alphas[] = {1, 64, 75, 138, 200, 254, 255};
        const double sweepOpacities[] = {0.12049074074074075,
                                         0.13792592592592592,
                                         0.25925925925925924,
                                         0.54992592592592593,
                                         0.86207407407407421,
                                         0.95140740740740748,
                                         1.0,
                                         0.5};
        const int rows = static_cast<int>(std::size(colors) * std::size(alphas));
        long long sweepCompared = 0, sweepWrong = 0, sweepOutside = 0;
        for (const std::uint8_t belowAlpha :
             {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{64}, std::uint8_t{128},
              std::uint8_t{200}, std::uint8_t{253}, std::uint8_t{254}, std::uint8_t{255}})
            for (const double opacity : sweepOpacities) {
                Image destination(static_cast<std::size_t>(sweepWidth) * rows * 4);
                Image raster(destination.size());
                for (int row = 0; row < rows; ++row)
                    for (int column = 0; column < sweepWidth; ++column) {
                        const auto at = (static_cast<std::size_t>(row) * sweepWidth + column) * 4;
                        destination[at] = static_cast<std::uint8_t>(column);
                        destination[at + 1] = static_cast<std::uint8_t>(255 - column);
                        destination[at + 2] = static_cast<std::uint8_t>((column * 7) % 256);
                        destination[at + 3] = belowAlpha;
                        const auto color =
                            colors[static_cast<std::size_t>(row) % std::size(colors)];
                        raster[at] = color;
                        raster[at + 1] = static_cast<std::uint8_t>(255 - color);
                        raster[at + 2] = 34;
                        raster[at + 3] = alphas[static_cast<std::size_t>(row) / std::size(colors)];
                    }
                // oracle は、binary32 の値が 255 を僅かに超える組 (255 と 255 の混合など) を
                // 定義域の外として拒否する。その画素は比べず、数だけを残す (一致に数えない)。
                Image want = destination;
                std::vector<bool> outside(want.size() / 4);
                for (std::size_t at = 0; at < want.size(); at += 4) {
                    try {
                        const auto result = mvm::test::mltSourceOver(
                            {want[at], want[at + 1], want[at + 2], want[at + 3]},
                            {raster[at], raster[at + 1], raster[at + 2], raster[at + 3]}, opacity);
                        std::memcpy(want.data() + at, result.data(), 4);
                    } catch (const std::domain_error&) {
                        outside[at / 4] = true;
                        ++sweepOutside;
                    }
                }
                MathGroupLayer layer;
                layer.rgba = raster.data();
                layer.width = sweepWidth;
                layer.height = rows;
                layer.opacity = opacity;
                std::string composeError;
                check(composeUnchecked(destination.data(), sweepWidth, rows, sweepWidth * 4U,
                                       &layer, 1, composeError, nullptr),
                      "総当たりの合成が成功する");
                for (std::size_t at = 0; at < want.size(); at += 4) {
                    if (outside[at / 4])
                        continue;
                    sweepWrong += std::memcmp(destination.data() + at, want.data() + at, 4) != 0;
                    sweepCompared += 4;
                }
            }
        if (sweepWrong != 0)
            std::fprintf(stderr,
                         "下の alpha 1〜255 (8 値) の総当たり: %lld 画素が oracle と違います\n",
                         sweepWrong);
        check(sweepWrong == 0, "下の alpha 1〜255 (8 値) の総当たりで oracle と全 byte 一致する");
        check(sweepCompared > sweepOutside * 4, "総当たりの大半は oracle の定義域の中にある");
        std::printf("下の alpha 1〜255 (8 値) の総当たりで比べた byte: %lld (oracle の定義域の外で"
                    "比べなかった画素: %lld)\n",
                    sweepCompared, sweepOutside);
        compared += sweepCompared;
    }

    // --- P3-1.2: 準備済みの画像 (字形の範囲を外から渡せない) と下地の検査 ----------------
    {
        std::mt19937 localRandom(312);
        Image base(static_cast<std::size_t>(width) * height * 4);
        for (std::size_t i = 0; i < base.size(); ++i)
            base[i] = i % 4 == 3 ? 255 : static_cast<std::uint8_t>(localRandom() % 256);
        Image source = glyph(localRandom, width, height);
        // 範囲の四隅を必ず不透明にする (範囲が画素から正しく求まることを確かめる)。
        const int left = width / 4, top = height / 4, right = left + width / 2 - 1,
                  bottom = top + height / 2 - 1;
        for (const auto& corner : {std::pair{left, top}, std::pair{right, bottom}})
            source[(static_cast<std::size_t>(corner.second) * width + corner.first) * 4 + 3] = 255;
        mvm::math::PreparedMathRaster raster;
        std::string prepareError;
        check(mvm::math::PreparedMathRaster::prepare(source.data(), width, height, width * 4U,
                                                     raster, prepareError),
              "画像を準備できる");
        check(raster.roiX() == left && raster.roiY() == top && raster.roiWidth() == width / 2 &&
                  raster.roiHeight() == height / 2,
              "字形の範囲は画素から求まる");
        // 期待値: 範囲を使わない全体の走査 (検査を通さない入口、roi 0)。
        MathGroupLayer whole;
        whole.rgba = source.data();
        whole.width = width;
        whole.height = height;
        whole.rectX = 3.36;
        whole.rectY = -2.5;
        whole.opacity = 0.75;
        Image want = base;
        std::string composeError;
        check(composeUnchecked(want.data(), width, height, width * 4U, &whole, 1, composeError,
                               nullptr),
              "全体の走査が成功する");
        mvm::math::MathGroupPlacement placement{&raster, whole.rectX, whole.rectY, whole.opacity};
        Image got = base;
        check(mvm::math::composeMathGroup(got.data(), width, height, width * 4U, &placement, 1,
                                          composeError) &&
                  got == want && got != base,
              "準備済みの画像の合成は全体の走査と同じ結果になる");
        // 準備の後で元の buffer を壊しても、準備済みの画像と結果は変わらない。
        Image scribbled = source;
        std::fill(source.begin(), source.end(), std::uint8_t{255});
        Image again = base;
        check(mvm::math::composeMathGroup(again.data(), width, height, width * 4U, &placement, 1,
                                          composeError) &&
                  again == want &&
                  std::memcmp(raster.rgba(), scribbled.data(), scribbled.size()) == 0,
              "準備の後で元の buffer を書き換えても結果が変わらない");
        // 準備できない入力・準備していない画像は拒否する。
        mvm::math::PreparedMathRaster rejected;
        check(!mvm::math::PreparedMathRaster::prepare(nullptr, width, height, width * 4U, rejected,
                                                      prepareError) &&
                  !rejected.valid(),
              "参照の無い画像は準備できない");
        check(!mvm::math::PreparedMathRaster::prepare(scribbled.data(), width, height, width * 2U,
                                                      rejected, prepareError) &&
                  !rejected.valid(),
              "行の間隔が足りない画像は準備できない");
        const auto rejects = [&](Image destination, mvm::math::MathGroupPlacement item,
                                 const char* message) {
            const Image before = destination;
            std::string rejectError;
            const bool composed = mvm::math::composeMathGroup(destination.data(), width, height,
                                                              width * 4U, &item, 1, rejectError);
            check(!composed && destination == before && !rejectError.empty(), message);
        };
        mvm::math::PreparedMathRaster empty;
        rejects(base, {&empty, 0.0, 0.0, 1.0}, "準備していない画像を拒否し、何も書かない");
        rejects(base, {nullptr, 0.0, 0.0, 1.0}, "画像の無い layer を拒否し、何も書かない");
        rejects(base, {&raster, 0.0, 0.0, 1.5}, "範囲外の不透明度を拒否し、何も書かない");
        Image smallSource(16 * 16 * 4, 255);
        mvm::math::PreparedMathRaster smallRaster;
        check(mvm::math::PreparedMathRaster::prepare(smallSource.data(), 16, 16, 64, smallRaster,
                                                     prepareError),
              "小さい画像を準備できる");
        rejects(base, {&smallRaster, 0.0, 0.0, 1.0},
                "出力と大きさの違う画像 (拡縮) を拒否し、何も書かない");
        // 下地の検査 (P3-3B): alpha 0 の画素が 1 つでもあれば拒否する。MLT は上下とも alpha 0 の
        // 画素の色を 0 に書き換えるが、この合成器は再現しない。
        {
            Image transparent = base;
            transparent[(static_cast<std::size_t>(height - 1) * width + (width - 1)) * 4 + 3] =
                0; // 字形の範囲の外の 1 画素だけ
            check(!mvm::math::mathGroupDestinationEligible(transparent.data(), width, height,
                                                           width * 4U),
                  "alpha 0 の画素がある下地は対象外と判定する");
            check(mvm::math::mathGroupMinimumDestinationAlpha(transparent.data(), width, height,
                                                              width * 4U) == 0,
                  "下地の alpha の最小値を求める");
            rejects(transparent, placement, "alpha 0 の画素がある下地を拒否し、何も書かない");
        }
        // alpha 1 以上の下地は対象 (実際の MLT と alpha 1〜255 の全値で一致を確かめた範囲)。
        // 結果は、検査を通さない入口で同じ下地へ重ねたものと同じ。
        for (const std::uint8_t alpha :
             {std::uint8_t{1}, std::uint8_t{128}, std::uint8_t{253}, std::uint8_t{254}}) {
            Image translucent = base;
            for (std::size_t at = 3; at < translucent.size(); at += 4)
                translucent[at] = alpha;
            check(mvm::math::mathGroupDestinationEligible(translucent.data(), width, height,
                                                          width * 4U),
                  "alpha 1 以上の下地は対象と判定する");
            Image expectedOutput = translucent;
            MathGroupLayer sameRaster = whole; // source は上で書き換えたので、準備済みの画素を使う
            sameRaster.rgba = raster.rgba();
            check(composeUnchecked(expectedOutput.data(), width, height, width * 4U, &sameRaster, 1,
                                   composeError, nullptr),
                  "半透明の下地への全体の走査が成功する");
            Image output = translucent;
            check(mvm::math::composeMathGroup(output.data(), width, height, width * 4U, &placement,
                                              1, composeError) &&
                      output == expectedOutput && output != translucent,
                  "alpha 1 以上の下地への合成は全体の走査と同じ結果になる");
            // 下限を引き上げれば拒否される (実験用の入口)。
            Image floorOutput = translucent;
            check(!mvm::math::detail::composeMathGroupWithAlphaFloor(floorOutput.data(), width,
                                                                     height, width * 4U, &placement,
                                                                     1, 255, composeError) &&
                      floorOutput == translucent,
                  "下限より小さい alpha の下地を拒否し、何も書かない");
        }
        check(mvm::math::mathGroupDestinationEligible(base.data(), width, height, width * 4U),
              "不透明な下地は対象と判定する");
        // 2 枚を 1 回で重ねる。1 枚目で alpha が 254 になった画素の上へ 2 枚目が重なる
        // (合成の途中の 254 は対象の中)。検査を通さない入口で 1 枚ずつ重ねた結果と同じ。
        mvm::math::MathGroupPlacement stack[] = {{&raster, 0.36, 0.0, 0.5},
                                                 {&raster, 2.36, 1.5, 0.3333}};
        Image stacked = base;
        check(mvm::math::composeMathGroup(stacked.data(), width, height, width * 4U, stack, 2,
                                          composeError),
              "2 枚の合成が成功する");
        Image stepped = base;
        for (const auto& item : stack) {
            MathGroupLayer one;
            one.rgba = raster.rgba();
            one.width = width;
            one.height = height;
            one.rectX = item.rectX;
            one.rectY = item.rectY;
            one.opacity = item.opacity;
            composeUnchecked(stepped.data(), width, height, width * 4U, &one, 1, composeError,
                             nullptr);
        }
        long long edge = 0;
        for (std::size_t at = 3; at < stacked.size(); at += 4)
            edge += stacked[at] == 254;
        check(stacked == stepped && edge > 0,
              "途中で alpha が 254 になる重なりも 1 枚ずつ重ねた結果と同じ");
    }
    std::printf("oracle と比べた byte: %lld、失敗: %d\n", compared, failures);
    return failures == 0 ? 0 : 1;
}

// 計測: 1920x1080、字形の範囲と layer 数を変える。合否には使わない。
int runBench() {
    constexpr int width = 1920, height = 1080;

    struct Case {
        const char* name;
        int layers, roiWidth, roiHeight;
        double fraction;
    };

    const Case cases[] = {
        {"1 layer 532x20 整数", 1, 532, 20, 0.0},    {"1 layer 532x20 端数", 1, 532, 20, 0.36},
        {"16 layer 532x20 端数", 16, 532, 20, 0.36}, {"40 layer 532x20 端数", 40, 532, 20, 0.36},
        {"40 layer 200x60 端数", 40, 200, 60, 0.36}, {"1 layer 全画面 端数", 1, 1920, 1080, 0.36},
    };
    std::mt19937 random(1);
    for (const auto& item : cases) {
        Image raster(static_cast<std::size_t>(width) * height * 4, 0);
        const int left = (width - item.roiWidth) / 2, top = (height - item.roiHeight) / 2;
        for (int row = top; row < top + item.roiHeight; ++row)
            for (int column = left; column < left + item.roiWidth; ++column) {
                auto* p = raster.data() + (static_cast<std::size_t>(row) * width + column) * 4;
                p[0] = p[1] = p[2] = static_cast<std::uint8_t>(random() % 256);
                p[3] = static_cast<std::uint8_t>(1 + random() % 255);
            }
        std::vector<MathGroupLayer> layers(static_cast<std::size_t>(item.layers));
        for (int index = 0; index < item.layers; ++index) {
            auto& layer = layers[static_cast<std::size_t>(index)];
            layer.rgba = raster.data();
            layer.width = width;
            layer.height = height;
            layer.rectX = (index % 8) * 3 + item.fraction; // 少しずつずらして重ねる
            layer.rectY = (index % 5) * 2 + item.fraction;
            layer.opacity = 0.9;
            layer.roiX = left;
            layer.roiY = top;
            layer.roiWidth = item.roiWidth == width ? 0 : item.roiWidth;
            layer.roiHeight = item.roiHeight;
        }
        Image destination(static_cast<std::size_t>(width) * height * 4, 255);
        std::string error;
        mvm::math::MathGroupStats stats;
        constexpr int frames = 20;
        const auto begin = std::chrono::steady_clock::now();
        for (int frame = 0; frame < frames; ++frame)
            if (!composeUnchecked(destination.data(), width, height, width * 4U, layers.data(),
                                  layers.size(), error, &stats))
                return 1;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf(
            "%-26s %8.3f ms/frame  %10llu 画素/frame  %6.1f ns/画素\n", item.name,
            seconds * 1e3 / frames, static_cast<unsigned long long>(stats.visitedPixels / frames),
            stats.visitedPixels ? seconds * 1e9 / static_cast<double>(stats.visitedPixels) : 0.0);
    }
    // 下地の検査 (全画素の alpha の走査) と、画像の準備 (複製と字形の範囲の走査) の時間。
    {
        Image destination(static_cast<std::size_t>(width) * height * 4, 255);
        constexpr int rounds = 200;
        bool eligible = true;
        auto begin = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round)
            eligible = mvm::math::mathGroupDestinationEligible(destination.data(), width, height,
                                                               width * 4U) &&
                       eligible;
        double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf("%-26s %8.3f ms/回 (対象=%d)\n", "下地の検査 1920x1080", seconds * 1e3 / rounds,
                    eligible ? 1 : 0);
        Image copy(destination.size());
        begin = std::chrono::steady_clock::now();
        for (int round = 0; round < rounds; ++round)
            std::memcpy(copy.data(), destination.data(), destination.size());
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf("%-26s %8.3f ms/回\n", "合成先の複製 1920x1080", seconds * 1e3 / rounds);
        Image raster(destination.size(), 0);
        for (int row = 530; row < 550; ++row)
            for (int column = 694; column < 1226; ++column)
                raster[(static_cast<std::size_t>(row) * width + column) * 4 + 3] = 200;
        constexpr int prepares = 20;
        std::string error;
        begin = std::chrono::steady_clock::now();
        for (int round = 0; round < prepares; ++round) {
            mvm::math::PreparedMathRaster prepared;
            if (!mvm::math::PreparedMathRaster::prepare(raster.data(), width, height, width * 4U,
                                                        prepared, error))
                return 1;
        }
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf("%-26s %8.3f ms/回 (1 枚 %.1f MB)\n", "画像の準備 1920x1080",
                    seconds * 1e3 / prepares, static_cast<double>(raster.size()) / 1048576.0);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return argc == 2 && std::string(argv[1]) == "bench" ? runBench() : runTests();
}
