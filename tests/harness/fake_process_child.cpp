// mvm_process の試験で起動される子 process。第 1 引数で振る舞いを選ぶ。
//
//   exit <code>                 標準出力・標準エラーへ 1 行ずつ書いて code で終わる
//   args <out-file> <args...>   受け取った引数を「UTF-8 の byte 数\n内容\n」の並びで書く
//   cwd <out-file>              current directory を UTF-8 で書く
//   sleep <ms>                  ms だけ待って 0 で終わる
//   spawn <pid-file> <ms>       自分を `sleep 600000` で孫として起動し、
//                               "<自分の PID> <孫の PID>" を pid-file へ書いてから ms 待って 0
//                               で終わる

#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <string>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

// 読む側が途中までの内容を見ないよう、別名で書いてから置き換える。
bool writeAtomically(const std::filesystem::path& path, const std::string& contents) {
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary);
        output << contents;
        if (!output.good())
            return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

int spawn(const std::filesystem::path& pidFile, unsigned long waitMs) {
    wchar_t self[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (length == 0 || length >= std::size(self))
        return 10;
    std::wstring commandLine = L"\"" + std::wstring(self) + L"\" sleep 600000";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(self, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return 11;
    const std::string pids =
        std::to_string(GetCurrentProcessId()) + " " + std::to_string(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!writeAtomically(pidFile, pids))
        return 12;
    Sleep(waitMs);
    return 0;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc < 2)
        return 2;
    const std::string mode = argv[1];
    int code = 2;
    if (mode == "exit" && argc == 3) {
        // 改行を \r\n にしない。試験は runner が byte 列を変えずに渡すことを照合する。
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
        std::fputs("子の標準出力\n", stdout);
        std::fputs("子の標準エラー\n", stderr);
        code = std::atoi(argv[2]);
    } else if (mode == "args" && argc >= 3) {
        std::string contents;
        for (int index = 3; index < argc; ++index) {
            const std::string argument = argv[index];
            contents += std::to_string(argument.size()) + "\n" + argument + "\n";
        }
        code = writeAtomically(fromUtf8(argv[2]), contents) ? 0 : 3;
    } else if (mode == "cwd" && argc == 3) {
        char* current = mvm_wide_to_utf8(std::filesystem::current_path().c_str());
        code = current && writeAtomically(fromUtf8(argv[2]), current) ? 0 : 3;
        mvm_str_free(current);
    } else if (mode == "sleep" && argc == 3) {
        Sleep(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 10)));
        code = 0;
    } else if (mode == "spawn" && argc == 4) {
        code = spawn(fromUtf8(argv[2]), std::strtoul(argv[3], nullptr, 10));
    }
    mvm_win_free_utf8_args(argv, argc);
    return code;
}
