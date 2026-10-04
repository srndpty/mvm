// 数式 backend "manim-mathtex" (preflight と render) を偽の Manim / latex / dvisvgm で検査する。
// 実 Manim と LaTeX は使わない (それは math_manim_real_smoke の役目)。

#include "media/manim/manim_math_tex.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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

namespace math = mvm::math;
namespace manim = mvm::manim;

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

// 偽の tool が読み込む実行時 DLL の directory (UCRT64 の bin)。latex と dvisvgm は含まない。
std::filesystem::path runtimeDirectory;

// PATH を「tool を探させたい項目」と実行時 DLL の directory だけにする。利用者の PATH に
// 本物の MiKTeX があっても、試験がそれを拾わないようにする。
void setPathEntries(const std::wstring& entries) {
    SetEnvironmentVariableW(L"PATH", (entries + L";" + runtimeDirectory.wstring()).c_str());
}

void setPath(const std::filesystem::path& directory) {
    setPathEntries(directory.wstring());
}

std::filesystem::path install(const std::filesystem::path& fake, const std::filesystem::path& dir,
                              const wchar_t* name) {
    std::filesystem::create_directories(dir);
    const auto target = dir / name;
    std::filesystem::copy_file(fake, target, std::filesystem::copy_options::overwrite_existing);
    return target;
}

void testRequestJson() {
    check(manim::manimFontSizeFor(17) == 12.0 && manim::manimFontSizeFor(85) == 60.0,
          "fontSize (em の px) x 12/17 を Manim の font_size にする");
    const math::MathRenderSpec spec{"latex", "a\"b\\c\n\t日本", 85};
    check(manim::manimMathTexRequestJson(spec) ==
              "{\"source\": \"a\\\"b\\\\c\\u000a\\u0009日本\", \"manim_font_size\": 60}",
          "式は JSON の文字列として escape して渡す (引用符・\\・制御文字)");
    const math::MathRenderSpec python{"latex", "\"\"\"\nimport os\n\"\"\"", 85};
    check(manim::manimMathTexRequestJson(python).find('\n') == std::string::npos,
          "改行を含む式でも request.json に生の改行を出さない");
    check(manim::manimMathWriteRequestJson({spec, math::MathAnimationKind::Write, 90}) ==
              "{\"source\": \"a\\\"b\\\\c\\u000a\\u0009日本\", \"manim_font_size\": 60, "
              "\"intro_frames\": 90}",
          "Write の request.json は静止の内容に intro_frames を足す");
}

math::MathPreflightResult preflight(const std::filesystem::path& manimExe,
                                    const std::filesystem::path& work) {
    return manim::preflightManimMathTex({manimExe, work, std::chrono::milliseconds(10000)},
                                        nullptr);
}

