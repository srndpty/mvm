#include "media/gpu_preview/physical_vblank_domain.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using mvm::gpu::buildPhysicalVBlankDomain;
using mvm::gpu::PhysicalVBlankDomain;
using mvm::gpu::PhysicalVBlankDomainError;
using mvm::gpu::PhysicalVBlankDomainInput;
using mvm::gpu::physicalVBlankDomainCanonicalReason;
using mvm::gpu::physicalVBlankDomainErrorName;
using mvm::gpu::VBlankObservation;

constexpr long long kQpcFrequency = 10000000; // 10 MHz
constexpr long long kRefreshNumerator = 60000;
constexpr long long kRefreshDenominator = 1000;
constexpr long long kPeriod = kQpcFrequency * kRefreshDenominator / kRefreshNumerator; // 166666
constexpr long long kBaseQpc = 1000000;
constexpr long long kFirstOrdinal = 500;

std::vector<VBlankObservation> vblanks(long long count) {
    std::vector<VBlankObservation> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (long long index = 0; index < count; ++index)
        samples.push_back({kFirstOrdinal + index, kBaseQpc + index * kPeriod});
    return samples;
}

// index番目のVBlankのQPC。
long long qpcAt(long long index) {
    return kBaseQpc + index * kPeriod;
}

PhysicalVBlankDomainInput baseInput(const std::vector<VBlankObservation>& samples, long long start,
                                    long long end) {
    PhysicalVBlankDomainInput input;
    input.samples = samples.data();
    input.sampleCount = samples.size();
    input.measurementStartQpc = start;
    input.measurementEndQpc = end;
    input.refreshNumerator = kRefreshNumerator;
    input.refreshDenominator = kRefreshDenominator;
    input.qpcFrequency = kQpcFrequency;
    input.observerStarted = true;
    input.timeCriticalPriority = true;
    input.outputStable = true;
    // W2-A.1。既定はprerollがmeasurement窓の前に成立している状態。
    input.prerollCompleted = true;
    input.prerollTimedOut = false;
    input.prerollSample = {kFirstOrdinal, kBaseQpc};
    input.prerollWaitElapsedQpc = 1234;
    input.requiredIntentCount = 0;
    return input;
}

std::string named(PhysicalVBlankDomainError error) {
    return physicalVBlankDomainErrorName(error);
}

// 対照群。domain / bracket / ordinal算術がすべて閉じる。
void goodDomain() {
    const auto samples = vblanks(200);
    // [V10.qpc, V110.qpc) を窓にする。domainはV10..V109の100本。
    auto input = baseInput(samples, qpcAt(10), qpcAt(110));
    input.requiredIntentCount = 100;
    PhysicalVBlankDomain domain;
    check(buildPhysicalVBlankDomain(input, domain), "good: shadow authorityがvalidではない");
    check(domain.shadowAuthorityError == PhysicalVBlankDomainError::None, "good: errorが残っている");
    check(domain.physicalOpportunityCount == 100, "good: physical_opportunity_countが100ではない");
    check(domain.originOrdinal == kFirstOrdinal + 10, "good: origin_ordinalが不正");
    check(domain.originQpc == qpcAt(10), "good: origin_qpcが不正");
    check(domain.lastOrdinal == kFirstOrdinal + 109, "good: last_ordinalが不正");
    check(domain.boundaryBracketed, "good: bracketedではない");
    check(domain.predecessorValid && domain.predecessor.ordinal == kFirstOrdinal + 9,
          "good: predecessorが不正");
    check(domain.predecessor.qpc < domain.measurementStartQpc,
          "good: predecessorがmeasurement_start_qpcより前ではない");
    check(domain.successorValid && domain.successor.ordinal == kFirstOrdinal + 110,
          "good: successorが不正");
    check(domain.successor.qpc >= domain.measurementEndQpc,
          "good: successorがmeasurement_end_qpc以上ではない");
    check(domain.physicalOpportunityCount == domain.lastOrdinal - domain.originOrdinal + 1,
          "good: count == last - origin + 1 が成立しない");
    check(domain.predecessor.ordinal + 1 == domain.originOrdinal,
          "good: predecessor.ordinal + 1 == origin_ordinal が成立しない");
    check(domain.successor.ordinal == domain.lastOrdinal + 1,
          "good: successor.ordinal == last_ordinal + 1 が成立しない");
    check(domain.longIntervalCount == 0 && domain.shortIntervalCount == 0,
          "good: interval anomalyが出ている");
    check(domain.cumulativeConsistent, "good: cumulative consistentではない");
}

