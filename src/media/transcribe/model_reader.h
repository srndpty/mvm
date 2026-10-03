#ifndef MVM_TRANSCRIBE_MODEL_READER_H
#define MVM_TRANSCRIBE_MODEL_READER_H
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mvm::transcribe::detail {
inline std::size_t readModelBytes(FILE* file, void* output, std::size_t size) {
    const auto read = std::fread(output, 1, size, file);
    if (read == size)
        return read;
    // 固定版Whisperはtensor末尾の後に3個のint32を試し読みしてからEOFを検査する。
    // 正常な末尾だけを許可し、部分ヘッダーやtensor本体の欠損は拒否する。
    if (read == 0 && size == sizeof(std::int32_t) && std::feof(file) && !std::ferror(file)) {
        std::memset(output, 0, size);
        return 0;
    }
    throw std::runtime_error("認識モデルが途中で切れているか、読み取りに失敗しました");
}
} // namespace mvm::transcribe::detail
#endif
