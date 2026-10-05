// 数式 backend "manim-mathtex" (preflight と render) を偽の Manim / latex / dvisvgm で検査する。
// 実 Manim と LaTeX は使わない (それは math_manim_real_smoke の役目)。

#include "math_test_transform.h"
#include "media/manim/manim_math_tex.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
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
              static_cast<bool>(ok.backend.renderSequence) &&
              ok.backend.maximumSequenceFrames == 9999,
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

// --- 式から式への変形 (P2-3) -------------------------------------------------

const std::string kD1 = "x + x = 2x";
const std::string kD2 = "x + x + x = 3x";

math::MathTransformSpec transformSpec(const std::string& source, const std::string& target,
                                      std::int64_t frames, int sourceEm = 17, int targetEm = 85) {
    return {{"latex", source, sourceEm}, {"latex", target, targetEm}, frames};
}

void testTransformRequestJson() {
    // 端点 5x3 → 4x2。canvas は大きい方 + 各辺 200 で 405x403。
    // source: left (405-5)/2 = 200、shift (400+5-405)/2 = 0。top (403-3)/2 = 200、shift 0。
    // target: left floor(401/2) = 200、shift (400+4-405)/2 = -0.5。top floor(401/2) = 200、
    //   shift (400+2-403)/2 = -0.5 (raster で上へ半画素) → Manim の +Y (上) へ +0.5。
    const auto spec = transformSpec(kD1, kD2, 30);
    manim::ManimTransformPlan plan;
    std::string error;
    check(manim::planManimMathTransform(spec, 5, 3, 4, 2, plan, error), "計画を作れる: " + error);
    check(plan.canvasWidth == 405 && plan.canvasHeight == 403, "canvas は端点の最大 + 各辺 200");
    check(plan.placement.source == math::MathEndpointPlacement{200, 200, 0.0, 0.0} &&
              plan.placement.target == math::MathEndpointPlacement{200, 200, -0.5, -0.5},
          "端点の配置は P2-2 の契約 (raster の座標)");
    const std::string expected =
        "{\"frames\": 30, \"canvas_px\": [405, 403], "
        "\"source\": {\"segments\": [\"x\", \" + \", \"x\", \" = \", \"2x\"], "
        "\"manim_font_size\": 12, \"static_px\": [5, 3], \"placement_px\": [200, 200], "
        "\"manim_shift_px\": [0, 0]}, "
        "\"target\": {\"segments\": [\"x\", \" + \", \"x\", \" + \", \"x\", \" = \", \"3x\"], "
        "\"manim_font_size\": 60, \"static_px\": [4, 2], \"placement_px\": [200, 200], "
        "\"manim_shift_px\": [-0.5, 0.5]}, "
        "\"pairs\": [[0, 0], [1, 1], [2, 2], [3, 5]], \"unmatched_source\": [4], "
        "\"unmatched_target\": [3, 4, 6]}";
    const std::string json = manim::manimMathTransformRequestJson(spec, plan);
    check(json == expected, "重複項の request.json (n 番目の出現の対応・余り・縦の符号): " + json);
    for (int i = 0; i < 3; ++i) {
        manim::ManimTransformPlan again;
        check(manim::planManimMathTransform(spec, 5, 3, 4, 2, again, error) &&
                  manim::manimMathTransformRequestJson(spec, again) == expected,
              "request.json は決定的");
    }

    check(manim::manimShiftUpFor(-0.5) == 0.5 && manim::manimShiftUpFor(0.5) == -0.5,
          "raster の下向き (+Y) は Manim の下向き (-Y)");
    check(manim::manimShiftUpFor(0.0) == 0.0 && !std::signbit(manim::manimShiftUpFor(0.0)),
          "補正なしは +0 (JSON に -0 を書かない)");
    // 逆に source が奇数側: source 4x2・target 5x3 → source の shift は (-0.5, -0.5)。
    manim::ManimTransformPlan swapped;
    check(manim::planManimMathTransform(spec, 4, 2, 5, 3, swapped, error) &&
              manim::manimMathTransformRequestJson(spec, swapped)
                      .find("\"static_px\": [4, 2], \"placement_px\": [200, 200], "
                            "\"manim_shift_px\": [-0.5, 0.5]}, \"target\"") != std::string::npos,
          "source 側の半画素の補正も縦の符号を反転して渡す");

    // E1 → E2: \frac・\left \right を含む部分を escape して渡し、対応は mvm の照合のまま。
    const auto e12 = transformSpec("x^2 + \\frac{b}{a}x = -\\frac{c}{a}",
                                   "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                                   "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2",
                                   30, 96, 96);
    manim::ManimTransformPlan e12Plan;
    check(manim::planManimMathTransform(e12, 100, 40, 200, 60, e12Plan, error), "E1→E2 の計画");
    const std::string e12Json = manim::manimMathTransformRequestJson(e12, e12Plan);
    check(e12Json.find("\"segments\": [\"x^2\", \" + \", \"\\\\frac{b}{a}x\", \" = \", "
                       "\"-\\\\frac{c}{a}\"]") != std::string::npos &&
              e12Json.find("\"\\\\left(\\\\frac{b}{2a}\\\\right)^2\"") != std::string::npos,
          "部分の文字列は JSON の escape で渡す: " + e12Json);
    check(e12Json.find("\"pairs\": [[0, 0], [1, 1], [2, 2], [3, 5], [4, 6]], "
                       "\"unmatched_source\": [], \"unmatched_target\": [3, 4, 7, 8]}") !=
              std::string::npos,
          "E1→E2 の対応と余り");

    manim::ManimTransformPlan rejected;
    check(!manim::planManimMathTransform(transformSpec(kD1, kD2, 0), 5, 3, 4, 2, rejected, error),
          "0 枚の変形は計画しない");
    check(!manim::planManimMathTransform(
              transformSpec(kD1, kD2, manim::kMaximumMathTransformFrames + 1), 5, 3, 4, 2, rejected,
              error),
          "照合の 1 枚を足して 4 桁に収まらない枚数は計画しない");
    check(manim::planManimMathTransform(transformSpec(kD1, kD2, 9998), 5, 3, 4, 2, rejected, error),
          "9998 枚 (照合の 1 枚を足して 9999) は計画する");
    auto typst = transformSpec(kD1, kD2, 30);
    typst.target.syntax = "typst";
    check(!manim::planManimMathTransform(typst, 5, 3, 4, 2, rejected, error),
          "latex 以外の記法は計画しない");
    check(!manim::planManimMathTransform(transformSpec("", kD2, 30), 5, 3, 4, 2, rejected, error),
          "空の式は計画しない");
    check(!manim::planManimMathTransform(transformSpec(kD1, kD2, 30), 5, 0, 4, 2, rejected, error),
          "大きさ 0 の端点は計画しない");
}

