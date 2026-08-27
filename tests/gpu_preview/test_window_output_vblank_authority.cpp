#include "media/gpu_preview/window_output_vblank_authority.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using mvm::gpu::VBlankBracketStatus;
using mvm::gpu::VBlankCadenceResult;
using mvm::gpu::VBlankObservation;
using mvm::gpu::VBlankSequenceStatus;

constexpr long long kQpcFrequency = 10000000; // 実測環境と同じ10 MHz
constexpr long long kRefreshNumerator = 59950;
constexpr long long kRefreshDenominator = 1000;

// 59.95 Hz outputのVBlank列。qpcは 10 MHz / 59.95 Hz = 166805.67... tick周期。
std::vector<VBlankObservation> outputVBlanks(long long count, long long firstOrdinal = 0,
                                             long long baseQpc = 1000000) {
    std::vector<VBlankObservation> samples;
    samples.reserve(static_cast<std::size_t>(count));
    for (long long index = 0; index < count; ++index) {
        const long long qpc =
            baseQpc + (index * kQpcFrequency * kRefreshDenominator) / kRefreshNumerator;
        samples.push_back({firstOrdinal + index, qpc});
    }
    return samples;
}

void sequenceStatus() {
    const auto good = outputVBlanks(120);
    check(mvm::gpu::vblankSequenceStatus(good.data(), good.size()) == VBlankSequenceStatus::Ok,
          "連続VBlank列をOkにできません");
    check(mvm::gpu::vblankSequenceStatus(nullptr, 0) == VBlankSequenceStatus::Empty,
          "空列をEmptyにできません");

    auto gap = good;
    gap.erase(gap.begin() + 50);
    check(mvm::gpu::vblankSequenceStatus(gap.data(), gap.size()) ==
              VBlankSequenceStatus::OrdinalGap,
          "observerのVBlank取りこぼしをfail-closedにできません");

    auto regression = good;
    regression[50].ordinal = 40;
    check(mvm::gpu::vblankSequenceStatus(regression.data(), regression.size()) ==
              VBlankSequenceStatus::OrdinalRegression,
          "VBlank ordinal regressionをfail-closedにできません");

    auto qpcBack = good;
    qpcBack[50].qpc = qpcBack[49].qpc - 1;
    check(mvm::gpu::vblankSequenceStatus(qpcBack.data(), qpcBack.size()) ==
              VBlankSequenceStatus::QpcRegression,
          "VBlank QPC regressionをfail-closedにできません");
}

void cadenceGate() {
    const auto samples = outputVBlanks(120);
    VBlankCadenceResult result;
    check(mvm::gpu::vblankCadenceConsistent(samples.data(), samples.size(), kRefreshNumerator,
                                            kRefreshDenominator, kQpcFrequency, result),
          "window output cadenceとQueryDisplayConfig rationalの整合を認められません");
    check(result.observedIntervals == 119, "観測VBlank間隔数が違います");

    // 今回A2で観測したDWM composition clock (約123.7 Hz) をordinal authorityへ
    // 使うと、同じrationalとは整合しない。preflightで開始前に弾ける。
    std::vector<VBlankObservation> dwmClock;
    for (long long index = 0; index < 120; ++index)
        dwmClock.push_back({index, 1000000 + (index * kQpcFrequency * 1000) / 123700});
    VBlankCadenceResult dwmResult;
    check(!mvm::gpu::vblankCadenceConsistent(dwmClock.data(), dwmClock.size(), kRefreshNumerator,
                                             kRefreshDenominator, kQpcFrequency, dwmResult),
          "約123.7 Hz clockを59.95 Hz rationalと整合と誤認しました");

    // 半分のtickしか観測できない場合も弾く。
    std::vector<VBlankObservation> halfRate;
    for (long long index = 0; index < 120; ++index)
        halfRate.push_back({index, 1000000 + (index * 2 * kQpcFrequency * kRefreshDenominator) /
                                                 kRefreshNumerator});
    VBlankCadenceResult halfResult;
    check(!mvm::gpu::vblankCadenceConsistent(halfRate.data(), halfRate.size(), kRefreshNumerator,
                                             kRefreshDenominator, kQpcFrequency, halfResult),
          "半分のcadenceを整合と誤認しました");

    // 1 VBlank以内のendpoint jitterは許容する。
    auto jitter = outputVBlanks(120);
    jitter.back().qpc += (kQpcFrequency * kRefreshDenominator) / kRefreshNumerator - 1;
    VBlankCadenceResult jitterResult;
    check(mvm::gpu::vblankCadenceConsistent(jitter.data(), jitter.size(), kRefreshNumerator,
                                            kRefreshDenominator, kQpcFrequency, jitterResult),
          "1 VBlank未満のendpoint jitterを拒否しました");

    auto gapped = outputVBlanks(120);
    gapped.erase(gapped.begin() + 10);
    VBlankCadenceResult gappedResult;
    check(!mvm::gpu::vblankCadenceConsistent(gapped.data(), gapped.size(), kRefreshNumerator,
                                             kRefreshDenominator, kQpcFrequency, gappedResult),
          "observer gapがある列をcadence整合と認めました");
}

