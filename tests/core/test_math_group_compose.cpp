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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using mvm::math::MathGroupLayer;
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
    return mvm::math::composeMathGroup(destination.data(), width, height,
                                       static_cast<std::size_t>(width) * 4, input.data(),
                                       input.size(), error);
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
            mvm::math::composeMathGroup(out.data(), width, height, width * 4U, fractional.data(), 1,
                                        error);
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
    check(!mvm::math::composeMathGroup(base.data(), width, height, width * 4U, &bad, 1, error),
          "出力と大きさの違う layer (拡縮) を拒否する");
    bad.rgba = base.data();
    bad.width = width;
    bad.height = height;
    bad.opacity = 1.5;
    check(!mvm::math::composeMathGroup(base.data(), width, height, width * 4U, &bad, 1, error),
          "範囲外の不透明度を拒否する");
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
            if (!mvm::math::composeMathGroup(destination.data(), width, height, width * 4U,
                                             layers.data(), layers.size(), error, &stats))
                return 1;
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf(
            "%-26s %8.3f ms/frame  %10llu 画素/frame  %6.1f ns/画素\n", item.name,
            seconds * 1e3 / frames, static_cast<unsigned long long>(stats.visitedPixels / frames),
            stats.visitedPixels ? seconds * 1e9 / static_cast<double>(stats.visitedPixels) : 0.0);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return argc == 2 && std::string(argv[1]) == "bench" ? runBench() : runTests();
}
