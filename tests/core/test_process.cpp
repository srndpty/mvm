// mvm_process_run の契約。
//
// - 終了コード・標準出力・標準エラー・current directory を子へ正しく渡し、受け取る
// - 引数は空白・引用符・\・改行・日本語・空文字列を含んでも、子の argv へそのまま届く
// - timeout と取消では、子だけでなく子が起動した孫まで終わらせてから返る
// - 子が正常に終わった後に残った孫も、返る時点で終わっている
//
// 孫が終わったことの検査が空振りしていないことを示すため、止める前に孫が生きていたことを
// 先に確かめる (生きていない PID を「終わった」と判定しても何も示さない)。

#include "util/mvm_process.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string toUtf8(const std::wstring& text) {
    char* utf8 = mvm_wide_to_utf8(text.c_str());
    std::string result = utf8 ? utf8 : "";
    mvm_str_free(utf8);
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

struct Run {
    MvmProcessStatus status = MVM_PROCESS_START_FAILED;
    MvmProcessResult result{};
    std::chrono::milliseconds elapsed{};
};

Run run(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
        MvmProcessRequest request = {}) {
    std::vector<const wchar_t*> pointers;
    for (const auto& argument : arguments)
        pointers.push_back(argument.c_str());
    request.executable = executable.c_str();
    request.arguments = pointers.data();
    request.argument_count = pointers.size();
    Run outcome;
    const auto started = std::chrono::steady_clock::now();
    outcome.status = mvm_process_run(&request, &outcome.result);
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return outcome;
}

