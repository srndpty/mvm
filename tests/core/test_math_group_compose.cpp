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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 既存の試験は、範囲と下地を確かめない入口 (detail) を直接使う: 誤った範囲を渡す対照と、
// 下の alpha が 254 の画素の検査は、検査を通す入口からは作れない。
using MathGroupLayer = mvm::math::detail::UncheckedMathGroupLayer;
constexpr auto composeUnchecked = mvm::math::detail::composeMathGroupUnchecked;
using Image = std::vector<std::uint8_t>;

// int の座標・寸法を size_t へ広げてから掛ける (符号の変換を 1 か所にまとめる)。
std::size_t rgbaBytes(int width, int height) {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}

// 画素 (x, y) の RGBA の先頭 byte の位置。
std::size_t pixelOffset(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x)) *
           4;
}

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
                auto* to = destination.data() + pixelOffset(column, row, width);
                const auto* from = layer.rgba.data() + pixelOffset(sx, sy, width);
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

// 準備済みの画像を、元の画像の座標で全部読み出す (保存していない所は 0)。
Image fullPixels(const mvm::math::PreparedMathRaster& raster) {
    Image image(rgbaBytes(raster.width(), raster.height()));
    for (int row = 0; row < raster.height(); ++row)
        for (int column = 0; column < raster.width(); ++column)
            raster.texel(column, row, image.data() + pixelOffset(column, row, raster.width()));
    return image;
}

long long differing(const Image& a, const Image& b) {
    long long count = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        count += a[i] != b[i];
    return count;
}

// 中央の 1/2 に、透明・半透明・不透明の texel が混ざった画像。外側は完全に透明 (0,0,0,0)。
Image glyph(std::mt19937& random, int width, int height) {
    Image image(rgbaBytes(width, height), 0);
    for (int row = height / 4; row < height / 4 + height / 2; ++row)
        for (int column = width / 4; column < width / 4 + width / 2; ++column) {
            auto* p = image.data() + pixelOffset(column, row, width);
            const int kind = static_cast<int>(random() % 4);
            p[3] = kind == 0 ? 0 : kind == 1 ? 255 : static_cast<std::uint8_t>(random() % 256);
            for (int c = 0; c < 3; ++c)
                p[c] = p[3] == 0 ? 0 : static_cast<std::uint8_t>(random() % 256);
        }
    return image;
}

// 同じ準備済みの画像 (変えられない) を、複数の thread が同時に読んで別々の合成先へ合成する。
// 置き場 (pool) の画像を使い、thread ごとに位置と不透明度を変える。結果は、同じ入力を 1 thread で
// 合成したものと全 byte 同じでなければならない。違った合成の数を返す。
// race の検出器 (ThreadSanitizer) を付けて実行する対象でもある。
int runConcurrentCompose(int threads, int rounds) {
    constexpr int width = 96, height = 64;
    std::mt19937 engine(3305);
    mvm::math::PreparedMathRasterPool pool;
    std::vector<const mvm::math::PreparedMathRaster*> rasters;
    for (int index = 0; index < 6; ++index) {
        Image image(rgbaBytes(width, height), 0);
        for (int row = 8 + index; row < 40 + index; ++row)
            for (int column = 10 + index * 3; column < 70 + index * 2; ++column) {
                auto* p = image.data() + pixelOffset(column, row, width);
                p[3] = static_cast<std::uint8_t>(engine() % 256);
                p[0] = p[3] ? 204 : 0;
                p[1] = p[3] ? 34 : 0;
                p[2] = p[3] ? static_cast<std::uint8_t>(engine() % 256) : 0;
            }
        mvm::math::PreparedMathRaster raster;
        std::string error;
        if (!mvm::math::PreparedMathRaster::prepare(image.data(), width, height, width * 4U, raster,
                                                    error, mvm::math::MathRasterStorage::RoiOnly,
                                                    "artifact=" + std::to_string(index)))
            return -1;
        rasters.push_back(pool.intern(std::move(raster)));
    }
    Image base(rgbaBytes(width, height));
    for (std::size_t i = 0; i < base.size(); ++i)
        base[i] = i % 4 == 3 ? static_cast<std::uint8_t>(1 + engine() % 255)
                             : static_cast<std::uint8_t>(engine() % 256);
    const auto compose = [&](int worker, int round, Image& out) {
        std::vector<mvm::math::MathGroupPlacement> placements;
        for (std::size_t index = 0; index < rasters.size(); ++index)
            placements.push_back(
                {rasters[index],
                 (worker * 7 + round * 3 + static_cast<int>(index)) % 23 - 11 + 0.36,
                 (worker * 5 + round) % 17 - 8 + 0.5 * (round % 2),
                 ((worker + round + static_cast<int>(index)) % 10 + 1) / 10.0});
        out = base;
        std::string error;
        return mvm::math::composeMathGroup(out.data(), width, height, width * 4U, placements.data(),
                                           placements.size(), error);
    };
    // 基準: 1 thread で先に全部を合成する。
    const auto slot = [rounds](int worker, int round) {
        return static_cast<std::size_t>(worker) * static_cast<std::size_t>(rounds) +
               static_cast<std::size_t>(round);
    };
    std::vector<Image> expectedOutputs(slot(threads, 0));
    for (int worker = 0; worker < threads; ++worker)
        for (int round = 0; round < rounds; ++round)
            if (!compose(worker, round, expectedOutputs[slot(worker, round)]))
                return -1;
    std::atomic<int> wrong{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < threads; ++worker)
        workers.emplace_back([&, worker] {
            Image out;
            for (int round = 0; round < rounds; ++round)
                if (!compose(worker, round, out) || out != expectedOutputs[slot(worker, round)])
                    ++wrong;
        });
    for (auto& thread : workers)
        thread.join();
    return wrong.load();
}

