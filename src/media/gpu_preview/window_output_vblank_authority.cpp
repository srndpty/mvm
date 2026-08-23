#include "media/gpu_preview/window_output_vblank_authority.h"

#include <algorithm>
#include <limits>

namespace mvm::gpu {
namespace {

bool checkedMultiply(long long left, long long right, long long& result) {
    if (left < 0 || right < 0 ||
        (left != 0 && right > std::numeric_limits<long long>::max() / left))
        return false;
    result = left * right;
    return true;
}

} // namespace

bool sameWindowOutput(const WindowOutputIdentity& left, const WindowOutputIdentity& right) {
    return left.available && right.available && left.monitorHandle == right.monitorHandle &&
           left.outputIndex == right.outputIndex && left.adapterLuidLow == right.adapterLuidLow &&
           left.adapterLuidHigh == right.adapterLuidHigh &&
           left.gdiDeviceName == right.gdiDeviceName &&
           left.outputDeviceName == right.outputDeviceName &&
           left.refreshNumerator == right.refreshNumerator &&
           left.refreshDenominator == right.refreshDenominator &&
           left.desktopLeft == right.desktopLeft && left.desktopTop == right.desktopTop &&
           left.desktopRight == right.desktopRight && left.desktopBottom == right.desktopBottom;
}

void VBlankRing::reset() {
    overflow_.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_release);
}

void VBlankRing::capture(const VBlankObservation& value) {
    const std::size_t index = count_.load(std::memory_order_relaxed);
    if (index >= samples_.size()) {
        overflow_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    samples_[index] = value;
    count_.store(index + 1, std::memory_order_release);
}

std::vector<VBlankObservation> VBlankRing::snapshot() const {
    const std::size_t count = publishedCount();
    return {samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count)};
}

bool VBlankRing::read(std::size_t index, VBlankObservation& value) const {
    if (index >= publishedCount())
        return false;
    value = samples_[index];
    return true;
}

std::size_t VBlankRing::publishedCount() const {
    return std::min(count_.load(std::memory_order_acquire), samples_.size());
}

long long VBlankRing::overflowCount() const {
    return overflow_.load(std::memory_order_acquire);
}

VBlankSequenceStatus vblankSequenceStatus(const VBlankObservation* samples, std::size_t count) {
    if (!samples || count == 0)
        return VBlankSequenceStatus::Empty;
    for (std::size_t index = 0; index < count; ++index) {
        if (samples[index].ordinal < 0 || samples[index].qpc <= 0)
            return VBlankSequenceStatus::Invalid;
    }
    for (std::size_t index = 1; index < count; ++index) {
        const long long previous = samples[index - 1].ordinal;
        const long long current = samples[index].ordinal;
        if (current <= previous)
            return VBlankSequenceStatus::OrdinalRegression;
        if (current != previous + 1)
            return VBlankSequenceStatus::OrdinalGap;
        if (samples[index].qpc <= samples[index - 1].qpc)
            return VBlankSequenceStatus::QpcRegression;
    }
    return VBlankSequenceStatus::Ok;
}

const char* vblankSequenceStatusName(VBlankSequenceStatus status) {
    switch (status) {
    case VBlankSequenceStatus::Ok: return "OK";
    case VBlankSequenceStatus::Empty: return "EMPTY";
    case VBlankSequenceStatus::Invalid: return "INVALID";
    case VBlankSequenceStatus::OrdinalRegression: return "ORDINAL_REGRESSION";
    case VBlankSequenceStatus::OrdinalGap: return "ORDINAL_GAP";
    case VBlankSequenceStatus::QpcRegression: return "QPC_REGRESSION";
    }
    return "UNKNOWN";
}

