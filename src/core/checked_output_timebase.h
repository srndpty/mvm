#ifndef MVM_CORE_CHECKED_OUTPUT_TIMEBASE_H
#define MVM_CORE_CHECKED_OUTPUT_TIMEBASE_H

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mvm::core {

enum class OutputTimebaseError {
    InvalidNumerator,
    InvalidDenominator,
    InvalidSampleRate,
    UnsupportedProductRate,
    IntermediateOverflow,
    ResultOverflow,
};

template<typename T>
struct OutputTimebaseResult {
    std::optional<T> storage;
    OutputTimebaseError error = OutputTimebaseError::InvalidNumerator;

    explicit operator bool() const { return storage.has_value(); }

    const T& value() const { return *storage; }

    T& value() { return *storage; }

    static OutputTimebaseResult success(T result) {
        OutputTimebaseResult out;
        out.storage.emplace(std::move(result));
        return out;
    }

    static OutputTimebaseResult failure(OutputTimebaseError failure) {
        OutputTimebaseResult out;
        out.error = failure;
        return out;
    }
};

struct SupportedFrameRate {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    bool operator==(const SupportedFrameRate&) const = default;
};

// **measured と configurable を混ぜない。**
//
//   measured     canonical preview workload を実測して qualify した rate。
//                「出る」と書いてよいのはこれだけである。
//   configurable product が設定として受理する rate。measured を含むが、
//                measured でない rate も含む。受理すること自体は
//                qualification ではない。
//
// 実装が受理するようになったことを根拠に measured へ昇格させてはいけない。
// 昇格は canonical workload の実測 (p1-matrix.ps1 相当) を取ってから行う。
const std::vector<SupportedFrameRate>& measuredOutputFrameRates();
bool isMeasuredOutputFrameRate(std::int64_t numerator, std::int64_t denominator);
const std::vector<SupportedFrameRate>& configurableOutputFrameRates();
bool isConfigurableOutputFrameRate(std::int64_t numerator, std::int64_t denominator);

// qualified な audio sample rate。timebase と Project の両方がこれを基準にする。
inline constexpr std::int64_t kQualifiedAudioSampleRate = 48000;

struct CanonicalRational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    bool operator==(const CanonicalRational&) const = default;
};

class CheckedOutputTimebase {
public:
    // create() は正の入力rationalを約分し、内部ではcanonicalな値だけを保持する。
    static OutputTimebaseResult<CheckedOutputTimebase> create(std::int64_t frameRateNumerator,
                                                              std::int64_t frameRateDenominator,
                                                              std::int64_t audioSampleRate);
    // measured な rate だけを受理する。qualification authority はこちらである。
    static OutputTimebaseResult<CheckedOutputTimebase>
    createQualified(std::int64_t frameRateNumerator, std::int64_t frameRateDenominator,
                    std::int64_t audioSampleRate);
    // configurable な rate を受理する。measured とは限らないので、これを通ったことを
    // 「qualify されている」と読み替えてはいけない。
    static OutputTimebaseResult<CheckedOutputTimebase>
    createConfigured(std::int64_t frameRateNumerator, std::int64_t frameRateDenominator,
                     std::int64_t audioSampleRate);

    CanonicalRational frameRate() const;
    std::int64_t audioSampleRate() const;

    OutputTimebaseResult<CanonicalRational> exactMediaTime(std::int64_t outputFrame) const;
    OutputTimebaseResult<std::int64_t> firstAudioSample(std::int64_t outputFrame) const;
    OutputTimebaseResult<std::int64_t> outputFrame(std::int64_t audioSample) const;

    // wall-clock経過nanosecondからoutput frameを導出する。audio masterが無い経路
    // (P5-C video-only)も独自の有理数演算を持たず、この換算へ委譲する。
    OutputTimebaseResult<std::int64_t> outputFrameForNanoseconds(std::int64_t nanoseconds) const;

    // seek、scheduler、statusは別の式を持たず、上記の同じ演算へ委譲する。
    OutputTimebaseResult<std::int64_t> seekTargetSample(std::int64_t outputFrame) const;
    OutputTimebaseResult<std::int64_t> schedulerOutputFrame(std::int64_t audioSample) const;
    OutputTimebaseResult<std::int64_t> statusOutputFrame(std::int64_t audioSample) const;

private:
    CheckedOutputTimebase(std::int64_t frameRateNumerator, std::int64_t frameRateDenominator,
                          std::int64_t audioSampleRate);

    std::int64_t frameRateNumerator_ = 1;
    std::int64_t frameRateDenominator_ = 1;
    std::int64_t audioSampleRate_ = 1;
};

const CheckedOutputTimebase& qualifiedOutputTimebase();

} // namespace mvm::core

#endif // MVM_CORE_CHECKED_OUTPUT_TIMEBASE_H
