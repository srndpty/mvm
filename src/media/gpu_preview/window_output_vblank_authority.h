#ifndef MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_AUTHORITY_H
#define MVM_GPU_PREVIEW_WINDOW_OUTPUT_VBLANK_AUTHORITY_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mvm::gpu {

// P2-D5-2/F3。formal presentation opportunityのauthorityは、windowが載っている
// 実display outputのphysical VBlank sequenceと、同じoutputのQueryDisplayConfig
// refresh rationalに限る。DwmGetCompositionTimingInfo(NULL)のcRefresh /
// rateRefreshはcomposition clockでありwindow outputとは限らないため、
// diagnostic-onlyとして扱う。
struct WindowOutputIdentity {
    bool available = false;
    std::uint64_t monitorHandle = 0;
    std::uint32_t outputIndex = 0;
    std::int64_t adapterLuidLow = 0;
    std::int64_t adapterLuidHigh = 0;
    std::string gdiDeviceName;
    std::string outputDeviceName;
    long long refreshNumerator = 0;
    long long refreshDenominator = 0;
    long long desktopLeft = 0;
    long long desktopTop = 0;
    long long desktopRight = 0;
    long long desktopBottom = 0;
};

bool sameWindowOutput(const WindowOutputIdentity& left, const WindowOutputIdentity& right);

struct VBlankObservation {
    long long ordinal = -1;
    long long qpc = 0;
};

constexpr std::size_t kVBlankRingCapacity = 32768;

// VBlank observer threadがsingle writerで書き込む。hot pathでallocation、
// mutex、logging、file I/Oを行わない。
class VBlankRing {
public:
    void reset();
    void capture(const VBlankObservation& value);
    std::vector<VBlankObservation> snapshot() const;
    std::size_t publishedCount() const;
    long long overflowCount() const;

private:
    std::array<VBlankObservation, kVBlankRingCapacity> samples_{};
    std::atomic<std::size_t> count_{0};
    std::atomic<long long> overflow_{0};
};

enum class VBlankSequenceStatus {
    Ok,
    Empty,
    Invalid,
    OrdinalRegression,
    OrdinalGap,
    QpcRegression,
};

// observerが取りこぼしなく単調に記録できているかを検査する。
VBlankSequenceStatus vblankSequenceStatus(const VBlankObservation* samples, std::size_t count);

struct VBlankCadenceResult {
    bool consistent = false;
    long long observedIntervals = 0;
    long long elapsedQpc = 0;
    // observed - expected をVBlank単位でscaleした差の分子。分母はtoleranceUnit。
    long long deviationNumerator = 0;
    long long toleranceUnit = 0;
};

// 観測したVBlank数と、経過QPC×refresh rationalから期待されるVBlank数の差が
// 1 VBlank以内であることを、浮動小数を使わずに検査する。新しい性能thresholdは
// 導入しない。
bool vblankCadenceConsistent(const VBlankObservation* samples, std::size_t count,
                             long long refreshNumerator, long long refreshDenominator,
                             long long qpcFrequency, VBlankCadenceResult& result);

struct VBlankIntervalReport {
    long long intervalCount = 0;
    // 公称周期の1.5倍以上に伸びたinterval。observerがVBlankを取りこぼすと
    // ordinalは自前のcounterなので静かにずれるため、間隔側から検出する。
    long long longIntervalCount = 0;
    // 0.5倍未満に縮んだinterval。直前のwakeが遅れた証拠であり、隣接VBlankと
    // 断定できない。midpoint criterionであって性能thresholdではない。
    long long shortIntervalCount = 0;
    long long maxIntervalQpc = 0;
    long long minIntervalQpc = 0;
    long long nominalPeriodQpc = 0;
    // origin基準の累積ずれ。self-counterはmissed VBlankを自分では知れないため、
    // intervalと累積の両方を残す。
    long long cumulativeDeviationNumerator = 0;
    long long cumulativeToleranceUnit = 0;
    bool cumulativeConsistent = false;
};

// 取りこぼし疑いのあるintervalを数える。ordinalの連続性だけでは検出できない。
bool vblankIntervalReport(const VBlankObservation* samples, std::size_t count,
                          long long refreshNumerator, long long refreshDenominator,
                          long long qpcFrequency, VBlankIntervalReport& report);

enum class SwapMappingStatus {
    Mapped,
    SameOpportunity,
    AfterLast,
    BeforeFirst,
    ObserverGap,
    Ambiguous,
};

struct SwapMappingRecord {
    long long swapOrdinal = -1;
    long long swapQpc = 0;
    long long opportunityOrdinal = -1;
    SwapMappingStatus status = SwapMappingStatus::Ambiguous;
};

struct SwapMappingReport {
    long long mappedCount = 0;
    long long sameOpportunityCount = 0;
    long long afterLastCount = 0;
    long long beforeFirstCount = 0;
    long long observerGapCount = 0;
    long long ambiguousCount = 0;
    long long distinctOpportunityCount = 0;
    long long firstOpportunityOrdinal = -1;
    long long lastOpportunityOrdinal = -1;
};

// swap列をphysical VBlank列へoffline mappingし、全件分類する。
// QPC補間による救済は行わない。
bool mapSwapsToVBlanks(const VBlankObservation* samples, std::size_t sampleCount,
                       const long long* swapQpc, const long long* swapOrdinal,
                       std::size_t swapCount, SwapMappingRecord* out, SwapMappingReport& report);

const char* swapMappingStatusName(SwapMappingStatus status);

enum class VBlankBracketStatus {
    Ok,
    BeforeFirst,
    AfterLast,
    Gap,
    InvalidSequence,
};

// swapQpcを V_k.qpc <= swapQpc < V_(k+1).qpc で一意にbracketし、そのopportunity
// ordinalを返す。上側のVBlankがまだ観測されていない間は解決しない
// (AfterLast)。これによりDWM composition clockのtickは一切入らない。
VBlankBracketStatus bracketSwapToVBlank(const VBlankObservation* samples, std::size_t count,
                                        long long swapQpc, long long& ordinal);

} // namespace mvm::gpu

#endif