// 書き出しの前の判定 (P3-3D) が使う性質: MLT の合成後の alpha の式
//   alpha = (uint8)(255 * ((上 + 下) - 上 * 下))、下 = d / 255 (binary32)
// は、下の alpha d が 1 以上なら 1 以上の値を返す (alpha 1 以上の画素は、何を重ねても alpha 0 に
// ならない)。上 (0〜1 の binary32) の全部の値について d = 1 を確かめ、d = 1〜255 については
// 「d より小さくならない」ことを、上の値を間引いて確かめる。下の求め方は、1/255 を掛ける形と
// 255 で割る形の両方を見る (MLT の補間の関数によって違いうるため)。
int runAlphaFloorLemma() {
    const auto f32 = [](float value) {
        volatile float settled = value;
        return static_cast<float>(settled);
    };
    const float reciprocal = f32(1.0f / 255.0f);
    const auto out = [&](float above, float below) {
        const float total = f32(f32(above + below) - f32(above * below));
        return static_cast<int>(static_cast<std::uint8_t>(f32(255.0f * total)));
    };
    long long checked = 0, violations = 0, lowered = 0;
    int worstDrop = 0, lowestOut = 255, lowestDropped = 256;
    const float belows[2] = {f32(1.0f * reciprocal), f32(1.0f / 255.0f)};
    // d = 1: 上の全部の値 (0.0 から 1.0 までの binary32、1,065,353,217 個)。
    std::uint32_t bits = 0;
    for (;; ++bits) {
        float above;
        std::memcpy(&above, &bits, 4);
        for (const float below : belows)
            violations += out(above, below) < 1;
        checked += 2;
        if (above == 1.0f)
            break;
    }
    // d = 1〜255: 上の値を間引いて (4099 個おき)、alpha が 1 より小さくならないこと。
    // alpha そのものは下がりうる (255 の上へ不透明な画素を重ねると 254 になる、など) ので、
    // 下がった数と、下がった幅の最大も数える。
    for (int d = 1; d <= 255; ++d) {
        const float multiplied = f32(static_cast<float>(d) * reciprocal);
        const float divided = f32(static_cast<float>(d) / 255.0f);
        const auto see = [&](float above, float below) {
            const int value = out(above, below);
            violations += value < 1;
            lowered += value < d;
            if (value < d)
                lowestDropped = std::min(lowestDropped, d);
            worstDrop = std::max(worstDrop, d - value);
            lowestOut = std::min(lowestOut, value);
            ++checked;
        };
        for (std::uint32_t sample = 0; sample <= 0x3f800000U; sample += 4099U) {
            float above;
            std::memcpy(&above, &sample, 4);
            see(above, multiplied);
            see(above, divided);
        }
        see(1.0f, multiplied);
        see(1.0f, divided);
    }
    std::printf("alpha が下がった数 %lld (下がった幅の最大 %d、下がった中で最小の下の alpha %d)、"
                "合成後の alpha の最小 %d\n",
                lowered, worstDrop, lowestDropped, lowestOut);
    std::printf("alpha の下限の性質: %lld 通りを確かめ、反例 %lld 個\n", checked, violations);
    return violations == 0 ? 0 : 1;
}