// half-open [start, end)。end と完全一致したVBlankはsuccessor側に置かれる。
// start と完全一致したVBlankはdomain memberである。
void halfOpenBoundary() {
    const auto samples = vblanks(60);
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(20));
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "half-open: validではない");
        check(domain.physicalOpportunityCount == 10, "half-open: end一致VBlankがdomainに入った");
        check(domain.lastOrdinal == kFirstOrdinal + 19, "half-open: last_ordinalが不正");
        check(domain.successor.ordinal == kFirstOrdinal + 20,
              "half-open: end一致VBlankがsuccessorになっていない");
        check(domain.successor.qpc == input.measurementEndQpc,
              "half-open: successor.qpc == end の場合を許していない");
        check(domain.originOrdinal == kFirstOrdinal + 10,
              "half-open: start一致VBlankがdomain memberではない");
    }
    {
        // endを1 tickだけ後ろへ動かすとV20がdomainに入る。境界がexactであること。
        auto input = baseInput(samples, qpcAt(10), qpcAt(20) + 1);
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "half-open+1: validではない");
        check(domain.physicalOpportunityCount == 11, "half-open+1: 境界が1 tickでずれない");
        check(domain.successor.ordinal == kFirstOrdinal + 21, "half-open+1: successorがずれない");
    }
    {
        // startを1 tickだけ後ろへ動かすとV10がdomainから外れpredecessorになる。
        auto input = baseInput(samples, qpcAt(10) + 1, qpcAt(20));
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "start+1: validではない");
        check(domain.physicalOpportunityCount == 9, "start+1: start境界がexactではない");
        check(domain.predecessor.ordinal == kFirstOrdinal + 10,
              "start+1: 外れたVBlankがpredecessorになっていない");
    }
}

// 窓が2本のVBlankの間に収まる場合。domainは空だがbracketは成立する。
void emptyDomainStillBracketed() {
    const auto samples = vblanks(40);
    auto input = baseInput(samples, qpcAt(10) + 10, qpcAt(10) + 20);
    PhysicalVBlankDomain domain;
    check(buildPhysicalVBlankDomain(input, domain), "empty: validではない");
    check(domain.physicalOpportunityCount == 0, "empty: countが0ではない");
    check(domain.originOrdinal == -1, "empty: origin_ordinalが-1ではない");
    check(domain.boundaryBracketed, "empty: bracketedではない");
    check(domain.successor.ordinal == domain.predecessor.ordinal + 1,
          "empty: successorがpredecessorの直後ではない");
}

// predecessor / successor はdomain memberではなくboundary authorityである。
// どちらが欠けてもBOUNDARY_NOT_BRACKETEDでfail-closeする。
void boundaryNotBracketed() {
    const auto samples = vblanks(40);
    {
        // preroll sampleはあるが、渡されたsample列にそれが含まれていない
        // (先頭が欠けた snapshot)。W2-A.1後に下側bracketが落ちるのはこの場合だけ。
        auto input = baseInput(samples, kBaseQpc - 1000, qpcAt(20));
        input.prerollSample = {kFirstOrdinal - 1, kBaseQpc - kPeriod};
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "no predecessor: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::BoundaryNotBracketed,
              "no predecessor: reasonが不正 " + named(domain.shadowAuthorityError));
        check(!domain.predecessorValid && domain.successorValid,
              "no predecessor: bracket flagが不正");
    }
    {
        // 窓の終端が最後のsample以降。successorが存在しない。
        auto input = baseInput(samples, qpcAt(10), qpcAt(39) + kPeriod);
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "no successor: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::BoundaryNotBracketed,
              "no successor: reasonが不正 " + named(domain.shadowAuthorityError));
        check(domain.predecessorValid && !domain.successorValid,
              "no successor: bracket flagが不正");
    }
    {
        // 最後のsampleのqpcがちょうどendに一致するならsuccessorとして成立する。
        auto input = baseInput(samples, qpcAt(10), qpcAt(39));
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "successor==end: validではない");
        check(domain.successor.ordinal == kFirstOrdinal + 39, "successor==end: successorが不正");
    }
}

