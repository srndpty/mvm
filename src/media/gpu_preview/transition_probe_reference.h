#ifndef MVM_GPU_PREVIEW_TRANSITION_PROBE_REFERENCE_H
#define MVM_GPU_PREVIEW_TRANSITION_PROBE_REFERENCE_H

#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/transition_probe.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace mvm::gpu {

struct Rgba8 {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    bool operator==(const Rgba8&) const = default;
};

Rgba8 bt709Limited(int y, int u, int v);
Rgba8 straightAlphaBlend(Rgba8 source, Rgba8 destination, double opacity);
bool probeWithinTolerance(const Rgba8& actual, const Rgba8& expected, int rgbTolerance = 3);
Rgba8 phase4ExpectedProbe(CompositionStateId state, TransitionProbePoint point, Rgba8 aTl,
                          Rgba8 aBr, Rgba8 bCenter);

struct Phase4ProbeExpected {
    long long boundary = -1;
    long long outputFrame = -1;
    CompositionStateId state;
    TransitionProbePoint point = TransitionProbePoint::TL;
    Rgba8 rgba;
};

struct Phase4CpuReferenceSet {
    std::string fixtureASha256;
    std::string fixtureBSha256;
    std::vector<Phase4ProbeExpected> candidates;

    const Phase4ProbeExpected* find(long long boundary, long long outputFrame,
                                    TransitionProbePoint point) const;
};

// Phase 4 harness専用。software decodeしたplanar YUVだけを入力に期待値を作る。
bool buildPhase4SmokeCpuReferences(const std::string& sourceA, const std::string& sourceB,
                                   const std::string& expectedShaA, const std::string& expectedShaB,
                                   Phase4CpuReferenceSet& output, std::string& err);

} // namespace mvm::gpu

#endif
