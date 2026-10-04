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

// module level で MathTex を作り、その大きさに frame を合わせる。CLI の --resolution より
// module level の config が優先される (docs/math-clips.md P0-0)。式は request.json から読み、
// Python の source には埋め込まない。失敗は error-kind.txt / error.txt に書いて exit 3。
// 静止 (kStaticScene) と Write (kWriteScene) はこの共通部分の後に scene を足す。
constexpr char kSceneCommon[] = R"PY(import json
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
PAD_PX = 8

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

// 共通の scene に sceneText を足した script と request.json を作業 directory へ書き、Manim で描く。
// 失敗の分類 (TeX の誤り・timeout・取消・起動失敗) は静止と Write で同じ。
SceneRun runScene(const std::filesystem::path& manimExecutablePath,
                  const std::filesystem::path& jobDirectory, const char* sceneText,
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
    if (!writeFile(script, std::string(kSceneCommon) + sceneText) ||
        !writeFile(jobDirectory / L"request.json", requestJson))
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
        runScene(manimExecutablePath, request.jobDirectory, kStaticScene, L"MvmMathTex",
                 manimMathTexRequestJson(spec), true, request.timeout, cancel);
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
        runScene(manimExecutablePath, request.jobDirectory, kWriteScene, L"MvmMathWrite",
                 manimMathWriteRequestJson(spec), false, request.timeout, cancel);
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

} // namespace mvm::manim