// observerの取りこぼしと「本当に長い物理interval」を区別しない。どちらも
// authority invalidであり、performance FAILではない。
void observerStall() {
    {
        auto samples = vblanks(60);
        for (std::size_t index = 15; index < samples.size(); ++index)
            samples[index].qpc += kPeriod; // V15の直前で1本ぶん遅延した
        auto input = baseInput(samples, qpcAt(10), qpcAt(30));
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "long interval: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverStall,
              "long interval: reasonが不正 " + named(domain.shadowAuthorityError));
        check(domain.longIntervalCount > 0, "long interval: long_interval_countが0");
    }
    {
        auto samples = vblanks(60);
        // 1本だけ極端に詰まる。隣接VBlankと断定できない。
        samples[20].qpc = samples[19].qpc + kPeriod / 8;
        auto input = baseInput(samples, qpcAt(10), qpcAt(30));
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "short interval: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverStall,
              "short interval: reasonが不正 " + named(domain.shadowAuthorityError));
        check(domain.shortIntervalCount > 0, "short interval: short_interval_countが0");
    }
}

// interval検査はbracketed closed range [predecessor, successor] に閉じる。
// warmup中のobserver hiccupでdomain authorityを落とさない。
void stallOutsideBracketIsIgnored() {
    auto samples = vblanks(80);
    for (std::size_t index = 5; index < samples.size(); ++index)
        samples[index].qpc += kPeriod; // warmup区間で1本ぶん遅延した
    auto input = baseInput(samples, samples[20].qpc, samples[60].qpc);
    PhysicalVBlankDomain domain;
    check(buildPhysicalVBlankDomain(input, domain),
          "warmup stall: bracket外のstallでfail-closeした " + named(domain.shadowAuthorityError));
    check(domain.physicalOpportunityCount == 40, "warmup stall: countが不正");
}

void sequenceBreak() {
    auto samples = vblanks(60);
    for (std::size_t index = 30; index < samples.size(); ++index)
        samples[index].ordinal += 1; // ordinal gap
    auto input = baseInput(samples, qpcAt(10), qpcAt(50));
    PhysicalVBlankDomain domain;
    check(!buildPhysicalVBlankDomain(input, domain), "sequence: validになっている");
    check(domain.shadowAuthorityError == PhysicalVBlankDomainError::SequenceBreak,
          "sequence: reasonが不正 " + named(domain.shadowAuthorityError));
}

void observerCounters() {
    const auto samples = vblanks(60);
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.ringOverflowCount = 1;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "overflow: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::RingOverflow,
              "overflow: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.waitFailureCount = 1;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "wait failure: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::WaitFailure,
              "wait failure: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.timeCriticalPriority = false;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "priority: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverUnavailable,
              "priority: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.observerStarted = false;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "not started: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverUnavailable,
              "not started: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        // refresh rationalはwindow output identityの一部。observer pathが解決
        // できていなければphysical opportunity authorityが成立していない。
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.refreshNumerator = 0;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "no refresh: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverUnavailable,
              "no refresh: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.outputStable = false;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "output: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::OutputOrModeChanged,
              "output: reasonが不正 " + named(domain.shadowAuthorityError));
    }
}

// malformed inputでsample列をdereferenceしない。sequence判定より前に
// pointer/countをfail-closeする。
void malformedSampleInput() {
    const auto samples = vblanks(60);
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.samples = nullptr;
        input.sampleCount = 1; // pointerがnullなのにcountが正、という壊れた入力
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "null+1: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverUnavailable,
              "null+1: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(50));
        input.samples = nullptr;
        input.sampleCount = 0;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "null+0: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::ObserverUnavailable,
              "null+0: reasonが不正 " + named(domain.shadowAuthorityError));
    }
}

