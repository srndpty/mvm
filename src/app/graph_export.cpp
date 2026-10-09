#include "app/graph_export.h"

#include "project/timeline_render.h"

#include <algorithm>
#include <chrono>

namespace mvm::app {
namespace {
GraphExportReadiness failed(GraphExportFailure reason, const std::string& detail) {
    return {reason, detail};
}

bool stopped(const GraphExportEnvironment& env, const std::function<bool()>& callback) {
    return (env.cancel && env.cancel->load()) || (callback && callback());
}

std::string packageKey(const graph::GraphRenderSpec& spec, const std::string& toolchain) {
    const auto key = graph::staticKey(spec, toolchain);
    return spec.drawFrames ? graph::drawKey(key, spec.drawFrames) : key;
}
} // namespace

GraphExportReadiness graphExportError(const graph::Error& error) {
    using F = graph::Failure;
    using E = GraphExportFailure;
    switch (error.failure) {
    case F::InvalidExpression:
        return failed(E::InvalidExpression, error.message);
    case F::UnsupportedExpression:
        return failed(E::UnsupportedExpression, error.message);
    case F::NoFiniteSamples:
        return failed(E::NoFiniteSamples, error.message);
    case F::ResourceLimit:
        return failed(E::ResourceLimit, error.message);
    case F::BackendUnavailable:
        return failed(E::BackendUnavailable, error.message);
    case F::ArtifactCorrupt:
        return failed(E::ArtifactCorrupt, error.message);
    case F::Cancelled:
        return failed(E::Cancelled, error.message);
    case F::Superseded:
        return failed(E::Superseded, error.message);
    case F::PublicationFailure:
        return failed(E::PublicationFailure, error.message);
    case F::RendererFailure:
    case F::LabelFailure:
        return failed(E::RendererFailure, error.message);
    default:
        return failed(E::InvalidGraph, error.message);
    }
}

GraphExportLedger prepareGraphExport(const project::Project& project, int width, int height,
                                     std::int64_t begin, std::int64_t end,
                                     const GraphExportEnvironment& env,
                                     const std::function<bool()>& cancelled) {
    GraphExportLedger ledger;
    const auto reject = [&](GraphExportReadiness reason) {
        ledger.readiness = std::move(reason);
        return ledger;
    };
    if (begin < 0 || end <= begin)
        return reject(failed(GraphExportFailure::InvalidGraph, "書き出し区間が不正です"));
    std::vector<project::TimelineRenderSegment> segments;
    std::string error;
    if (!project::timelineRenderSegments(project, project::TrackKind::Video, segments, error))
        return reject(failed(GraphExportFailure::InvalidGraph, error));
    std::map<std::string, graph::GraphRenderSpec> specs;
    for (const auto& segment : segments) {
        if (stopped(env, cancelled))
            return reject(failed(GraphExportFailure::Cancelled, "Graph の依存計画を取消しました"));
        const auto& clip = segment.original;
        if (clip.kind != project::TimelineClipKind::Graph || !clip.enabled ||
            !project::isTrackOutputEnabled(project, clip.track))
            continue;
        const auto duration = project::timelineClipDuration(project, segment.clip);
        if (!duration.success)
            return reject(failed(GraphExportFailure::InvalidGraph, duration.error));
        const auto first = std::max(begin, segment.clip.timelineStartFrame);
        const auto last = std::min(end, segment.clip.timelineStartFrame + duration.frame);
        if (first >= last)
            continue;
        if (std::any_of(project.timelineTransitions.begin(), project.timelineTransitions.end(),
                        [&](const auto& transition) {
                            return transition.outgoingClipId == clip.id ||
                                   transition.incomingClipId == clip.id;
                        }))
            return reject(failed(GraphExportFailure::UnsupportedTransition,
                                 "Graph の外側トランジションは未対応です"));
        auto compiled = compileGraphRender(clip.graph, clip.sourceFrameCount, width, height);
        if (const auto* reason = std::get_if<graph::Error>(&compiled))
            return reject(graphExportError(*reason));
        specs.emplace(clip.id, std::get<graph::GraphRenderSpec>(std::move(compiled)));
        auto& dependency = ledger.clips[clip.id];
        constexpr std::size_t maximumLedgerFrames = 1000000;
        if (static_cast<std::uint64_t>(last - first) >
            maximumLedgerFrames - dependency.frames.size())
            return reject(failed(GraphExportFailure::ResourceLimit,
                                 "Graph の書き出し frame 台帳が上限を超えます"));
        for (auto t = first; t < last; ++t) {
            if (stopped(env, cancelled))
                return reject(
                    failed(GraphExportFailure::Cancelled, "Graph の依存計画を取消しました"));
            const auto frame =
                project::evaluateGraphClip(clip, {project.timelineFpsNum, project.timelineFpsDen},
                                           t - clip.timelineStartFrame, error);
            if (!frame)
                return reject(failed(GraphExportFailure::InvalidGraph, error));
            const auto index = clip.graph.intro.kind == project::GraphIntroKind::Draw &&
                                       frame->sourceFrame < clip.graph.intro.frames
                                   ? frame->sourceFrame
                                   : -1;
            dependency.frames.push_back({t, frame->sourceFrame, index});
        }
    }
    if (ledger.clips.empty())
        return ledger;
    if (env.cache.empty())
        return reject(failed(GraphExportFailure::ArtifactMissing, "Graph の cache が未指定です"));
    // 確認済み identity があれば backend 不在でも検証できる。manifest 自体から期待値を捏造しない。
    std::string toolchain = env.toolchain;
    std::optional<graph::GraphBackend> backend;
    const auto work =
        env.cache /
        (".export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto preflight = [&]() -> GraphExportReadiness {
        if (stopped(env, cancelled))
            return failed(GraphExportFailure::Cancelled, "Graph の準備を取消しました");
        if (!env.preflight)
            return failed(GraphExportFailure::BackendUnavailable,
                          "Graph の描画環境を利用できません");
        auto result = env.preflight(work / "preflight", env.cancel);
        if (const auto* reason = std::get_if<graph::Error>(&result))
            return graphExportError(*reason);
        backend = std::get<graph::GraphBackend>(std::move(result));
        if (backend->toolchain.empty() || (!toolchain.empty() && toolchain != backend->toolchain))
            return failed(GraphExportFailure::ProvenanceMismatch,
                          "Graph の toolchain が一致しません");
        toolchain = backend->toolchain;
        return {};
    };
    if (toolchain.empty()) {
        if (auto ready = preflight(); !ready.ready())
            return reject(ready);
    }
    graph::PublicationAuthority authority;
    const auto generation = authority.supersede();
    // 同じ描画 key の全所有者を先に集約する。static clip が先でも Draw を二重生成しない。
    std::set<std::string> requiredDraw;
    for (const auto& [id, dependency] : ledger.clips)
        if (std::any_of(dependency.frames.begin(), dependency.frames.end(),
                        [](const auto& frame) { return frame.artifactFrame >= 0; }))
            requiredDraw.insert(packageKey(specs.at(id), toolchain));
    for (auto& [id, dependency] : ledger.clips) {
        if (stopped(env, cancelled))
            return reject(failed(GraphExportFailure::Cancelled, "Graph の準備を取消しました"));
        auto spec = specs.at(id);
        auto key = packageKey(spec, toolchain);
        const bool needsDraw = requiredDraw.contains(key);
        std::error_code ec;
        // 契約 B: 静止範囲は既存 Draw package の端点だけで十分。存在する破損 package は隠さない。
        if (!needsDraw && !std::filesystem::exists(env.cache / key, ec)) {
            spec.drawFrames = 0;
            key = packageKey(spec, toolchain);
        }
        dependency.packageKey = key;
        if (ledger.packages.contains(key))
            continue;
        graph::ArtifactResult result;
        if (std::filesystem::exists(env.cache / key, ec)) {
            result = graph::validateArtifact(env.cache / key, spec, toolchain, env.cancel);
        } else {
            if (!backend) {
                if (auto ready = preflight(); !ready.ready())
                    return reject(ready);
            }
            graph::RenderRequest request{spec, toolchain, work / key};
            result =
                authority.generate(request, env.cache, generation, backend->render, env.cancel);
        }
        if (const auto* reason = std::get_if<graph::Error>(&result))
            return reject(graphExportError(*reason));
        if (stopped(env, cancelled))
            return reject(failed(GraphExportFailure::Cancelled, "Graph の準備を取消しました"));
        auto package = std::make_shared<const GraphExportPackage>(
            GraphExportPackage{spec, toolchain, std::get<graph::Artifact>(std::move(result))});
        ledger.packages.emplace(key, std::move(package));
    }
    return ledger;
}

GraphExportFrameResult loadGraphExportFrame(const GraphExportLedger& ledger, const std::string& id,
                                            std::int64_t output, const std::atomic<bool>* cancel) {
    GraphExportFrameResult result;
    const auto reject = [&](GraphExportFailure failure, const std::string& message) {
        result.readiness = failed(failure, message);
        return result;
    };
    if (!ledger.readiness.ready()) {
        result.readiness = ledger.readiness;
        return result;
    }
    if (cancel && cancel->load())
        return reject(GraphExportFailure::Cancelled, "Graph の decode を取消しました");
    const auto clip = ledger.clips.find(id);
    if (clip == ledger.clips.end())
        return reject(GraphExportFailure::FrameMissing, "Graph の clip 依存がありません");
    const auto frame =
        std::lower_bound(clip->second.frames.begin(), clip->second.frames.end(), output,
                         [](const auto& value, auto t) { return value.outputFrame < t; });
    if (frame == clip->second.frames.end() || frame->outputFrame != output)
        return reject(GraphExportFailure::FrameMissing, "Graph の output frame 依存がありません");
    const auto found = ledger.packages.find(clip->second.packageKey);
    if (found == ledger.packages.end())
        return reject(GraphExportFailure::ArtifactMissing, "検証済み Graph package がありません");
    const auto& package = *found->second;
    const auto& artifact = package.artifact;
    const auto expectedKey = packageKey(package.spec, package.toolchain);
    if (expectedKey != clip->second.packageKey ||
        artifact.staticIdentity != graph::staticKey(package.spec, package.toolchain) ||
        (package.spec.drawFrames && artifact.drawIdentity != expectedKey))
        return reject(GraphExportFailure::ProvenanceMismatch,
                      "Graph package の identity が不正です");
    const auto index = frame->artifactFrame;
    const auto hashIndex = static_cast<std::size_t>(index + 1);
    if (index < -1 || index >= package.spec.drawFrames || hashIndex >= artifact.pixelHashes.size())
        return reject(GraphExportFailure::FrameMissing, "必要な Draw frame がありません");
    const auto name = index < 0 ? "static.png" : "frame-" + std::to_string(index) + ".png";
    std::error_code pathError;
    if (!std::filesystem::exists(artifact.directory / name, pathError))
        return reject(GraphExportFailure::FrameMissing, "必要な Graph PNG がありません");
    auto decoded =
        graph::readRgba(artifact.directory / name, package.spec.width, package.spec.height);
    if (const auto* reason = std::get_if<graph::Error>(&decoded))
        return reject(GraphExportFailure::DecodeFailure, reason->message);
    auto raster = std::get<graph::Raster>(std::move(decoded));
    const auto hash =
        graph::digest({reinterpret_cast<const char*>(raster.rgba.data()), raster.rgba.size()});
    if (hash != artifact.pixelHashes[hashIndex])
        return reject(GraphExportFailure::ArtifactCorrupt,
                      "Graph の decoded RGBA SHA が一致しません");
    if (cancel && cancel->load())
        return reject(GraphExportFailure::Cancelled, "Graph の decode を取消しました");
    result.sourceFrame = frame->sourceFrame;
    result.packageKey = expectedKey;
    result.pixelHash = hash;
    result.raster = std::make_shared<const graph::Raster>(std::move(raster));
    return result;
}
} // namespace mvm::app
