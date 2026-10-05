#include "media/manim/manim_math_tex.h"

#include "util/mvm_process.h"
#include "util/mvm_win_utf8.h"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace mvm::manim {
namespace {

// TeX の log から利用者へ見せる error 行の上限。
constexpr std::size_t kMaximumTexErrorLines = 3;
// result.log へ残す標準出力・標準エラーの上限 (末尾を残す)。
constexpr std::size_t kMaximumLogBytes = 64 * 1024;

// 全 script の共通部分。式は request.json から読み、Python の source には埋め込まない。
// 失敗は error-kind.txt / error.txt に書いて exit 3。
constexpr char kScriptPrelude[] = R"PY(import json
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent


def fail(kind, message):
    (HERE / "error-kind.txt").write_text(kind, encoding="utf-8")
    (HERE / "error.txt").write_text(message, encoding="utf-8")
    sys.exit(3)


try:
    from manim import ORIGIN, WHITE, MathTex, Scene, config
except Exception as error:
    fail("other", "manim を import できません: " + repr(error))

REQUEST = json.loads((HERE / "request.json").read_text(encoding="utf-8"))
PX_PER_UNIT = 1080.0 / 8.0
)PY";

// module level で MathTex を作り、その大きさに frame を合わせる。CLI の --resolution より
// module level の config が優先される (docs/math-clips.md P0-0)。
// 静止 (kStaticScene) と Write (kWriteScene) は kScriptPrelude + これの後に scene を足す。
// 3 つを連結した script は P1 までの script と同じ byte 列 (template の版を変えない)。
constexpr char kSingleTexSetup[] = R"PY(PAD_PX = 8

try:
    TEX = MathTex(REQUEST["source"], font_size=REQUEST["manim_font_size"])
except ValueError as error:
    # Manim は latex の失敗を ValueError ("latex error converting to dvi") で知らせる。
    fail("latex" if "latex error" in str(error).lower() else "other", str(error))
except Exception as error:
    fail("other", repr(error))

TEX.set_color(WHITE)
WIDTH = math.ceil(TEX.width * PX_PER_UNIT) + 2 * PAD_PX
HEIGHT = math.ceil(TEX.height * PX_PER_UNIT) + 2 * PAD_PX
config.pixel_width = WIDTH
config.pixel_height = HEIGHT
config.frame_width = WIDTH / PX_PER_UNIT
config.frame_height = HEIGHT / PX_PER_UNIT
(HERE / "info.txt").write_text(f"{WIDTH} {HEIGHT}", encoding="utf-8")
)PY";

constexpr char kStaticScene[] = R"PY(

class MvmMathTex(Scene):
    def construct(self):
        TEX.move_to(ORIGIN)
        self.add(TEX)
)PY";

// 1 秒の Write を frame_rate = frames で描くので、PNG はちょうど frames 枚で、frame i は進み具合
// i / frames。frame の大きさは静止と同じ (docs/math-clips.md P1-0)。
constexpr char kWriteScene[] = R"PY(
try:
    from manim import Write
except Exception as error:
    fail("other", "manim の Write を import できません: " + repr(error))

config.frame_rate = int(REQUEST["intro_frames"])


class MvmMathWrite(Scene):
    def construct(self):
        TEX.move_to(ORIGIN)
        self.play(Write(TEX), run_time=1)
)PY";

// 式から式への変形 (P2)。kScriptPrelude の後に置く。
// - 部分は mvm が分けた文字列のまま MathTex(*segments) で作る。
// - 部分の構造 (型・文字列・glyph の数) と Manim の代用の log を structure.txt へ事実として書く。
//   合否は mvm が決める (checkManimTransformStructure)。
// - 対応は mvm の照合 (pairs・unmatched_*) だけで組み、TransformMatchingTex に任せない。
// - 端点は canvas の中心に置き、mvm が Manim の向き (+Y が上) へ換算した shift だけ動かす。
// - frame_rate = frames で 1 秒の変形 (frames 枚) を描き、終状態を 1 枚足す (照合用)。
constexpr char kTransformScene[] = R"PY(import logging

try:
    from manim import (RIGHT, UP, AnimationGroup, FadeIn, FadeOut, ReplacementTransform, logger,
                       smooth)
except Exception as error:
    fail("other", "manim の変形を import できません: " + repr(error))


# Manim は部分の SVG group が見つからないと error を log に出し、式全体の group で代用する
# (tex_mobject.py の _break_up_by_substrings)。その log を集めて mvm へ報告する。
class FallbackLog(logging.Handler):
    def __init__(self):
        super().__init__(logging.ERROR)
        self.messages = []

    def emit(self, record):
        text = record.getMessage()
        if "Could not find SVG group" in text:
            self.messages.append(text)


