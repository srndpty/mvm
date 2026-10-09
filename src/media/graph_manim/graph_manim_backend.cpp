#include "media/manim/manim_graph.h"

#include "util/mvm_process.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace mvm::manim {
namespace {
struct Stop {
    const std::atomic<bool>* cancel;
    std::filesystem::path out, err;
    bool overflow = false;
};

int shouldStop(void* opaque) {
    auto& state = *static_cast<Stop*>(opaque);
    std::error_code ec;
    for (const auto& path : {state.out, state.err}) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > graph::Limits::outputBytes)
            state.overflow = true;
    }
    return state.overflow || (state.cancel && state.cancel->load());
}

std::string read(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size > graph::Limits::outputBytes)
        return {};
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

graph::RenderResult run(const std::filesystem::path& python, const std::filesystem::path& script,
                        const std::filesystem::path& work, bool fingerprint,
                        std::chrono::milliseconds timeout, const std::atomic<bool>* cancel) {
    if (cancel && cancel->load())
        return graph::Error{graph::Failure::Cancelled, 0, "起動前に取り消しました"};
    Stop stop{cancel, work / "stdout.txt", work / "stderr.txt"};
    const auto scriptText = std::filesystem::absolute(script).wstring();
    const wchar_t* argv[] = {scriptText.c_str(), fingerprint ? L"--fingerprint" : L"--render"};
    const auto executable = std::filesystem::absolute(python).wstring();
    const auto directory = std::filesystem::absolute(work).wstring();
    const auto stdoutPath = stop.out.wstring(), stderrPath = stop.err.wstring();
    MvmProcessRequest request{executable.c_str(),
                              argv,
                              2,
                              directory.c_str(),
                              stdoutPath.c_str(),
                              stderrPath.c_str(),
                              static_cast<unsigned long>(timeout.count()),
                              shouldStop,
                              &stop};
    MvmProcessResult result{};
    mvm_process_run(&request, &result);
    shouldStop(&stop);
    if (cancel && cancel->load())
        return graph::Error{graph::Failure::Cancelled, 0, "子 process を取り消しました"};
    if (stop.overflow)
        return graph::Error{graph::Failure::ResourceLimit, 0, "process 出力の上限を超えました"};
    if (result.status == MVM_PROCESS_START_FAILED)
        return graph::Error{graph::Failure::BackendUnavailable, 0, "Python を起動できません"};
    if (result.status != MVM_PROCESS_EXITED || result.exit_code != 0)
        return graph::Error{result.exit_code == 42   ? graph::Failure::LabelFailure
                            : result.exit_code == 43 ? graph::Failure::ArtifactCorrupt
                                                     : graph::Failure::RendererFailure,
                            0, "Graph renderer が失敗しました: " + read(stop.err)};
    return std::monostate{};
}
} // namespace

