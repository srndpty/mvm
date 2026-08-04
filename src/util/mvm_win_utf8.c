#include "mvm_win_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

wchar_t* mvm_utf8_to_wide(const char* utf8) {
    if (!utf8)
        return NULL;

    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (need <= 0)
        return NULL;

    wchar_t* out = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
    if (!out)
        return NULL;

    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, need) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

char* mvm_wide_to_utf8(const wchar_t* wide) {
    if (!wide)
        return NULL;

    int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0)
        return NULL;

    char* out = (char*)malloc((size_t)need);
    if (!out)
        return NULL;

    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, need, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

wchar_t* mvm_wide_concat(const wchar_t* a, const wchar_t* b) {
    if (!a || !b)
        return NULL;

    size_t la = wcslen(a);
    size_t lb = wcslen(b);

    /* MAX_PATH には依存しない。必要な長さを都度確保する。 */
    wchar_t* out = (wchar_t*)malloc((la + lb + 1) * sizeof(wchar_t));
    if (!out)
        return NULL;

    memcpy(out, a, la * sizeof(wchar_t));
    memcpy(out + la, b, lb * sizeof(wchar_t));
    out[la + lb] = L'\0';
    return out;
}

char* mvm_win_error_message(unsigned long error_code) {
    LPWSTR buf = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, (DWORD)error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, NULL);

    char* text = NULL;
    if (n > 0 && buf) {
        /* 末尾の CR/LF を落とす (1 行として出したいため) */
        while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) {
            buf[--n] = L'\0';
        }
        text = mvm_wide_to_utf8(buf);
    }
    if (buf)
        LocalFree(buf);

    if (text)
        return text;

    /* FormatMessageW が失敗しても、少なくとも数値は返す */
    char* fallback = (char*)malloc(64);
    if (fallback)
        snprintf(fallback, 64, "(説明を取得できません)");
    return fallback;
}

char** mvm_win_get_utf8_args(int* out_argc) {
    if (out_argc)
        *out_argc = 0;

    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv)
        return NULL;

    char** argv = (char**)calloc((size_t)wargc + 1, sizeof(char*));
    if (!argv) {
        LocalFree(wargv);
        return NULL;
    }

    for (int i = 0; i < wargc; i++) {
        argv[i] = mvm_wide_to_utf8(wargv[i]);
        if (!argv[i]) {
            for (int j = 0; j < i; j++)
                free(argv[j]);
            free(argv);
            LocalFree(wargv);
            return NULL;
        }
    }
    LocalFree(wargv);

    if (out_argc)
        *out_argc = wargc;
    return argv;
}

void mvm_win_free_utf8_args(char** argv, int argc) {
    if (!argv)
        return;
    for (int i = 0; i < argc; i++)
        free(argv[i]);
    free(argv);
}

void mvm_str_free(void* p) {
    free(p);
}

void mvm_enable_utf8_console(void) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