FALLBACK_LOG = FallbackLog()
logger.addHandler(FALLBACK_LOG)


def build(side):
    part = REQUEST[side]
    try:
        return MathTex(*part["segments"], font_size=part["manim_font_size"])
    except ValueError as error:
        fail("latex" if "latex error" in str(error).lower() else "other", str(error))
    except Exception as error:
        fail("other", repr(error))


SOURCE = build("source")
TARGET = build("target")


def hex_text(text):
    return "x" + text.encode("utf-8").hex()


LINES = ["fallback " + hex_text(message) for message in FALLBACK_LOG.messages]
for side, tex in (("source", SOURCE), ("target", TARGET)):
    for part in tex.submobjects:
        text = getattr(part, "tex_string", None)
        LINES.append(" ".join(["part", side, type(part).__name__,
                               "none" if text is None else hex_text(text),
                               str(len(part.submobjects))]))
(HERE / "structure.txt").write_text("\n".join(LINES) + "\n", encoding="utf-8")

for side, tex in (("source", SOURCE), ("target", TARGET)):
    tex.set_color(WHITE)
    tex.move_to(ORIGIN)
    shift_x, shift_up = REQUEST[side]["manim_shift_px"]
    tex.shift(RIGHT * (shift_x / PX_PER_UNIT) + UP * (shift_up / PX_PER_UNIT))

WIDTH, HEIGHT = REQUEST["canvas_px"]
config.pixel_width = WIDTH
config.pixel_height = HEIGHT
config.frame_width = WIDTH / PX_PER_UNIT
config.frame_height = HEIGHT / PX_PER_UNIT
config.frame_rate = int(REQUEST["frames"])
(HERE / "info.txt").write_text(f"{WIDTH} {HEIGHT}", encoding="utf-8")


class MvmMathTransform(Scene):
    def construct(self):
        animations = [ReplacementTransform(SOURCE[i], TARGET[j], rate_func=smooth)
                      for i, j in REQUEST["pairs"]]
        animations += [FadeOut(SOURCE[i], rate_func=smooth) for i in REQUEST["unmatched_source"]]
        animations += [FadeIn(TARGET[j], rate_func=smooth) for j in REQUEST["unmatched_target"]]
        self.add(SOURCE)
        self.play(AnimationGroup(*animations), run_time=1)
        self.wait(1 / config.frame_rate)
)PY";

// 1 つの式を描く script (静止・Write)。
std::string singleTexScript(const char* sceneText) {
    return std::string(kScriptPrelude) + kSingleTexSetup + sceneText;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    char* text = mvm_wide_to_utf8(path.c_str());
    std::string result = text ? text : "";
    mvm_str_free(text);
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

bool writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    output.close();
    return output.good();
}

std::string trimLine(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' ||
                             line.back() == '\t'))
        line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
    return line.substr(start);
}

std::string firstNonEmptyLine(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (!line.empty())
            return line;
    }
    return {};
}

std::string tail(const std::string& text, std::size_t limit) {
    return text.size() <= limit ? text : text.substr(text.size() - limit);
}

int stopRequested(void* opaque) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(opaque);
    return cancel && cancel->load() ? 1 : 0;
}

struct ToolRun {
    MvmProcessResult result{};
    std::string stdoutText;
    std::string stderrText;
};

ToolRun runTool(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
                const std::filesystem::path& workingDirectory,
                const std::filesystem::path& stdoutPath, const std::filesystem::path& stderrPath,
                std::chrono::milliseconds timeout, const std::atomic<bool>* cancel) {
    std::vector<const wchar_t*> pointers;
    pointers.reserve(arguments.size());
    for (const auto& argument : arguments)
        pointers.push_back(argument.c_str());
    MvmProcessRequest request{};
    request.executable = executable.c_str();
    request.arguments = pointers.data();
    request.argument_count = pointers.size();
    request.working_directory = workingDirectory.c_str();
    request.stdout_path = stdoutPath.c_str();
    request.stderr_path = stderrPath.c_str();
    request.timeout_ms = static_cast<unsigned long>(timeout.count());
    request.should_stop = cancel ? stopRequested : nullptr;
    request.opaque = const_cast<std::atomic<bool>*>(cancel);
    ToolRun run;
    mvm_process_run(&request, &run.result);
    run.stdoutText = readFile(stdoutPath);
    run.stderrText = readFile(stderrPath);
    return run;
}

// 環境変数 PATH の各 directory だけから name を探す (子 process へ引き継ぐのと同じ PATH)。
std::filesystem::path findOnPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (size == 0)
        return {};
    std::wstring path(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"PATH", path.data(), size);
    path.resize(written);
    std::wistringstream entries(path);
    std::wstring entry;
    while (std::getline(entries, entry, L';')) {
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
            entry = entry.substr(1, entry.size() - 2);
        if (entry.empty())
            continue;
        const std::filesystem::path candidate = std::filesystem::path(entry) / name;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return {};
}

