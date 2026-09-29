#ifndef MVM_CORE_TIMECODE_H
#define MVM_CORE_TIMECODE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mvm::core {

std::string formatTimecode(std::int64_t frames, std::int64_t fpsNum, std::int64_t fpsDen);
std::optional<std::int64_t> parseTimecode(std::string_view text, std::int64_t fpsNum,
                                          std::int64_t fpsDen);

} // namespace mvm::core

#endif
