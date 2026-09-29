#include "core/timecode.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mvm::core {
namespace {
std::int64_t nominalFps(std::int64_t num, std::int64_t den) {
    if (num <= 0 || den <= 0)
        return 0;
    return static_cast<std::int64_t>(
        std::llround(static_cast<double>(num) / static_cast<double>(den)));
}
} // namespace

std::string formatTimecode(std::int64_t frames, std::int64_t fpsNum, std::int64_t fpsDen) {
    const auto fps = nominalFps(fpsNum, fpsDen);
    if (fps <= 0)
        return {};
    frames = std::max<std::int64_t>(0, frames);
    const auto seconds = frames / fps;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                  static_cast<long long>(seconds / 3600),
                  static_cast<long long>((seconds / 60) % 60), static_cast<long long>(seconds % 60),
                  static_cast<long long>(frames % fps));
    return buffer;
}

std::optional<std::int64_t> parseTimecode(std::string_view text, std::int64_t fpsNum,
                                          std::int64_t fpsDen) {
    const auto fps = nominalFps(fpsNum, fpsDen);
    if (fps <= 0)
        return std::nullopt;
    std::int64_t parts[4]{};
    for (int index = 0; index < 4; ++index) {
        const auto end = index == 3 ? text.size() : text.find(':');
        if (end == std::string_view::npos || end == 0)
            return std::nullopt;
        const auto* first = text.data();
        const auto parsed = std::from_chars(first, first + end, parts[index]);
        if (parsed.ec != std::errc{} || parsed.ptr != first + end || parts[index] < 0)
            return std::nullopt;
        text.remove_prefix(index == 3 ? end : end + 1);
    }
    if (!text.empty() || parts[1] >= 60 || parts[2] >= 60 || parts[3] >= fps)
        return std::nullopt;
    __extension__ using WideInteger = __int128;
    const WideInteger frames =
        (((static_cast<WideInteger>(parts[0]) * 60 + parts[1]) * 60 + parts[2]) * fps) + parts[3];
    if (frames > std::numeric_limits<std::int64_t>::max())
        return std::nullopt;
    return static_cast<std::int64_t>(frames);
}

} // namespace mvm::core