math::MathPreflightResult unavailable(std::string message) {
    math::MathPreflightResult result;
    result.status = math::MathPreflightStatus::Unavailable;
    result.message = std::move(message);
    return result;
}

// TeX の log の "!" で始まる行 (error の要約) を集める。
std::string texErrorLines(const std::filesystem::path& mediaDirectory) {
    std::vector<std::string> lines;
    std::error_code error;
    const auto texDirectory = mediaDirectory / L"Tex";
    for (std::filesystem::directory_iterator it(texDirectory, error), end; !error && it != end;
         it.increment(error)) {
        if (it->path().extension() != L".log")
            continue;
        std::istringstream log(readFile(it->path()));
        std::string line;
        while (std::getline(log, line) && lines.size() < kMaximumTexErrorLines) {
            if (!line.empty() && line.front() == '!')
                lines.push_back(trimLine(line.substr(1)));
        }
    }
    std::string joined;
    for (const auto& line : lines) {
        if (!joined.empty())
            joined += '\n';
        joined += line;
    }
    return joined;
}

// directory の下の PNG を file 名の順に返す。Manim は連番を固定桁 (<Scene>0000.png)
// で名付けるので、 名前の順が frame の順になる。
std::vector<std::filesystem::path> findPngs(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator
             it(directory, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == L".png")
            found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

// scene を 1 回描いた結果。status が Ok なら width / height / pngs が有効。
struct SceneRun {
    math::MathRenderStatus status = math::MathRenderStatus::Failed;
    std::string message;
    std::string log;
    int width = 0;
    int height = 0;
    std::vector<std::filesystem::path> pngs;
};

SceneRun sceneFailure(math::MathRenderStatus status, std::string message, std::string log = {}) {
    SceneRun run;
    run.status = status;
    run.message = std::move(message);
    run.log = std::move(log);
    return run;
}

// script と request.json を作業 directory へ書き、Manim で描く。
// 失敗の分類 (TeX の誤り・timeout・取消・起動失敗) は静止・Write・変形で同じ。
SceneRun runScene(const std::filesystem::path& manimExecutablePath,
                  const std::filesystem::path& jobDirectory, const std::string& scriptText,
                  const std::wstring& sceneName, const std::string& requestJson, bool lastFrameOnly,
                  std::chrono::milliseconds timeout, const std::atomic<bool>* cancel) {
    if (jobDirectory.empty())
        return sceneFailure(math::MathRenderStatus::Failed,
                            "数式の作業 directory が指定されていません");
    std::error_code error;
    std::filesystem::create_directories(jobDirectory, error);
    if (error)
        return sceneFailure(math::MathRenderStatus::Failed,
                            "数式の作業 directory を作成できません: " + pathToUtf8(jobDirectory) +
                                " (" + error.message() + ")");
    const auto script = jobDirectory / L"mvm_math_tex.py";
    if (!writeFile(script, scriptText) || !writeFile(jobDirectory / L"request.json", requestJson))
        return sceneFailure(math::MathRenderStatus::Failed,
                            "数式の描画 script を書けません: " + pathToUtf8(jobDirectory));

    const auto media = jobDirectory / L"media";
    std::vector<std::wstring> arguments = {L"render"};
    if (lastFrameOnly)
        arguments.push_back(L"-s");
    for (const wchar_t* argument :
         {L"--format", L"png", L"--transparent", L"--progress_bar", L"none", L"--media_dir"})
        arguments.push_back(argument);
    arguments.push_back(media.wstring());
    arguments.push_back(script.wstring());
    arguments.push_back(sceneName);
    const ToolRun run =
        runTool(manimExecutablePath, arguments, jobDirectory, jobDirectory / L"stdout.txt",
                jobDirectory / L"stderr.txt", timeout, cancel);
    const std::string log = tail(run.stdoutText + run.stderrText, kMaximumLogBytes);

    switch (run.result.status) {
    case MVM_PROCESS_CANCELLED:
        return sceneFailure(math::MathRenderStatus::Cancelled, "数式の描画を中断しました", log);
    case MVM_PROCESS_TIMED_OUT:
        return sceneFailure(math::MathRenderStatus::TimedOut,
                            "数式の描画が " + std::to_string(timeout.count() / 1000) +
                                " 秒で終わりませんでした",
                            log);
    case MVM_PROCESS_START_FAILED:
        return sceneFailure(math::MathRenderStatus::BackendUnavailable,
                            "Manim を起動できません (Win32 error " +
                                std::to_string(run.result.win32_error) +
                                "): " + pathToUtf8(manimExecutablePath),
                            log);
    case MVM_PROCESS_WAIT_FAILED:
        return sceneFailure(math::MathRenderStatus::Failed, "Manim の終了を待てませんでした", log);
    case MVM_PROCESS_EXITED:
        break;
    }

    if (run.result.exit_code != 0) {
        const std::string kind = trimLine(readFile(jobDirectory / L"error-kind.txt"));
        if (kind == "latex") {
            std::string message = texErrorLines(media);
            if (message.empty())
                message = "LaTeX が式を処理できませんでした";
            return sceneFailure(math::MathRenderStatus::InvalidSource, message, log);
        }
        std::string message = firstNonEmptyLine(readFile(jobDirectory / L"error.txt"));
        if (message.empty())
            message =
                "Manim が終了コード " + std::to_string(run.result.exit_code) + " で失敗しました";
        return sceneFailure(math::MathRenderStatus::Failed, message, log);
    }

    SceneRun result;
    std::istringstream info(readFile(jobDirectory / L"info.txt"));
    if (!(info >> result.width >> result.height) || result.width <= 0 || result.height <= 0)
        return sceneFailure(math::MathRenderStatus::Failed,
                            "Manim が数式の大きさを出力しませんでした", log);
    result.pngs = findPngs(media / L"images");
    for (const auto& png : result.pngs) {
        const auto size = std::filesystem::file_size(png, error);
        if (error || size == 0)
            return sceneFailure(math::MathRenderStatus::Failed, "Manim の PNG が空です", log);
    }
    result.status = math::MathRenderStatus::Ok;
    result.log = log;
    return result;
}

math::MathStaticRenderResult failure(math::MathRenderStatus status, std::string message,
                                     std::string log = {}) {
    math::MathStaticRenderResult result;
    result.status = status;
    result.message = std::move(message);
    result.log = std::move(log);
    return result;
}

math::MathSequenceRenderResult sequenceFailure(math::MathRenderStatus status, std::string message,
                                               std::string log = {}) {
    math::MathSequenceRenderResult result;
    result.status = status;
    result.message = std::move(message);
    result.log = std::move(log);
    return result;
}

std::string numberText(double value) {
    char number[64] = {};
    const auto converted = std::to_chars(number, number + sizeof(number) - 1, value);
    *converted.ptr = '\0';
    return number;
}

void appendJsonString(std::string& json, const std::string& text) {
    static const char kHex[] = "0123456789abcdef";
    json += '"';
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"') {
            json += "\\\"";
        } else if (c == '\\') {
            json += "\\\\";
        } else if (c < 0x20) {
            json += "\\u00";
            json += kHex[c >> 4];
            json += kHex[c & 0x0F];
        } else {
            json += static_cast<char>(c);
        }
    }
    json += '"';
}

} // namespace