void testPreflight(const std::filesystem::path& fake, const std::filesystem::path& root) {
    const auto tools = root / L"tools 全部";
    const auto manimExe = install(fake, root / L"manim bin", L"manim.exe");
    install(fake, tools, L"latex.exe");
    install(fake, tools, L"dvisvgm.exe");
    // PATH の先頭に存在しない directory と引用符付きの項目を置いても探せる。
    setPathEntries(L"C:\\存在しない;\"" + tools.wstring() + L"\"");
    const auto ok = preflight(manimExe, root / L"work");
    check(ok.status == math::MathPreflightStatus::Available,
          "揃っていれば Available: " + ok.message);
    check(ok.backend.fingerprint.backendId == "manim-mathtex", "backend id");
    check(ok.backend.fingerprint.canonical ==
              "backend=manim-mathtex\ntemplate=1\nmanim=Manim Community v0.0-fake\n"
              "latex=latex fake 1.2.3\ndvisvgm=dvisvgm fake 3.4.5\n",
          "fingerprint は各 tool の標準出力の最初の行から作る (標準エラーを混ぜない): " +
              ok.backend.fingerprint.canonical);
    check(static_cast<bool>(ok.backend.render), "Available なら render 関数を束ねる");
    check(ok.backend.sequenceTemplate == "manim-write/1" &&
              static_cast<bool>(ok.backend.renderSequence),
          "Available なら Write の連番の関数と script の識別を束ねる (fingerprint には入れない)");

    const auto onlyDvisvgm = root / L"tools latex 無し";
    install(fake, onlyDvisvgm, L"dvisvgm.exe");
    setPath(onlyDvisvgm);
    const auto noLatex = preflight(manimExe, root / L"work");
    check(noLatex.status == math::MathPreflightStatus::Unavailable &&
              noLatex.message.find("latex.exe") != std::string::npos,
          "latex が PATH に無ければ Unavailable で latex.exe を名指しする");
    check(!noLatex.backend.render, "Unavailable では render 関数を渡さない");

    const auto onlyLatex = root / L"tools dvisvgm 無し";
    install(fake, onlyLatex, L"latex.exe");
    setPath(onlyLatex);
    const auto noDvisvgm = preflight(manimExe, root / L"work");
    check(noDvisvgm.status == math::MathPreflightStatus::Unavailable &&
              noDvisvgm.message.find("dvisvgm.exe") != std::string::npos,
          "dvisvgm が無ければ Unavailable");

    setPath(tools);
    const auto noManim = preflight(root / L"無い manim.exe", root / L"work");
    check(noManim.status == math::MathPreflightStatus::Unavailable,
          "Manim の executable が無ければ Unavailable");

    std::atomic<bool> cancel{true};
    const auto cancelled =
        manim::preflightManimMathTex({manimExe, root / L"work", std::chrono::seconds(10)}, &cancel);
    check(cancelled.status == math::MathPreflightStatus::Cancelled, "取消なら Cancelled");
}

math::MathStaticRenderResult render(const std::filesystem::path& manimExe,
                                    const std::filesystem::path& job, const std::string& source,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(20),
                                    const std::atomic<bool>* cancel = nullptr) {
    math::MathStaticRenderRequest request;
    request.spec = {"latex", source, 96};
    request.jobDirectory = job;
    request.timeout = timeout;
    return manim::renderManimMathTex(manimExe, request, cancel);
}

void testRender(const std::filesystem::path& fake, const std::filesystem::path& root) {
    const auto manimExe = install(fake, root / L"manim bin", L"manim.exe");
    const auto jobs = root / L"jobs 日本語";

    const auto ok = render(manimExe, jobs / L"ok", "x^2");
    check(ok.status == math::MathRenderStatus::Ok, "正常な描画は Ok: " + ok.message);
    check(ok.width == 3 && ok.height == 2, "大きさは script の info.txt から取る");
    check(std::filesystem::is_regular_file(ok.png) && ok.png.extension() == L".png",
          "PNG の path を返す");
    check(ok.png.wstring().find(L"jobs 日本語") != std::wstring::npos,
          "空白・日本語を含む作業 directory を扱える");
    check(readFile(jobs / L"ok" / L"request.json") ==
              manim::manimMathTexRequestJson({"latex", "x^2", 96}),
          "式は request.json で渡す");
    check(readFile(jobs / L"ok" / L"mvm_math_tex.py").find("x^2") == std::string::npos,
          "式を Python の source へ埋め込まない");

    const auto latex = render(manimExe, jobs / L"latex", "\\fracc FAKE_LATEX_ERROR");
    check(latex.status == math::MathRenderStatus::InvalidSource, "TeX の error は InvalidSource");
    check(latex.message == "Undefined control sequence.",
          "message は TeX の log の ! 行から作る: " + latex.message);
    check(latex.log.find("latex error converting to dvi") != std::string::npos,
          "log に標準エラーを残す");

    const auto other = render(manimExe, jobs / L"other", "FAKE_OTHER_ERROR");
    check(other.status == math::MathRenderStatus::Failed && other.message == "fake の別の失敗",
          "TeX 以外の失敗は Failed で、error.txt の最初の行を message にする");

    const auto noOutput = render(manimExe, jobs / L"no output", "FAKE_NO_OUTPUT");
    check(noOutput.status == math::MathRenderStatus::Failed, "exit 0 でも PNG が無ければ Failed");
    const auto twoOutputs = render(manimExe, jobs / L"two outputs", "FAKE_TWO_OUTPUTS");
    check(twoOutputs.status == math::MathRenderStatus::Failed, "PNG が 2 つなら Failed");

    const auto started = std::chrono::steady_clock::now();
    const auto timedOut =
        render(manimExe, jobs / L"hang", "FAKE_HANG", std::chrono::milliseconds(400));
    check(timedOut.status == math::MathRenderStatus::TimedOut, "終わらない描画は TimedOut");
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
          "timeout 後に速やかに返る");

    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    const auto cancelled =
        render(manimExe, jobs / L"cancel", "FAKE_HANG", std::chrono::seconds(30), &cancel);
    canceller.join();
    check(cancelled.status == math::MathRenderStatus::Cancelled, "取消は Cancelled");

    const auto missing = render(root / L"無い manim.exe", jobs / L"missing", "x");
    check(missing.status == math::MathRenderStatus::BackendUnavailable,
          "Manim を起動できなければ BackendUnavailable");

    math::MathStaticRenderRequest invalid;
    invalid.spec = {"typst", "x", 96};
    invalid.jobDirectory = jobs / L"invalid";
    check(manim::renderManimMathTex(manimExe, invalid, nullptr).status ==
              math::MathRenderStatus::Failed,
          "latex 以外の syntax は描かない");
    invalid.spec = {"latex", "", 96};
    check(manim::renderManimMathTex(manimExe, invalid, nullptr).status ==
              math::MathRenderStatus::Failed,
          "空の式は描かない");
}

