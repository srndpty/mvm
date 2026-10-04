#include "media/manim/manim_renderer.h"

#include "util/mvm_process.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <climits>
#include <cwchar>
#include <fstream>
#include <objbase.h>
#include <sstream>
#include <system_error>
#include <vector>

namespace mvm::manim {
namespace {

constexpr wchar_t kOutputBaseName[] = L"mvm_manim_output";
constexpr wchar_t kOutputFileName[] = L"mvm_manim_output.mp4";

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
    GUID invocationId{};
    const HRESULT guidResult = CoCreateGuid(&invocationId);
    if (FAILED(guidResult)) {
        appendError(result, "Manim invocation GUID を生成できません (HRESULT=" +
                                std::to_string(static_cast<unsigned long>(guidResult)) + ")");
        return {};
    }

    wchar_t guidText[33]{};
    const int written = std::swprintf(
        guidText, 33, L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", invocationId.Data1,
        static_cast<unsigned>(invocationId.Data2), static_cast<unsigned>(invocationId.Data3),
        static_cast<unsigned>(invocationId.Data4[0]), static_cast<unsigned>(invocationId.Data4[1]),
        static_cast<unsigned>(invocationId.Data4[2]), static_cast<unsigned>(invocationId.Data4[3]),
        static_cast<unsigned>(invocationId.Data4[4]), static_cast<unsigned>(invocationId.Data4[5]),
        static_cast<unsigned>(invocationId.Data4[6]), static_cast<unsigned>(invocationId.Data4[7]));
    if (written != 32) {
        appendError(result, "Manim invocation GUID を文字列化できません");
        return {};
    }

    const auto name = L"mvm-manim-" + std::to_wstring(GetCurrentProcessId()) + L"-" + guidText;
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

    const auto resolution = std::to_wstring(request.width) + L"," + std::to_wstring(request.height);
    wchar_t* sceneText = mvm_utf8_to_wide(request.sceneName.c_str());
    if (!sceneText) {
        appendError(result, "Manim Scene 名が正しい UTF-8 ではありません");
        return result;
    }
    const std::wstring sceneName = sceneText;
    mvm_str_free(sceneText);
    const std::vector<std::wstring> arguments = {
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

    std::vector<const wchar_t*> argumentPointers;
    argumentPointers.reserve(arguments.size());
    for (const auto& argument : arguments)
        argumentPointers.push_back(argument.c_str());

    const auto workingDirectory = std::filesystem::absolute(request.scriptPath).parent_path();
    MvmProcessRequest process{};
    process.executable = request.manimExecutablePath.c_str();
    process.arguments = argumentPointers.data();
    process.argument_count = argumentPointers.size();
    process.working_directory = workingDirectory.c_str();
    process.stdout_path = stdoutPath.c_str();
    process.stderr_path = stderrPath.c_str();
    MvmProcessResult ran{};
    mvm_process_run(&process, &ran);
    if (ran.status == MVM_PROCESS_START_FAILED) {
        char* message = mvm_win_error_message(ran.win32_error);
        appendError(result, "Manim process を起動できません (Win32 error " +
                                std::to_string(ran.win32_error) + "): " + (message ? message : ""));
        mvm_str_free(message);
        return result;
    }
    // timeout も取消も指定していないので、残るのは終了か待機の失敗だけである。
    result.exitCode = ran.status == MVM_PROCESS_EXITED ? ran.exit_code : INT_MAX;

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
