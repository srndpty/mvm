// 数式 renderer の中立な部分: cache key と、mask の出力 raster への配置。
//
// 期待値は実装から作らない。key の golden 値は、header に書いた正準形を
// `printf ... | sha256sum` で別に計算したもの。画素の期待値は手で計算した。

#include "media/math/math_raster_layout.h"
#include "media/math/math_render.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using mvm::math::MathRenderSpec;
using mvm::math::MathToolchainFingerprint;

MathRenderSpec baseSpec() {
    return {"latex", "x^2", 96};
}

MathToolchainFingerprint baseToolchain() {
    return {"manim-mathtex", "backend=manim-mathtex\ntemplate=1\nmanim=M\nlatex=L\ndvisvgm=D\n"};
}

void testKey() {
    // printf 'mvm-math-static/1\nsyntax=5:latex\nsource=3:x^2\nfont_size=96\n
    //   backend=13:manim-mathtex\ntoolchain=59:backend=manim-mathtex\ntemplate=1\nmanim=M\n
    //   latex=L\ndvisvgm=D\n\n' | sha256sum
    const std::string golden = "1451b35693b81e1a4b69677e48f4e487bc94c29c7c19280d473c14015170bf6b";
    const std::string key = mvm::math::mathRenderKey(baseSpec(), baseToolchain());
    check(key == golden, "key が独立に計算した golden 値と一致する: " + key);
    check(mvm::math::mathRenderKey(baseSpec(), baseToolchain()) == key, "key は決定的");

    auto differs = [&](MathRenderSpec spec, MathToolchainFingerprint toolchain,
                       const std::string& what) {
        check(mvm::math::mathRenderKey(spec, toolchain) != key, what + " を変えると key が変わる");
    };
    auto spec = baseSpec();
    spec.syntax = "typst";
    differs(spec, baseToolchain(), "syntax");
    spec = baseSpec();
    spec.source = "x^3";
    differs(spec, baseToolchain(), "source");
    spec = baseSpec();
    spec.source = "x^2 ";
    differs(spec, baseToolchain(), "source の末尾の空白");
    spec = baseSpec();
    spec.fontSize = 97;
    differs(spec, baseToolchain(), "fontSize");
    auto toolchain = baseToolchain();
    toolchain.backendId = "latex-dvisvgm";
    differs(baseSpec(), toolchain, "backend id");
    for (const char* line : {"template=1", "manim=M", "latex=L", "dvisvgm=D"}) {
        toolchain = baseToolchain();
        const auto at = toolchain.canonical.find(line);
        toolchain.canonical.insert(at + std::string(line).size(), "x");
        differs(baseSpec(), toolchain, std::string("toolchain の ") + line);
    }

    // byte 数を前置しているので、field の境界をずらした入力は同じ key にならない。
    const MathRenderSpec shifted{"latex\nsource=3:x^2", "", 96};
    check(mvm::math::mathRenderKey(shifted, baseToolchain()) != key,
          "field の境界をずらした入力は別の key になる");
}

std::vector<std::uint8_t> mask(int width, int height, std::vector<std::uint8_t> alphas) {
    std::vector<std::uint8_t> rgba;
    for (const auto alpha : alphas) {
        // RGB は使われないことを示すため、白ではなく黒にしておく。
        rgba.insert(rgba.end(), {0, 0, 0, alpha});
    }
    (void)width;
    (void)height;
    return rgba;
}

std::vector<std::uint8_t> pixel(const mvm::math::MathComposeResult& result, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(result.width) +
                     static_cast<std::size_t>(x)) *
                    4U;
    return {result.rgba[at], result.rgba[at + 1], result.rgba[at + 2], result.rgba[at + 3]};
}

void testLayout() {
    using mvm::math::composeMathRaster;
    using P = std::vector<std::uint8_t>;

    // 2x1 の mask を 4x3 の中央 (左上 = (1,1)) に置く。glyph は mask の alpha だけで決まる。
    const auto m = mask(2, 1, {255, 0});
    const auto placed = composeMathRaster(m.data(), 2, 1, {0xFF102030u, 0x00000000u}, 4, 3);
    check(placed.success && placed.width == 4 && placed.height == 3 &&
              placed.rgba.size() == 4U * 3U * 4U,
          "出力解像度の RGBA を返す");
    check(pixel(placed, 1, 1) == P{0x10, 0x20, 0x30, 0xFF}, "glyph は中央に、指定した色で置かれる");
    check(pixel(placed, 2, 1) == P{0, 0, 0, 0}, "被覆 0 で背景も透明なら透明");
    check(pixel(placed, 0, 0) == P{0, 0, 0, 0} && pixel(placed, 3, 2) == P{0, 0, 0, 0},
          "mask の外は透明");

    // 背景は mask 全体 (被覆 0 の所も) を塗る。
    const auto background = composeMathRaster(m.data(), 2, 1, {0xFF102030u, 0x80FF0000u}, 4, 3);
    check(pixel(background, 2, 1) == P{0xFF, 0, 0, 0x80}, "被覆 0 の所は背景色そのもの");
    check(pixel(background, 0, 1) == P{0, 0, 0, 0}, "背景は mask の外へ広げない");

    // 被覆 128/255 の青を不透明な緑の上へ: G = 255 * (1 - 128/255) = 127、B = 128。
    const auto half = mask(1, 1, {128});
    const auto blended = composeMathRaster(half.data(), 1, 1, {0xFF0000FFu, 0xFF00FF00u}, 1, 1);
    check(pixel(blended, 0, 0) == P{0, 127, 128, 255},
          "glyph を背景の上へ straight alpha で重ねる");

    // 色の alpha は被覆に掛かる (透明背景の上では alpha がそのまま残り、色は薄めない)。
    const auto full = mask(1, 1, {255});
    const auto translucent = composeMathRaster(full.data(), 1, 1, {0x80FFFFFFu, 0x00000000u}, 1, 1);
    check(pixel(translucent, 0, 0) == P{255, 255, 255, 128}, "色の alpha を保つ (straight alpha)");

    // 奇数の余りは左上へ寄せる: 1x1 を 4x4 へ置くと (1,1)。
    const auto odd = composeMathRaster(full.data(), 1, 1, {0xFFFFFFFFu, 0}, 4, 4);
    check(pixel(odd, 1, 1)[3] == 255 && pixel(odd, 2, 2)[3] == 0, "余りは左上寄せ");

    const auto wide = mask(5, 1, {255, 255, 255, 255, 255});
    const auto oversized = composeMathRaster(wide.data(), 5, 1, {}, 4, 3);
    check(!oversized.success && oversized.rgba.empty() && !oversized.error.empty(),
          "出力より大きい mask は切らずに失敗する");
    check(!composeMathRaster(nullptr, 1, 1, {}, 4, 3).success, "mask が NULL なら失敗する");
    check(!composeMathRaster(full.data(), 1, 1, {}, 0, 3).success, "出力の大きさが 0 なら失敗する");
}

} // namespace

int main() {
    testKey();
    testLayout();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