void testTransformStructure() {
    const auto source = math::segmentMathTex("a + b"); // a | " + " | b
    const auto target = math::segmentMathTex("b");
    // 'a' = 61、' ' = 20、'+' = 2b、'b' = 62、'c' = 63 (手で書いた 16 進)。
    const std::string ok = "part source MathTexPart x61 1\r\n"
                           "part source MathTexPart x202b20 1\n"
                           "part source MathTexPart x62 1\n"
                           "part target MathTexPart x62 1\n";
    check(manim::checkManimTransformStructure(ok, source, target).empty(),
          "部分の数・型・文字列が一致すれば受け付ける (CRLF も可)");
    const auto rejects = [&](const std::string& report, const std::string& fragment,
                             const std::string& what) {
        const auto error = manim::checkManimTransformStructure(report, source, target);
        check(error.find(fragment) != std::string::npos, what + ": " + error);
    };
    rejects("fallback x" + std::string("436f756c64") + "\n" + ok, "式全体で代用",
            "Manim の代用の log があれば、部分が揃っていても拒否する");
    rejects("part source MathTexPart x61 1\npart source MathTexPart x202b20 1\n"
            "part target MathTexPart x62 1\n",
            "部分の数", "部分が足りなければ拒否する");
    rejects(ok + "part target MathTexPart x62 1\n", "部分の数", "部分が多ければ拒否する");
    rejects("part source VGroup none 9\npart source MathTexPart x202b20 1\n"
            "part source MathTexPart x62 1\npart target MathTexPart x62 1\n",
            "部分として作りませんでした", "式全体の group (tex_string なし) を拒否する");
    rejects("part source MathTexPart x63 1\npart source MathTexPart x202b20 1\n"
            "part source MathTexPart x62 1\npart target MathTexPart x62 1\n",
            "文字列が分けた部分と違います", "部分の文字列が違えば拒否する");
    rejects("part source MathTexPart xzz 1\n", "読めません", "16 進でない文字列を拒否する");
    rejects("garbage\n", "読めません", "知らない行を拒否する");
    rejects("", "報告しませんでした", "空の報告を拒否する");
}

std::uint64_t coverageLoads = 0;

bool loadFakeCoverage(const std::filesystem::path& file, math::MathCoverage& coverage,
                      std::string& error) {
    ++coverageLoads;
    if (!mvm::test::parseMathTestCoverage(readFile(file), coverage.width, coverage.height,
                                          coverage.alpha)) {
        error = "偽の被覆率の画像ではありません";
        return false;
    }
    return true;
}

math::MathCoverage endpointStatic(bool target, int width, int height) {
    math::MathCoverage coverage{width, height, {}};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            coverage.alpha.push_back(mvm::test::mathTestEndpointAlpha(target, x, y));
    return coverage;
}