int runTests() {
    std::printf("alpha の書き直しは恒等: %s\n",
                mvm::math::mathGroupAlphaRoundTripIsIdentity() ? "はい" : "いいえ");
    constexpr int width = 64, height = 48;
    std::mt19937 random(20261012);
    long long compared = 0;
    for (int round = 0; round < 40; ++round) {
        // 下地は不透明 (alpha 255)。MLT の書き出しの最下層と同じ条件。
        Image base(rgbaBytes(width, height));
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
                auto* p = bottom.rgba.data() + pixelOffset(right, row, width);
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
    Image rejectTarget(rgbaBytes(width, height), 255);
    Image small(16 * 16 * 4, 255);
    MathGroupLayer wrongLayer;
    wrongLayer.rgba = small.data();
    wrongLayer.width = 16;
    wrongLayer.height = 16;
    std::string error;
    check(!composeUnchecked(rejectTarget.data(), width, height, width * 4U, &wrongLayer, 1, error,
                            nullptr),
          "出力と大きさの違う layer (拡縮) を拒否する");
    wrongLayer.rgba = rejectTarget.data();
    wrongLayer.width = width;
    wrongLayer.height = height;
    wrongLayer.opacity = 1.5;
    check(!composeUnchecked(rejectTarget.data(), width, height, width * 4U, &wrongLayer, 1, error,
                            nullptr),
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
                Image destination(rgbaBytes(sweepWidth, rows));
                Image raster(destination.size());
                for (int row = 0; row < rows; ++row)
                    for (int column = 0; column < sweepWidth; ++column) {
                        const auto at = pixelOffset(column, row, sweepWidth);
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
        Image base(rgbaBytes(width, height));
        for (std::size_t i = 0; i < base.size(); ++i)
            base[i] = i % 4 == 3 ? 255 : static_cast<std::uint8_t>(localRandom() % 256);
        Image source = glyph(localRandom, width, height);
        // 範囲の四隅を必ず不透明にする (範囲が画素から正しく求まることを確かめる)。
        const int left = width / 4, top = height / 4, right = left + width / 2 - 1,
                  bottom = top + height / 2 - 1;
        for (const auto& corner : {std::pair{left, top}, std::pair{right, bottom}})
            source[pixelOffset(corner.first, corner.second, width) + 3] = 255;
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
                  again == want && fullPixels(raster) == scribbled,
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
            transparent[pixelOffset(width - 1, height - 1, width) + 3] =
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
            const Image rasterPixels = fullPixels(raster);
            sameRaster.rgba = rasterPixels.data();
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
        const Image stackPixels = fullPixels(raster);
        for (const auto& item : stack) {
            MathGroupLayer one;
            one.rgba = stackPixels.data();
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

    // --- P3-3C: 中身のある範囲だけを持つ準備済みの画像 ---------------------------------------
    // 基準は 2 つ: (1) 全画素を持つ準備済みの画像 (P3-3B までの形)、(2) 元の画素を全部走査する、
    // 検査を通さない入口。範囲だけを持つ画像の合成が、どちらとも全 byte 一致すること。
    {
        using mvm::math::MathRasterStorage;
        using mvm::math::PreparedMathRaster;
        const long long liveBefore = mvm::math::preparedMathRasterLiveBytes();
        long long roiCompared = 0;

        struct Place {
            double x, y, opacity;
        };

        // 下地: 色は乱数、alpha は指定の値 (1・254・255 と、混ぜたもの)。
        const auto makeBase = [&](std::mt19937& engine, int alphaKind) {
            Image image(rgbaBytes(width, height));
            for (std::size_t i = 0; i < image.size(); ++i)
                image[i] = static_cast<std::uint8_t>(engine() % 256);
            for (std::size_t at = 3; at < image.size(); at += 4)
                image[at] = alphaKind == 0   ? 255
                            : alphaKind == 1 ? 254
                            : alphaKind == 2 ? 1
                                             : static_cast<std::uint8_t>(1 + engine() % 255);
            return image;
        };
        // sources[i] を places[i] へ置いて下から重ねる。3 通りの結果が同じことを確かめ、範囲だけを
        // 持つ画像の結果を返す。
        const auto sameAsReferences = [&](const std::vector<Image>& sources,
                                          const std::vector<Place>& places, const Image& baseImage,
                                          const char* message) {
            std::vector<PreparedMathRaster> roi(sources.size()), full(sources.size());
            std::vector<mvm::math::MathGroupPlacement> roiPlaces, fullPlaces;
            std::vector<MathGroupLayer> wholeLayers;
            std::string localError;
            bool prepared = true;
            for (std::size_t index = 0; index < sources.size(); ++index) {
                prepared = PreparedMathRaster::prepare(sources[index].data(), width, height,
                                                       width * 4U, roi[index], localError,
                                                       MathRasterStorage::RoiOnly) &&
                           PreparedMathRaster::prepare(sources[index].data(), width, height,
                                                       width * 4U, full[index], localError,
                                                       MathRasterStorage::FullFrame) &&
                           prepared;
                roiPlaces.push_back(
                    {&roi[index], places[index].x, places[index].y, places[index].opacity});
                fullPlaces.push_back(
                    {&full[index], places[index].x, places[index].y, places[index].opacity});
                MathGroupLayer layer; // 全画素・範囲の指定なし (全体を走査する)
                layer.rgba = sources[index].data();
                layer.width = width;
                layer.height = height;
                layer.rectX = places[index].x;
                layer.rectY = places[index].y;
                layer.opacity = places[index].opacity;
                wholeLayers.push_back(layer);
                // 持ち方が違っても、元の画像として読める内容は同じ。
                prepared = prepared &&
                           roi[index].contentChecksum() == full[index].contentChecksum() &&
                           roi[index].roiX() == full[index].roiX() &&
                           roi[index].roiWidth() == full[index].roiWidth() &&
                           roi[index].roiY() == full[index].roiY() &&
                           roi[index].roiHeight() == full[index].roiHeight() &&
                           roi[index].storedBytes() ==
                               static_cast<std::size_t>(roi[index].storedWidth()) *
                                   static_cast<std::size_t>(roi[index].storedHeight()) * 4U;
            }
            Image fromRoi = baseImage, fromFull = baseImage, fromWhole = baseImage;
            const bool composed =
                prepared &&
                mvm::math::composeMathGroup(fromRoi.data(), width, height, width * 4U,
                                            roiPlaces.data(), roiPlaces.size(), localError) &&
                mvm::math::composeMathGroup(fromFull.data(), width, height, width * 4U,
                                            fullPlaces.data(), fullPlaces.size(), localError) &&
                composeUnchecked(fromWhole.data(), width, height, width * 4U, wholeLayers.data(),
                                 wholeLayers.size(), localError, nullptr);
            if (!composed || fromRoi != fromFull || fromRoi != fromWhole)
                std::fprintf(stderr,
                             "%s: 準備・合成 %d、全画面との差 %lld byte、全走査との差 %lld byte\n",
                             message, composed ? 1 : 0, differing(fromRoi, fromFull),
                             differing(fromRoi, fromWhole));
            check(composed && fromRoi == fromFull && fromRoi == fromWhole, message);
            roiCompared += static_cast<long long>(fromRoi.size());
            return fromRoi;
        };
        const auto blank = [&]() { return Image(rgbaBytes(width, height), 0); };
        const auto put = [&](Image& image, int x, int y, std::uint8_t r, std::uint8_t g,
                             std::uint8_t b, std::uint8_t a) {
            auto* p = image.data() + pixelOffset(x, y, width);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        };
        std::mt19937 engine(3303);
        // 位置: 整数、端数、大きな正負 (ほとんど外・完全に外)。
        const std::vector<Place> positions = {{0.0, 0.0, 1.0},
                                              {0.36, -0.5, 0.75},
                                              {-7.25, 5.75, 0.12049074074074075},
                                              {width - 3.5, 2.25, 1.0},
                                              {-(width - 3.5), -2.25, 0.5},
                                              {3.0, height - 2.5, 0.9},
                                              {1.5, -(height - 2.5), 0.3333},
                                              {width + 5.0, 0.0, 1.0},
                                              {-(width + 5.0), 0.5, 1.0},
                                              {0.5, height + 4.0, 1.0},
                                              {0.999, 0.001, 0.86207407407407421}};
        for (int alphaKind = 0; alphaKind < 4; ++alphaKind) {
            const Image localBase = makeBase(engine, alphaKind);
            // 中央の字形 (透明・半透明・不透明が混ざる)。
            const Image center = glyph(engine, width, height);
            // 画像の端に掛かる字形 (切れた字形): 左上の角と右下の角。
            Image clipped = blank();
            for (int row = 0; row < 6; ++row)
                for (int column = 0; column < 9; ++column) {
                    put(clipped, column, row, 204, 34, 34,
                        static_cast<std::uint8_t>(40 + (column * 23 + row * 7) % 216));
                    put(clipped, width - 1 - column, height - 1 - row, 10, 200, 90,
                        static_cast<std::uint8_t>(255 - (column * 11 + row * 29) % 200));
                }
            // 幅 1 画素の字形、1 texel だけの字形、全部 0 の画像。
            Image thin = blank(), single = blank();
            for (int row = 9; row < 30; ++row)
                put(thin, 21, row, 255, 255, 255, static_cast<std::uint8_t>(row * 8));
            put(single, 10, 10, 200, 40, 40, 255);
            const Image empty = blank();
            // alpha が 0 で色が 0 でない texel が字形の外にある画像。MLT は色を alpha
            // と独立に補間する ので、こういう texel は縁の画素の色に効く。捨ててはならない。
            Image tinted = blank();
            for (int row = 14; row < 22; ++row)
                for (int column = 18; column < 40; ++column)
                    put(tinted, column, row, 204, 34, 34,
                        column >= 24 && column < 34 && row >= 16 && row < 20 ? 180 : 0);
            // qtcrop の描き直しの後のような、色の付いた半透明の字形 (色が alpha
            // に応じて落ちている)。
            Image colored = blank();
            for (int row = 12; row < 26; ++row)
                for (int column = 8; column < 50; ++column) {
                    const int alpha = (column * 37 + row * 91) % 256;
                    put(colored, column, row, static_cast<std::uint8_t>(204 * alpha / 255),
                        static_cast<std::uint8_t>(34 * alpha / 255),
                        static_cast<std::uint8_t>(34 * alpha / 255),
                        static_cast<std::uint8_t>(alpha));
                }
            const std::pair<const Image*, const char*> images[] = {
                {&center, "中央の字形: 範囲だけを持つ画像の合成は基準と同じ"},
                {&clipped, "画像の端に掛かる字形: 範囲だけを持つ画像の合成は基準と同じ"},
                {&thin, "幅 1 画素の字形: 範囲だけを持つ画像の合成は基準と同じ"},
                {&single, "1 texel の字形: 範囲だけを持つ画像の合成は基準と同じ"},
                {&empty, "全部 0 の画像: 範囲だけを持つ画像の合成は基準と同じ"},
                {&tinted, "alpha 0 で色のある texel: 範囲だけを持つ画像の合成は基準と同じ"},
                {&colored, "色の付いた半透明の字形: 範囲だけを持つ画像の合成は基準と同じ"}};
            for (const auto& [image, message] : images)
                for (const auto& position : positions)
                    sameAsReferences({*image}, {position}, localBase, message);
            // 重なる複数の layer (不透明度は fade の途中の値を含む)。
            sameAsReferences({center, colored, clipped, tinted, thin, center},
                             {{0.36, 0.0, 0.25925925925925924},
                              {2.75, -1.5, 0.54992592592592593},
                              {-1.25, 0.5, 1.0},
                              {4.5, 3.25, 0.95140740740740748},
                              {-3.0, 0.75, 0.13792592592592592},
                              {6.36, 2.0, 0.0}},
                             localBase, "重なる複数の layer: 範囲だけを持つ画像の合成は基準と同じ");
        }
        // 範囲の縁の補間: 不透明な 1 texel を縦横に半画素ずらすと、ちょうど 2 x 2 画素が変わる
        // (範囲の外へ 1 画素はみ出す分を含む)。範囲の外の texel を透明として読めていること。
        {
            Image single = blank();
            put(single, 10, 10, 200, 40, 40, 255);
            const Image localBase = makeBase(engine, 0);
            const Image out =
                sameAsReferences({single}, {{0.5, 0.5, 1.0}}, localBase,
                                 "範囲の縁の補間: 範囲だけを持つ画像の合成は基準と同じ");
            long long changed = 0;
            bool inside = true;
            for (int row = 0; row < height; ++row)
                for (int column = 0; column < width; ++column) {
                    const auto at = pixelOffset(column, row, width);
                    if (std::memcmp(out.data() + at, localBase.data() + at, 4) == 0)
                        continue;
                    ++changed;
                    inside = inside && column >= 10 && column <= 11 && row >= 10 && row <= 11;
                }
            check(changed == 4 && inside, "範囲の縁の 1 画素外まで補間で描かれる (2 x 2 画素)");
        }
        // 持ち方: 範囲だけを持つ画像は範囲の画素しか持たず、元の画像として読むと同じ内容になる。
        {
            Image tinted = blank();
            for (int row = 14; row < 22; ++row)
                for (int column = 18; column < 40; ++column)
                    put(tinted, column, row, 204, 34, 34,
                        column >= 24 && column < 34 && row >= 16 && row < 20 ? 180 : 0);
            PreparedMathRaster roi, full, none;
            std::string localError;
            check(PreparedMathRaster::prepare(tinted.data(), width, height, width * 4U, roi,
                                              localError, MathRasterStorage::RoiOnly,
                                              "artifact=test;preprocess=none") &&
                      PreparedMathRaster::prepare(tinted.data(), width, height, width * 4U, full,
                                                  localError, MathRasterStorage::FullFrame),
                  "2 通りの持ち方で準備できる");
            check(roi.width() == width && roi.height() == height && roi.storedX() == 18 &&
                      roi.storedY() == 14 && roi.storedWidth() == 22 && roi.storedHeight() == 8 &&
                      roi.storedBytes() == 22U * 8U * 4U,
                  "保存する範囲は、どれかの byte が 0 でない texel の外接矩形");
            check(roi.roiX() == 24 && roi.roiY() == 16 && roi.roiWidth() == 10 &&
                      roi.roiHeight() == 4,
                  "字形の範囲 (alpha が 0 でない texel) は保存する範囲の中にある");
            check(full.storedWidth() == width && full.storedHeight() == height &&
                      full.storedBytes() == tinted.size(),
                  "全画面の持ち方は全画素を持つ");
            bool same = true;
            for (int row = -1; row <= height; ++row)
                for (int column = -1; column <= width; ++column) {
                    std::uint8_t fromRoi[4], fromFull[4], want[4] = {0, 0, 0, 0};
                    roi.texel(column, row, fromRoi);
                    full.texel(column, row, fromFull);
                    if (column >= 0 && row >= 0 && column < width && row < height)
                        std::memcpy(want, tinted.data() + pixelOffset(column, row, width), 4);
                    same = same && std::memcmp(fromRoi, want, 4) == 0 &&
                           std::memcmp(fromFull, want, 4) == 0;
                }
            check(same, "元の画像の座標で読むと、どちらの持ち方も元の画素と同じ (外は 0)");
            check(roi.provenance() == "artifact=test;preprocess=none", "出どころの記録を保つ");
            Image otherImage = tinted;
            put(otherImage, 0, 0, 1, 0, 0, 0);
            PreparedMathRaster other;
            check(PreparedMathRaster::prepare(otherImage.data(), width, height, width * 4U, other,
                                              localError) &&
                      other.contentChecksum() != roi.contentChecksum() && other.storedX() == 0 &&
                      other.storedY() == 0,
                  "1 byte でも違う画像は照合用の値が違い、保存する範囲に入る");
            const Image empty = blank();
            check(PreparedMathRaster::prepare(empty.data(), width, height, width * 4U, none,
                                              localError) &&
                      none.valid() && none.storedBytes() == 0 && none.storedRgba() == nullptr &&
                      none.roiWidth() == 0,
                  "全部 0 の画像は画素を持たない");
            // 複製は同じ画素を共有し、生きている byte 数は増えない。
            const long long live = mvm::math::preparedMathRasterLiveBytes();
            {
                const PreparedMathRaster copy = roi;
                check(copy.storedRgba() == roi.storedRgba() &&
                          mvm::math::preparedMathRasterLiveBytes() == live,
                      "複製は同じ画素を共有する");
            }
            check(live - liveBefore ==
                      static_cast<long long>(roi.storedBytes() + full.storedBytes() +
                                             other.storedBytes()),
                  "生きている画素の byte 数を数える");
        }
        check(mvm::math::preparedMathRasterLiveBytes() == liveBefore,
              "準備済みの画像を解放すると、生きている画素は元の数に戻る");
        // 検査を通さない入口: 字形の範囲が保存した範囲からはみ出す指定は拒否する。
        {
            Image some = blank();
            put(some, 5, 5, 1, 2, 3, 255);
            MathGroupLayer bad;
            bad.rgba = some.data();
            bad.width = width;
            bad.height = height;
            bad.storedRegion = true;
            bad.storedX = 5;
            bad.storedY = 5;
            bad.storedWidth = 1;
            bad.storedHeight = 1;
            bad.roiX = 4;
            bad.roiY = 5;
            bad.roiWidth = 2;
            bad.roiHeight = 1;
            Image untouched = makeBase(engine, 0);
            const Image before = untouched;
            std::string localError;
            check(!composeUnchecked(untouched.data(), width, height, width * 4U, &bad, 1,
                                    localError, nullptr) &&
                      untouched == before,
                  "字形の範囲が保存した範囲からはみ出す layer を拒否する");
        }
        std::printf("範囲だけを持つ画像と基準を比べた byte: %lld\n", roiCompared);
        compared += roiCompared;
    }

    // --- P3-3D: 準備済みの画像の共有 (出どころと内容の両方が同じときだけ) -----------------
    {
        using mvm::math::PreparedMathRaster;
        const long long liveBefore = mvm::math::preparedMathRasterLiveBytes();
        std::mt19937 engine(3304);
        const Image first = glyph(engine, width, height);
        Image changed = first; // 同じ path の成果物が書き換わった場合: 字形の中の 1 byte だけが違う
        changed[pixelOffset(width / 2, height / 2, width) + 1] ^= 1;
        const auto make = [&](const Image& image, const char* provenance) {
            PreparedMathRaster raster;
            std::string localError;
            check(PreparedMathRaster::prepare(image.data(), width, height, width * 4U, raster,
                                              localError, mvm::math::MathRasterStorage::RoiOnly,
                                              provenance),
                  "置き場へ入れる画像を準備できる");
            return raster;
        };
        {
            mvm::math::PreparedMathRasterPool pool;
            const auto* a = pool.intern(make(first, "artifact=a.png;preprocess=none"));
            const auto* again = pool.intern(make(first, "artifact=a.png;preprocess=none"));
            check(a && again == a && pool.size() == 1 && pool.sharedCount() == 1 &&
                      pool.conflictCount() == 0,
                  "出どころも内容も同じ画像は共有する");
            const auto* rewritten = pool.intern(make(changed, "artifact=a.png;preprocess=none"));
            std::uint8_t fromFirst[4], fromChanged[4];
            a->texel(width / 2, height / 2, fromFirst);
            rewritten->texel(width / 2, height / 2, fromChanged);
            check(rewritten && rewritten != a && pool.size() == 2 && pool.conflictCount() == 1 &&
                      std::memcmp(fromFirst, fromChanged, 4) != 0 && !a->sameContent(*rewritten),
                  "出どころが同じでも内容が違う画像は共有しない (同じ path の成果物の書き換え)");
            const auto* other = pool.intern(make(first, "artifact=b.png;preprocess=none"));
            check(other && other != a && pool.size() == 3 && a->sameContent(*other),
                  "内容が同じでも出どころが違う画像は共有しない");
            const auto* repainted = pool.intern(make(first, "artifact=a.png;preprocess=qtcrop"));
            check(repainted && repainted != a && pool.size() == 4, "前処理が違う画像は共有しない");
            const auto* afterConflict =
                pool.intern(make(changed, "artifact=a.png;preprocess=none"));
            check(afterConflict == rewritten && pool.size() == 4 && pool.sharedCount() == 2,
                  "内容が違った後も、それぞれの内容ごとに共有する");
            check(pool.intern(PreparedMathRaster()) == nullptr && pool.size() == 4,
                  "準備していない画像は置き場に入らない");
            check(pool.storedBytes() == a->storedBytes() + rewritten->storedBytes() +
                                            other->storedBytes() + repainted->storedBytes() &&
                      mvm::math::preparedMathRasterLiveBytes() - liveBefore ==
                          static_cast<long long>(pool.storedBytes()),
                  "置き場が持つ画素の byte 数を数える (捨てた画像の分は残らない)");
        }
        check(mvm::math::preparedMathRasterLiveBytes() == liveBefore,
              "置き場を解放すると、生きている画素は元の数に戻る");
    }

    // --- P3-3D: 変えられない画像を、複数の thread から同時に読んで合成する -----------------
    check(runConcurrentCompose(8, 40) == 0, "複数の thread の同時の合成は、1 thread の結果と同じ");
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
        Image raster(rgbaBytes(width, height), 0);
        const int left = (width - item.roiWidth) / 2, top = (height - item.roiHeight) / 2;
        for (int row = top; row < top + item.roiHeight; ++row)
            for (int column = left; column < left + item.roiWidth; ++column) {
                auto* p = raster.data() + pixelOffset(column, row, width);
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
        Image destination(rgbaBytes(width, height), 255);
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
        Image destination(rgbaBytes(width, height), 255);
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
                raster[pixelOffset(column, row, width) + 3] = 200;
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
        // P3-3C: 持ち方ごとの準備と合成 (40 層、字形 532x20、端数位置)。
        for (const auto storage :
             {mvm::math::MathRasterStorage::FullFrame, mvm::math::MathRasterStorage::RoiOnly}) {
            const char* name =
                storage == mvm::math::MathRasterStorage::FullFrame ? "全画面" : "範囲だけ";
            mvm::math::PreparedMathRaster prepared;
            begin = std::chrono::steady_clock::now();
            for (int round = 0; round < prepares; ++round)
                if (!mvm::math::PreparedMathRaster::prepare(raster.data(), width, height,
                                                            width * 4U, prepared, error, storage))
                    return 1;
            const double prepareMs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() *
                1e3 / prepares;
            std::vector<mvm::math::MathGroupPlacement> placements(40);
            for (int index = 0; index < 40; ++index)
                placements[static_cast<std::size_t>(index)] = {&prepared, (index % 8) * 3 + 0.36,
                                                               (index % 5) * 2 + 0.36, 0.9};
            constexpr int frames = 20;
            mvm::math::MathGroupStats stats;
            begin = std::chrono::steady_clock::now();
            for (int frame = 0; frame < frames; ++frame)
                if (!mvm::math::composeMathGroup(destination.data(), width, height, width * 4U,
                                                 placements.data(), placements.size(), error,
                                                 &stats))
                    return 1;
            const double composeMs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() *
                1e3 / frames;
            std::printf("%-10s 準備 %7.3f ms/枚、保存 %9zu byte、40 層の合成 %7.3f ms/frame "
                        "(うち素材座標の表 %6.3f ms、下地の検査を含む)、%llu 画素/frame\n",
                        name, prepareMs, prepared.storedBytes(), composeMs,
                        static_cast<double>(stats.coordinateNs) / 1e6 / frames,
                        static_cast<unsigned long long>(stats.visitedPixels / frames));
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc == 2 ? argv[1] : "";
    if (mode == "bench")
        return runBench();
    if (mode == "alpha-floor")
        return runAlphaFloorLemma();
    if (mode == "race-control") {
        // race の検出器そのものの対照: 2 つの thread が同じ合成先へ同時に書く (意図した data
        // race)。 検出器を付けて実行すると報告されなければならない。通常の試験では実行しない。
        constexpr int width = 64, height = 48;
        Image source(rgbaBytes(width, height), 200);
        mvm::math::PreparedMathRaster raster;
        std::string error;
        if (!mvm::math::PreparedMathRaster::prepare(source.data(), width, height, width * 4U,
                                                    raster, error))
            return 2;
        Image shared(source.size(), 255);
        const auto work = [&] {
            const mvm::math::MathGroupPlacement placement{&raster, 0.5, 0.5, 0.5};
            std::string localError;
            for (int round = 0; round < 50; ++round)
                mvm::math::composeMathGroup(shared.data(), width, height, width * 4U, &placement, 1,
                                            localError);
        };
        std::thread first(work), second(work);
        first.join();
        second.join();
        return 0;
    }
    if (mode == "concurrent") {
        // race の検出器の下で実行するための入口 (同時の合成だけを、回数を増やして行う)。
        const int wrong = runConcurrentCompose(8, 200);
        std::printf("同時の合成: 基準と違った数 %d\n", wrong);
        return wrong == 0 ? 0 : 1;
    }
    return runTests();
}
