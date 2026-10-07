#ifndef MVM_CORE_TEXT_OFFSETS_H
#define MVM_CORE_TEXT_OFFSETS_H
#include <cstddef>
#include <optional>
#include <string_view>

namespace mvm::core {
// 書記素ではなく byte / code unit の境界。文字列全体が正しい UTF-8 でなければ失敗する。
std::optional<std::size_t> utf8ToUtf16Offset(std::string_view, std::size_t byteOffset);
std::optional<std::size_t> utf16ToUtf8Offset(std::string_view, std::size_t unitOffset);
} // namespace mvm::core
#endif
