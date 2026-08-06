/*
 * mvm Phase 1 / P1 - 色空間・レンジの決定と YUV->RGB 係数
 *
 * decode 側 (FFmpeg の AVColorSpace / AVColorRange) と
 * 表示側 (shader の係数) をつなぐ唯一の場所。
 *
 * **FFmpeg のヘッダを include しない。** 入力は素の int で受ける。
 * こうしておくと、この判断ロジックを単体テストできる
 * (テストに FFmpeg も D3D11 も要らない)。
 * FFmpeg の enum 値と一致していることは color_metadata.cpp 側の
 * static_assert ではなく、decoder 側の変換関数 1 箇所だけで保証する。
 */

#ifndef MVM_GPU_PREVIEW_COLOR_METADATA_H
#define MVM_GPU_PREVIEW_COLOR_METADATA_H

#include "media/gpu_preview/gpu_frame.h"

namespace mvm::gpu {

// FFmpeg の AVColorSpace / AVColorRange と同じ値。
// 名前を変えているのは「ここは FFmpeg に依存していない」ことを明示するため。
namespace avcol {
inline constexpr int kSpcRgb = 0;
inline constexpr int kSpcBt709 = 1;
inline constexpr int kSpcUnspecified = 2;
inline constexpr int kSpcFcc = 4;
inline constexpr int kSpcBt470bg = 5;   // BT.601 625
inline constexpr int kSpcSmpte170m = 6; // BT.601 525
inline constexpr int kSpcSmpte240m = 7;
inline constexpr int kSpcBt2020Ncl = 9;
inline constexpr int kSpcBt2020Cl = 10;

inline constexpr int kRangeUnspecified = 0;
inline constexpr int kRangeMpeg = 1; // limited
inline constexpr int kRangeJpeg = 2; // full
} // namespace avcol

struct ColorDecision {
    ColorSpace space = ColorSpace::Unknown;
    ColorRange range = ColorRange::Unknown;
    bool spaceInferred = false;
    bool rangeInferred = false;
};

// metadata と解像度から色空間・レンジを決める。
//
// 未指定 (AVCOL_SPC_UNSPECIFIED) は実素材にごく普通に現れる。
// 「決められないので Unknown のまま表示する」わけにはいかないので推定するが、
// **推定したことを inferred フラグで残す**。
// 黙って決め打ちにすると、色がおかしいときに
// 「metadata が間違っている」のか「推定が外れた」のかが分からなくなる。
//
// 推定規則:
//   - space: 幅 <= 1024 かつ 高さ <= 576 なら BT.601、それ以外は BT.709
//   - range: limited (H.264 / HEVC の既定)
//   - BT.2020 CL (constant luminance) は shader を持たないので
//     BT.2020 NCL として扱い、inferred を立てる
ColorDecision decideColor(int avColorSpace, int avColorRange, int width, int height);

// shader へ渡す YUV -> RGB 係数。
//
//   R = yScale * (Y - yOffset) + vr * (V - 0.5)
//   G = yScale * (Y - yOffset) - ug * (U - 0.5) - vg * (V - 0.5)
//   B = yScale * (Y - yOffset) + ub * (U - 0.5)
//
// U / V は shader 側で正規化済み (0..1) を前提とし、
// limited range の chroma スケールも uvScale に畳み込む。
struct YuvToRgbCoefficients {
    float yScale = 1.0f;
    float yOffset = 0.0f;
    float uvScale = 1.0f;
    float vr = 0.0f;
    float ug = 0.0f;
    float vg = 0.0f;
    float ub = 0.0f;
};

YuvToRgbCoefficients coefficientsFor(ColorSpace space, ColorRange range);

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_COLOR_METADATA_H
