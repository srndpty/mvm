#include "core/text_offsets.h"

#include <cstdint>

namespace mvm::core {
namespace {
std::optional<std::size_t> convert(std::string_view text, std::size_t offset, bool toUnits) {
    std::size_t bytes = 0, units = 0;
    std::optional<std::size_t> result;
    while (true) {
        if ((toUnits ? bytes : units) == offset)
            result = toUnits ? units : bytes;
        if (bytes == text.size())
            return result;
        const auto first = static_cast<unsigned char>(text[bytes]);
        std::size_t count = 0;
        std::uint32_t cp = 0, minimum = 0;
        if (first < 0x80) {
            count = 1;
            cp = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            count = 2;
            cp = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 3;
            cp = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 4;
            cp = first & 7;
            minimum = 0x10000;
        } else
            return std::nullopt;
        if (count > text.size() - bytes)
            return std::nullopt;
        for (std::size_t j = 1; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[bytes + j]);
            if ((next & 0xc0) != 0x80)
                return std::nullopt;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return std::nullopt;
        bytes += count;
        units += cp > 0xffff ? 2 : 1;
    }
}
} // namespace

std::optional<std::size_t> utf8ToUtf16Offset(std::string_view text, std::size_t offset) {
    return convert(text, offset, true);
}

std::optional<std::size_t> utf16ToUtf8Offset(std::string_view text, std::size_t offset) {
    return convert(text, offset, false);
}
} // namespace mvm::core
