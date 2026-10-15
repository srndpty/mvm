/* P3-7C 診断用の起動器 (repo の外。製品・試験の code ではない)。
 *
 * 目的: CTest と同じ command を、WER の LocalDumps が働く形で起動する。
 *   CTest と MSYS の shell は SEM_NOGPFAULTERRORBOX を子へ受け継がせるので、未処理の例外で
 *   WER が呼ばれず dump も event log も残らない (2026-10-15 に probe で実測)。
 *   ここでは error mode を明示的に 0 にしてから起動する。debugger は付けない (heap
 * と時間を変えない)。
 *
 * 使い方:
 *   p3_7c_launch --timeout-ms N --window-ms M --error-mode {wer|ctest} --stdout F --stderr F
 *                --cwd D -- <exe> [args...]
 *   window-ms > 0 なら、その時間を生き延びた process を job ごと止め、outcome=window_survived
 * とする (起動直後の区間だけを数多く試すため。合格ではない)。 出力: 1 行の JSON (stdout)。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void append_quoted(wchar_t* dst, size_t cap, const wchar_t* arg) {
    size_t n = wcslen(dst);
    int needs = (*arg == 0) || wcspbrk(arg, L" \t\"") != NULL;
    if (n && n + 1 < cap)
        dst[n++] = L' ';
    if (!needs) {
        wcsncpy(dst + n, arg, cap - n - 1);
        dst[cap - 1] = 0;
        return;
    }
    if (n + 1 < cap)
        dst[n++] = L'"';
    for (const wchar_t* p = arg;; ++p) {
        size_t bs = 0;
        while (*p == L'\\') {
            ++bs;
            ++p;
        }
        if (*p == 0) {
            for (size_t i = 0; i < bs * 2 && n + 1 < cap; ++i)
                dst[n++] = L'\\';
            break;
        }
        if (*p == L'"') {
            for (size_t i = 0; i < bs * 2 + 1 && n + 1 < cap; ++i)
                dst[n++] = L'\\';
        } else {
            for (size_t i = 0; i < bs && n + 1 < cap; ++i)
                dst[n++] = L'\\';
        }
        if (n + 1 < cap)
            dst[n++] = *p;
    }
    if (n + 1 < cap)
        dst[n++] = L'"';
    dst[n] = 0;
}

static HANDLE open_out(const wchar_t* path) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

int wmain(int argc, wchar_t** argv) {
    DWORD timeout = 0, window = 0;
    const wchar_t *mode = L"wer", *out = NULL, *err = NULL, *cwd = NULL;
    int i = 1;
    for (; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--")) {
            ++i;
            break;
        }
        if (i + 1 >= argc) {
            fwprintf(stderr, L"引数が足りません: %ls\n", argv[i]);
            return 2;
        }
        if (!wcscmp(argv[i], L"--timeout-ms"))
            timeout = wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--window-ms"))
            window = wcstoul(argv[++i], NULL, 10);
        else if (!wcscmp(argv[i], L"--error-mode"))
            mode = argv[++i];
        else if (!wcscmp(argv[i], L"--stdout"))
            out = argv[++i];
        else if (!wcscmp(argv[i], L"--stderr"))
            err = argv[++i];
        else if (!wcscmp(argv[i], L"--cwd"))
            cwd = argv[++i];
        else {
            fwprintf(stderr, L"不明な引数: %ls\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc || !timeout || !out || !err || !cwd) {
        fwprintf(stderr, L"使い方の誤り\n");
        return 2;
    }
    if (!wcscmp(mode, L"wer"))
        SetErrorMode(0);
    else if (!wcscmp(mode, L"ctest"))
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    else {
        fwprintf(stderr, L"--error-mode は wer か ctest\n");
        return 2;
    }

    static wchar_t line[32768];
    line[0] = 0;
    for (int k = i; k < argc; ++k)
        append_quoted(line, 32768, argv[k]);

    HANDLE ho = open_out(out), he = open_out(err);
    if (ho == INVALID_HANDLE_VALUE || he == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"出力 file を作れません (既存は上書きしない): %lu\n", GetLastError());
        return 2;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);

    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = ho;
    si.hStdError = he;
    PROCESS_INFORMATION pi;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (!CreateProcessW(argv[i], line, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL,
                        cwd, &si, &pi)) {
        fwprintf(stderr, L"CreateProcess に失敗: %lu\n", GetLastError());
        return 2;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        fwprintf(stderr, L"AssignProcessToJobObject に失敗: %lu\n", GetLastError());
        return 2;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    const char* outcome = "exited";
    DWORD wait = WaitForSingleObject(pi.hProcess, window ? window : timeout);
    if (wait == WAIT_TIMEOUT && window) {
        outcome = "window_survived";
        TerminateJobObject(job, 0xE0C0FFEE);
        WaitForSingleObject(pi.hProcess, 10000);
    } else if (wait == WAIT_TIMEOUT) {
        outcome = "timeout";
        TerminateJobObject(job, 0xE0C0FFEF);
        WaitForSingleObject(pi.hProcess, 10000);
    }
    QueryPerformanceCounter(&t1);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("{\"pid\":%lu,\"outcome\":\"%s\",\"exit_code\":\"0x%08lX\",\"elapsed_ms\":%.1f,\"error_"
           "mode\":\"%ls\"}\n",
           pi.dwProcessId, outcome, code, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
           mode);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    CloseHandle(ho);
    CloseHandle(he);
    return 0;
}
