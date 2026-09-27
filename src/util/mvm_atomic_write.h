#ifndef MVM_UTIL_MVM_ATOMIC_WRITE_H
#define MVM_UTIL_MVM_ATOMIC_WRITE_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 同じdirectoryの一時fileへflushしてからtargetを置換する。 */
int mvm_atomic_write_file(const wchar_t* target_path, const void* data, size_t size, char* error,
                          size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