void bracketing() {
    const auto samples = outputVBlanks(10);
    long long ordinal = -1;

    check(mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), samples[3].qpc, ordinal) ==
                  VBlankBracketStatus::Ok &&
              ordinal == 3,
          "VBlank境界ちょうどのswapを下側opportunityへ帰属できません");
    check(mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), samples[3].qpc + 10,
                                        ordinal) == VBlankBracketStatus::Ok &&
              ordinal == 3,
          "VBlank間のswapを一意にbracketできません");
    check(mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), samples[4].qpc - 1,
                                        ordinal) == VBlankBracketStatus::Ok &&
              ordinal == 3,
          "上側VBlank直前のswapの帰属が違います");

    check(mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), samples[0].qpc - 1,
                                        ordinal) == VBlankBracketStatus::BeforeFirst,
          "observer開始前のswapを解決してはいけません");
    check(mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), samples.back().qpc,
                                        ordinal) == VBlankBracketStatus::AfterLast,
          "上側VBlank未観測のswapを先に解決してはいけません");

    auto gapped = samples;
    gapped.erase(gapped.begin() + 4);
    check(mvm::gpu::bracketSwapToVBlank(gapped.data(), gapped.size(), samples[4].qpc + 10,
                                        ordinal) == VBlankBracketStatus::Gap,
          "observer gap内のswapをfail-closedにできません");
}

// A2 formal Playback run 1で観測した実sequence。DWM composition clockは
// swapあたり約2 tick進むが、window outputのVBlankは1本しか跨がない。
void dwmClockDoesNotAffectWindowOpportunity() {
    const auto samples = outputVBlanks(64);
    const long long period = (kQpcFrequency * kRefreshDenominator) / kRefreshNumerator;
    long long previousOrdinal = -1;
    long long advanced = 0;
    long long swaps = 0;
    // 16.68 ms間隔のswapを32本。実測A2と同じcadence。
    for (long long index = 0; index < 32; ++index) {
        const long long swapQpc = samples[0].qpc + index * period + period / 3;
        long long ordinal = -1;
        const auto status =
            mvm::gpu::bracketSwapToVBlank(samples.data(), samples.size(), swapQpc, ordinal);
        check(status == VBlankBracketStatus::Ok, "実測cadenceのswapをbracketできません");
        if (previousOrdinal >= 0)
            advanced += ordinal - previousOrdinal;
        previousOrdinal = ordinal;
        ++swaps;
    }
    check(swaps == 32 && advanced == 31,
          "window output VBlankが1本なのにopportunityが2進みました "
          "(DWM composition clockをordinal authorityへ再接続するmutation)");
}