double manimFontSizeFor(int emPixels) {
    return static_cast<double>(emPixels) * 12.0 / 17.0;
}

std::string manimMathTexRequestJson(const math::MathRenderSpec& spec) {
    std::string json = "{\"source\": ";
    appendJsonString(json, spec.source);
    json += ", \"manim_font_size\": ";
    json += numberText(manimFontSizeFor(spec.fontSize));
    json += "}";
    return json;
}

std::string manimMathWriteRequestJson(const math::MathSequenceSpec& spec) {
    std::string json = manimMathTexRequestJson(spec.still);
    json.pop_back();
    json += ", \"intro_frames\": " + std::to_string(spec.frames) + "}";
    return json;
}

math::MathPreflightResult preflightManimMathTex(const ManimMathTexConfig& config,
                                                const std::atomic<bool>* cancel) {
    std::error_code error;
    if (config.manimExecutablePath.empty() ||
        !std::filesystem::is_regular_file(config.manimExecutablePath, error))
        return unavailable("Manim の executable が見つかりません: " +
                           pathToUtf8(config.manimExecutablePath));
    if (config.workDirectory.empty())
        return unavailable("数式 backend の作業 directory が指定されていません");
    std::filesystem::create_directories(config.workDirectory, error);
    if (error)
        return unavailable("数式 backend の作業 directory を作成できません: " +
                           pathToUtf8(config.workDirectory) + " (" + error.message() + ")");

    struct Tool {
        const char* key;
        std::filesystem::path executable;
    };

    const auto latex = findOnPath(L"latex.exe");
    if (latex.empty())
        return unavailable("LaTeX (latex.exe) が PATH に見つかりません。MiKTeX を導入し、"
                           "PATH に追加してから mvm を起動し直してください");
    const auto dvisvgm = findOnPath(L"dvisvgm.exe");
    if (dvisvgm.empty())
        return unavailable("dvisvgm.exe が PATH に見つかりません。MiKTeX を導入し、"
                           "PATH に追加してから mvm を起動し直してください");
    const Tool tools[] = {
        {"manim", config.manimExecutablePath}, {"latex", latex}, {"dvisvgm", dvisvgm}};

    std::string canonical = "backend=" + std::string(kMathTexBackendId) + "\n" +
                            "template=" + std::to_string(kMathTexTemplateVersion) + "\n";
    const auto cancelled = [] {
        math::MathPreflightResult result;
        result.status = math::MathPreflightStatus::Cancelled;
        result.message = "数式 backend の確認を中断しました";
        return result;
    };
    for (const auto& tool : tools) {
        // 版の出力はすぐ終わるので、待機中の取消の確認だけでは間に合わない。起動の前にも見る。
        if (cancel && cancel->load())
            return cancelled();
        const std::string name = tool.key;
        const auto stdoutPath = config.workDirectory / (name + "-version.stdout.txt");
        const auto stderrPath = config.workDirectory / (name + "-version.stderr.txt");
        const ToolRun run = runTool(tool.executable, {L"--version"}, config.workDirectory,
                                    stdoutPath, stderrPath, config.toolTimeout, cancel);
        if (run.result.status == MVM_PROCESS_CANCELLED)
            return cancelled();
        if (run.result.status != MVM_PROCESS_EXITED || run.result.exit_code != 0)
            return unavailable(name + " --version が失敗しました (" + pathToUtf8(tool.executable) +
                               "): " + firstNonEmptyLine(run.stderrText));
        // 版は標準出力の最初の行だけから取る。標準エラーには MiKTeX の更新の催促などが
        // 混ざるので使わない (docs/math-clips.md P0-0)。
        const std::string version = firstNonEmptyLine(run.stdoutText);
        if (version.empty())
            return unavailable(name + " --version が版を出力しませんでした");
        canonical += name + "=" + version + "\n";
    }

    math::MathPreflightResult result;
    result.status = math::MathPreflightStatus::Available;
    result.backend.fingerprint = {kMathTexBackendId, canonical};
    const auto manim = config.manimExecutablePath;
    result.backend.render = [manim](const math::MathStaticRenderRequest& request,
                                    const std::atomic<bool>* renderCancel) {
        return renderManimMathTex(manim, request, renderCancel);
    };
    result.backend.sequenceTemplate =
        std::string(kMathWriteTemplateId) + "/" + std::to_string(kMathWriteTemplateVersion);
    result.backend.renderSequence = [manim](const math::MathSequenceRenderRequest& request,
                                            const std::atomic<bool>* renderCancel) {
        return renderManimMathWrite(manim, request, renderCancel);
    };
    result.backend.maximumSequenceFrames = kMaximumMathWriteFrames;
    return result;
}