bool vblankCadenceConsistent(const VBlankObservation* samples, std::size_t count,
                             long long refreshNumerator, long long refreshDenominator,
                             long long qpcFrequency, VBlankCadenceResult& result) {
    result = {};
    if (!samples || count < 2 || refreshNumerator <= 0 || refreshDenominator <= 0 ||
        qpcFrequency <= 0)
        return false;
    if (vblankSequenceStatus(samples, count) != VBlankSequenceStatus::Ok)
        return false;
    const long long observed = samples[count - 1].ordinal - samples[0].ordinal;
    const long long elapsed = samples[count - 1].qpc - samples[0].qpc;
    if (observed <= 0 || elapsed <= 0)
        return false;
    // observed * qpcFrequency * refreshDenominator と elapsed * refreshNumerator を
    // 比較する。差が1 VBlank分 (qpcFrequency * refreshDenominator) 以内なら整合。
    long long observedScaled = 0;
    long long expectedScaled = 0;
    long long tolerance = 0;
    if (!checkedMultiply(observed, qpcFrequency, observedScaled) ||
        !checkedMultiply(observedScaled, refreshDenominator, observedScaled) ||
        !checkedMultiply(elapsed, refreshNumerator, expectedScaled) ||
        !checkedMultiply(qpcFrequency, refreshDenominator, tolerance) || tolerance <= 0)
        return false;
    const long long deviation = observedScaled - expectedScaled;
    result.observedIntervals = observed;
    result.elapsedQpc = elapsed;
    result.deviationNumerator = deviation;
    result.toleranceUnit = tolerance;
    result.consistent = deviation <= tolerance && -deviation <= tolerance;
    return result.consistent;
}

bool vblankIntervalReport(const VBlankObservation* samples, std::size_t count,
                          long long refreshNumerator, long long refreshDenominator,
                          long long qpcFrequency, VBlankIntervalReport& report) {
    report = {};
    if (!samples || count < 2 || refreshNumerator <= 0 || refreshDenominator <= 0 ||
        qpcFrequency <= 0)
        return false;
    if (vblankSequenceStatus(samples, count) != VBlankSequenceStatus::Ok)
        return false;
    long long scaled = 0;
    if (!checkedMultiply(qpcFrequency, refreshDenominator, scaled))
        return false;
    const long long period = scaled / refreshNumerator;
    if (period <= 0)
        return false;
    report.nominalPeriodQpc = period;
    report.minIntervalQpc = std::numeric_limits<long long>::max();
    for (std::size_t index = 1; index < count; ++index) {
        const long long interval = samples[index].qpc - samples[index - 1].qpc;
        ++report.intervalCount;
        if (interval > report.maxIntervalQpc)
            report.maxIntervalQpc = interval;
        if (interval < report.minIntervalQpc)
            report.minIntervalQpc = interval;
        // 1.5周期以上はVBlankを1本以上跨いだ疑い、0.5周期未満は直前のwakeが
        // 遅れた疑いとして数える。どちらも隣接VBlankと断定できない。
        if (interval * 2 >= period * 3)
            ++report.longIntervalCount;
        if (interval * 2 < period)
            ++report.shortIntervalCount;
    }
    if (report.minIntervalQpc == std::numeric_limits<long long>::max())
        report.minIntervalQpc = 0;
    const long long observed = samples[count - 1].ordinal - samples[0].ordinal;
    const long long elapsed = samples[count - 1].qpc - samples[0].qpc;
    long long observedScaled = 0;
    long long expectedScaled = 0;
    long long tolerance = 0;
    if (!checkedMultiply(observed, qpcFrequency, observedScaled) ||
        !checkedMultiply(observedScaled, refreshDenominator, observedScaled) ||
        !checkedMultiply(elapsed, refreshNumerator, expectedScaled) ||
        !checkedMultiply(qpcFrequency, refreshDenominator, tolerance) || tolerance <= 0)
        return false;
    report.cumulativeDeviationNumerator = observedScaled - expectedScaled;
    report.cumulativeToleranceUnit = tolerance;
    report.cumulativeConsistent = report.cumulativeDeviationNumerator <= tolerance &&
                                  -report.cumulativeDeviationNumerator <= tolerance;
    return true;
}

