#include "media/gpu_preview/color_metadata.h"

namespace mvm::gpu {
namespace {

// Kr / Kb から係数を組み立てる。式を 3 回書くと、1 つだけ直し忘れる。
YuvToRgbCoefficients build(double kr, double kb, ColorRange range) {
    const double kg = 1.0 - kr - kb;
    YuvToRgbCoefficients c;

    if (range == ColorRange::Full) {
        c.yScale = 1.0f;
        c.yOffset = 0.0f;
        c.uvScale = 1.0f;
    } else {
        // limited: Y は 16..235、chroma は 16..240 (8bit 換算)
        c.yScale = static_cast<float>(255.0 / 219.0);
        c.yOffset = static_cast<float>(16.0 / 255.0);
        c.uvScale = static_cast<float>(255.0 / 224.0);
    }

    c.vr = static_cast<float>(2.0 * (1.0 - kr));
    c.ub = static_cast<float>(2.0 * (1.0 - kb));
    c.ug = static_cast<float>(2.0 * (1.0 - kb) * kb / kg);
    c.vg = static_cast<float>(2.0 * (1.0 - kr) * kr / kg);
    return c;
}

} // namespace

ColorDecision decideColor(int avColorSpace, int avColorRange, int width, int height) {
    ColorDecision d;

    switch (avColorSpace) {
    case avcol::kSpcBt709:
        d.space = ColorSpace::BT709;
        break;
    case avcol::kSpcBt470bg:
    case avcol::kSpcSmpte170m:
    case avcol::kSpcFcc:
    case avcol::kSpcSmpte240m:
        // SMPTE 240M は厳密には BT.601 と係数が違うが、
        // P1 の対象素材には現れない。BT.601 として扱い、推定扱いにする。
        d.space = ColorSpace::BT601;
        d.spaceInferred = (avColorSpace == avcol::kSpcSmpte240m);
        break;
    case avcol::kSpcBt2020Ncl:
        d.space = ColorSpace::BT2020NCL;
        break;
    case avcol::kSpcBt2020Cl:
        // constant luminance 用の shader を持たない。
        // NCL として扱うが、**推定であることを残す**。
        d.space = ColorSpace::BT2020NCL;
        d.spaceInferred = true;
        break;
    default:
        // AVCOL_SPC_UNSPECIFIED / RGB / YCGCO / 未知。
        // 解像度から推定する。SD 相当なら BT.601、それ以外は BT.709。
        d.space = (width <= 1024 && height <= 576) ? ColorSpace::BT601 : ColorSpace::BT709;
        d.spaceInferred = true;
        break;
    }

    switch (avColorRange) {
    case avcol::kRangeMpeg:
        d.range = ColorRange::Limited;
        break;
    case avcol::kRangeJpeg:
        d.range = ColorRange::Full;
        break;
    default:
        // H.264 / HEVC の既定は limited。
        d.range = ColorRange::Limited;
        d.rangeInferred = true;
        break;
    }

    return d;
}

YuvToRgbCoefficients coefficientsFor(ColorSpace space, ColorRange range) {
    switch (space) {
    case ColorSpace::BT601:
        return build(0.299, 0.114, range);
    case ColorSpace::BT2020NCL:
        return build(0.2627, 0.0593, range);
    case ColorSpace::BT709:
    default:
        // Unknown を無変換 (パススルー) にしない。
        // 色が出ない状態より「BT.709 として出す」方が診断しやすく、
        // 実際に Unknown が最終的に残るのは decideColor がバグったときだけである。
        return build(0.2126, 0.0722, range);
    }
}

} // namespace mvm::gpu
