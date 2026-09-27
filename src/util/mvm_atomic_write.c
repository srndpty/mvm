#include "util/mvm_atomic_write.h"

#include <windows.h>
#include <stdio.h>

static LONG g_temp_serial = 0;

static void set_error(char* error, size_t error_size, const char* operation, DWORD code) {
    if (error && error_size > 0)
        snprintf(error, error_size, "%sに失敗しました (Win32 error %lu)", operation,
                 (unsigned long)code);
}

int mvm_atomic_write_file(const wchar_t* target_path, const void* data, size_t size, char* error,
                          size_t error_size) {
    if (!target_path || !*target_path || (!data && size > 0)) {
        set_error(error, error_size, "atomic writeの引数検査", ERROR_INVALID_PARAMETER);
        return 1;
    }
    wchar_t temporary[32768];
    const DWORD process_id = GetCurrentProcessId();
    const LONG serial = InterlockedIncrement(&g_temp_serial);
    const int length =
        swprintf(temporary, sizeof(temporary) / sizeof(temporary[0]), L"%ls.mvmtmp.%lu.%ld",
                 target_path, (unsigned long)process_id, (long)serial);
    if (length <= 0 || (size_t)length >= sizeof(temporary) / sizeof(temporary[0])) {
        set_error(error, error_size, "一時file pathの生成", ERROR_BUFFER_OVERFLOW);
        return 1;
    }

    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error, error_size, "一時fileの作成", GetLastError());
        return 1;
    }

    const unsigned char* cursor = (const unsigned char*)data;
    size_t remaining = size;
    int failed = 0;
    while (remaining > 0) {
        const DWORD chunk = remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, NULL) || written != chunk) {
            set_error(error, error_size, "一時fileへの書き込み", GetLastError());
            failed = 1;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    if (!failed && !FlushFileBuffers(file)) {
        set_error(error, error_size, "一時fileのflush", GetLastError());
        failed = 1;
    }
    if (!CloseHandle(file) && !failed) {
        set_error(error, error_size, "一時fileのclose", GetLastError());
        failed = 1;
    }
    if (failed) {
        DeleteFileW(temporary);
        return 1;
    }

    // 同一directory内のrenameで置換する。REPLACEFILE_WRITE_THROUGH はWin32で
    // 未サポートであり、ReplaceFileWは一部の開発directory ACLでERROR_ACCESS_DENIEDに
    // なるため使わない。一時fileは既にFlushFileBuffers済みである。
    const BOOL replaced =
        MoveFileExW(temporary, target_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!replaced) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary);
        set_error(error, error_size, "Project fileのatomic置換", code);
        return 1;
    }
    if (error && error_size > 0)
        error[0] = '\0';
    return 0;
}