const char* swapMappingStatusName(SwapMappingStatus status) {
    switch (status) {
    case SwapMappingStatus::Mapped:
        return "MAPPED";
    case SwapMappingStatus::SameOpportunity:
        return "SAME_OPPORTUNITY";
    case SwapMappingStatus::AfterLast:
        return "AFTER_LAST";
    case SwapMappingStatus::BeforeFirst:
        return "BEFORE_FIRST";
    case SwapMappingStatus::ObserverGap:
        return "OBSERVER_GAP";
    case SwapMappingStatus::Ambiguous:
        return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

bool mapSwapsToVBlanks(const VBlankObservation* samples, std::size_t sampleCount,
                       const long long* swapQpc, const long long* swapOrdinal,
                       std::size_t swapCount, SwapMappingRecord* out, SwapMappingReport& report) {
    report = {};
    if (!samples || !swapQpc || !out || sampleCount < 2)
        return false;
    long long previousOpportunity = -1;
    for (std::size_t index = 0; index < swapCount; ++index) {
        SwapMappingRecord record;
        record.swapQpc = swapQpc[index];
        record.swapOrdinal = swapOrdinal ? swapOrdinal[index] : static_cast<long long>(index);
        long long ordinal = -1;
        const auto status = bracketSwapToVBlank(samples, sampleCount, record.swapQpc, ordinal);
        switch (status) {
        case VBlankBracketStatus::Ok:
            record.opportunityOrdinal = ordinal;
            if (ordinal == previousOpportunity) {
                record.status = SwapMappingStatus::SameOpportunity;
                ++report.sameOpportunityCount;
            } else if (ordinal < previousOpportunity) {
                record.status = SwapMappingStatus::Ambiguous;
                ++report.ambiguousCount;
            } else {
                record.status = SwapMappingStatus::Mapped;
                ++report.mappedCount;
                ++report.distinctOpportunityCount;
                if (report.firstOpportunityOrdinal < 0)
                    report.firstOpportunityOrdinal = ordinal;
                report.lastOpportunityOrdinal = ordinal;
                previousOpportunity = ordinal;
            }
            break;
        case VBlankBracketStatus::BeforeFirst:
            record.status = SwapMappingStatus::BeforeFirst;
            ++report.beforeFirstCount;
            break;
        case VBlankBracketStatus::AfterLast:
            record.status = SwapMappingStatus::AfterLast;
            ++report.afterLastCount;
            break;
        case VBlankBracketStatus::Gap:
            record.status = SwapMappingStatus::ObserverGap;
            ++report.observerGapCount;
            break;
        case VBlankBracketStatus::InvalidSequence:
            record.status = SwapMappingStatus::Ambiguous;
            ++report.ambiguousCount;
            break;
        }
        out[index] = record;
    }
    return true;
}

VBlankBracketStatus bracketSwapToVBlank(const VBlankObservation* samples, std::size_t count,
                                        long long swapQpc, long long& ordinal) {
    ordinal = -1;
    if (!samples || count < 2)
        return VBlankBracketStatus::InvalidSequence;
    if (swapQpc <= 0)
        return VBlankBracketStatus::InvalidSequence;
    if (swapQpc < samples[0].qpc)
        return VBlankBracketStatus::BeforeFirst;
    if (swapQpc >= samples[count - 1].qpc)
        return VBlankBracketStatus::AfterLast;
    // samples[index].qpc <= swapQpc となる最後のindexを二分探索する。
    std::size_t low = 0;
    std::size_t high = count - 1;
    while (low + 1 < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (samples[middle].qpc <= swapQpc)
            low = middle;
        else
            high = middle;
    }
    const auto& lower = samples[low];
    const auto& upper = samples[low + 1];
    if (lower.ordinal < 0 || upper.ordinal < 0 || lower.qpc <= 0 || upper.qpc <= 0)
        return VBlankBracketStatus::InvalidSequence;
    if (upper.qpc <= lower.qpc || upper.ordinal <= lower.ordinal)
        return VBlankBracketStatus::InvalidSequence;
    if (upper.ordinal != lower.ordinal + 1)
        return VBlankBracketStatus::Gap;
    if (swapQpc < lower.qpc || swapQpc >= upper.qpc)
        return VBlankBracketStatus::InvalidSequence;
    ordinal = lower.ordinal;
    return VBlankBracketStatus::Ok;
}

} // namespace mvm::gpu
