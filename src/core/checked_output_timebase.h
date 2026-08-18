#ifndef MVM_CORE_CHECKED_OUTPUT_TIMEBASE_H
#define MVM_CORE_CHECKED_OUTPUT_TIMEBASE_H

#include <cstdint>
#include <optional>
#include <utility>

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
    static OutputTimebaseResult<CheckedOutputTimebase>
    createQualified(std::int64_t frameRateNumerator, std::int64_t frameRateDenominator,
                    std::int64_t audioSampleRate);

    CanonicalRational frameRate() const;
    std::int64_t audioSampleRate() const;

    OutputTimebaseResult<CanonicalRational> exactMediaTime(std::int64_t outputFrame) const;
    OutputTimebaseResult<std::int64_t> firstAudioSample(std::int64_t outputFrame) const;
    OutputTimebaseResult<std::int64_t> outputFrame(std::int64_t audioSample) const;

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