math::MathStaticRenderResult renderManimMathTex(const std::filesystem::path& manimExecutablePath,
                                                const math::MathStaticRenderRequest& request,
                                                const std::atomic<bool>* cancel) {
    const auto& spec = request.spec;
    if (spec.syntax != "latex" || spec.source.empty() || spec.fontSize <= 0)
        return failure(math::MathRenderStatus::Failed, "数式の描画要求が不正です");
    const SceneRun run =
        runScene(manimExecutablePath, request.jobDirectory, singleTexScript(kStaticScene),
                 L"MvmMathTex", manimMathTexRequestJson(spec), true, request.timeout, cancel);
    if (run.status != math::MathRenderStatus::Ok)
        return failure(run.status, run.message, run.log);
    if (run.pngs.size() != 1)
        return failure(math::MathRenderStatus::Failed,
                       "Manim の PNG がちょうど 1 件ではありません (件数=" +
                           std::to_string(run.pngs.size()) + ")",
                       run.log);

    math::MathStaticRenderResult result;
    result.status = math::MathRenderStatus::Ok;
    result.png = run.pngs.front();
    result.width = run.width;
    result.height = run.height;
    result.log = run.log;
    return result;
}

math::MathSequenceRenderResult
renderManimMathWrite(const std::filesystem::path& manimExecutablePath,
                     const math::MathSequenceRenderRequest& request,
                     const std::atomic<bool>* cancel) {
    const auto& spec = request.spec;
    if (spec.animation != math::MathAnimationKind::Write || spec.still.syntax != "latex" ||
        spec.still.source.empty() || spec.still.fontSize <= 0 || spec.frames < 1 ||
        spec.frames > kMaximumMathWriteFrames)
        return sequenceFailure(math::MathRenderStatus::Failed, "数式の Write の描画要求が不正です");
    const SceneRun run =
        runScene(manimExecutablePath, request.jobDirectory, singleTexScript(kWriteScene),
                 L"MvmMathWrite", manimMathWriteRequestJson(spec), false, request.timeout, cancel);
    if (run.status != math::MathRenderStatus::Ok)
        return sequenceFailure(run.status, run.message, run.log);
    // frame の数が違えば、進み具合と frame 番号の対応が崩れる。黙って詰めたり補ったりしない。
    if (static_cast<std::int64_t>(run.pngs.size()) != spec.frames)
        return sequenceFailure(math::MathRenderStatus::Failed,
                               "Manim の Write の PNG が " + std::to_string(spec.frames) +
                                   " 枚ではありません (件数=" + std::to_string(run.pngs.size()) +
                                   ")",
                               run.log);

    math::MathSequenceRenderResult result;
    result.status = math::MathRenderStatus::Ok;
    result.frames = run.pngs;
    result.width = run.width;
    result.height = run.height;
    result.log = run.log;
    return result;
}

