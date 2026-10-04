#include "media/manim/manim_fingerprint.h"

#include "util/mvm_sha256.h"
#include "util/mvm_win_utf8.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace mvm::manim {
namespace {

struct Sha256Deleter {
    void operator()(MvmSha256* hash) const { mvm_sha256_destroy(hash); }
};

std::string pathToUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
    return result;
}

std::string statusText(long status) {
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

    long status = 0;
    const std::unique_ptr<MvmSha256, Sha256Deleter> hash(mvm_sha256_create(&status));
    if (!hash) {
        result.error = "SHA-256 hash を作成できません: " + statusText(status);
        return result;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            status = mvm_sha256_update(hash.get(), buffer.data(), static_cast<std::size_t>(count));
            if (status != 0) {
                result.error = "Manim script の SHA-256 計算に失敗しました: " + statusText(status);
                return result;
            }
        }
    }
    if (input.bad()) {
        result.error = "Manim script の読み取り中に失敗しました: " + pathToUtf8(scriptPath);
        return result;
    }

    char digest[MVM_SHA256_HEX_SIZE] = {};
    status = mvm_sha256_finish_hex(hash.get(), digest);
    if (status != 0) {
        result.error = "SHA-256 digest を確定できません: " + statusText(status);
        return result;
    }
    result.fingerprint = digest;
    result.success = true;
    return result;
}

} // namespace mvm::manim