// Layer 1A の入力は正常化しない。負値を0へ丸めて事実を隠さない。
void requiredIntentCountIsNotNormalized() {
    const auto samples = vblanks(200);
    auto input = baseInput(samples, qpcAt(10), qpcAt(110));
    input.requiredIntentCount = -1;
    PhysicalVBlankDomain domain;
    check(buildPhysicalVBlankDomain(input, domain), "raw required: validではない");
    check(domain.requiredIntentCount == -1, "raw required: 入力値が保存されていない");
}

// W2-A.1。measurement窓を開く前にphysical VBlankを1本観測していなければ、
// 下側bracketはraceでしか成立しない。fail-closeする。
void lowerBoundaryPreroll() {
    const auto samples = vblanks(200);
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(110));
        input.prerollCompleted = false;
        input.prerollTimedOut = true;
        input.prerollSample = {};
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "preroll timeout: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::PrerollTimeout,
              "preroll timeout: reasonが不正 " + named(domain.shadowAuthorityError));
        check(domain.prerollTimedOut, "preroll timeout: timeout flagが伝播していない");
    }
    {
        // observerが死んだ場合。timeoutではないがprerollは成立していない。
        auto input = baseInput(samples, qpcAt(10), qpcAt(110));
        input.prerollCompleted = false;
        input.prerollTimedOut = false;
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "preroll dead: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::PrerollTimeout,
              "preroll dead: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        // preroll sampleがmeasurement_start_qpc以降。下側witnessになっていない。
        auto input = baseInput(samples, qpcAt(10), qpcAt(110));
        input.prerollSample = {kFirstOrdinal + 10, qpcAt(10)};
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "preroll not before: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::PrerollNotBeforeStart,
              "preroll not before: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(110));
        input.prerollSample = {kFirstOrdinal, 0}; // sampleが空
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "preroll empty: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::PrerollNotBeforeStart,
              "preroll empty: reasonが不正 " + named(domain.shadowAuthorityError));
    }
    {
        // 確認したい不変量はordinalが0かどうかではなくqpc < start である。
        auto input = baseInput(samples, qpcAt(10), qpcAt(110));
        input.prerollSample = {kFirstOrdinal + 9, qpcAt(9)};
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain),
              "preroll ordinal非0: validではない " + named(domain.shadowAuthorityError));
        check(domain.prerollSample.qpc < domain.measurementStartQpc,
              "preroll ordinal非0: preroll sampleがstartより前ではない");
    }
}

// 窓のauthorityはmeasurement lifecycle側にある。collectorは独自のendを作らない。
void measurementWindowAuthority() {
    const auto samples = vblanks(60);
    const long long badWindows[][2] = {
        {0, qpcAt(50)}, {qpcAt(50), qpcAt(50)}, {qpcAt(50), qpcAt(10)}};
    for (const auto& window : badWindows) {
        auto input = baseInput(samples, window[0], window[1]);
        PhysicalVBlankDomain domain;
        check(!buildPhysicalVBlankDomain(input, domain), "window: validになっている");
        check(domain.shadowAuthorityError == PhysicalVBlankDomainError::MeasurementWindowInvalid,
              "window: reasonが不正 " + named(domain.shadowAuthorityError));
    }
}

// W2-Aではrequired_intent_countとphysical countの一致を要求しない。差を
// dropと判定しない。
void intentCountIsNotAVerdict() {
    const auto samples = vblanks(4000);
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(3607));
        input.requiredIntentCount = 3600; // physicalは3597
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "overhang: validではない");
        check(domain.physicalOpportunityCount == 3597, "overhang: countが不正");
        check(domain.intentOverhangCount == 3, "overhang: overhangが3ではない");
        check(domain.intentSurplusCount == 0, "overhang: surplusが0ではない");
    }
    {
        auto input = baseInput(samples, qpcAt(10), qpcAt(3613));
        input.requiredIntentCount = 3600; // physicalは3603
        PhysicalVBlankDomain domain;
        check(buildPhysicalVBlankDomain(input, domain), "surplus: validではない");
        check(domain.intentSurplusCount == 3, "surplus: surplusが3ではない");
        check(domain.intentOverhangCount == 0, "surplus: overhangが0ではない");
    }
}

