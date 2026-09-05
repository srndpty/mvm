#ifndef MVM_CORE_CHECKED_INTEGER_H
#define MVM_CORE_CHECKED_INTEGER_H

#include <cstdint>
#include <limits>

namespace mvm::core {

inline bool checkedAdd(std::int64_t left, std::int64_t right, std::int64_t& result) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
        return false;
    result = left + right;
    return true;
}

inline bool checkedSubtract(std::int64_t left, std::int64_t right, std::int64_t& result) {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right) ||
        (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right))
        return false;
    result = left - right;
    return true;
}

} // namespace mvm::core

#endif // MVM_CORE_CHECKED_INTEGER_H
