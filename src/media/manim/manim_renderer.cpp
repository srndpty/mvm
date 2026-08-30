#include "media/manim/manim_renderer.h"

#include "util/mvm_win_utf8.h"

#include <atomic>
#include <climits>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>
#include <windows.h>

namespace mvm::manim {
namespace {

constexpr wchar_t kOutputBaseName[] = L"mvm_manim_output";
constexpr wchar_t kOutputFileName[] = L"mvm_manim_output.mp4";

class WinHandle {
public:
    explicit WinHandle(HANDLE handle = nullptr) : handle_(handle) {}

    ~WinHandle() { close(); }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    HANDLE get() const { return handle_; }

    bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

    void close() {
        if (valid())
            CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_;
};

std::string pathToUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
    return result;
}

void appendError(ManimRenderResult& result, const std::string& message) {
    if (!result.stderrText.empty() && result.stderrText.back() != '\n')
        result.stderrText += '\n';
    result.stderrText += message;
    if (result.stderrText.empty() || result.stderrText.back() != '\n')
        result.stderrText += '\n';
}

std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring result = L"\"";
    for (std::size_t index = 0;; ++index) {
        std::size_t backslashes = 0;
        while (index < argument.size() && argument[index] == L'\\') {
            ++index;
            ++backslashes;
        }
        if (index == argument.size()) {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[index] == L'\"')
            result.append(backslashes * 2 + 1, L'\\');
        else
            result.append(backslashes, L'\\');
        result += argument[index];
    }
    result += L'\"';
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool validateRequest(const ManimRenderRequest& request, ManimRenderResult& result) {
    std::error_code error;
    if (request.manimExecutablePath.empty() ||
        !std::filesystem::is_regular_file(request.manimExecutablePath, error)) {
        appendError(result, "Manim executable が見つかりません: " +
                                pathToUtf8(request.manimExecutablePath));
        return false;
    }
    error.clear();
    if (request.scriptPath.empty() ||
        !std::filesystem::is_regular_file(request.scriptPath, error)) {
        appendError(result, "Manim script が見つかりません: " + pathToUtf8(request.scriptPath));
        return false;
    }
    if (request.sceneName.empty()) {
        appendError(result, "Manim Scene 名が空です");
        return false;
    }
    if (request.outputDirectory.empty()) {
        appendError(result, "Manim 出力ディレクトリが空です");
        return false;
    }
    if (request.width <= 0 || request.height <= 0 || request.fps <= 0) {
        appendError(result, "Manim の width、height、fps は正の整数で指定してください");
        return false;
    }
    return true;
}

std::filesystem::path createJobDirectory(const std::filesystem::path& outputDirectory,
                                         ManimRenderResult& result) {
    static std::atomic<unsigned long> counter{0};
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    const auto name =
        L"mvm-manim-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence);
    const auto jobDirectory = std::filesystem::absolute(outputDirectory) / name;

    std::error_code error;
    if (!std::filesystem::create_directories(jobDirectory, error) || error) {
        appendError(result, "Manim job directory を作成できません: " + pathToUtf8(jobDirectory) +
                                " (" + error.message() + ")");
        return {};
    }
    return jobDirectory;
}

std::vector<std::filesystem::path> findExpectedOutputs(const std::filesystem::path& jobDirectory) {
    std::vector<std::filesystem::path> matches;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        jobDirectory, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && !error &&
            iterator->path().filename() == kOutputFileName) {
            matches.push_back(std::filesystem::absolute(iterator->path()));
        }
        iterator.increment(error);
    }
    return matches;
}

} // namespace

ManimRenderResult renderManim(const ManimRenderRequest& request) {
    ManimRenderResult result;
    if (!validateRequest(request, result))
        return result;

    const auto jobDirectory = createJobDirectory(request.outputDirectory, result);
    if (jobDirectory.empty())
        return result;

    const auto stdoutPath = jobDirectory / L".mvm-stdout.txt";
    const auto stderrPath = jobDirectory / L".mvm-stderr.txt";

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    WinHandle stdoutHandle(CreateFileW(stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                       &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                       nullptr));
    WinHandle stderrHandle(CreateFileW(stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                       &security, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY,
                                       nullptr));
    WinHandle stdinHandle(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!stdoutHandle.valid() || !stderrHandle.valid() || !stdinHandle.valid()) {
        appendError(result, "Manim の標準入出力を準備できません");
        return result;
    }

    const auto resolution = std::to_wstring(request.width) + L"," + std::to_wstring(request.height);
    wchar_t* sceneText = mvm_utf8_to_wide(request.sceneName.c_str());
    if (!sceneText) {
        appendError(result, "Manim Scene 名が正しい UTF-8 ではありません");
        return result;
    }
    const std::wstring sceneName = sceneText;
    mvm_str_free(sceneText);
    const std::vector<std::wstring> arguments = {
        request.manimExecutablePath.wstring(),
        L"render",
        L"--format",
        L"mp4",
        L"--progress_bar",
        L"none",
        L"--resolution",
        resolution,
        L"--fps",
        std::to_wstring(request.fps),
        L"--media_dir",
        jobDirectory.wstring(),
        L"--output_file",
        kOutputBaseName,
        request.scriptPath.wstring(),
        sceneName,
    };

    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty())
            commandLine += L' ';
        commandLine += quoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = stdinHandle.get();
    startup.hStdOutput = stdoutHandle.get();
    startup.hStdError = stderrHandle.get();

    PROCESS_INFORMATION process{};
    const auto workingDirectory = std::filesystem::absolute(request.scriptPath).parent_path();
    const BOOL started = CreateProcessW(
        request.manimExecutablePath.c_str(), mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(), &startup, &process);
    if (!started) {
        const DWORD errorCode = GetLastError();
        char* message = mvm_win_error_message(errorCode);
        appendError(result, "Manim process を起動できません (Win32 error " +
                                std::to_string(errorCode) + "): " + (message ? message : ""));
        mvm_str_free(message);
        return result;
    }

    WinHandle processHandle(process.hProcess);
    WinHandle threadHandle(process.hThread);
    stdoutHandle.close();
    stderrHandle.close();
    stdinHandle.close();

    WaitForSingleObject(processHandle.get(), INFINITE);
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode))
        exitCode = static_cast<DWORD>(INT_MAX);
    result.exitCode = exitCode > static_cast<DWORD>(INT_MAX) ? INT_MAX : static_cast<int>(exitCode);

    result.stdoutText = readFile(stdoutPath);
    result.stderrText = readFile(stderrPath);
    std::error_code removeError;
    std::filesystem::remove(stdoutPath, removeError);
    removeError.clear();
    std::filesystem::remove(stderrPath, removeError);

    if (result.exitCode != 0)
        return result;

    const auto matches = findExpectedOutputs(jobDirectory);
    if (matches.size() != 1) {
        appendError(result, "固定名の Manim MP4 がちょうど 1 件ではありません (件数=" +
                                std::to_string(matches.size()) + "): " + pathToUtf8(jobDirectory));
        return result;
    }

    std::error_code sizeError;
    const auto size = std::filesystem::file_size(matches.front(), sizeError);
    if (sizeError || size == 0) {
        appendError(result, "Manim が生成した MP4 が空です: " + pathToUtf8(matches.front()));
        return result;
    }

    result.outputVideoPath = matches.front();
    result.success = true;
    return result;
}

} // namespace mvm::manim