math::MathSequenceRenderResult
renderWrite(const std::filesystem::path& manimExe, const std::filesystem::path& job,
            const std::string& source, std::int64_t frames,
            std::chrono::milliseconds timeout = std::chrono::seconds(20),
            const std::atomic<bool>* cancel = nullptr) {
    math::MathSequenceRenderRequest request;
    request.spec = {{"latex", source, 96}, math::MathAnimationKind::Write, frames};
    request.jobDirectory = job;
    request.timeout = timeout;
    return manim::renderManimMathWrite(manimExe, request, cancel);
}

void testRenderWrite(const std::filesystem::path& fake, const std::filesystem::path& root) {
    const auto manimExe = install(fake, root / L"manim bin", L"manim.exe");
    const auto jobs = root / L"write jobs 日本語";

    const auto ok = renderWrite(manimExe, jobs / L"ok", "x^2", 12);
    check(ok.status == math::MathRenderStatus::Ok, "Write の連番は Ok: " + ok.message);
    check(ok.frames.size() == 12 && ok.width == 3 && ok.height == 2,
          "intro_frames 枚の PNG と、静止と同じ info.txt の大きさを返す");
    bool ordered = ok.frames.size() == 12;
    for (std::size_t index = 0; ordered && index < ok.frames.size(); ++index) {
        wchar_t expected[64] = {};
        std::swprintf(expected, std::size(expected), L"MvmMathWrite%04zu.png", index);
        ordered = ok.frames[index].filename() == expected;
    }
    check(ordered, "frame は名前の順 (0000 から) に並べる (偽の Manim は逆順に書く)");
    check(readFile(jobs / L"ok" / L"request.json") ==
              manim::manimMathWriteRequestJson(
                  {{"latex", "x^2", 96}, math::MathAnimationKind::Write, 12}),
          "Write の式と枚数は request.json で渡す");
    const std::string script = readFile(jobs / L"ok" / L"mvm_math_tex.py");
    check(script.find("class MvmMathWrite") != std::string::npos &&
              script.find("x^2") == std::string::npos,
          "Write の scene を書き、式を Python の source へ埋め込まない");
    const std::string staticScript = readFile(root / L"jobs 日本語" / L"ok" / L"mvm_math_tex.py");
    check(!staticScript.empty() &&
              script.compare(0, script.find("\ntry:\n    from manim import Write"), staticScript, 0,
                             staticScript.find("\n\nclass MvmMathTex")) == 0,
          "Write と静止は frame の大きさを決める共通部分が同じ");

    const auto shortRun = renderWrite(manimExe, jobs / L"short", "FAKE_WRITE_SHORT", 12);
    check(shortRun.status == math::MathRenderStatus::Failed && shortRun.frames.empty(),
          "PNG が 1 枚足りなければ Failed (詰めたり補ったりしない)");
    const auto latex = renderWrite(manimExe, jobs / L"latex", "\\fracc FAKE_LATEX_ERROR", 12);
    check(latex.status == math::MathRenderStatus::InvalidSource &&
              latex.message == "Undefined control sequence.",
          "Write でも TeX の error は InvalidSource で、message は ! 行");

    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    const auto cancelled =
        renderWrite(manimExe, jobs / L"cancel", "FAKE_HANG", 12, std::chrono::seconds(30), &cancel);
    canceller.join();
    check(cancelled.status == math::MathRenderStatus::Cancelled, "Write の取消は Cancelled");
    const auto timedOut =
        renderWrite(manimExe, jobs / L"hang", "FAKE_HANG", 12, std::chrono::milliseconds(400));
    check(timedOut.status == math::MathRenderStatus::TimedOut, "終わらない Write は TimedOut");

    check(renderWrite(manimExe, jobs / L"zero", "x", 0).status == math::MathRenderStatus::Failed,
          "0 枚の Write は描かない");
    check(
        renderWrite(manimExe, jobs / L"too many", "x", manim::kMaximumMathWriteFrames + 1).status ==
            math::MathRenderStatus::Failed,
        "4 桁の連番に収まらない枚数は描かない");
}

