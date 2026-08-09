#include "media/gpu_preview/phase4_composition_catalog.h"

namespace mvm::gpu {
namespace {

// docs/phase4-plan.md §3.1。A は常に全面 opacity 1.0、B は PiP。
// sourceUv は全 state で full、A の z=0 / B の z=1 も固定である。
LayerLayout layerA() {
    return {kPhase4SourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0};
}

LayerLayout layerB(const RectF& destination, float opacity) {
    return {kPhase4SourceB, destination, {0, 0, 1, 1}, opacity, 1};
}

constexpr RectF kBottomRight{0.5f, 0.5f, 0.5f, 0.5f};
constexpr RectF kTopLeft{0, 0, 0.5f, 0.5f};

} // namespace

const char* phase4ScheduleKindName(Phase4ScheduleKind kind) {
    return kind == Phase4ScheduleKind::Smoke ? "smoke" : "formal";
}

const char* phase4StateName(CompositionStateId state) {
    if (state == kPhase4S0)
        return "S0";
    if (state == kPhase4S1)
        return "S1";
    if (state == kPhase4S2)
        return "S2";
    if (state == kPhase4S3)
        return "S3";
    return nullptr;
}

CompositionStateId phase4StateFromName(std::string_view name) {
    if (name == "S0")
        return kPhase4S0;
    if (name == "S1")
        return kPhase4S1;
    if (name == "S2")
        return kPhase4S2;
    if (name == "S3")
        return kPhase4S3;
    return {};
}

std::vector<LayerLayout> phase4CanonicalLayout(CompositionStateId state) {
    if (state == kPhase4S0)
        return {layerA(), layerB(kBottomRight, 0.75f)};
    if (state == kPhase4S1)
        return {layerA(), layerB(kTopLeft, 0.75f)};
    if (state == kPhase4S2)
        return {layerA(), layerB(kTopLeft, 0.50f)};
    if (state == kPhase4S3)
        return {layerA(), layerB(kBottomRight, 0.50f)};
    return {};
}

std::vector<CompositionScheduleEntry> phase4ScheduleEntries(Phase4ScheduleKind kind) {
    if (kind == Phase4ScheduleKind::Smoke)
        return {{0, kPhase4S0}, {200, kPhase4S1}, {400, kPhase4S2}};
    return {{0, kPhase4S0},    {600, kPhase4S1},  {1200, kPhase4S2},
            {1800, kPhase4S3}, {2400, kPhase4S0}, {3000, kPhase4S1}};
}

std::optional<CompositionSchedule> phase4Schedule(Phase4ScheduleKind kind) {
    return CompositionSchedule::create(phase4ScheduleEntries(kind));
}

std::string phase4CanonicalScheduleString(Phase4ScheduleKind kind) {
    std::string canonical;
    for (const auto& entry : phase4ScheduleEntries(kind)) {
        const char* name = phase4StateName(entry.state);
        if (!name)
            return {};
        if (!canonical.empty())
            canonical.push_back(';');
        canonical += std::to_string(entry.boundaryOutputFrame);
        canonical.push_back(':');
        canonical += name;
    }
    return canonical;
}

const char* phase4ExpectedScheduleSha256(Phase4ScheduleKind kind) {
    return kind == Phase4ScheduleKind::Smoke
               ? "418ae09f4bb9349aa7ac53ca38028782aef074ca1696338758ccfa6b4e4398e8"
               : "5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79";
}

} // namespace mvm::gpu
