// 数式 backend "manim-mathtex" の試験用の偽 Manim / latex / dvisvgm。
//
// 自分の file 名 (拡張子を除く) で振る舞いを変える。
//   latex / dvisvgm      --version に版を 1 行出す (標準エラーには MiKTeX の催促に似た雑音)
//   それ以外 (Manim 役)  --version に版を出す。render では mvm の scene template の契約
//                        (request.json を読み、info.txt と PNG、失敗時は error-kind.txt /
//                        error.txt と TeX の log) を真似る。振る舞いは request.json に含まれる
//                        印で選ぶ:
//     FAKE_LATEX_ERROR   TeX の error (exit 3)
//     FAKE_OTHER_ERROR   TeX 以外の error (exit 3)
//     FAKE_HANG          終わらない
//     FAKE_NO_OUTPUT     PNG を書かずに 0 で終わる
//     FAKE_TWO_OUTPUTS   PNG を 2 つ書く
//     (印なし)           3x2 の白い glyph の PNG を書く

#include "math_test_png.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
}

void writePng(const std::filesystem::path& path) {
    writeFile(path, mvm::test::mathTestPngBytes());
}

int render(const std::filesystem::path& script, const std::filesystem::path& media) {
    const auto job = script.parent_path();
    const std::string request = readFile(job / L"request.json");
    if (request.empty())
        return 20;
    if (request.find("FAKE_HANG") != std::string::npos) {
        Sleep(INFINITE);
        return 21;
    }
    if (request.find("FAKE_LATEX_ERROR") != std::string::npos) {
        writeFile(media / L"Tex" / L"0123abcd.log", "This is pdfTeX (fake)\n"
                                                    "! Undefined control sequence.\n"
                                                    "<argument> ...\\fracc \n"
                                                    "l.9 ...\n");
        writeFile(job / L"error-kind.txt", "latex");
        writeFile(job / L"error.txt", "latex error converting to dvi. See log output above");
        std::fputs("ValueError: latex error converting to dvi\n", stderr);
        return 3;
    }
    if (request.find("FAKE_OTHER_ERROR") != std::string::npos) {
        writeFile(job / L"error-kind.txt", "other");
        writeFile(job / L"error.txt", "fake の別の失敗\n2 行目は使わない");
        return 3;
    }
    writeFile(job / L"info.txt", "3 2");
    if (request.find("FAKE_NO_OUTPUT") != std::string::npos)
        return 0;
    const auto images = media / L"images" / L"mvm_math_tex";
    writePng(images / L"MvmMathTex_ManimCE_fake.png");
    if (request.find("FAKE_TWO_OUTPUTS") != std::string::npos)
        writePng(images / L"MvmMathTex_second.png");
    std::puts("fake Manim: rendered");
    return 0;
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv)
        return 2;

    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const std::wstring role = std::filesystem::path(self).stem().wstring();

    int code = 2;
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::fputs("催促: MiKTeX の更新を確認していません\n", stderr);
        if (role == L"latex")
            std::puts("latex fake 1.2.3");
        else if (role == L"dvisvgm")
            std::puts("dvisvgm fake 3.4.5");
        else
            std::puts("Manim Community v0.0-fake");
        code = 0;
    } else if (argc >= 4 && std::string(argv[1]) == "render") {
        std::filesystem::path media;
        for (int index = 2; index + 1 < argc; ++index) {
            if (std::string(argv[index]) == "--media_dir")
                media = fromUtf8(argv[index + 1]);
        }
        const auto script = fromUtf8(argv[argc - 2]);
        code = media.empty() ? 2 : render(script, media);
    }
    mvm_win_free_utf8_args(argv, argc);
    return code;
}
