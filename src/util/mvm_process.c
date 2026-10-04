#include "util/mvm_process.h"

#include <windows.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* 待機の 1 回の長さ。取消と timeout はこの間隔で見る。 */
#define MVM_PROCESS_POLL_MS 50

static int needs_quotes(const wchar_t* argument) {
    if (!argument[0])
        return 1;
    for (const wchar_t* c = argument; *c; ++c) {
        if (*c == L' ' || *c == L'\t' || *c == L'\n' || *c == L'\v' || *c == L'"')
            return 1;
    }
    return 0;
}

size_t mvm_process_quote_argument(const wchar_t* argument, wchar_t* out, size_t out_count) {
    if (!argument)
        argument = L"";
    const size_t length = wcslen(argument);
    if (!needs_quotes(argument)) {
        if (out && out_count >= length + 1)
            memcpy(out, argument, (length + 1) * sizeof(wchar_t));
        return length + 1;
    }

    /* 1 回目で長さを数え、2 回目で書く。規則は CommandLineToArgvW の逆:
     * 引用符の直前と末尾の \ の並びは倍にし、引用符は \" にする。 */
    size_t required = 3; /* 前後の引用符と終端 */
    for (int pass = 0; pass < 2; ++pass) {
        size_t written = 0;
        if (pass == 1) {
            if (!out || out_count < required)
                return required;
            out[written++] = L'"';
        }
        size_t index = 0;
        for (;;) {
            size_t backslashes = 0;
            while (index < length && argument[index] == L'\\') {
                ++index;
                ++backslashes;
            }
            if (index == length) {
                if (pass == 0)
                    required += backslashes * 2;
                else
                    for (size_t n = 0; n < backslashes * 2; ++n)
                        out[written++] = L'\\';
                break;
            }
            const size_t emitted = argument[index] == L'"' ? backslashes * 2 + 1 : backslashes;
            if (pass == 0) {
                required += emitted + 1;
            } else {
                for (size_t n = 0; n < emitted; ++n)
                    out[written++] = L'\\';
                out[written++] = argument[index];
            }
            ++index;
        }
        if (pass == 1) {
            out[written++] = L'"';
            out[written] = L'\0';
        }
    }
    return required;
}

/* executable と引数を引用して 1 本の command line にする。失敗で NULL。free で解放する。 */
static wchar_t* build_command_line(const MvmProcessRequest* request) {
    size_t total = mvm_process_quote_argument(request->executable, NULL, 0);
    for (size_t i = 0; i < request->argument_count; ++i)
        total +=
            mvm_process_quote_argument(request->arguments[i], NULL, 0); /* 区切りの空白を兼ねる */
    wchar_t* line = (wchar_t*)malloc(total * sizeof(wchar_t));
    if (!line)
        return NULL;
    size_t used = mvm_process_quote_argument(request->executable, line, total) - 1;
    for (size_t i = 0; i < request->argument_count; ++i) {
        line[used++] = L' ';
        used += mvm_process_quote_argument(request->arguments[i], line + used, total - used) - 1;
    }
    line[used] = L'\0';
    return line;
}

