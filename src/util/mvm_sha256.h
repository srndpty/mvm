/*
 * SHA-256 (Windows CNG)。結果は小文字 16 進 64 桁。
 *
 * Manim script の fingerprint と数式 clip の cache key が使う。mvm_file_identity の
 * FNV-1a と違い暗号学的 hash であり、値を永続化して比べる用途に使う。
 */

#ifndef MVM_SHA256_H
#define MVM_SHA256_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 終端を含む 16 進文字列の長さ。 */
#define MVM_SHA256_HEX_SIZE 65

typedef struct MvmSha256 MvmSha256;

/* 失敗した API の NTSTATUS は status へ返す (NULL 可)。失敗で NULL。 */
MvmSha256* mvm_sha256_create(long* status);

/* 成功で 0、失敗で NTSTATUS (負)。 */
long mvm_sha256_update(MvmSha256* hash, const void* data, size_t size);

/* 確定して out へ書く。以後 update できない。成功で 0、失敗で NTSTATUS (負)。 */
long mvm_sha256_finish_hex(MvmSha256* hash, char out[MVM_SHA256_HEX_SIZE]);

void mvm_sha256_destroy(MvmSha256* hash);

/* 1 回で済む入力の hash。成功で 0、失敗で NTSTATUS (負)。 */
long mvm_sha256_hex(const void* data, size_t size, char out[MVM_SHA256_HEX_SIZE]);

#ifdef __cplusplus
}
#endif

#endif
