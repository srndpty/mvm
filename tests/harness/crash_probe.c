/* wer_launch・crash の archive・minidump の解析の契約試験で使う、制御された終わり方をする process。
 *
 *   crash          既知の呼び出しの連鎖で access violation を起こす (minidump の unwind の検証用):
 *                  main -> probe_big_frame -> qsort (UCRT の DLL) -> probe_compare -> probe_alloca
 *                  -> probe_leaf (0x10 へ書き込む)
 *   abort          abort() (fast fail 0xC0000409)
 *   exit <hex>     ExitProcess で指定の code
 *   sleep <ms>     待ってから 0
 *   spawn-sleep <ms>  自分を sleep で起動 (孫) し、自分も待つ。job の停止の確認用
 *   echo           標準出力・標準エラーに既知の行、作業 directory と MVM_CRASH_PROBE_ENV を書く
 *   hang           main -> probe_hang で終わらずに待つ (起動器の、打ち切り前の dump の検証用)
 */
#include <windows.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int sink;
/* 定数の null 付近を直接書くと compiler が警告・最適化するので、volatile な変数を経由する。 */
static volatile uintptr_t fault_address = 0x10;

__attribute__((noinline)) static void probe_leaf(int depth) {
    volatile int* target = (volatile int*)fault_address;
    sink = depth;
    *target = depth;
}

/* alloca で frame pointer を使わせる (unwind 情報の SET_FPREG)。 */
__attribute__((noinline)) static void probe_alloca(int n) {
    volatile char* buffer = alloca((size_t)n + 16);
    buffer[0] = (char)n;
    probe_leaf(buffer[0] + 1);
    sink = buffer[0];
}

__attribute__((noinline)) static int probe_compare(const void* a, const void* b) {
    probe_alloca(*(const int*)a + 32);
    return *(const int*)a - *(const int*)b;
}

/* 大きな stack frame (unwind 情報の ALLOC_LARGE)。 */
__attribute__((noinline)) static void probe_big_frame(void) {
    volatile char page[70000];
    int values[2] = {3, 1};
    page[0] = 1;
    page[sizeof page - 1] = 2;
    qsort(values, 2, sizeof values[0], probe_compare);
    sink = page[0] + page[sizeof page - 1] + values[0];
}

/* 止まった試験の代わり。dump の stack に、この名前が main の上に出ることを確かめる。 */
__attribute__((noinline)) static void probe_hang(void) {
    puts("hanging");
    fflush(stdout);
    for (;;)
        Sleep(INFINITE);
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "hang"))
        probe_hang();
    if (argc >= 2 && !strcmp(argv[1], "crash")) {
        probe_big_frame();
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "abort"))
        abort();
    if (argc >= 3 && !strcmp(argv[1], "exit"))
        ExitProcess((UINT)strtoul(argv[2], NULL, 16));
    if (argc >= 3 && !strcmp(argv[1], "sleep")) {
        Sleep((DWORD)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "spawn-sleep")) {
        char self[MAX_PATH], line[MAX_PATH + 64];
        GetModuleFileNameA(NULL, self, MAX_PATH);
        snprintf(line, sizeof line, "\"%s\" sleep %s", self, argv[2]);
        STARTUPINFOA si = {0};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(self, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            return 3;
        printf("grandchild %lu\n", pi.dwProcessId);
        fflush(stdout);
        Sleep((DWORD)strtoul(argv[2], NULL, 10));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "echo")) {
        char cwd[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, cwd);
        const char* env = getenv("MVM_CRASH_PROBE_ENV");
        printf("probe-stdout\ncwd=%s\nenv=%s\n", cwd, env ? env : "(none)");
        fprintf(stderr, "probe-stderr\n");
        return 0;
    }
    fprintf(stderr, "使い方: mvm_test_crash_probe crash|abort|exit <hex>|sleep <ms>|spawn-sleep "
                    "<ms>|echo|hang\n");
    return 2;
}
