#include "media/manim/manim_renderer.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int gChecks = 0;
int gFailures = 0;

void check(bool condition, const std::string& description) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::fprintf(stderr, "FAIL %s\n", description.c_str());
    }
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

void writeScript(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << "# fake Manim script\n";
}

mvm::manim::ManimRenderRequest makeRequest(const std::filesystem::path& executable,
                                           const std::filesystem::path& script,
                                           const std::filesystem::path& outputDirectory,
                                           std::string sceneName) {
    return {
        .manimExecutablePath = executable,
        .scriptPath = script,
        .sceneName = std::move(sceneName),
        .outputDirectory = outputDirectory,
        .width = 320,
        .height = 180,
        .fps = 15,
    };
}

void testGood(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto script = root / L"good" / L"scene.py";
    writeScript(script);
    const auto result = mvm::manim::renderManim(
        makeRequest(executable, script, root / L"good-output", "GoodScene"));
    check(result.success, "正常な fake CLI が成功する");
    check(result.exitCode == 0, "正常終了コードを返す");
    check(result.outputVideoPath.filename() == L"mvm_manim_output.mp4", "固定名 MP4 のパスを返す");
    check(std::filesystem::file_size(result.outputVideoPath) > 0, "MP4 が空ではない");
    check(result.stdoutText.find("fake Manim 標準出力") != std::string::npos, "stdout を捕捉する");
    check(result.stderrText.find("fake Manim 標準エラー") != std::string::npos,
          "stderr を捕捉する");
}

void testNonZero(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto script = root / L"nonzero" / L"scene.py";
    writeScript(script);
    const auto result = mvm::manim::renderManim(
        makeRequest(executable, script, root / L"nonzero-output", "NonZeroScene"));
    check(!result.success, "非ゼロ終了を失敗にする");
    check(result.exitCode == 17, "Manim の非ゼロ終了コードを保持する");
    check(result.outputVideoPath.empty(), "失敗時に出力パスを返さない");
}

void testMissingOutput(const std::filesystem::path& executable, const std::filesystem::path& root) {
    const auto script = root / L"missing" / L"scene.py";
    writeScript(script);
    const auto result = mvm::manim::renderManim(
        makeRequest(executable, script, root / L"missing-output", "MissingOutputScene"));
    check(!result.success, "終了コード 0 でも MP4 が無ければ失敗する");
    check(result.exitCode == 0, "MP4 欠損時も process 終了コードを保持する");
    check(result.outputVideoPath.empty(), "MP4 欠損時に出力パスを返さない");
}

void testUnicodeAndSpaces(const std::filesystem::path& originalExecutable,
                          const std::filesystem::path& root) {
    const auto unicodeRoot = root / L"空白 日本語";
    const auto executable = unicodeRoot / L"実行 file.exe";
    std::filesystem::create_directories(unicodeRoot);
    std::filesystem::copy_file(originalExecutable, executable,
                               std::filesystem::copy_options::overwrite_existing);

    const auto script = unicodeRoot / L"script dir" / L"場面 file.py";
    writeScript(script);
    const auto result = mvm::manim::renderManim(
        makeRequest(executable, script, unicodeRoot / L"出力 dir", "UnicodeScene"));
    check(result.success, "空白・Unicode を含む executable/script/output path を扱える");
    check(result.outputVideoPath.wstring().find(L"出力 dir") != std::wstring::npos,
          "Unicode 出力ディレクトリ配下のパスを返す");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 3) {
        std::fprintf(stderr, "使い方: mvm_test_manim_renderer <fake-cli.exe> <test-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    const auto executable = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto root = std::filesystem::absolute(fromUtf8(argv[2]));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testGood(executable, root);
    testNonZero(executable, root);
    testMissingOutput(executable, root);
    testUnicodeAndSpaces(executable, root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", gChecks, gFailures);
    mvm_win_free_utf8_args(argv, argc);
    return gFailures == 0 ? 0 : 1;
}
