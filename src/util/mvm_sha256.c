#include "util/mvm_sha256.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdlib.h>

#define MVM_SHA256_DIGEST_SIZE 32

struct MvmSha256 {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
    unsigned char* object;
    int finished;
};

/* 呼び出し側の誤用 (NULL・確定後の update) に返す値。CNG の STATUS_INVALID_PARAMETER と同じ。 */
static const long kInvalidParameter = (long)0xC000000DL;

void mvm_sha256_destroy(MvmSha256* hash) {
    if (!hash)
        return;
    if (hash->hash)
        BCryptDestroyHash(hash->hash);
    if (hash->algorithm)
        BCryptCloseAlgorithmProvider(hash->algorithm, 0);
    free(hash->object);
    free(hash);
}

MvmSha256* mvm_sha256_create(long* status) {
    long ignored = 0;
    if (!status)
        status = &ignored;
    *status = 0;
    MvmSha256* hash = (MvmSha256*)calloc(1, sizeof(*hash));
    if (!hash) {
        *status = (long)0xC0000017L; /* STATUS_NO_MEMORY */
        return NULL;
    }
    NTSTATUS result =
        BCryptOpenAlgorithmProvider(&hash->algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    DWORD object_length = 0;
    DWORD digest_length = 0;
    ULONG received = 0;
    if (BCRYPT_SUCCESS(result))
        result = BCryptGetProperty(hash->algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_length,
                                   sizeof(object_length), &received, 0);
    if (BCRYPT_SUCCESS(result))
        result = BCryptGetProperty(hash->algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&digest_length,
                                   sizeof(digest_length), &received, 0);
    if (BCRYPT_SUCCESS(result) && digest_length != MVM_SHA256_DIGEST_SIZE)
        result = (NTSTATUS)kInvalidParameter;
    if (BCRYPT_SUCCESS(result)) {
        hash->object = (unsigned char*)malloc(object_length);
        if (!hash->object)
            result = (NTSTATUS)0xC0000017L;
    }
    if (BCRYPT_SUCCESS(result))
        result =
            BCryptCreateHash(hash->algorithm, &hash->hash, hash->object, object_length, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(result)) {
        *status = (long)result;
        mvm_sha256_destroy(hash);
        return NULL;
    }
    return hash;
}

long mvm_sha256_update(MvmSha256* hash, const void* data, size_t size) {
    if (!hash || hash->finished || (size > 0 && !data))
        return kInvalidParameter;
    const unsigned char* bytes = (const unsigned char*)data;
    /* BCryptHashData の長さは ULONG なので、4GiB を超える入力は分けて渡す。 */
    while (size > 0) {
        const ULONG chunk = size > 0x40000000u ? 0x40000000u : (ULONG)size;
        const NTSTATUS result = BCryptHashData(hash->hash, (PUCHAR)bytes, chunk, 0);
        if (!BCRYPT_SUCCESS(result))
            return (long)result;
        bytes += chunk;
        size -= chunk;
    }
    return 0;
}

long mvm_sha256_finish_hex(MvmSha256* hash, char out[MVM_SHA256_HEX_SIZE]) {
    if (!hash || hash->finished || !out)
        return kInvalidParameter;
    unsigned char digest[MVM_SHA256_DIGEST_SIZE];
    const NTSTATUS result = BCryptFinishHash(hash->hash, digest, sizeof(digest), 0);
    hash->finished = 1;
    if (!BCRYPT_SUCCESS(result))
        return (long)result;
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < MVM_SHA256_DIGEST_SIZE; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    out[MVM_SHA256_DIGEST_SIZE * 2] = '\0';
    return 0;
}

long mvm_sha256_hex(const void* data, size_t size, char out[MVM_SHA256_HEX_SIZE]) {
    long status = 0;
    MvmSha256* hash = mvm_sha256_create(&status);
    if (!hash)
        return status;
    status = mvm_sha256_update(hash, data, size);
    if (status == 0)
        status = mvm_sha256_finish_hex(hash, out);
    mvm_sha256_destroy(hash);
    return status;
}