static HANDLE open_output(const wchar_t* path, SECURITY_ATTRIBUTES* security) {
    if (!path)
        path = L"NUL";
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, security,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void close_handle(HANDLE* handle) {
    if (*handle && *handle != INVALID_HANDLE_VALUE)
        CloseHandle(*handle);
    *handle = NULL;
}

static MvmProcessStatus finish(MvmProcessResult* out, MvmProcessStatus status, DWORD error) {
    out->status = status;
    out->win32_error = error;
    return status;
}

MvmProcessStatus mvm_process_run(const MvmProcessRequest* request, MvmProcessResult* out) {
    MvmProcessResult ignored;
    if (!out)
        out = &ignored;
    out->status = MVM_PROCESS_START_FAILED;
    out->exit_code = -1;
    out->win32_error = 0;
    if (!request || !request->executable || !request->executable[0] ||
        (request->argument_count > 0 && !request->arguments))
        return finish(out, MVM_PROCESS_START_FAILED, ERROR_INVALID_PARAMETER);

    HANDLE job = NULL;
    HANDLE stdin_handle = NULL;
    HANDLE stdout_handle = NULL;
    HANDLE stderr_handle = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    wchar_t* command_line = NULL;
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    MvmProcessStatus status = MVM_PROCESS_START_FAILED;
    DWORD error = 0;

    job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        error = GetLastError();
        goto cleanup;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = GetLastError();
        goto cleanup;
    }

    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    stdin_handle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    stdout_handle = open_output(request->stdout_path, &security);
    stderr_handle = open_output(request->stderr_path, &security);
    if (stdin_handle == INVALID_HANDLE_VALUE || stdout_handle == INVALID_HANDLE_VALUE ||
        stderr_handle == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        goto cleanup;
    }

    /* 継承させる handle をこの 3 つに限る。 */
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
    if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        error = attributes ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        free(attributes);
        attributes = NULL;
        goto cleanup;
    }
    HANDLE inherited[3] = {stdin_handle, stdout_handle, stderr_handle};
    /* stdout と stderr が同じ NUL でも別 handle なので重複にはならない。 */
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), NULL, NULL)) {
        error = GetLastError();
        goto cleanup;
    }

    command_line = build_command_line(request);
    if (!command_line) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }

    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof(startup));
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = stdin_handle;
    startup.StartupInfo.hStdOutput = stdout_handle;
    startup.StartupInfo.hStdError = stderr_handle;
    startup.lpAttributeList = attributes;

    /* job へ入れる前に子を起動させないよう、停止状態で作ってから job へ入れて再開する。 */
    if (!CreateProcessW(request->executable, command_line, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        NULL, request->working_directory, &startup.StartupInfo, &process)) {
        error = GetLastError();
        memset(&process, 0, sizeof(process));
        goto cleanup;
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        goto cleanup;
    }
    if (ResumeThread(process.hThread) == (DWORD)-1) {
        error = GetLastError();
        TerminateJobObject(job, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        goto cleanup;
    }
    /* 子は自分の handle を持ったので、こちらの複製は閉じる。 */
    close_handle(&stdin_handle);
    close_handle(&stdout_handle);
    close_handle(&stderr_handle);

    const ULONGLONG started = GetTickCount64();
    for (;;) {
        const DWORD waited = WaitForSingleObject(process.hProcess, MVM_PROCESS_POLL_MS);
        if (waited == WAIT_OBJECT_0) {
            DWORD exit_code = 0;
            if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
                error = GetLastError();
                status = MVM_PROCESS_WAIT_FAILED;
            } else {
                out->exit_code = exit_code > (DWORD)INT_MAX ? INT_MAX : (int)exit_code;
                status = MVM_PROCESS_EXITED;
            }
            break;
        }
        if (waited != WAIT_TIMEOUT) {
            error = GetLastError();
            status = MVM_PROCESS_WAIT_FAILED;
            break;
        }
        if (request->should_stop && request->should_stop(request->opaque)) {
            status = MVM_PROCESS_CANCELLED;
            break;
        }
        if (request->timeout_ms && GetTickCount64() - started >= request->timeout_ms) {
            status = MVM_PROCESS_TIMED_OUT;
            break;
        }
    }
    if (status != MVM_PROCESS_EXITED) {
        TerminateJobObject(job, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
    }

cleanup:
    if (attributes) {
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
    }
    free(command_line);
    close_handle(&stdin_handle);
    close_handle(&stdout_handle);
    close_handle(&stderr_handle);
    close_handle(&process.hThread);
    close_handle(&process.hProcess);
    /* 正常終了の後に残った子も、ここで job ごと終わらせる。 */
    if (job) {
        TerminateJobObject(job, 1);
        close_handle(&job);
    }
    return finish(out, status, error);
}
