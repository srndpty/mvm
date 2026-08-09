/*
 * Phase 4 の canonical な state / layout / schedule を一箇所に固定する値層。
 *
 * runtime (controller / render path) と test の両方がここだけを参照する。
 * mapping を二重定義すると、実装と test が同じ誤りを共有して
 * 「片方だけ通る」ではなく「両方とも間違ったまま通る」形で表面化する。
 *
 * 依存は gpu_preview の値型だけ。Qt / QML / audio / MLT へは依存しない。
 * generic な CompositorCoordinator と GpuCompositor は S0～S3 を知らない。
 */
#ifndef MVM_GPU_PREVIEW_PHASE4_COMPOSITION_CATALOG_H
#define MVM_GPU_PREVIEW_PHASE4_COMPOSITION_CATALOG_H

#include "media/gpu_preview/composition_schedule.h"
#include "media/gpu_preview/compositor_coordinator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mvm::gpu {

// docs/phase4-plan.md §3.1 で freeze 済みの state id。値そのものは opaque だが、
// symbolic name との対応はここでだけ定義する。
inline constexpr CompositionStateId kPhase4S0{1};
inline constexpr CompositionStateId kPhase4S1{2};
inline constexpr CompositionStateId kPhase4S2{3};
inline constexpr CompositionStateId kPhase4S3{4};

// Phase 3 integrated path と同じ source id。A が H264/AAC、B が HEVC。
inline constexpr SourceId kPhase4SourceA{1};
inline constexpr SourceId kPhase4SourceB{2};

enum class Phase4ScheduleKind { Smoke, Formal };

const char* phase4ScheduleKindName(Phase4ScheduleKind kind);

// 未知の state では nullptr を返す。既定値へ縮退させない。
const char* phase4StateName(CompositionStateId state);

// 未知の name では invalid な CompositionStateId{} を返す。
CompositionStateId phase4StateFromName(std::string_view name);

// 未知の state では空 vector を返す。呼び出し側が空を弾く。
std::vector<LayerLayout> phase4CanonicalLayout(CompositionStateId state);

std::vector<CompositionScheduleEntry> phase4ScheduleEntries(Phase4ScheduleKind kind);

// entries を CompositionSchedule::create で検証して返す。
std::optional<CompositionSchedule> phase4Schedule(Phase4ScheduleKind kind);

// entries から組み立てる。文字列と entries が独立に書かれて食い違うことを防ぐ。
// 末尾改行も末尾 separator も付けない。
std::string phase4CanonicalScheduleString(Phase4ScheduleKind kind);

// docs/phase4-plan.md §3.2 / §3.8 の freeze 済み SHA-256 (lowercase hex)。
// 実行時に canonical string から再計算した値と照合するための期待値であり、
// これ自体を hash の出力として raw へ書かない。
const char* phase4ExpectedScheduleSha256(Phase4ScheduleKind kind);

} // namespace mvm::gpu

#endif
