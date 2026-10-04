// 実 Manim + LaTeX で数式 backend "manim-mathtex" を確かめる手動の smoke。
//
// 通常の CTest には入れない (Manim と MiKTeX の導入を build・試験の必須条件にしない。
// mvm_manim_smoke と同じ扱い)。latex と dvisvgm は、この process の PATH から探す。
//
//   mvm_math_manim_smoke <manim.exe> <作業 directory>
//
// 二次方程式の解の公式の導出 (docs/math-clips.md の受け入れ scenario) の式をすべて描き、
// mvm の静止画 decoder で読み直して、大きさ・透過の余白・白い glyph を確かめる。
// 不正な式が InvalidSource になり、TeX の error 行が message になることも確かめる。

#include "media/manim/manim_math_tex.h"
#include "media/still_image/static_image.h"
#include "util/mvm_win_utf8.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
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

void inspectPng(const mvm::math::MathStaticRenderResult& rendered, const std::string& label) {
    const auto decoded = mvm::media::loadStaticImage(rendered.png);
    check(decoded.success, label + ": PNG を mvm の静止画 decoder で読める: " + decoded.error);
    if (!decoded.success)
        return;
    const auto& image = decoded.image;
    check(image.width == rendered.width && image.height == rendered.height,
          label + ": PNG の大きさが backend の報告と一致する");
    bool cornersClear = true;
    for (const auto [x, y] :
         {std::pair{0, 0}, std::pair{image.width - 1, 0}, std::pair{0, image.height - 1},
          std::pair{image.width - 1, image.height - 1}}) {
        const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x)) *
                        4U;
        cornersClear = cornersClear && image.rgba[at + 3] == 0;
    }
    check(cornersClear, label + ": 四隅 (padding) は透明");
    std::size_t opaque = 0;
    bool opaqueWhite = true;
    for (std::size_t at = 0; at + 3 < image.rgba.size(); at += 4) {
        if (image.rgba[at + 3] != 255)
            continue;
        ++opaque;
        opaqueWhite = opaqueWhite && image.rgba[at] == 255 && image.rgba[at + 1] == 255 &&
                      image.rgba[at + 2] == 255;
    }
    check(opaque > 0, label + ": 不透明な glyph の画素がある");
    check(opaqueWhite, label + ": 不透明な画素は白 (色は mvm 側で付ける)");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 3) {
        std::fprintf(stderr, "使い方: mvm_math_manim_smoke <manim.exe> <作業 directory>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    const auto manimExe = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto root = std::filesystem::absolute(fromUtf8(argv[2]));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    namespace math = mvm::math;
    const auto preflight =
        mvm::manim::preflightManimMathTex({manimExe, root / L"preflight"}, nullptr);
    if (preflight.status != math::MathPreflightStatus::Available) {
        std::fprintf(stderr, "UNAVAILABLE: %s\n", preflight.message.c_str());
        mvm_win_free_utf8_args(argv, argc);
        return 3;
    }
    std::printf("fingerprint:\n%s", preflight.backend.fingerprint.canonical.c_str());

    const std::vector<std::string> sources = {
        "ax^2 + bx + c = 0",
        "x^2 + \\frac{b}{a}x = -\\frac{c}{a}",
        "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
        "\\left(\\frac{b}{2a}\\right)^2 - \\frac{c}{a}",
        "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}",
        "x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}",
        "x = \\frac{-b \\pm \\sqrt{\\boxed{b^2 - 4ac}}}{2a}",
        // 引用符と改行を含む式が JSON 経由でそのまま Python へ届くこと。
        "\\text{\"quoted\"}\n+ 1",
    };
    int index = 0;
    for (const auto& source : sources) {
        math::MathStaticRenderRequest request;
        request.spec = {"latex", source, 96};
        request.jobDirectory = root / (L"job " + std::to_wstring(index));
        const auto started = std::chrono::steady_clock::now();
        const auto rendered = preflight.backend.render(request, nullptr);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const std::string label = "式 " + std::to_string(index);
        std::printf("%s: %s %dx%d %lld ms\n", label.c_str(),
                    math::mathRenderStatusName(rendered.status), rendered.width, rendered.height,
                    static_cast<long long>(elapsed));
        check(rendered.status == math::MathRenderStatus::Ok,
              label + " を描ける: " + rendered.message + "\n" + rendered.log);
        if (rendered.status == math::MathRenderStatus::Ok)
            inspectPng(rendered, label);
        ++index;
    }

    math::MathStaticRenderRequest bad;
    bad.spec = {"latex", "\\fracc{a}{b}", 96};
    bad.jobDirectory = root / L"job invalid";
    const auto invalid = preflight.backend.render(bad, nullptr);
    std::printf("不正な式: %s \"%s\"\n", math::mathRenderStatusName(invalid.status),
                invalid.message.c_str());
    check(invalid.status == math::MathRenderStatus::InvalidSource, "不正な式は InvalidSource");
    check(invalid.message.find("Undefined control sequence") != std::string::npos,
          "message は TeX の error 行");

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    mvm_win_free_utf8_args(argv, argc);
    return failures == 0 && checks > 0 ? 0 : 1;
}