void testBackendRender(const std::filesystem::path& fake, const std::filesystem::path& root) {
    const auto tools = root / L"tools 全部";
    setPath(tools);
    const auto manimExe = install(fake, root / L"manim bin", L"manim.exe");
    const auto ready = preflight(manimExe, root / L"work");
    if (!ready.backend.render) {
        check(false, "preflight が render 関数を返す");
        return;
    }
    math::MathStaticRenderRequest request;
    request.spec = {"latex", "y", 96};
    request.jobDirectory = root / L"backend job";
    const auto rendered = ready.backend.render(request, nullptr);
    check(rendered.status == math::MathRenderStatus::Ok && rendered.width == 3,
          "preflight が束ねた render 関数で描ける");
    math::MathSequenceRenderRequest sequence;
    sequence.spec = {{"latex", "y", 96}, math::MathAnimationKind::Write, 5};
    sequence.jobDirectory = root / L"backend write job";
    const auto written = ready.backend.renderSequence(sequence, nullptr);
    check(written.status == math::MathRenderStatus::Ok && written.frames.size() == 5,
          "preflight が束ねた連番の関数で描ける (-s を付けない)");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 4) {
        std::fprintf(stderr, "使い方: mvm_test_manim_math_tex <fake-math-tex.exe> <test-dir> "
                             "<runtime-dll-dir>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    const auto fake = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto root = std::filesystem::absolute(fromUtf8(argv[2]));
    runtimeDirectory = std::filesystem::absolute(fromUtf8(argv[3]));
    for (const wchar_t* tool : {L"latex.exe", L"dvisvgm.exe"}) {
        if (std::filesystem::exists(runtimeDirectory / tool)) {
            std::fprintf(stderr,
                         "PROTOCOL_INVALID: 実行時 DLL の directory に %ls があり、"
                         "tool が無い場合を試験できません\n",
                         tool);
            mvm_win_free_utf8_args(argv, argc);
            return 2;
        }
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    testRequestJson();
    testPreflight(fake, root);
    testRender(fake, root);
    testRenderWrite(fake, root);
    testBackendRender(fake, root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    mvm_win_free_utf8_args(argv, argc);
    return failures == 0 && checks > 0 ? 0 : 1;
}
