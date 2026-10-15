/* 試験の process を、WER の dump が残る形で起動する (docs/crash-evidence.md)。
 *
 * CTest は子 process の error mode に SEM_NOGPFAULTERRORBOX を渡すので、CTest の下で試験が落ちても
 * WER は呼ばれず、dump も Application の event log も残らない (2026-10-15 に実測。pwsh から起動した
 * ctest でも同じ)。HKCU の LocalDumps は効かず、HKLM の設定には管理者の権限が要る (同日に実測)。
 * この起動器は error mode を戻してから試験を起動する。debugger は付けない (heap と時間を変えない)。
 *
 * --test <exe> [args...]
 *   CTest の TEST_LAUNCHER として使う。標準入出力・作業 directory・環境はそのまま受け継ぎ、子の終了
 *   コードをそのまま返す。子が例外で終わったら、WER が書いた dump (%LOCALAPPDATA%\CrashDumps の
 *   <exe>.<pid>.dmp) を、MVM_CRASH_STAGING があればそこへ複製する (WER の既定の 10
 * 件の回転で消える前に)。 起動器自身の失敗は 0xE0C0FF02 で終わる。
 *
 * --timeout-ms N --window-ms M --error-mode {wer|ctest} --stdout F --stderr F --cwd D -- <exe>
 * [args...] 診断の cohort 用。window-ms > 0 なら、その時間を生き延びた process を job ごと止め、
 *   outcome=window_survived とする (合格ではない)。結果を 1 行の JSON で標準出力へ書く。
 *
 * どちらも子を Job Object (KILL_ON_JOB_CLOSE) で所有する。起動器が止められると子も止まる。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define LAUNCHER_FAILURE 0xE0C0FF02u

/* message は他の試験と同じく UTF-8 で書く (wide の出力は console・pipe の code page で化ける)。 */
static const char* u8(const wchar_t* text, char* buffer, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, size, NULL, NULL))
        buffer[0] = 0;
    return buffer;
}

#define U8(text) u8((text), (char[MAX_PATH * 3]){0}, MAX_PATH * 3)

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

static HANDLE make_job(void) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job)
        return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {0};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* 一時停止で作って job に入れてから動かす。後から作られる孫も job に入る。 */
static int start_in_job(HANDLE job, const wchar_t* exe, wchar_t* line, const wchar_t* cwd,
                        STARTUPINFOW* si, DWORD flags, PROCESS_INFORMATION* pi) {
    if (!CreateProcessW(exe, line, NULL, NULL, TRUE, CREATE_SUSPENDED | flags, NULL, cwd, si, pi)) {
        fprintf(stderr, "wer_launch: CreateProcess に失敗 (%lu): %s\n", GetLastError(), U8(exe));
        return 0;
    }
    if (!AssignProcessToJobObject(job, pi->hProcess)) {
        fprintf(stderr, "wer_launch: AssignProcessToJobObject に失敗 (%lu)\n", GetLastError());
        TerminateProcess(pi->hProcess, LAUNCHER_FAILURE);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        return 0;
    }
    ResumeThread(pi->hThread);
    CloseHandle(pi->hThread);
    return 1;
}

/* NTSTATUS の error (0xC...) と breakpoint は、未処理の例外による終了とみなす。 */
static int is_exception_exit(DWORD code) {
    return (code & 0xF0000000u) == 0xC0000000u || code == 0x80000003u;
}

