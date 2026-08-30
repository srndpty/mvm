#include "media/manim/manim_fingerprint.h"

#include "util/mvm_win_utf8.h"

#include <windows.h>

#include <array>
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace mvm::manim {
namespace {

class AlgorithmHandle {
public:
    ~AlgorithmHandle() {
        if (handle)
            BCryptCloseAlgorithmProvider(handle, 0);
    }

    BCRYPT_ALG_HANDLE handle = nullptr;
};

class HashHandle {
public:
    ~HashHandle() {
        if (handle)
            BCryptDestroyHash(handle);
    }

    BCRYPT_HASH_HANDLE handle = nullptr;
};

bool failed(NTSTATUS status) {
    return status < 0;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
    return result;
}

std::string statusText(NTSTATUS status) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setfill('0') << std::setw(8)
         << static_cast<unsigned long>(status);
    return text.str();
}

} // namespace

ManimFingerprintResult fingerprintManimSource(const std::filesystem::path& scriptPath) {
    ManimFingerprintResult result;
    if (scriptPath.empty()) {
        result.error = "Manim script path が空です";
        return result;
    }

    std::ifstream input(scriptPath, std::ios::binary);
    if (!input) {
        result.error = "fingerprint 対象の Manim script を読めません: " + pathToUtf8(scriptPath);
        return result;
    }

    AlgorithmHandle algorithm;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (failed(status)) {
        result.error = "SHA-256 provider を初期化できません: " + statusText(status);
        return result;
    }

    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD received = 0;
    status = BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH,
                               reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                               &received, 0);
    if (failed(status)) {
        result.error = "SHA-256 object length を取得できません: " + statusText(status);
        return result;
    }
    status =
        BCryptGetProperty(algorithm.handle, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0);
    if (failed(status) || hashLength != 32) {
        result.error = "SHA-256 digest length を取得できません";
        return result;
    }

    std::vector<unsigned char> hashObject(objectLength);
    std::vector<unsigned char> digest(hashLength);
    HashHandle hash;
    status = BCryptCreateHash(algorithm.handle, &hash.handle, hashObject.data(), objectLength,
                              nullptr, 0, 0);
    if (failed(status)) {
        result.error = "SHA-256 hash を作成できません: " + statusText(status);
        return result;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            status = BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()),
                                    static_cast<ULONG>(count), 0);
            if (failed(status)) {
                result.error = "Manim script の SHA-256 計算に失敗しました: " + statusText(status);
                return result;
            }
        }
    }
    if (input.bad()) {
        result.error = "Manim script の読み取り中に失敗しました: " + pathToUtf8(scriptPath);
        return result;
    }

    status = BCryptFinishHash(hash.handle, digest.data(), hashLength, 0);
    if (failed(status)) {
        result.error = "SHA-256 digest を確定できません: " + statusText(status);
        return result;
    }

    std::ostringstream fingerprint;
    fingerprint << std::hex << std::setfill('0');
    for (const unsigned char byte : digest)
        fingerprint << std::setw(2) << static_cast<unsigned>(byte);
    result.fingerprint = fingerprint.str();
    result.success = true;
    return result;
}

} // namespace mvm::manim
