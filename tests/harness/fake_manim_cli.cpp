#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv)
        return 2;

    std::filesystem::path mediaDirectory;
    std::filesystem::path outputName;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--media_dir")
            mediaDirectory = fromUtf8(argv[++index]);
        else if (argument == "--output_file")
            outputName = fromUtf8(argv[++index]);
    }
    const std::string sceneName = argc > 1 ? argv[argc - 1] : "";

    std::puts("fake Manim 標準出力");
    std::fputs("fake Manim 標準エラー\n", stderr);

    if (sceneName == "NonZeroScene") {
        mvm_win_free_utf8_args(argv, argc);
        return 17;
    }
    if (sceneName == "MissingOutputScene") {
        mvm_win_free_utf8_args(argv, argc);
        return 0;
    }
    if (mediaDirectory.empty() || outputName.empty()) {
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    if (outputName.extension() != L".mp4")
        outputName += L".mp4";
    const auto nestedDirectory = mediaDirectory / L"内部" / L"任意 hierarchy";
    std::error_code error;
    std::filesystem::create_directories(nestedDirectory, error);
    if (error) {
        mvm_win_free_utf8_args(argv, argc);
        return 3;
    }

    std::ofstream output(nestedDirectory / outputName, std::ios::binary);
    output << "fake-mp4";
    const bool wrote = output.good();
    output.close();

    mvm_win_free_utf8_args(argv, argc);
    return wrote ? 0 : 4;
}
