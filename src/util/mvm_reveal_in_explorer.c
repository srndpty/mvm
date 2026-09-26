#include "util/mvm_reveal_in_explorer.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>

static void set_err(char* err, size_t err_size, const char* message, HRESULT hr) {
    if (err && err_size > 0)
        snprintf(err, err_size, "%s (HRESULT=0x%08lX)", message, (unsigned long)hr);
}

int mvm_reveal_in_explorer(const wchar_t* path, char* err, size_t err_size) {
    if (err && err_size > 0)
        err[0] = '\0';
    if (!path || !path[0]) {
        set_err(err, err_size, "表示するファイルのパスが空です", E_INVALIDARG);
        return 1;
    }
    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        set_err(err, err_size, "表示するファイルがありません", HRESULT_FROM_WIN32(GetLastError()));
        return 1;
    }

    /* 呼び出し thread の COM 状態は呼び出し側 (Qt) が決める。既に別 mode で初期化
     * 済みなら (RPC_E_CHANGED_MODE) その状態のまま使い、ここでは解放しない。 */
    const HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const int uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        set_err(err, err_size, "COMを初期化できません", initialized);
        return 1;
    }

    PIDLIST_ABSOLUTE item = NULL;
    HRESULT hr = SHParseDisplayName(path, NULL, &item, 0, NULL);
    if (SUCCEEDED(hr))
        hr = SHOpenFolderAndSelectItems(item, 0, NULL, 0);
    if (item)
        CoTaskMemFree(item);
    if (uninitialize)
        CoUninitialize();
    if (FAILED(hr)) {
        set_err(err, err_size, "Explorerでファイルを表示できません", hr);
        return 1;
    }
    return 0;
}
