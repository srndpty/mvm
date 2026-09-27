#include "project/path_identity.h"

#include "util/mvm_file_identity.h"

#include <cwctype>
#include <system_error>

namespace mvm::project {

std::wstring canonicalPathKey(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error)
        return {};
    auto text = absolute.generic_wstring();
    for (auto& character : text)
        character = static_cast<wchar_t>(std::towlower(character));
    return L"path:" + text;
}

std::wstring mediaFileKey(const std::filesystem::path& path) {
    MvmFileIdentity identity{};
    if (mvm_file_identity_query(path.c_str(), &identity) != 0)
        return canonicalPathKey(path);
    // size / 更新時刻は同一性に含めない。同じファイルが書き換わっても同じ素材である。
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring key = L"file:" + std::to_wstring(identity.volume_serial) + L':';
    for (const unsigned char byte : identity.file_id) {
        key += kHex[byte >> 4];
        key += kHex[byte & 0x0f];
    }
    return key;
}

} // namespace mvm::project
