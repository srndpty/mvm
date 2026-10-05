#ifndef MVM_APP_MATH_CLIP_RENDER_H
#define MVM_APP_MATH_CLIP_RENDER_H

// Project の数式 clip と、backend 中立な描画 (src/media/math) をつなぐ。
// preview (controller) と書き出し (timeline_export) が同じ関数を通し、同じ画素を得る。
// Qt に依存しない。

#include "media/math/math_raster_layout.h"
#include "media/math/math_render.h"
#include "media/math/math_transform.h"
#include "media/still_image/still_image_decoder.h"
#include "project/math_clip.h"
#include "project/project.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mvm::app {

// 描画結果 (cache artifact) を変える field だけを取り出す。色・背景・ClipEffects・尺は含めない。
math::MathRenderSpec mathRenderSpecFor(const project::MathClipData& data);

// clip の Write の連番の描画要求。Write が無ければ nullopt。
std::optional<math::MathSequenceSpec> mathSequenceSpecFor(const project::TimelineClip& clip);

// 変形のトランジションの描画要求。端点の式は両 clip を参照して得る (写さない)。
// frame 数はトランジションの区間 (前 + 後) の timeline frame 数。
// 種類が変形でない、clip が数式でない、または clip が transition の参照する ID でなければ nullopt。
// 隣接・Write・ClipEffects の条件は見ない (validateTimelineTransitions が決める)。
std::optional<math::MathTransformSpec>
mathTransformSpecFor(const project::TimelineTransition& transition,
                     const project::TimelineClip& outgoing, const project::TimelineClip& incoming);

// 数式 clip の色・背景 (preview の patch と書き出しの合成が共有する)。形式が不正なら false。
bool mathComposeStyleFor(const project::MathClipData& data, math::MathComposeStyle& style);

// clip の先頭からの timeline frame clipLocalFrame で見せる Write の frame 番号。Write の後
// (と Write の無い clip) は -1 で、静止の描画を見せる。fade と同じ素材 local frame
// (clipFadeSourceFrameAt) で数えるので、置いた fps と timeline の fps が違っても秒で揃う。
// preview と書き出しがこの関数を通し、同じ frame を見せる。換算できなければ nullopt。
std::optional<std::int64_t> mathIntroFrameAt(const project::TimelineClip& clip,
                                             std::int64_t timelineFpsNum,
                                             std::int64_t timelineFpsDen,
                                             std::int64_t clipLocalFrame);

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
