#include "util/mvm_file_identity.h"

#include <windows.h>
#include <string.h>

static HANDLE open_shared(const wchar_t* path, DWORD access) {
    /* 他のアプリが書き込み中・rename 中でも読めるよう、共有はすべて許す。 */
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

MvmFileIdentityStatus mvm_file_identity_probe(const wchar_t* path, MvmFileIdentity* out) {
    if (!out)
        return MVM_FILE_IDENTITY_UNAVAILABLE;
    memset(out, 0, sizeof(*out));
    if (!path || !path[0])
        return MVM_FILE_IDENTITY_UNAVAILABLE;
    HANDLE file = open_shared(path, FILE_READ_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? MVM_FILE_IDENTITY_MISSING
                   : MVM_FILE_IDENTITY_UNAVAILABLE;
    }
    FILE_ID_INFO id;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    const BOOL ok =
        GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard));
    CloseHandle(file);
    if (!ok || standard.Directory)
        return MVM_FILE_IDENTITY_UNAVAILABLE;
    out->volume_serial = id.VolumeSerialNumber;
    memcpy(out->file_id, id.FileId.Identifier, sizeof(out->file_id));
    out->size = standard.EndOfFile.QuadPart;
    out->last_write_time = basic.LastWriteTime.QuadPart;
    return MVM_FILE_IDENTITY_OK;
}

int mvm_file_identity_query(const wchar_t* path, MvmFileIdentity* out) {
    return mvm_file_identity_probe(path, out) == MVM_FILE_IDENTITY_OK ? 0 : 1;
}

/* FNV-1a 64bit。 */
static unsigned long long fnv1a(unsigned long long hash, const unsigned char* data, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int read_at(HANDLE file, long long offset, unsigned char* buffer, DWORD size, DWORD* read) {
    LARGE_INTEGER position;
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN))
        return 1;
    return ReadFile(file, buffer, size, read, NULL) ? 0 : 1;
}

int mvm_file_content_hash(const wchar_t* path, unsigned long long* out) {
    return mvm_file_content_hash_cancellable(path, out, NULL, NULL) == MVM_FILE_HASH_OK ? 0 : 1;
}

MvmFileHashStatus mvm_file_content_hash_cancellable(const wchar_t* path, unsigned long long* out,
                                                    int (*should_stop)(void* opaque),
                                                    void* opaque) {
    if (!out)
        return MVM_FILE_HASH_IO_ERROR;
    *out = 0;
    if (!path || !path[0])
        return MVM_FILE_HASH_IO_ERROR;
    HANDLE file = open_shared(path, GENERIC_READ);
    if (file == INVALID_HANDLE_VALUE)
        return MVM_FILE_HASH_IO_ERROR;

    enum { CHUNK = 1024 * 1024 };

    /* worker thread から並行に呼ばれるので、static buffer は使わない。 */
    unsigned char* chunk = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, CHUNK);
    LARGE_INTEGER size;
    int failed = !chunk || !GetFileSizeEx(file, &size);
    unsigned long long hash = 14695981039346656037ULL;
    if (!failed)
        hash = fnv1a(hash, (const unsigned char*)&size.QuadPart, sizeof(size.QuadPart));
    long long total = 0;
    int cancelled = 0;
    while (!failed) {
        if (should_stop && should_stop(opaque)) {
            cancelled = 1;
            break;
        }
        DWORD read = 0;
        if (!ReadFile(file, chunk, CHUNK, &read, NULL)) {
            failed = 1;
            break;
        }
        if (read == 0)
            break;
        hash = fnv1a(hash, chunk, read);
        total += read;
    }
    /* 読んでいる間に伸び縮みした file は、読んだ範囲が size と合わない。 */
    if (!failed && !cancelled && total != size.QuadPart)
        failed = 1;
    if (chunk)
        HeapFree(GetProcessHeap(), 0, chunk);
    CloseHandle(file);
    if (cancelled)
        return MVM_FILE_HASH_CANCELLED;
    if (failed)
        return MVM_FILE_HASH_IO_ERROR;
    *out = hash;
    return MVM_FILE_HASH_OK;
}

int mvm_file_content_fingerprint(const wchar_t* path, unsigned long long* out) {
    if (!out)
        return 1;
    *out = 0;
    if (!path || !path[0])
        return 1;
    HANDLE file = open_shared(path, GENERIC_READ);
    if (file == INVALID_HANDLE_VALUE)
        return 1;
    /* worker thread から並行に呼ばれるので、static buffer は使わない。 */
    unsigned char* edge =
        (unsigned char*)HeapAlloc(GetProcessHeap(), 0, MVM_FILE_FINGERPRINT_EDGE_BYTES);
    LARGE_INTEGER size;
    int failed = !edge || !GetFileSizeEx(file, &size);
    unsigned long long hash = 14695981039346656037ULL;
    if (!failed)
        hash = fnv1a(hash, (const unsigned char*)&size.QuadPart, sizeof(size.QuadPart));
    DWORD read = 0;
    if (!failed)
        failed = read_at(file, 0, edge, MVM_FILE_FINGERPRINT_EDGE_BYTES, &read);
    if (!failed)
        hash = fnv1a(hash, edge, read);
    /* 末尾は先頭と重ならない範囲だけ読む。 */
    if (!failed && size.QuadPart > MVM_FILE_FINGERPRINT_EDGE_BYTES) {
        long long tail = size.QuadPart - MVM_FILE_FINGERPRINT_EDGE_BYTES;
        if (tail < MVM_FILE_FINGERPRINT_EDGE_BYTES)
            tail = MVM_FILE_FINGERPRINT_EDGE_BYTES;
        failed = read_at(file, tail, edge, MVM_FILE_FINGERPRINT_EDGE_BYTES, &read);
        if (!failed)
            hash = fnv1a(hash, edge, read);
    }
    if (edge)
        HeapFree(GetProcessHeap(), 0, edge);
    CloseHandle(file);
    if (failed)
        return 1;
    *out = hash;
    return 0;
}
