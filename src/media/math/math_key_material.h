#ifndef MVM_MEDIA_MATH_MATH_KEY_MATERIAL_H
#define MVM_MEDIA_MATH_MATH_KEY_MATERIAL_H

// cache key の正準形を組む部品 (src/media/math の内部用)。静止・連番・変形の key が共有する。

#include <string>

namespace mvm::math::detail {

// "<name>=<byte 数>:<value>\n" を足す。
void appendKeyField(std::string& material, const char* name, const std::string& value);

// material の SHA-256 (小文字 16 進 64 桁)。失敗 (CNG の失敗) なら空文字列。
std::string keyDigest(const std::string& material);

} // namespace mvm::math::detail

#endif // MVM_MEDIA_MATH_MATH_KEY_MATERIAL_H