namespace {

math::MathTransformRenderResult transformFailure(math::MathRenderStatus status, std::string message,
                                                 std::string log = {}) {
    math::MathTransformRenderResult result;
    result.status = status;
    result.message = std::move(message);
    result.log = std::move(log);
    return result;
}

bool specValid(const math::MathRenderSpec& spec) {
    return spec.syntax == "latex" && !spec.source.empty() && spec.fontSize > 0;
}

// "x" + 16 進 (UTF-8 の byte 列) を戻す。形が違えば false。
bool decodeHexText(const std::string& token, std::string& text) {
    if (token.empty() || token[0] != 'x' || token.size() % 2 == 0)
        return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };
    text.clear();
    for (std::size_t at = 1; at + 1 < token.size(); at += 2) {
        const int high = nibble(token[at]);
        const int low = nibble(token[at + 1]);
        if (high < 0 || low < 0)
            return false;
        text += static_cast<char>(high * 16 + low);
    }
    return true;
}

void appendJsonPair(std::string& json, const char* name, double first, double second) {
    json += '"';
    json += name;
    json += "\": [" + numberText(first) + ", " + numberText(second) + "]";
}

void appendJsonIndices(std::string& json, const std::vector<std::size_t>& indices) {
    json += '[';
    for (std::size_t at = 0; at < indices.size(); ++at) {
        if (at > 0)
            json += ", ";
        json += std::to_string(indices[at]);
    }
    json += ']';
}

void appendTransformSide(std::string& json, const math::MathRenderSpec& spec,
                         const std::vector<math::MathTexSegment>& segments, int width, int height,
                         const math::MathEndpointPlacement& placement) {
    json += "{\"segments\": [";
    for (std::size_t at = 0; at < segments.size(); ++at) {
        if (at > 0)
            json += ", ";
        appendJsonString(json, segments[at].text);
    }
    json += "], \"manim_font_size\": " + numberText(manimFontSizeFor(spec.fontSize)) + ", ";
    appendJsonPair(json, "static_px", width, height);
    json += ", ";
    appendJsonPair(json, "placement_px", placement.left, placement.top);
    json += ", ";
    appendJsonPair(json, "manim_shift_px", placement.shiftX, manimShiftUpFor(placement.shiftY));
    json += '}';
}

} // namespace

bool planManimMathTransform(const math::MathTransformSpec& spec, int sourceWidth, int sourceHeight,
                            int targetWidth, int targetHeight, ManimTransformPlan& plan,
                            std::string& error) {
    if (!specValid(spec.source) || !specValid(spec.target) || spec.frames < 1 ||
        spec.frames > kMaximumMathTransformFrames) {
        error = "数式の変形の描画要求が不正です";
        return false;
    }
    // canvas の大きさの計算が int をあふれない範囲。
    constexpr int kMaximumSide = 1 << 20;
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 || targetHeight <= 0 ||
        std::max({sourceWidth, sourceHeight, targetWidth, targetHeight}) > kMaximumSide) {
        error = "数式の変形の端点の静止の大きさが不正です";
        return false;
    }
    ManimTransformPlan result;
    result.sourceSegments = math::segmentMathTex(spec.source.source);
    result.targetSegments = math::segmentMathTex(spec.target.source);
    result.matching = math::matchMathTexSegments(result.sourceSegments, result.targetSegments);
    result.sourceWidth = sourceWidth;
    result.sourceHeight = sourceHeight;
    result.targetWidth = targetWidth;
    result.targetHeight = targetHeight;
    result.canvasWidth = std::max(sourceWidth, targetWidth) + 2 * kMathTransformCanvasPadding;
    result.canvasHeight = std::max(sourceHeight, targetHeight) + 2 * kMathTransformCanvasPadding;
    if (!math::mathTransformPlacement(sourceWidth, sourceHeight, targetWidth, targetHeight,
                                      result.canvasWidth, result.canvasHeight, result.placement)) {
        error = "数式の変形の端点を canvas に置けません";
        return false;
    }
    plan = std::move(result);
    return true;
}

