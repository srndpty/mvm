#include "project/path_identity.h"

#include "util/mvm_file_identity.h"

#include <system_error>

namespace mvm::project {

std::wstring canonicalPathKey(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error)
        return {};
    return absolute.generic_wstring();
}

FileIdentityKey fileIdentityKey(const std::filesystem::path& path) {
    FileIdentityKey result;
    result.pathKey = canonicalPathKey(path);
    MvmFileIdentity identity{};
    switch (mvm_file_identity_probe(path.c_str(), &identity)) {
    case MVM_FILE_IDENTITY_OK: {
        // size / 更新時刻は同一性に含めない。同じファイルが書き換わっても同じ実体である。
        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        result.fileKey = std::to_wstring(identity.volume_serial) + L':';
        for (const unsigned char byte : identity.file_id) {
            result.fileKey += kHex[byte >> 4];
            result.fileKey += kHex[byte & 0x0f];
        }
        result.status = FileIdentityStatus::FileId;
        break;
    }
    case MVM_FILE_IDENTITY_MISSING:
        result.status = FileIdentityStatus::Missing;
        break;
    case MVM_FILE_IDENTITY_UNAVAILABLE:
        result.status = FileIdentityStatus::Unavailable;
        break;
    }
    return result;
}

PathSameness comparePathIdentity(const FileIdentityKey& left, const FileIdentityKey& right) {
    if (left.pathKey.empty() || right.pathKey.empty())
        return PathSameness::Unknown;
    // 実体が取れているなら表記より実体で決める。表記の一致だけで Same にすると、
    // 実体の違いを見落とす経路が残る。
    if (left.status == FileIdentityStatus::FileId && right.status == FileIdentityStatus::FileId)
        return left.fileKey == right.fileKey ? PathSameness::Same : PathSameness::Different;
    if (left.pathKey == right.pathKey)
        return PathSameness::Same;
    if (left.status == FileIdentityStatus::Unavailable ||
        right.status == FileIdentityStatus::Unavailable)
        return PathSameness::Unknown;
    // 少なくとも片方は何も指していない。表記も違うので同じ実体ではない。
    return PathSameness::Different;
}

PathSameness comparePathIdentity(const std::filesystem::path& left,
                                 const std::filesystem::path& right) {
    return comparePathIdentity(fileIdentityKey(left), fileIdentityKey(right));
}

bool mayAdoptLegacyCaseSpelling(const FileIdentityKey& clip, const FileIdentityKey& item) {
    if (clip.status == FileIdentityStatus::FileId && item.status == FileIdentityStatus::FileId)
        return clip.fileKey == item.fileKey;
    return clip.status == FileIdentityStatus::Missing && item.status == FileIdentityStatus::Missing;
}

} // namespace mvm::project
