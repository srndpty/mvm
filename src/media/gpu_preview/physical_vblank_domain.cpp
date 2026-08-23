#include "media/gpu_preview/physical_vblank_domain.h"

#include <algorithm>

namespace mvm::gpu {

const char* physicalVBlankDomainErrorName(PhysicalVBlankDomainError error) {
    switch (error) {
    case PhysicalVBlankDomainError::None: return "NONE";
    case PhysicalVBlankDomainError::MeasurementWindowInvalid:
        return "PHYSICAL_VBLANK_MEASUREMENT_WINDOW_INVALID";
    case PhysicalVBlankDomainError::ObserverUnavailable:
        return "PHYSICAL_VBLANK_OBSERVER_UNAVAILABLE";
    case PhysicalVBlankDomainError::RingOverflow: return "PHYSICAL_VBLANK_RING_OVERFLOW";
    case PhysicalVBlankDomainError::WaitFailure: return "PHYSICAL_VBLANK_WAIT_FAILURE";
    case PhysicalVBlankDomainError::SequenceBreak: return "PHYSICAL_VBLANK_SEQUENCE_BREAK";
    case PhysicalVBlankDomainError::OutputOrModeChanged: return "OUTPUT_OR_MODE_CHANGED";
    case PhysicalVBlankDomainError::BoundaryNotBracketed:
        return "PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED";
    case PhysicalVBlankDomainError::ObserverStall: return "PHYSICAL_VBLANK_OBSERVER_STALL";
    }
    return "PHYSICAL_VBLANK_OBSERVER_STALL";
}

const char* physicalVBlankDomainCanonicalReason(PhysicalVBlankDomainError error) {
    switch (error) {
    case PhysicalVBlankDomainError::None: return "NONE";
    // 窓を供給するのはmeasurement lifecycle authorityであり、observerではない。
    // 供給されなかった時点でcanonical authority setから逸脱している。
    case PhysicalVBlankDomainError::MeasurementWindowInvalid: return "RUNTIME_AUTHORITY_OVERRIDE";
    case PhysicalVBlankDomainError::ObserverUnavailable:
    case PhysicalVBlankDomainError::RingOverflow:
    case PhysicalVBlankDomainError::WaitFailure:
    case PhysicalVBlankDomainError::ObserverStall: return "PHYSICAL_VBLANK_OBSERVER_INVALID";
    case PhysicalVBlankDomainError::SequenceBreak: return "PHYSICAL_VBLANK_SEQUENCE_BREAK";
    case PhysicalVBlankDomainError::OutputOrModeChanged: return "OUTPUT_OR_MODE_CHANGED";
    case PhysicalVBlankDomainError::BoundaryNotBracketed:
        return "PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED";
    }
    return "PHYSICAL_VBLANK_OBSERVER_INVALID";
}

namespace {

bool fail(PhysicalVBlankDomain& out, PhysicalVBlankDomainError error) {
    out.shadowAuthorityValid = false;
    out.shadowAuthorityError = error;
    return false;
}

} // namespace

bool buildPhysicalVBlankDomain(const PhysicalVBlankDomainInput& input, PhysicalVBlankDomain& out) {
    out = {};
    out.measurementStartQpc = input.measurementStartQpc;
    out.measurementEndQpc = input.measurementEndQpc;
    out.ringOverflowCount = input.ringOverflowCount;
    out.waitFailureCount = input.waitFailureCount;
    out.outputStable = input.outputStable;
    // Layer 1A の入力は正常化しない。負値を0へ丸めると「そう入力された」事実が
    // 消える。Layer 1A の validity は後段が判定する。
    out.requiredIntentCount = input.requiredIntentCount;

    // measurement窓が確定していなければdomainは定義されない。collectorが独自の
    // endを作って埋めることはしない。
    if (input.measurementStartQpc <= 0 || input.measurementEndQpc <= input.measurementStartQpc)
        return fail(out, PhysicalVBlankDomainError::MeasurementWindowInvalid);
    if (input.qpcFrequency <= 0)
        return fail(out, PhysicalVBlankDomainError::MeasurementWindowInvalid);

    // sample列に触れる前にpointer/countをfail-closeする。refresh rationalは
    // window output identityの一部であり、observer pathが解決できていなければ
    // physical opportunity authorityが成立していない。
    if (!input.observerStarted || !input.timeCriticalPriority || !input.samples ||
        input.sampleCount == 0 || input.refreshNumerator <= 0 || input.refreshDenominator <= 0)
        return fail(out, PhysicalVBlankDomainError::ObserverUnavailable);

    out.sequenceStatus = vblankSequenceStatus(input.samples, input.sampleCount);
    if (input.ringOverflowCount != 0)
        return fail(out, PhysicalVBlankDomainError::RingOverflow);
    if (input.waitFailureCount != 0)
        return fail(out, PhysicalVBlankDomainError::WaitFailure);
    if (out.sequenceStatus != VBlankSequenceStatus::Ok)
        return fail(out, PhysicalVBlankDomainError::SequenceBreak);
    if (!input.outputStable)
        return fail(out, PhysicalVBlankDomainError::OutputOrModeChanged);

    // half-open [start, end)。end と完全一致したVBlankはsuccessor側に置く。
    std::size_t begin = 0;
    while (begin < input.sampleCount && input.samples[begin].qpc < input.measurementStartQpc)
        ++begin;
    std::size_t end = begin;
    while (end < input.sampleCount && input.samples[end].qpc < input.measurementEndQpc)
        ++end;

    // predecessor / successorはdomain memberではなくboundary authorityである。
    //   predecessor.qpc <  measurement_start_qpc
    //   successor.qpc   >= measurement_end_qpc
    if (begin >= 1) {
        out.predecessor = input.samples[begin - 1];
        out.predecessorValid = out.predecessor.qpc < input.measurementStartQpc;
    }
    if (end < input.sampleCount) {
        out.successor = input.samples[end];
        out.successorValid = out.successor.qpc >= input.measurementEndQpc;
    }
    out.boundaryBracketed = out.predecessorValid && out.successorValid;

    out.physicalOpportunityCount = static_cast<long long>(end - begin);
    if (out.physicalOpportunityCount > 0) {
        out.originOrdinal = input.samples[begin].ordinal;
        out.originQpc = input.samples[begin].qpc;
        out.lastOrdinal = input.samples[end - 1].ordinal;
        out.lastQpc = input.samples[end - 1].qpc;
    }
    // Layer 1A と Layer 1B の差。shadow出力のみ。verdictには接続しない。
    out.intentOverhangCount =
        std::max(0LL, out.requiredIntentCount - out.physicalOpportunityCount);
    out.intentSurplusCount = std::max(0LL, out.physicalOpportunityCount - out.requiredIntentCount);

    if (!out.boundaryBracketed)
        return fail(out, PhysicalVBlankDomainError::BoundaryNotBracketed);

    // interval / 累積の検査はbracketed closed range [predecessor, successor] で
    // 行う。domainのexact accountingに必要なsampleはこの範囲に閉じており、
    // warmup中のobserver hiccupをdomain authorityの判定に混ぜない。
    const std::size_t bracketBegin = begin - 1;
    const std::size_t bracketCount = (end - bracketBegin) + 1;
    VBlankIntervalReport intervals{};
    const bool intervalsOk =
        vblankIntervalReport(input.samples + bracketBegin, bracketCount, input.refreshNumerator,
                             input.refreshDenominator, input.qpcFrequency, intervals);
    out.longIntervalCount = intervals.longIntervalCount;
    out.shortIntervalCount = intervals.shortIntervalCount;
    out.cumulativeConsistent = intervals.cumulativeConsistent;
    if (!intervalsOk || out.longIntervalCount != 0 || out.shortIntervalCount != 0 ||
        !out.cumulativeConsistent)
        return fail(out, PhysicalVBlankDomainError::ObserverStall);

    out.shadowAuthorityValid = true;
    out.shadowAuthorityError = PhysicalVBlankDomainError::None;
    return true;
}

} // namespace mvm::gpu