std::variant<GraphBackend, graph::Error> preflightGraph(const std::filesystem::path& python,
                                                        const std::filesystem::path& script,
                                                        const std::filesystem::path& work,
                                                        const std::atomic<bool>* cancel) {
    std::error_code ec;
    if (!std::filesystem::create_directories(work, ec) || ec)
        return graph::Error{graph::Failure::BackendUnavailable, 0,
                            "preflight の新規 directory が必要です"};
    auto result = run(python, script, work, true, std::chrono::milliseconds(30000), cancel);
    if (auto* error = std::get_if<graph::Error>(&result))
        return *error;
    auto fingerprint = read(work / "stdout.txt");
    for (std::size_t pos = 0; (pos = fingerprint.find("\r\n", pos)) != std::string::npos;)
        fingerprint.erase(pos, 1);
    if (!fingerprint.starts_with("mvm-graph-toolchain/1\n") || fingerprint.size() < 150)
        return graph::Error{graph::Failure::BackendUnavailable, 0,
                            "toolchain の実 identity を取得できません"};
    const auto source = read(script);
    if (source.empty())
        return graph::Error{graph::Failure::BackendUnavailable, 0, "backend template を読めません"};
    fingerprint +=
        "template_sha256=" + graph::digest(source) + "\nnumeric_build=" + MVM_GRAPH_BUILD_ID + "\n";
    GraphBackend backend;
    backend.toolchain = fingerprint;
    backend.render = [python, script, source,
                      fingerprint](const graph::RenderRequest& request,
                                   const std::atomic<bool>* flag) -> graph::RenderResult {
        if (request.toolchain != fingerprint || read(script) != source)
            return graph::Error{graph::Failure::BackendUnavailable, 0,
                                "backend authority が変わりました"};
        if (auto check = graph::validateSpec(request.spec);
            std::holds_alternative<graph::Error>(check))
            return std::get<graph::Error>(check);
        const auto json = graph::requestJson(request.spec, fingerprint);
        if (json.empty())
            return graph::Error{graph::Failure::ResourceLimit, 0,
                                "geometry JSON の上限を超えました"};
        if (request.timeout.count() <= 0 || request.timeout > std::chrono::milliseconds(300000))
            return graph::Error{graph::Failure::ResourceLimit, 0, "backend timeout が不正です"};
        {
            std::ofstream out(request.job / "request.json", std::ios::binary);
            out << json;
            out.close();
            if (!out)
                return graph::Error{graph::Failure::RendererFailure, 0, "request を書けません"};
        }
        auto rendered = run(python, script, request.job, false, request.timeout, flag);
        if (auto* error = std::get_if<graph::Error>(&rendered))
            return *error;
        std::string expected;
        for (std::int64_t i = -1; i < request.spec.drawFrames; ++i)
            expected += graph::frameProtocol(request.spec, i);
        auto structure = read(request.job / "structure.txt");
        for (std::size_t pos = 0; (pos = structure.find("\r\n", pos)) != std::string::npos;)
            structure.erase(pos, 1);
        if (structure != expected)
            return graph::Error{graph::Failure::ArtifactCorrupt, 0,
                                "backend の object 構造が一致しません"};
        std::istringstream labels(read(request.job / "labels.txt"));
        for (std::size_t i = 0; i < request.spec.labels.size(); ++i) {
            const auto& label = request.spec.labels[i];
            if (label.text.empty())
                continue;
            std::size_t index = 0;
            std::string text;
            double left = 0, top = 0, width = 0, height = 0;
            std::string hex;
            constexpr char digits[] = "0123456789abcdef";
            for (char c : label.text) {
                const auto byte = static_cast<unsigned char>(c);
                hex += digits[byte >> 4];
                hex += digits[byte & 15];
            }
            if (!(labels >> index >> text >> left >> top >> width >> height) || index != i ||
                text != hex || !std::isfinite(left) || !std::isfinite(top) ||
                !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 ||
                left < label.band.left || top < label.band.top ||
                left + width > label.band.left + label.band.width ||
                top + height > label.band.top + label.band.height)
                return graph::Error{graph::Failure::ArtifactCorrupt, i,
                                    "ラベルの内容または実際の配置が一致しません"};
        }
        std::string extra;
        if (labels >> extra)
            return graph::Error{graph::Failure::ArtifactCorrupt, 0, "余分なラベル報告があります"};
        if (request.spec.drawFrames) {
            const auto endpoint = graph::readRgba(request.job / "endpoint.png", request.spec.width,
                                                  request.spec.height);
            const auto still = graph::readRgba(request.job / "static.png", request.spec.width,
                                               request.spec.height);
            if (!std::holds_alternative<graph::Raster>(endpoint) ||
                !std::holds_alternative<graph::Raster>(still) ||
                std::get<graph::Raster>(endpoint).rgba != std::get<graph::Raster>(still).rgba)
                return graph::Error{graph::Failure::ArtifactCorrupt, 0,
                                    "Draw の静止端点画素が一致しません"};
        }
        return std::monostate{};
    };
    return backend;
}
} // namespace mvm::manim