/* WER が書いた子の dump を staging へ複製する。見つからなければ、その旨を書く (成功を装わない)。 */
static void stage_dump(const wchar_t* exe, DWORD pid, DWORD code) {
    wchar_t local[MAX_PATH], dump[MAX_PATH * 2], staging[MAX_PATH], target[MAX_PATH * 2];
    const wchar_t* base = wcsrchr(exe, L'\\');
    const wchar_t* slash = wcsrchr(exe, L'/');
    if (slash && (!base || slash > base))
        base = slash;
    base = base ? base + 1 : exe;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        fprintf(stderr, "wer_launch: 例外 0x%08lX で終了。LOCALAPPDATA が無く dump を探せません\n",
                code);
        return;
    }
    swprintf(dump, MAX_PATH * 2, L"%ls\\CrashDumps\\%ls.%lu.dmp", local, base, pid);
    /* WER は process が終わる前に dump を書き終える。念のため、現れるまで最大 10 秒待つ。 */
    for (int i = 0; i < 100 && GetFileAttributesW(dump) == INVALID_FILE_ATTRIBUTES; ++i)
        Sleep(100);
    if (GetFileAttributesW(dump) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "wer_launch: 例外 0x%08lX で終了。dump がありません: %s\n", code, U8(dump));
        return;
    }
    if (!GetEnvironmentVariableW(L"MVM_CRASH_STAGING", staging, MAX_PATH)) {
        fprintf(stderr,
                "wer_launch: 例外 0x%08lX で終了。dump: %s (MVM_CRASH_STAGING "
                "が無いので複製しません)\n",
                code, U8(dump));
        return;
    }
    swprintf(target, MAX_PATH * 2, L"%ls\\%ls.%lu.%llu.dmp", staging, base, pid,
             (unsigned long long)GetTickCount64());
    if (!CopyFileW(dump, target, TRUE)) {
        fprintf(stderr, "wer_launch: 例外 0x%08lX で終了。dump の複製に失敗 (%lu): %s -> %s\n",
                code, GetLastError(), U8(dump), U8(target));
        return;
    }
    fprintf(stderr, "wer_launch: 例外 0x%08lX で終了。dump: %s (複製: %s)\n", code, U8(dump),
            U8(target));
}

static int run_test(int argc, wchar_t** argv) {
    /* SEM_NOGPFAULTERRORBOX を外す。critical error の dialog は出さないままにする。 */
    SetErrorMode(SEM_FAILCRITICALERRORS);
    static wchar_t line[32768];
    line[0] = 0;
    for (int k = 0; k < argc; ++k)
        append_quoted(line, 32768, argv[k]);
    HANDLE job = make_job();
    if (!job) {
        fprintf(stderr, "wer_launch: Job Object を作れません (%lu)\n", GetLastError());
        return (int)LAUNCHER_FAILURE;
    }
    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std[3] = {si.hStdInput, si.hStdOutput, si.hStdError};
    for (int i = 0; i < 3; ++i) {
        if (std[i] && std[i] != INVALID_HANDLE_VALUE)
            SetHandleInformation(std[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    PROCESS_INFORMATION pi;
    if (!start_in_job(job, argv[0], line, NULL, &si, 0, &pi))
        return (int)LAUNCHER_FAILURE;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = LAUNCHER_FAILURE;
    GetExitCodeProcess(pi.hProcess, &code);
    if (is_exception_exit(code))
        stage_dump(argv[0], pi.dwProcessId, code);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    return (int)code;
}

static int run_cohort(int argc, wchar_t** argv) {
    DWORD timeout = 0, window = 0;
    const wchar_t *mode = L"wer", *out = NULL, *err = NULL, *cwd = NULL;
    int i = 1;
    for (; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--")) {
            ++i;
            break;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "引数が足りません: %s\n", U8(argv[i]));
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
            fprintf(stderr, "不明な引数: %s\n", U8(argv[i]));
            return 2;
        }
    }
    if (i >= argc || !timeout || !out || !err || !cwd) {
        fprintf(stderr, "使い方の誤り\n");
        return 2;
    }
    if (!wcscmp(mode, L"wer"))
        SetErrorMode(0);
    else if (!wcscmp(mode, L"ctest"))
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    else {
        fprintf(stderr, "--error-mode は wer か ctest\n");
        return 2;
    }

    static wchar_t line[32768];
    line[0] = 0;
    for (int k = i; k < argc; ++k)
        append_quoted(line, 32768, argv[k]);

    HANDLE ho = open_out(out), he = open_out(err);
    if (ho == INVALID_HANDLE_VALUE || he == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "出力 file を作れません (既存は上書きしない): %lu\n", GetLastError());
        return 2;
    }
    HANDLE job = make_job();
    if (!job) {
        fprintf(stderr, "Job Object を作れません (%lu)\n", GetLastError());
        return 2;
    }
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
    if (!start_in_job(job, argv[i], line, cwd, &si, CREATE_NO_WINDOW, &pi))
        return 2;

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
    printf("{\"pid\":%lu,\"outcome\":\"%s\",\"exit_code\":\"0x%08lX\",\"elapsed_ms\":%.1f,"
           "\"error_mode\":\"%s\"}\n",
           pi.dwProcessId, outcome, code,
           (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, U8(mode));
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    CloseHandle(ho);
    CloseHandle(he);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && !wcscmp(argv[1], L"--test"))
        return run_test(argc - 2, argv + 2);
    return run_cohort(argc, argv);
}
