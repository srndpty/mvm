// preview と書き出しが共有する layer の幾何 (letterbox -> crop -> 縦横別の拡縮) を検査する。
// 期待値は手で計算した値であり、実装の式を呼んで作らない。

#include "core/layer_placement.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-9;
}

bool rectIs(const mvm::core::LayerRect& rect, double x, double y, double width, double height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

using mvm::core::LayerRect;
using mvm::core::placeLayer;

void testDefaultIsLetterbox() {
    // 16:9 の素材を 16:9 へ: そのまま全面。
    const auto same = placeLayer(1920, 1080, 1920, 1080, {}, {});
    check(!same.empty && rectIs(same.destination, 0, 0, 1, 1) && rectIs(same.sourceUv, 0, 0, 1, 1),
          "同じ比率の素材が全面に置かれません");
    // 4:3 (1440x1080) を 16:9 へ: 左右に 240px ずつの余白 (aspectFit と同じ)。
    const auto pillar = placeLayer(1440, 1080, 1920, 1080, {}, {});
    check(!pillar.empty && rectIs(pillar.destination, 240.0 / 1920, 0, 1440.0 / 1920, 1) &&
              rectIs(pillar.sourceUv, 0, 0, 1, 1),
          "4:3素材の左右の余白がletterboxと一致しません");
    check(near(pillar.pivotX, 0.5) && near(pillar.pivotY, 0.5),
          "既定の回転中心が中央ではありません");
    // 縦長 (1080x1920) を 16:9 へ: 高さを合わせる。
    const auto portrait = placeLayer(1080, 1920, 1920, 1080, {}, {});
    const double width = (1080.0 * 1080.0 / 1920.0) / 1920.0;
    check(!portrait.empty && rectIs(portrait.destination, (1 - width) / 2, 0, width, 1),
          "縦長素材のletterboxが違います");
}

void testCropOnCanvas() {
    // crop は canvas 座標。4:3 素材の左の余白 (0..0.125) だけを含む crop は、素材の左端から切る。
    const LayerRect crop{0.0, 0.0, 0.5, 1.0};
    const auto placed = placeLayer(1440, 1080, 1920, 1080, crop, crop);
    check(!placed.empty && rectIs(placed.destination, 0.125, 0, 0.375, 1) &&
              rectIs(placed.sourceUv, 0, 0, 0.5, 1),
          "余白を含むcropで見える範囲がcanvas基準になりません");
    // 回転の中心は描く範囲ではなく crop 範囲を置いた矩形の中心。
    check(near(placed.pivotX, 0.25) && near(placed.pivotY, 0.5),
          "回転の中心がcrop範囲の中心になりません");
    // crop 範囲を横 2 倍・縦 0.5 倍に置くと、見える範囲も同じ倍率で写る (比率を保たない)。
    const auto stretched = placeLayer(1440, 1080, 1920, 1080, crop, LayerRect{0.0, 0.25, 1.0, 0.5});
    check(!stretched.empty && rectIs(stretched.destination, 0.25, 0.25, 0.75, 0.5),
          "縦横別の拡縮で見える範囲を引き伸ばしません");
}

void testEmptyAndInvalid() {
    // 余白だけの crop は何も描かない。
    const auto margin = placeLayer(1440, 1080, 1920, 1080, {0.0, 0.0, 0.1, 1.0}, {});
    check(margin.empty, "余白だけのcropで描く範囲が残りました");
    const LayerRect zero{0.0, 0.0, 0.0, 1.0};
    check(placeLayer(0, 1080, 1920, 1080, {}, {}).empty, "幅0の素材を受理しました");
    check(placeLayer(1920, 1080, 1920, -1, {}, {}).empty, "負のcanvasを受理しました");
    check(placeLayer(1920, 1080, 1920, 1080, zero, {}).empty, "幅0のcropを受理しました");
    check(placeLayer(1920, 1080, 1920, 1080, {}, zero).empty, "幅0のdestinationを受理しました");
    const LayerRect nan{std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, 1.0};
    check(placeLayer(1920, 1080, 1920, 1080, nan, {}).empty, "NaNのcropを受理しました");
}

} // namespace

int main() {
    testDefaultIsLetterbox();
    testCropOnCanvas();
    testEmptyAndInvalid();
    if (failures != 0) {
        std::fprintf(stderr, "layer placement: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("layer placement: PASS");
    return 0;
}
