#ifndef MVM_GPU_PREVIEW_PHYSICAL_VBLANK_DOMAIN_H
#define MVM_GPU_PREVIEW_PHYSICAL_VBLANK_DOMAIN_H

#include "media/gpu_preview/window_output_vblank_authority.h"

#include <cstddef>

namespace mvm::gpu {

// P2-D5-2-W2-A。measurement窓に対するphysical VBlank domainをexactに構築する。
// shadow onlyであり、legacy formal scheduler/counters/shutdown/thresholdへは
// 一切入力しない。
//
// authorityの分離:
//   measurement lifecycle authority   既存formal runner/controller
//                                     (measurement_start_qpc / measurement_end_qpc)
//   physical opportunity authority    physical VBlank observer
//
// collectorが「60秒経ったから独自にendを作る」ことはしない。窓は必ずlifecycle
// 側から与えられたexact QPCを使う。
enum class PhysicalVBlankDomainError {
    None,
    // measurement lifecycle authorityがexactな窓を供給しなかった。
    MeasurementWindowInvalid,
    // observerが起動していない / TIME_CRITICALへ昇格できていない。
    ObserverUnavailable,
    RingOverflow,
    WaitFailure,
    // ordinal / QPCの単調連続性が破れている。
    SequenceBreak,
    // adapter / output / HMONITOR / refresh rationalが変化した。
    OutputOrModeChanged,
    // domain前のpredecessorかend以上のsuccessorが無い。
    BoundaryNotBracketed,
    // 隣接VBlankと断定できないinterval、または累積ずれ。
    // 「実displayが長いintervalだった」のか「observerが起きられなかった」のかは
    // 区別できないが、どちらにせよauthority invalidである。performance FAILでは
    // ない。
    ObserverStall,
};

const char* physicalVBlankDomainErrorName(PhysicalVBlankDomainError error);

// W2-A固有reasonを、W1でfreezeしたv2 canonical fail-close reasonへ射影する。
// W1 contractのreason語彙は変更しない。W2-E cutover時にこの射影を使う。
const char* physicalVBlankDomainCanonicalReason(PhysicalVBlankDomainError error);

struct PhysicalVBlankDomainInput {
    const VBlankObservation* samples = nullptr;
    std::size_t sampleCount = 0;
    // 既存formal measurement lifecycleが確定したexact QPC。half-open [start,end)。
    long long measurementStartQpc = 0;
    long long measurementEndQpc = 0;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    long long qpcFrequency = 0;
    long long ringOverflowCount = 0;
    long long waitFailureCount = 0;
    bool observerStarted = false;
    bool timeCriticalPriority = false;
    bool outputStable = false;
    // Layer 1A。shadow出力のためだけに受け取る。physical countとの一致は
    // 要求せず、verdictにも接続しない。
    long long requiredIntentCount = 0;
};

struct PhysicalVBlankDomain {
    long long measurementStartQpc = 0;
    long long measurementEndQpc = 0;

    // predecessor / successorはdomain memberではなくboundary authorityである。
    bool predecessorValid = false;
    VBlankObservation predecessor{};
    bool successorValid = false;
    VBlankObservation successor{};

    // domain member: measurement_start_qpc <= vblank.qpc < measurement_end_qpc
    long long originOrdinal = -1;
    long long originQpc = 0;
    long long lastOrdinal = -1;
    long long lastQpc = 0;
    long long physicalOpportunityCount = 0;

    VBlankSequenceStatus sequenceStatus = VBlankSequenceStatus::Empty;
    long long longIntervalCount = 0;
    long long shortIntervalCount = 0;
    long long ringOverflowCount = 0;
    long long waitFailureCount = 0;
    bool cumulativeConsistent = false;
    bool outputStable = false;
    bool boundaryBracketed = false;

    bool shadowAuthorityValid = false;
    PhysicalVBlankDomainError shadowAuthorityError = PhysicalVBlankDomainError::None;

    // shadow only。performance semanticsへは接続しない。
    // physical_opportunity_countがrequired_intent_countと一致しないことは
    // W2-Aでは正常であり、differenceをdropと判定してはならない。
    long long requiredIntentCount = 0;
    long long intentOverhangCount = 0;
    long long intentSurplusCount = 0;
};

// 全fieldを常に埋める。戻り値はshadowAuthorityValidと同じ。
bool buildPhysicalVBlankDomain(const PhysicalVBlankDomainInput& input, PhysicalVBlankDomain& out);

} // namespace mvm::gpu

#endif