// ordinalは自前counterなので、observerがVBlankを取りこぼしても連続に見える。
// interval側から検出できることを固定する。
void intervalAnomalyDetectsMissedVBlank() {
    const auto good = outputVBlanks(120);
    mvm::gpu::VBlankIntervalReport report;
    check(mvm::gpu::vblankIntervalReport(good.data(), good.size(), kRefreshNumerator,
                                         kRefreshDenominator, kQpcFrequency, report),
          "正常なVBlank列のinterval reportを作れません");
    check(report.intervalCount == 119 && report.longIntervalCount == 0,
          "正常cadenceを取りこぼし疑いとして数えました");

    // 1本取りこぼした列。ordinalは連番のままなのでsequence statusでは見えない。
    std::vector<VBlankObservation> missed;
    const long long period = (kQpcFrequency * kRefreshDenominator) / kRefreshNumerator;
    for (long long index = 0; index < 120; ++index) {
        const long long skew = index >= 60 ? period : 0;
        missed.push_back({index, 1000000 + index * period + skew});
    }
    check(mvm::gpu::vblankSequenceStatus(missed.data(), missed.size()) == VBlankSequenceStatus::Ok,
          "取りこぼし列はordinal連続性では検出できないはずです");
    mvm::gpu::VBlankIntervalReport missedReport;
    check(mvm::gpu::vblankIntervalReport(missed.data(), missed.size(), kRefreshNumerator,
                                         kRefreshDenominator, kQpcFrequency, missedReport),
          "取りこぼし列のinterval reportを作れません");
    check(missedReport.longIntervalCount == 1,
          "observerのVBlank取りこぼしをinterval側で検出できません");
}

// F3-B0: swap列をphysical VBlankへ一意にmappingできることを固定する。
void shadowMappingClassifiesEverySwap() {
    const auto samples = outputVBlanks(64);
    const long long period = (kQpcFrequency * kRefreshDenominator) / kRefreshNumerator;
    std::vector<long long> swapQpc;
    std::vector<long long> swapOrdinal;
    // 通常cadenceのswapを10本。
    for (long long index = 0; index < 10; ++index) {
        swapQpc.push_back(samples[0].qpc + index * period + period / 3);
        swapOrdinal.push_back(index);
    }
    // 同一physical VBlank内の2本目。
    swapQpc.push_back(samples[0].qpc + 9 * period + period / 2);
    swapOrdinal.push_back(10);
    std::vector<mvm::gpu::SwapMappingRecord> out(swapQpc.size());
    mvm::gpu::SwapMappingReport report;
    check(mvm::gpu::mapSwapsToVBlanks(samples.data(), samples.size(), swapQpc.data(),
                                      swapOrdinal.data(), swapQpc.size(), out.data(), report),
          "shadow mappingを実行できません");
    check(report.mappedCount == 10 && report.sameOpportunityCount == 1 &&
              report.ambiguousCount == 0 && report.observerGapCount == 0 &&
              report.afterLastCount == 0 && report.beforeFirstCount == 0,
          "swapのphysical opportunity分類が違います");
    check(out.back().status == mvm::gpu::SwapMappingStatus::SameOpportunity &&
              out.back().opportunityOrdinal == out[9].opportunityOrdinal,
          "同一physical VBlank内の2本目をsupersede候補として分類できません");

    // observer未観測の上側VBlankは解決しない。QPC補間で救済してはいけない。
    std::vector<long long> lateSwap{samples.back().qpc + 10};
    std::vector<mvm::gpu::SwapMappingRecord> lateOut(1);
    mvm::gpu::SwapMappingReport lateReport;
    check(mvm::gpu::mapSwapsToVBlanks(samples.data(), samples.size(), lateSwap.data(), nullptr, 1,
                                      lateOut.data(), lateReport),
          "AfterLast caseのmappingを実行できません");
    check(lateReport.afterLastCount == 1 && lateReport.mappedCount == 0,
          "upper bracket未観測のswapを解決してしまいました");

    // observer gap内のswapはfail-closed。
    auto gapped = samples;
    gapped.erase(gapped.begin() + 20);
    std::vector<long long> gapSwap{samples[20].qpc + period / 3};
    std::vector<mvm::gpu::SwapMappingRecord> gapOut(1);
    mvm::gpu::SwapMappingReport gapReport;
    check(mvm::gpu::mapSwapsToVBlanks(gapped.data(), gapped.size(), gapSwap.data(), nullptr, 1,
                                      gapOut.data(), gapReport),
          "observer gap caseのmappingを実行できません");
    check(gapReport.observerGapCount == 1 && gapReport.mappedCount == 0,
          "observer gap内のswapをQPC補間で救済してしまいました");
}

