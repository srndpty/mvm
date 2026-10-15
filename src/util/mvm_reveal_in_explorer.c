#include "util/mvm_reveal_in_explorer.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

static const MvmRevealShell* g_shell = NULL;

void mvm_reveal_set_shell_for_test(const MvmRevealShell* shell) {
    g_shell = shell;
}

static void set_err(char* err, size_t err_size, const char* message, HRESULT hr) {
    if (err && err_size > 0)
        snprintf(err, err_size, "%s (HRESULT=0x%08lX)", message, (unsigned long)hr);
}

/* path を Windows の絶対 path (区切りは \) にする。呼び出し側が free する。失敗なら NULL と hr。 */
static wchar_t* full_path(const wchar_t* path, HRESULT* hr) {
    const DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        *hr = HRESULT_FROM_WIN32(GetLastError());
        return NULL;
    }
    wchar_t* full = (wchar_t*)malloc((size_t)needed * sizeof(wchar_t));
    if (!full) {
        *hr = E_OUTOFMEMORY;
        return NULL;
    }
    const DWORD written = GetFullPathNameW(path, needed, full, NULL);
    if (written == 0 || written >= needed) {
        *hr = written == 0 ? HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED;
        free(full);
        return NULL;
    }
    *hr = S_OK;
    return full;
}

int mvm_reveal_in_explorer(const wchar_t* path, char* err, size_t err_size) {
    if (err && err_size > 0)
        err[0] = '\0';
    if (!path || !path[0]) {
        set_err(err, err_size, "表示するファイルのパスが空です", E_INVALIDARG);
        return 1;
    }
    /* SHParseDisplayName は / 区切りを E_INVALIDARG で拒否するので、先に正規の絶対 path にする。 */
    HRESULT hr = S_OK;
    wchar_t* full = full_path(path, &hr);
    if (!full) {
        set_err(err, err_size, "表示するファイルのパスを解決できません", hr);
        return 1;
    }
    const DWORD attributes = GetFileAttributesW(full);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        /* GetLastError() は失敗した直後にだけ意味を持つ。他の API を挟まずに読む。 */
        const DWORD error = GetLastError();
        free(full);
        set_err(err, err_size, "表示するファイルがありません", HRESULT_FROM_WIN32(error));
        return 1;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        /* GetFileAttributesW は成功しているので GetLastError() は使えない。 */
        free(full);
        set_err(err, err_size, "表示対象はファイルではありません", E_INVALIDARG);
        return 1;
    }
    const MvmRevealShell* shell = g_shell;
    if (shell && shell->observe_path)
        shell->observe_path(full, shell->opaque);

    /* 呼び出し thread の COM 状態は呼び出し側 (Qt) が決める。既に別 mode で初期化
     * 済みなら (RPC_E_CHANGED_MODE) その状態のまま使い、ここでは解放しない。 */
    const HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    const int uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        free(full);
        set_err(err, err_size, "COMを初期化できません", initialized);
        return 1;
    }

    void* item = NULL;
    if (shell && shell->parse)
        hr = (HRESULT)shell->parse(full, &item, shell->opaque);
    else
        hr = SHParseDisplayName(full, NULL, (PIDLIST_ABSOLUTE*)&item, 0, NULL);
    const int parsed = SUCCEEDED(hr);
    if (parsed) {
        if (shell && shell->open)
            hr = (HRESULT)shell->open(item, shell->opaque);
        else
            hr = SHOpenFolderAndSelectItems((PCIDLIST_ABSOLUTE)item, 0, NULL, 0);
    }
    if (item)
        CoTaskMemFree(item);
    free(full);
    if (uninitialize)
        CoUninitialize();
    if (FAILED(hr)) {
        set_err(err, err_size,
                parsed ? "Explorerでファイルを表示できません"
                       : "Explorerに渡すファイルのパスを解釈できません",
                hr);
        return 1;
    }
    return 0;
}