// 窓を1 tickずつ掃引し、domain / bracketの不変量が常に閉じることを固定する。
void sweepProperty() {
    const auto samples = vblanks(120);
    for (long long offset = -3; offset <= 3; ++offset) {
        for (long long span = 1; span <= 40; ++span) {
            const long long start = qpcAt(20) + offset;
            const long long end = qpcAt(20 + span) + offset;
            auto input = baseInput(samples, start, end);
            input.requiredIntentCount = span;
            PhysicalVBlankDomain domain;
            const bool ok = buildPhysicalVBlankDomain(input, domain);
            const std::string tag =
                "sweep(" + std::to_string(offset) + "," + std::to_string(span) + "): ";
            check(ok, tag + "validではない " + named(domain.shadowAuthorityError));
            if (!ok)
                continue;
            check(domain.physicalOpportunityCount == span, tag + "countが不正");
            check(domain.predecessor.qpc < start, tag + "predecessorがstartより前ではない");
            check(domain.successor.qpc >= end, tag + "successorがend以上ではない");
            check(domain.predecessor.ordinal + 1 == domain.originOrdinal,
                  tag + "predecessorがoriginの直前ではない");
            check(domain.successor.ordinal == domain.lastOrdinal + 1,
                  tag + "successorがlastの直後ではない");
            check(domain.originQpc >= start && domain.lastQpc < end,
                  tag + "domain memberが半開区間に収まっていない");
            check(domain.physicalOpportunityCount == domain.lastOrdinal - domain.originOrdinal + 1,
                  tag + "ordinal算術が閉じていない");
            check(domain.intentOverhangCount == 0 && domain.intentSurplusCount == 0,
                  tag + "overhang/surplusが0ではない");
        }
    }
}

// fail-close reasonは必ずW1でfreezeしたv2 canonical語彙へ射影できる。
void canonicalReasonProjection() {
    const PhysicalVBlankDomainError all[] = {PhysicalVBlankDomainError::None,
                                             PhysicalVBlankDomainError::MeasurementWindowInvalid,
                                             PhysicalVBlankDomainError::ObserverUnavailable,
                                             PhysicalVBlankDomainError::RingOverflow,
                                             PhysicalVBlankDomainError::WaitFailure,
                                             PhysicalVBlankDomainError::SequenceBreak,
                                             PhysicalVBlankDomainError::OutputOrModeChanged,
                                             PhysicalVBlankDomainError::PrerollTimeout,
                                             PhysicalVBlankDomainError::PrerollNotBeforeStart,
                                             PhysicalVBlankDomainError::BoundaryNotBracketed,
                                             PhysicalVBlankDomainError::ObserverStall};
    const std::vector<std::string> canonical{"NONE",
                                             "PHYSICAL_VBLANK_OBSERVER_INVALID",
                                             "PHYSICAL_VBLANK_SEQUENCE_BREAK",
                                             "PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED",
                                             "OUTPUT_OR_MODE_CHANGED",
                                             "RUNTIME_AUTHORITY_OVERRIDE"};
    for (const auto error : all) {
        const std::string reason = physicalVBlankDomainCanonicalReason(error);
        bool found = false;
        for (const auto& candidate : canonical)
            found = found || candidate == reason;
        check(found, "canonical射影がv2語彙にありません: " + reason);
        check(std::string(physicalVBlankDomainErrorName(error)) != std::string(),
              "reason名が空です");
    }
}

} // namespace

int main() {
    goodDomain();
    halfOpenBoundary();
    emptyDomainStillBracketed();
    boundaryNotBracketed();
    observerStall();
    stallOutsideBracketIsIgnored();
    sequenceBreak();
    observerCounters();
    malformedSampleInput();
    lowerBoundaryPreroll();
    requiredIntentCountIsNotNormalized();
    measurementWindowAuthority();
    intentCountIsNotAVerdict();
    sweepProperty();
    canonicalReasonProjection();
    if (failures != 0) {
        std::fprintf(stderr, "physical vblank domain: %d 件の失敗\n", failures);
        return 1;
    }
    std::printf("physical vblank domain: OK\n");
    return 0;
}