// 短すぎるintervalも隣接VBlankと断定できない。長短を対称にfail-closedにする。
void shortIntervalIsAlsoAuthorityInvalid() {
    const long long period = (kQpcFrequency * kRefreshDenominator) / kRefreshNumerator;
    std::vector<VBlankObservation> samples;
    for (long long index = 0; index < 120; ++index) {
        // 通常優先度で実測した0.29周期のshort intervalを再現する。
        const long long skew = index == 60 ? -(period * 71) / 100 : 0;
        samples.push_back({index, 1000000 + index * period + skew});
    }
    mvm::gpu::VBlankIntervalReport report;
    check(mvm::gpu::vblankIntervalReport(samples.data(), samples.size(), kRefreshNumerator,
                                         kRefreshDenominator, kQpcFrequency, report),
          "short interval列のreportを作れません");
    check(report.shortIntervalCount == 1,
          "0.5周期未満のintervalをauthority invalidとして数えません");
    check(report.longIntervalCount == 1, "short intervalの直前に生じる長いintervalを数えません");
}

void ringPublishesInOrder() {
    mvm::gpu::VBlankRing ring;
    ring.reset();
    for (long long index = 0; index < 500; ++index)
        ring.capture({index, 1000 + index * 10});
    const auto snapshot = ring.snapshot();
    check(snapshot.size() == 500 && ring.overflowCount() == 0, "ringのpublish数が違います");
    check(mvm::gpu::vblankSequenceStatus(snapshot.data(), snapshot.size()) ==
              VBlankSequenceStatus::Ok,
          "ringが単調なVBlank列を保てていません");
}

// W2-A.1。publish serialはringのlifetimeを通じて単調で、resetでも戻らない。
// これが無いと「reset前のstale sampleが1件あるからprerollできている」という
// 誤判定が起こりうる。count / overflow はresetで0に戻ることも同時に固定する。
void publishSerialIsMonotonicAcrossReset() {
    mvm::gpu::VBlankRing ring;
    ring.reset();
    check(ring.publishSerial() == 0, "初期publish serialが0ではありません");
    for (long long index = 0; index < 10; ++index)
        ring.capture({index, 1000 + index * 10});
    const auto beforeReset = ring.publishSerial();
    check(beforeReset == 10, "publish serialがcapture数と一致しません");
    ring.reset();
    check(ring.publishedCount() == 0, "resetでcountが0に戻っていません");
    check(ring.overflowCount() == 0, "resetでoverflowが0に戻っていません");
    check(ring.publishSerial() == beforeReset, "resetでpublish serialが戻っています");
    ring.capture({0, 5000});
    check(ring.publishSerial() == beforeReset + 1, "reset後のcaptureでserialが進みません");
    // baseline(reset前) との比較で「新しくpublishされた」を判定できる。
    check(ring.publishSerial() > beforeReset, "baseline比較でprerollを判定できません");
}

} // namespace

int main() {
    sequenceStatus();
    cadenceGate();
    bracketing();
    intervalAnomalyDetectsMissedVBlank();
    shortIntervalIsAlsoAuthorityInvalid();
    shadowMappingClassifiesEverySwap();
    dwmClockDoesNotAffectWindowOpportunity();
    ringPublishesInOrder();
    publishSerialIsMonotonicAcrossReset();
    std::fprintf(stderr, "P2-D5-2/F3 window output VBlank authority: 失敗 %d件\n", failures);
    return failures == 0 ? 0 : 1;
}