double manimShiftUpFor(double rasterShiftY) {
    // 0.0 - (+0.0) は +0.0 になり、JSON に "-0" を書かない。
    return 0.0 - rasterShiftY;
}

std::string manimMathTransformRequestJson(const math::MathTransformSpec& spec,
                                          const ManimTransformPlan& plan) {
    std::string json = "{\"frames\": " + std::to_string(spec.frames) + ", ";
    appendJsonPair(json, "canvas_px", plan.canvasWidth, plan.canvasHeight);
    json += ", \"source\": ";
    appendTransformSide(json, spec.source, plan.sourceSegments, plan.sourceWidth, plan.sourceHeight,
                        plan.placement.source);
    json += ", \"target\": ";
    appendTransformSide(json, spec.target, plan.targetSegments, plan.targetWidth, plan.targetHeight,
                        plan.placement.target);
    json += ", \"pairs\": [";
    for (std::size_t at = 0; at < plan.matching.pairs.size(); ++at) {
        if (at > 0)
            json += ", ";
        json += '[' + std::to_string(plan.matching.pairs[at].source) + ", " +
                std::to_string(plan.matching.pairs[at].target) + ']';
    }
    json += "], \"unmatched_source\": ";
    appendJsonIndices(json, plan.matching.unmatchedSource);
    json += ", \"unmatched_target\": ";
    appendJsonIndices(json, plan.matching.unmatchedTarget);
    json += '}';
    return json;
}

std::string checkManimTransformStructure(const std::string& report,
                                         const std::vector<math::MathTexSegment>& source,
                                         const std::vector<math::MathTexSegment>& target) {
    struct Part {
        std::string type;
        bool hasText = false;
        std::string text;
    };

    std::vector<Part> parts[2];
    std::istringstream lines(report);
    std::string line;
    bool any = false;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (line.empty())
            continue;
        any = true;
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "fallback") {
            std::string token;
            std::string message;
            fields >> token;
            if (!decodeHexText(token, message))
                message = "(log を読めません)";
            return "Manim が式の部分を見つけられず、式全体で代用しました: " + message;
        }
        std::string side;
        std::string text;
        long long glyphs = -1;
        Part part;
        if (kind != "part" || !(fields >> side >> part.type >> text >> glyphs) || glyphs < 0 ||
            (side != "source" && side != "target"))
            return "Manim の部分の構造の報告を読めません: " + line;
        if (text != "none") {
            if (!decodeHexText(text, part.text))
                return "Manim の部分の構造の報告を読めません: " + line;
            part.hasText = true;
        }
        parts[side == "source" ? 0 : 1].push_back(std::move(part));
    }
    if (!any)
        return "Manim が部分の構造を報告しませんでした";
    const std::vector<math::MathTexSegment>* expected[2] = {&source, &target};
    const char* names[2] = {"変形前", "変形後"};
    for (int side = 0; side < 2; ++side) {
        const auto& want = *expected[side];
        const auto& got = parts[side];
        if (got.size() != want.size())
            return std::string(names[side]) + "の式の部分の数が分けた数と違います (Manim " +
                   std::to_string(got.size()) + "、mvm " + std::to_string(want.size()) + ")";
        for (std::size_t at = 0; at < want.size(); ++at) {
            if (got[at].type != "MathTexPart" || !got[at].hasText)
                return std::string(names[side]) + "の式の " + std::to_string(at + 1) +
                       " 番目の部分を Manim が部分として作りませんでした (" + got[at].type + ")";
            if (got[at].text != want[at].text)
                return std::string(names[side]) + "の式の " + std::to_string(at + 1) +
                       " 番目の部分の文字列が分けた部分と違います (Manim \"" + got[at].text +
                       "\"、mvm \"" + want[at].text + "\")";
        }
    }
    return {};
}