math::MathTransformRenderResult
renderTransform(const std::filesystem::path& manimExe, const std::filesystem::path& job,
                const std::string& source, const std::string& target, std::int64_t frames = 6,
                std::chrono::milliseconds timeout = std::chrono::seconds(20),
                const std::atomic<bool>* cancel = nullptr) {
    math::MathTransformRenderRequest request;
    request.spec = transformSpec(source, target, frames, 96, 64);
    request.sourceStatic = endpointStatic(false, 30, 20);
    request.targetStatic = endpointStatic(true, 25, 15);
    request.jobDirectory = job;
    request.timeout = timeout;
    return manim::renderManimMathTransform(manimExe, request, loadFakeCoverage, cancel);
}

void testRenderTransform(const std::filesystem::path& fake, const std::filesystem::path& root) {
    const auto manimExe = install(fake, root / L"manim bin", L"manim.exe");
    const auto jobs = root / L"transform jobs 日本語";

    // 端点 30x20 → 25x15。canvas 430x420。
    // source: (200, 200)、shift 0。target: left floor(405/2) = 202、shift (404+25-430)/2 = -0.5、
    //   top floor(405/2) = 202、shift (404+15-420)/2 = -0.5。
    // artifact は source の矩形 (200,200,30,20) と target の矩形 (202,202,25,15) の和。
    coverageLoads = 0;
    const auto ok = renderTransform(manimExe, jobs / L"ok", "a + b", "b + a");
    check(ok.status == math::MathRenderStatus::Ok, "変形の連番は Ok: " + ok.message);
    check(ok.frames.size() == 6, "frames 枚を返す (照合の 1 枚は含めない)");
    bool ordered = ok.frames.size() == 6;
    for (std::size_t index = 0; ordered && index < ok.frames.size(); ++index) {
        wchar_t name[64] = {};
        std::swprintf(name, std::size(name), L"MvmMathTransform%04zu.png", index);
        ordered = ok.frames[index].filename() == name;
    }
    check(ordered, "frame は名前の順 (偽の Manim は逆順に書く)");
    check(coverageLoads == 7,
          "照合の 1 枚を含む全 7 枚の alpha を読む: " + std::to_string(coverageLoads));
    check(ok.canvasWidth == 430 && ok.canvasHeight == 420, "canvas は端点の最大 + 各辺 200");
    check(ok.placement.source == math::MathEndpointPlacement{200, 200, 0.0, 0.0} &&
              ok.placement.target == math::MathEndpointPlacement{202, 202, -0.5, -0.5},
          "端点の配置を返す");
    check(ok.artifact == math::MathRect{200, 200, 30, 20},
          "artifact は全 frame の外接矩形と両端の矩形の和");
    manim::ManimTransformPlan plan;
    std::string planError;
    manim::planManimMathTransform(transformSpec("a + b", "b + a", 6, 96, 64), 30, 20, 25, 15, plan,
                                  planError);
    check(
        readFile(jobs / L"ok" / L"request.json") ==
            manim::manimMathTransformRequestJson(transformSpec("a + b", "b + a", 6, 96, 64), plan),
        "計画の request.json を渡す");
    const std::string script = readFile(jobs / L"ok" / L"mvm_math_tex.py");
    check(script.find("class MvmMathTransform") != std::string::npos &&
              script.find("a + b") == std::string::npos,
          "変形の scene を書き、式を Python の source へ埋め込まない");
    check(script.find("TransformMatchingTex") == std::string::npos &&
              script.find("ReplacementTransform(SOURCE[i], TARGET[j]") != std::string::npos &&
              script.find("FadeOut(SOURCE[i]") != std::string::npos &&
              script.find("FadeIn(TARGET[j]") != std::string::npos,
          "対応は mvm の pairs・余りで組み、TransformMatchingTex を使わない");
    check(script.find("MathTex(*part[\"segments\"]") != std::string::npos &&
              script.find("UP * (shift_up / PX_PER_UNIT)") != std::string::npos,
          "MathTex(*segments) で作り、縦は Manim の上向きの shift で動かす");

    const auto spread = renderTransform(manimExe, jobs / L"spread", "FAKE_SPREAD + b", "b");
    // (10,12) から source の右下 (230,220) まで。
    check(spread.status == math::MathRenderStatus::Ok &&
              spread.artifact == math::MathRect{10, 12, 220, 208},
          "端点の外に出た途中の frame の alpha も artifact に含める");

    const auto fails = [&](const wchar_t* job, const std::string& source,
                           const std::string& fragment, const std::string& what) {
        const auto result = renderTransform(manimExe, jobs / job, source, "b");
        check(result.status == math::MathRenderStatus::Failed && result.frames.empty() &&
                  result.message.find(fragment) != std::string::npos,
              what + ": " + result.message);
    };
    fails(L"fallback", "FAKE_FALLBACK", "式全体で代用",
          "Manim の代用は、script がその後で失敗しても代用として拒否する");
    fails(L"part count", "FAKE_PART_COUNT + a", "部分の数", "部分の数が違えば拒否する");
    fails(L"part text", "FAKE_PART_TEXT + a", "文字列が分けた部分と違います",
          "部分の文字列が違えば拒否する");
    fails(L"no structure", "FAKE_NO_STRUCTURE", "報告しませんでした",
          "構造の報告が無ければ拒否する");
    fails(L"info size", "FAKE_INFO_SIZE", "canvas の大きさが要求と違います",
          "canvas の大きさが違えば拒否する");
    fails(L"missing", "FAKE_MISSING_FRAME", "7 枚ではありません (件数=6)",
          "1 枚足りなければ拒否する");
    fails(L"extra", "FAKE_EXTRA_FRAME", "7 枚ではありません (件数=8)", "1 枚多ければ拒否する");
    fails(L"malformed", "FAKE_MALFORMED_FRAME", "変形の frame 1 を読めません",
          "読めない frame を拒否する");
    fails(L"frame size", "FAKE_FRAME_SIZE", "変形の frame 1 の大きさが canvas と違います",
          "大きさの違う frame を拒否する");
    fails(L"edge", "FAKE_EDGE", "縁に触れました", "canvas の縁に触れた frame を拒否する");
    fails(L"first", "FAKE_FIRST_MISMATCH",
          "最初の frame が変形前の式の静止の描画と一致しません (違う画素 1)",
          "frame 0 が source の静止と 1 画素違えば拒否する");
    fails(L"last", "FAKE_LAST_MISMATCH", "終状態が変形後の式の静止の描画と一致しません",
          "終状態が target の静止とずれていれば拒否する");

    // 前の実行の structure.txt を読まない。
    const auto stale = jobs / L"stale";
    std::filesystem::create_directories(stale);
    {
        std::ofstream(stale / L"structure.txt") << "part source MathTexPart x7a 1\n";
    }
    const auto staleRun = renderTransform(manimExe, stale, "FAKE_NO_STRUCTURE", "b");
    check(staleRun.status == math::MathRenderStatus::Failed &&
              staleRun.message.find("報告しませんでした") != std::string::npos,
          "前の実行の structure.txt を報告として読まない: " + staleRun.message);

    const auto latex = renderTransform(manimExe, jobs / L"latex", "\\fracc FAKE_LATEX_ERROR", "b");
    check(latex.status == math::MathRenderStatus::InvalidSource &&
              latex.message == "Undefined control sequence.",
          "変形でも TeX の error は InvalidSource で、message は ! 行");

    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    const auto cancelled = renderTransform(manimExe, jobs / L"cancel", "FAKE_HANG", "b", 6,
                                           std::chrono::seconds(30), &cancel);
    canceller.join();
    check(cancelled.status == math::MathRenderStatus::Cancelled && cancelled.frames.empty(),
          "変形の取消は Cancelled");
    const auto timedOut = renderTransform(manimExe, jobs / L"hang", "FAKE_HANG", "b", 6,
                                          std::chrono::milliseconds(400));
    check(timedOut.status == math::MathRenderStatus::TimedOut, "終わらない変形は TimedOut");

    // Manim を起動しない不正な要求。
    const auto notRun = [&](const math::MathTransformRenderRequest& request,
                            const math::MathCoverageLoader& loader, const std::string& what) {
        const auto result = manim::renderManimMathTransform(manimExe, request, loader, nullptr);
        check(result.status == math::MathRenderStatus::Failed &&
                  !std::filesystem::exists(request.jobDirectory / L"request.json"),
              what + " は Manim を起動せずに Failed: " + result.message);
    };
    math::MathTransformRenderRequest invalid;
    invalid.spec = transformSpec("a", "b", 0, 96, 64);
    invalid.sourceStatic = endpointStatic(false, 30, 20);
    invalid.targetStatic = endpointStatic(true, 25, 15);
    invalid.jobDirectory = jobs / L"invalid frames";
    notRun(invalid, loadFakeCoverage, "0 枚の要求");
    invalid.spec.frames = 6;
    invalid.jobDirectory = jobs / L"invalid static";
    invalid.targetStatic.alpha.pop_back();
    notRun(invalid, loadFakeCoverage, "byte 数の合わない静止の mask");
    invalid.targetStatic = endpointStatic(true, 25, 15);
    invalid.jobDirectory = jobs / L"no loader";
    notRun(invalid, {}, "loader の無い要求");
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
    testTransformRequestJson();
    testTransformStructure();
    testRenderTransform(fake, root);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    mvm_win_free_utf8_args(argv, argc);
    return failures == 0 && checks > 0 ? 0 : 1;
}
