#ifndef MVM_APP_MATH_CLIP_RENDER_H
#define MVM_APP_MATH_CLIP_RENDER_H

// Project の数式 clip と、backend 中立な描画 (src/media/math) をつなぐ。
// preview (controller) と書き出し (timeline_export) が同じ関数を通し、同じ画素を得る。
// Qt に依存しない。

#include "media/math/math_raster_layout.h"
#include "media/math/math_render.h"
#include "media/still_image/still_image_decoder.h"
#include "project/math_clip.h"

#include <filesystem>
#include <string>

namespace mvm::app {

// 描画結果 (cache artifact) を変える field だけを取り出す。色・背景・ClipEffects・尺は含めない。
math::MathRenderSpec mathRenderSpecFor(const project::MathClipData& data);

// mask (backend の PNG を decode したもの) に数式 clip の色・背景を付け、出力 raster の
// 中央へ置く。色の形式が不正、または mask が出力より大きければ失敗する。
math::MathComposeResult composeMathClipRaster(const media::StillImage& mask,
                                              const project::MathClipData& data, int outputWidth,
                                              int outputHeight);

// cache の PNG を読み、composeMathClipRaster まで行う (書き出しの経路)。
math::MathComposeResult composeMathClipFromPng(const std::filesystem::path& png,
                                               const project::MathClipData& data, int outputWidth,
                                               int outputHeight);

} // namespace mvm::app

#endif // MVM_APP_MATH_CLIP_RENDER_H
