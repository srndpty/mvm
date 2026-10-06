#include "util/mvm_long_path.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

/* parent + "\" + name を新しく確保する。失敗時は NULL。 */
static wchar_t* join_path(const wchar_t* parent, const wchar_t* name) {
    const size_t parentLength = wcslen(parent);
    const size_t nameLength = wcslen(name);
    wchar_t* joined = (wchar_t*)malloc((parentLength + 1 + nameLength + 1) * sizeof(wchar_t));
    if (!joined)
        return NULL;
    memcpy(joined, parent, parentLength * sizeof(wchar_t));
    joined[parentLength] = L'\\';
    memcpy(joined + parentLength + 1, name, (nameLength + 1) * sizeof(wchar_t));
    return joined;
}

static int missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

unsigned long mvm_remove_tree(const wchar_t* extended_path) {
    if (!extended_path || !*extended_path)
        return ERROR_INVALID_PARAMETER;
    const DWORD attributes = GetFileAttributesW(extended_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return missing(error) ? 0 : error;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(extended_path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    /* file と reparse point は中を辿らずにその項目だけを消す。 */
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileW(extended_path) ? 0 : GetLastError();
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return RemoveDirectoryW(extended_path) ? 0 : GetLastError();

    DWORD first = 0;
    wchar_t* pattern = join_path(extended_path, L"*");
    if (!pattern)
        return ERROR_NOT_ENOUGH_MEMORY;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, NULL,
                                   FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            first = error;
    } else {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;
            wchar_t* child = join_path(extended_path, data.cFileName);
            const DWORD error = child ? mvm_remove_tree(child) : ERROR_NOT_ENOUGH_MEMORY;
            free(child);
            if (error && !first)
                first = error;
        } while (FindNextFileW(find, &data));
        const DWORD end = GetLastError();
        if (end != ERROR_NO_MORE_FILES && !first)
            first = end;
        FindClose(find);
    }
    if (!RemoveDirectoryW(extended_path) && !first)
        first = GetLastError();
    return first;
}
