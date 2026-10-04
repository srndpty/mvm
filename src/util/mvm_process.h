/*
 * 外部 process を 1 つ起動し、終了・timeout・取消のいずれかまで待つ。
 *
 * 起動した process は Job Object に入れる。process が起動した子 (Manim が起動する latex など)
 * も同じ job に入るので、timeout・取消のときも、正常終了の後に残った子も、job を閉じた時点で
 * まとめて終わらせる (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)。
 *
 * 子 process へ継承させる handle は、この呼び出しの標準入出力だけに限る
 * (PROC_THREAD_ATTRIBUTE_HANDLE_LIST)。別の thread が同時に起動した process の
 * 出力 file の handle が混ざらない。
 */

#ifndef MVM_PROCESS_H
#define MVM_PROCESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MvmProcessRequest {
    /* 実行する file の絶対 path。PATH から探さない。 */
    const wchar_t* executable;
    /* argv[0] を含まない引数。必要な引用符は mvm_process_run が付ける。 */
    const wchar_t* const* arguments;
    size_t argument_count;
    /* NULL なら呼び出し側と同じ。 */
    const wchar_t* working_directory;
    /* 標準出力・標準エラーを書く file (作り直す)。NULL なら捨てる。標準入力は常に NUL。 */
    const wchar_t* stdout_path;
    const wchar_t* stderr_path;
    /* 0 なら制限しない。 */
    unsigned long timeout_ms;
    /* 待っている間、約 50ms ごとに呼ぶ (NULL なら呼ばない)。非 0 を返したら止める。 */
    int (*should_stop)(void* opaque);
    void* opaque;
} MvmProcessRequest;

typedef enum MvmProcessStatus {
    /* process が終わった。exit_code が有効。0 以外の終了も EXITED である。 */
    MVM_PROCESS_EXITED = 0,
    /* job・標準入出力の準備、または CreateProcessW に失敗した。win32_error が有効。 */
    MVM_PROCESS_START_FAILED = 1,
    /* timeout_ms を過ぎたので止めた。 */
    MVM_PROCESS_TIMED_OUT = 2,
    /* should_stop が非 0 を返したので止めた。 */
    MVM_PROCESS_CANCELLED = 3,
    /* 待機・終了コードの取得に失敗した。process は止めてある。win32_error が有効。 */
    MVM_PROCESS_WAIT_FAILED = 4
} MvmProcessStatus;

typedef struct MvmProcessResult {
    MvmProcessStatus status;
    int exit_code;
    unsigned long win32_error;
} MvmProcessResult;

/* 戻り値は out->status と同じ。request か out が不正なら START_FAILED
 * (win32_error = ERROR_INVALID_PARAMETER)。返った時点で job の process はすべて終わっている。 */
MvmProcessStatus mvm_process_run(const MvmProcessRequest* request, MvmProcessResult* out);

/* CommandLineToArgvW が元の引数へ戻せる形に 1 つの引数を引用する。
 * 必要な文字数 (終端を含む) を返す。out_count がそれ以上のときだけ out へ書く。 */
size_t mvm_process_quote_argument(const wchar_t* argument, wchar_t* out, size_t out_count);

#ifdef __cplusplus
}
#endif

#endif
