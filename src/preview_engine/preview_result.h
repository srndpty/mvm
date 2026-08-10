#ifndef MVM_PREVIEW_ENGINE_PREVIEW_RESULT_H
#define MVM_PREVIEW_ENGINE_PREVIEW_RESULT_H

#include "preview_engine/preview_types.h"

#include <optional>
#include <utility>
#include <variant>

namespace mvm::preview {

template<typename T>
class PreviewResult {
public:
    static PreviewResult success(T value) { return PreviewResult(std::move(value)); }

    static PreviewResult failure(PreviewError error) { return PreviewResult(std::move(error)); }

    bool hasValue() const { return std::holds_alternative<T>(storage_); }

    explicit operator bool() const { return hasValue(); }

    T& value() { return std::get<T>(storage_); }

    const T& value() const { return std::get<T>(storage_); }

    PreviewError& error() { return std::get<PreviewError>(storage_); }

    const PreviewError& error() const { return std::get<PreviewError>(storage_); }

private:
    explicit PreviewResult(T value) : storage_(std::move(value)) {}

    explicit PreviewResult(PreviewError error) : storage_(std::move(error)) {}

    std::variant<T, PreviewError> storage_;
};

template<>
class PreviewResult<void> {
public:
    static PreviewResult success() { return PreviewResult(); }

    static PreviewResult failure(PreviewError error) { return PreviewResult(std::move(error)); }

    bool hasValue() const { return !error_.has_value(); }

    explicit operator bool() const { return hasValue(); }

    const PreviewError& error() const { return error_.value(); }

private:
    PreviewResult() = default;

    explicit PreviewResult(PreviewError error) : error_(std::move(error)) {}

    std::optional<PreviewError> error_;
};

template<typename T>
using Result = PreviewResult<T>;

} // namespace mvm::preview

#endif // MVM_PREVIEW_ENGINE_PREVIEW_RESULT_H
