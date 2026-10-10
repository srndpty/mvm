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
/* 試験用観測。0/1 は flush の開始/終了、2/3 は rename の開始/終了。処理は同じ。 */
typedef void (*mvm_atomic_write_observer)(void* context, int stage, int success);
int mvm_atomic_write_file_observed(const wchar_t* target_path, const void* data, size_t size,
                                   char* error, size_t error_size,
                                   mvm_atomic_write_observer observer, void* context);

#ifdef __cplusplus
}
#endif

#endif
