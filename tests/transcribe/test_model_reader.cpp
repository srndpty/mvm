#include "media/transcribe/model_reader.h"

#include <array>
#include <cstdlib>
#include <memory>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(std::tmpfile(), std::fclose);
    require(static_cast<bool>(file), "検査用ファイルを開く");
    const std::int32_t expected = 123456;
    require(std::fwrite(&expected, sizeof(expected), 1, file.get()) == 1, "対照群を書く");
    std::rewind(file.get());
    std::int32_t value = 0;
    require(mvm::transcribe::detail::readModelBytes(file.get(), &value, sizeof(value)) == 4 &&
                value == expected && !std::feof(file.get()),
            "最終データを読み終えただけではEOFにしない");
    for (int index = 0; index < 3; ++index)
        require(mvm::transcribe::detail::readModelBytes(file.get(), &value, sizeof(value)) == 0 &&
                    std::feof(file.get()) && value == 0,
                "Whisperの末尾の試し読みを正常なEOFとして扱う");
    bool rejected = false;
    std::array<char, 8> tensor{};
    try {
        mvm::transcribe::detail::readModelBytes(file.get(), tensor.data(), tensor.size());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "tensor本体の欠損はEOFでも拒否する");
    std::rewind(file.get());
    require(mvm::transcribe::detail::readModelBytes(file.get(), tensor.data(), 2) == 2,
            "部分ヘッダーの対照群");
    rejected = false;
    try {
        mvm::transcribe::detail::readModelBytes(file.get(), &value, sizeof(value));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "途中で切れたヘッダーを正常なEOFとして扱わない");
    std::puts("モデル末尾と途中の欠損の区別を検査しました");
}