math::MathTransformRenderResult
renderManimMathTransform(const std::filesystem::path& manimExecutablePath,
                         const math::MathTransformRenderRequest& request,
                         const math::MathCoverageLoader& loader, const std::atomic<bool>* cancel) {
    const auto& spec = request.spec;
    if (!loader || !math::mathCoverageValid(request.sourceStatic) ||
        !math::mathCoverageValid(request.targetStatic))
        return transformFailure(math::MathRenderStatus::Failed,
                                "数式の変形の端点の静止の画像が不正です");
    ManimTransformPlan plan;
    std::string planError;
    if (!planManimMathTransform(spec, request.sourceStatic.width, request.sourceStatic.height,
                                request.targetStatic.width, request.targetStatic.height, plan,
                                planError))
        return transformFailure(math::MathRenderStatus::Failed, planError);

    // 前の実行の structure.txt を読まないよう、描く前に消す。
    std::error_code error;
    const auto structurePath = request.jobDirectory / L"structure.txt";
    std::filesystem::remove(structurePath, error);
    const SceneRun run =
        runScene(manimExecutablePath, request.jobDirectory,
                 std::string(kScriptPrelude) + kTransformScene, L"MvmMathTransform",
                 manimMathTransformRequestJson(spec, plan), false, request.timeout, cancel);
    if (run.status == math::MathRenderStatus::Cancelled ||
        run.status == math::MathRenderStatus::TimedOut ||
        run.status == math::MathRenderStatus::BackendUnavailable)
        return transformFailure(run.status, run.message, run.log);
    // 部分の構造の誤りは、それが原因で script が後で失敗した場合も含めて先に知らせる
    // (代用された式全体の group では、部分の番号で組む変形が IndexError になる)。
    if (std::filesystem::is_regular_file(structurePath, error)) {
        const std::string structureError = checkManimTransformStructure(
            readFile(structurePath), plan.sourceSegments, plan.targetSegments);
        if (!structureError.empty())
            return transformFailure(math::MathRenderStatus::Failed, structureError, run.log);
    } else if (run.status == math::MathRenderStatus::Ok) {
        return transformFailure(math::MathRenderStatus::Failed,
                                "Manim が部分の構造を報告しませんでした", run.log);
    }
    if (run.status != math::MathRenderStatus::Ok)
        return transformFailure(run.status, run.message, run.log);
    if (run.width != plan.canvasWidth || run.height != plan.canvasHeight)
        return transformFailure(math::MathRenderStatus::Failed,
                                "Manim の変形の canvas の大きさが要求と違います (" +
                                    std::to_string(run.width) + "x" + std::to_string(run.height) +
                                    ")",
                                run.log);
    // frames 枚 + 終状態の 1 枚。足りない・多い連番を詰めたり補ったりしない。
    const auto expectedFrames = static_cast<std::size_t>(spec.frames) + 1;
    if (run.pngs.size() != expectedFrames)
        return transformFailure(math::MathRenderStatus::Failed,
                                "Manim の変形の PNG が " + std::to_string(expectedFrames) +
                                    " 枚ではありません (件数=" + std::to_string(run.pngs.size()) +
                                    ")",
                                run.log);

    const auto& placement = plan.placement;
    math::MathRect artifact = math::mathRectUnion(
        {placement.source.left, placement.source.top, plan.sourceWidth, plan.sourceHeight},
        {placement.target.left, placement.target.top, plan.targetWidth, plan.targetHeight});
    for (std::size_t index = 0; index < run.pngs.size(); ++index) {
        if (cancel && cancel->load())
            return transformFailure(math::MathRenderStatus::Cancelled, "数式の描画を中断しました",
                                    run.log);
        const std::string name = "変形の frame " + std::to_string(index);
        math::MathCoverage frame;
        std::string loadError;
        if (!loader(run.pngs[index], frame, loadError))
            return transformFailure(math::MathRenderStatus::Failed,
                                    name + " を読めません: " + loadError, run.log);
        if (!math::mathCoverageValid(frame) || frame.width != plan.canvasWidth ||
            frame.height != plan.canvasHeight)
            return transformFailure(math::MathRenderStatus::Failed,
                                    name + " の大きさが canvas と違います (" +
                                        std::to_string(frame.width) + "x" +
                                        std::to_string(frame.height) + ")",
                                    run.log);
        const math::MathRect bounds = math::mathCoverageBounds(frame);
        if (math::mathRectTouchesEdge(bounds, frame.width, frame.height))
            return transformFailure(math::MathRenderStatus::Failed,
                                    name + " の式が一時的な canvas の縁に触れました "
                                           "(はみ出した可能性があります)",
                                    run.log);
        artifact = math::mathRectUnion(artifact, bounds);
        // frame 0 は source の静止、最後 (照合の 1 枚) は target の静止と画素で一致する。
        const bool last = index + 1 == run.pngs.size();
        if (index == 0 || last) {
            const auto& mask = last ? request.targetStatic : request.sourceStatic;
            const auto& at = last ? placement.target : placement.source;
            const std::int64_t different =
                math::mathEndpointDifference(frame, mask, at.left, at.top);
            if (different != 0)
                return transformFailure(
                    math::MathRenderStatus::Failed,
                    std::string(last ? "変形の終状態が変形後" : "変形の最初の frame が変形前") +
                        "の式の静止の描画と一致しません (違う画素 " + std::to_string(different) +
                        ")",
                    run.log);
        }
    }

    math::MathTransformRenderResult result;
    result.status = math::MathRenderStatus::Ok;
    result.frames.assign(run.pngs.begin(), run.pngs.end() - 1);
    result.canvasWidth = plan.canvasWidth;
    result.canvasHeight = plan.canvasHeight;
    result.artifact = artifact;
    result.placement = placement;
    result.log = run.log;
    return result;
}

} // namespace mvm::manim