bool processAlive(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return false; // 存在しない PID は開けない
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

// 終了の反映は非同期なので、少しだけ待つ。
bool processExitsWithin(DWORD pid, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (processAlive(pid)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

bool readPids(const std::filesystem::path& pidFile, DWORD& child, DWORD& grandchild) {
    std::ifstream input(pidFile);
    unsigned long first = 0;
    unsigned long second = 0;
    if (!(input >> first >> second))
        return false;
    child = first;
    grandchild = second;
    return true;
}

void testExitAndOutput(const std::filesystem::path& child, const std::filesystem::path& root) {
    MvmProcessRequest request{};
    const auto out = root / L"出力 stdout.txt";
    const auto err = root / L"出力 stderr.txt";
    request.stdout_path = out.c_str();
    request.stderr_path = err.c_str();
    const Run zero = run(child, {L"exit", L"0"}, request);
    check(zero.status == MVM_PROCESS_EXITED && zero.result.exit_code == 0,
          "exit 0 を EXITED/0 で返す");
    check(readFile(out) == "子の標準出力\n", "標準出力を file へ書く");
    check(readFile(err) == "子の標準エラー\n", "標準エラーを file へ書く");

    const Run nonzero = run(child, {L"exit", L"23"});
    check(nonzero.status == MVM_PROCESS_EXITED, "0 以外の終了も EXITED である");
    check(nonzero.result.exit_code == 23, "0 以外の終了コードを保持する");
}

void testArguments(const std::filesystem::path& child, const std::filesystem::path& root) {
    const auto out = root / L"args.txt";
    const std::vector<std::wstring> sent = {
        L"plain",
        L"with space",
        L"",
        L"quote\"inside",
        L"trailing\\",
        L"trailing space\\",
        L"a\\\\\"b",
        L"\"\"\"",
        L"改行\nを含む",
        L"日本語 の 引数",
        L"\\\\server\\share",
        L"tab\there",
    };
    std::vector<std::wstring> arguments = {L"args", out.wstring()};
    arguments.insert(arguments.end(), sent.begin(), sent.end());
    const Run outcome = run(child, arguments);
    check(outcome.status == MVM_PROCESS_EXITED && outcome.result.exit_code == 0,
          "引数を書く子が成功する");
    std::string expected;
    for (const auto& argument : sent) {
        const std::string utf8 = toUtf8(argument);
        expected += std::to_string(utf8.size()) + "\n" + utf8 + "\n";
    }
    check(readFile(out) == expected,
          "空白・引用符・\\・改行・日本語・空文字列の引数がそのまま届く");
}

void testWorkingDirectory(const std::filesystem::path& child, const std::filesystem::path& root) {
    const auto directory = root / L"作業 dir";
    std::filesystem::create_directories(directory);
    const auto out = root / L"cwd.txt";
    MvmProcessRequest request{};
    request.working_directory = directory.c_str();
    const Run outcome = run(child, {L"cwd", out.wstring()}, request);
    check(outcome.status == MVM_PROCESS_EXITED && outcome.result.exit_code == 0,
          "cwd の子が成功する");
    check(std::filesystem::equivalent(fromUtf8(readFile(out).c_str()), directory),
          "working_directory を子の current directory にする");
}

void testStartFailure(const std::filesystem::path& root) {
    const Run missing = run(root / L"存在しない.exe", {});
    check(missing.status == MVM_PROCESS_START_FAILED, "存在しない executable は START_FAILED");
    check(missing.result.win32_error == ERROR_FILE_NOT_FOUND ||
              missing.result.win32_error == ERROR_PATH_NOT_FOUND,
          "起動失敗の Win32 error を返す");

    MvmProcessResult result{};
    check(mvm_process_run(nullptr, &result) == MVM_PROCESS_START_FAILED &&
              result.win32_error == ERROR_INVALID_PARAMETER,
          "request が NULL なら ERROR_INVALID_PARAMETER");
}

void testTimeout(const std::filesystem::path& child) {
    MvmProcessRequest request{};
    request.timeout_ms = 300;
    const Run outcome = run(child, {L"sleep", L"20000"}, request);
    check(outcome.status == MVM_PROCESS_TIMED_OUT, "timeout_ms を過ぎたら TIMED_OUT");
    check(outcome.elapsed < std::chrono::seconds(5), "timeout 後に子の終了を待って速やかに返る");
}

struct StopWhenFileExists {
    std::filesystem::path pidFile;
    DWORD child = 0;
    DWORD grandchild = 0;
    bool aliveBeforeStop = false;
};

int stopWhenGrandchildStarted(void* opaque) {
    auto* state = static_cast<StopWhenFileExists*>(opaque);
    if (!readPids(state->pidFile, state->child, state->grandchild))
        return 0;
    // 止める直前に、孫と子が実際に生きていたことを記録する (検査の空振り防止)。
    state->aliveBeforeStop = processAlive(state->child) && processAlive(state->grandchild);
    return 1;
}

void testCancelKillsDescendants(const std::filesystem::path& child,
                                const std::filesystem::path& root) {
    StopWhenFileExists state{root / L"cancel-pids.txt"};
    MvmProcessRequest request{};
    request.should_stop = stopWhenGrandchildStarted;
    request.opaque = &state;
    request.timeout_ms = 30000; // 取消が効かないときの保険 (TIMED_OUT なら失敗にする)
    const Run outcome = run(child, {L"spawn", state.pidFile.wstring(), L"600000"}, request);
    check(outcome.status == MVM_PROCESS_CANCELLED, "should_stop が非 0 なら CANCELLED");
    check(state.aliveBeforeStop, "取消の直前に子と孫が生きていた (検査が空振りしていない)");
    check(processExitsWithin(state.child, std::chrono::seconds(2)), "取消で子が終わる");
    check(processExitsWithin(state.grandchild, std::chrono::seconds(2)), "取消で孫まで終わる");
}

void testTimeoutKillsDescendants(const std::filesystem::path& child,
                                 const std::filesystem::path& root) {
    const auto pidFile = root / L"timeout-pids.txt";
    MvmProcessRequest request{};
    request.timeout_ms = 1500;
    const Run outcome = run(child, {L"spawn", pidFile.wstring(), L"600000"}, request);
    DWORD childPid = 0;
    DWORD grandchildPid = 0;
    check(outcome.status == MVM_PROCESS_TIMED_OUT, "孫を持つ子も timeout で止まる");
    check(readPids(pidFile, childPid, grandchildPid), "timeout 前に孫を起動していた");
    check(processExitsWithin(grandchildPid, std::chrono::seconds(2)), "timeout で孫まで終わる");
}

void testOrphanAfterNormalExit(const std::filesystem::path& child,
                               const std::filesystem::path& root) {
    const auto pidFile = root / L"orphan-pids.txt";
    const Run outcome = run(child, {L"spawn", pidFile.wstring(), L"0"});
    DWORD childPid = 0;
    DWORD grandchildPid = 0;
    check(outcome.status == MVM_PROCESS_EXITED && outcome.result.exit_code == 0,
          "孫を残して終わる子は EXITED/0");
    check(readPids(pidFile, childPid, grandchildPid), "子が孫の PID を書いた");
    check(processExitsWithin(grandchildPid, std::chrono::seconds(2)),
          "子の正常終了後に残った孫も、返る時点で終わらせる");
}

// 対照: job に入れずに起動した孫は、親が終わっても生き残る。上の検査は「孫が自然に
// 終わった」のではなく job が終わらせたことを示している。
void testControlGrandchildSurvivesWithoutJob(const std::filesystem::path& child,
                                             const std::filesystem::path& root) {
    const auto pidFile = root / L"control-pids.txt";
    std::wstring commandLine =
        L"\"" + child.wstring() + L"\" spawn \"" + pidFile.wstring() + L"\" 0";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(child.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        check(false, "対照の子を起動できる");
        return;
    }
    WaitForSingleObject(process.hProcess, 10000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DWORD childPid = 0;
    DWORD grandchildPid = 0;
    check(readPids(pidFile, childPid, grandchildPid), "対照の子が孫の PID を書いた");
    check(!processExitsWithin(grandchildPid, std::chrono::milliseconds(500)),
          "対照: job の外では孫が親の終了後も生き残る");
    if (HANDLE grandchild = OpenProcess(PROCESS_TERMINATE, FALSE, grandchildPid)) {
        TerminateProcess(grandchild, 1);
        CloseHandle(grandchild);
    }
}

void testQuoteArgumentSize() {
    const size_t required = mvm_process_quote_argument(L"a b", nullptr, 0);
    check(required == 6, "\"a b\" の必要文字数は終端込みで 6");
    wchar_t small[3] = {L'x', L'x', L'x'};
    check(mvm_process_quote_argument(L"a b", small, 3) == 6 && small[0] == L'x',
          "小さすぎる out には書かない");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 3) {
        std::fprintf(stderr, "使い方: mvm_test_process <fake-child.exe> <test-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    const auto child = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto root = std::filesystem::absolute(fromUtf8(argv[2])) / L"空白 日本語";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testQuoteArgumentSize();
    testExitAndOutput(child, root);
    testArguments(child, root);
    testWorkingDirectory(child, root);
    testStartFailure(root);
    testTimeout(child);
    testCancelKillsDescendants(child, root);
    testTimeoutKillsDescendants(child, root);
    testOrphanAfterNormalExit(child, root);
    testControlGrandchildSurvivesWithoutJob(child, root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    mvm_win_free_utf8_args(argv, argc);
    return failures == 0 && checks > 0 ? 0 : 1;
}
