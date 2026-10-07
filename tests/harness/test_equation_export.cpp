// P3-6: disk の検証・可視範囲・分割・履歴のない取得を、preview の常駐と独立に検査する。
#include "app/math_clip_render.h"
#include "app/timeline_export.h"
#include "math_fake_backend.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/equation_sequence_edit.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <future>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
#include <QThread>
#include <QUuid>

using namespace mvm;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message.c_str());
    }
}

bool pump(const std::function<bool()>& predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), {}};
}

void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
}

project::Project fixture(int stateCount = 2) {
    auto p = project::createDefaultProject();
    p.outputWidth = 320;
    p.outputHeight = 240;
    project::EquationSequenceClipData data;
    for (int i = 0; i < stateCount; ++i) {
        project::EquationState state;
        state.id = {"state" + std::to_string(i)};
        state.revision = "revision" + std::to_string(i);
        state.equation.source = std::string(1, static_cast<char>('x' + i)) + "=b^2-4ac";
        state.equation.color = i == 0 ? "#FF102030" : "#FF708090";
        state.equation.fontSize = 64;
        state.holdFrames = 12;
        state.parts.push_back({{"part" + std::to_string(i)},
                               "判別式",
                               {state.revision, 2, 9, "b^2-4ac", project::BindingStatus::Bound}});
        data.states.push_back(state);
    }
    for (int i = 0; i + 1 < stateCount; ++i)
        data.transitions.push_back(
            {{i == 0 ? "transition" : "transition" + std::to_string(i)},
             {"state" + std::to_string(i)},
             {"state" + std::to_string(i + 1)},
             4,
             {{{"part" + std::to_string(i)}, {"part" + std::to_string(i + 1)}}}});
    project::EquationAction outline;
    outline.id = {"outline"};
    outline.state = {"state0"};
    outline.target = {"part0"};
    outline.start = 2;
    outline.duration = 3;
    outline.operation = project::EquationOperation::Outline;
    auto pulse = outline;
    pulse.id = {"pulse"};
    pulse.start = 7;
    pulse.operation = project::EquationOperation::Pulse;
    data.actions = {outline, pulse};
    check(
        project::addEquationSequence(p, data, "sequence", "数式", {project::TrackKind::Video, 0}, 0)
            .success,
        "対照 Project の構造が有効");
    return p;
}

// exporter の取得関数を呼ばず、P3-1 と検証済み artifact と CPU preview から期待値を作る。
std::vector<std::uint8_t> oracle(const app::EquationExportSnapshot& snapshot,
                                 const app::EquationSequenceArtifact& artifact,
                                 std::int64_t frame) {
    auto inputs = snapshot.presentation;
    std::string error;
    const auto time = app::equationPreviewTimeAt(
        inputs.clip, inputs.timelineFpsNum, inputs.timelineFpsDen, &*inputs.spec, frame, error);
    if (!time)
        return {};
    for (const auto& ref : app::equationPreviewCurrentLayers(time->lookup)) {
        const app::EquationArtifactFrame* record = nullptr;
        int width = 0, height = 0;
        if (ref.role == app::EquationPreviewLayerRole::TransitionFrame) {
            const auto& item = artifact.transitions.at(ref.interval);
            record = &item.frames.at(static_cast<std::size_t>(ref.frame));
            width = item.width;
            height = item.height;
        } else {
            const auto& item = artifact.actions.at(ref.interval);
            record = ref.role == app::EquationPreviewLayerRole::ActionBase
                         ? &item.base
                         : &item.accent.at(static_cast<std::size_t>(ref.frame));
            width = item.width;
            height = item.height;
        }
        std::vector<std::uint8_t> bytes;
        if (!app::loadEquationArtifactFrame(*record, width, height, bytes, error))
            return {};
        auto coverage = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        if (ref.role == app::EquationPreviewLayerRole::TransitionFrame)
            inputs.transitionFrames[{ref.interval, ref.frame}] = coverage;
        else if (ref.role == app::EquationPreviewLayerRole::ActionBase)
            inputs.actionBases[ref.interval] = coverage;
        else
            inputs.actionAccents[{ref.interval, ref.frame}] = coverage;
    }
    app::EquationPreviewModel model(inputs);
    const auto rect = model.patchRect();
    std::vector<std::uint8_t> patch(static_cast<std::size_t>(rect.width * rect.height * 4));
    model.fill(model.stateCode(model.shownAt(frame)), patch.data());
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(inputs.outputWidth * inputs.outputHeight * 4));
    for (int y = 0; y < rect.height; ++y)
        std::copy_n(patch.data() + y * rect.width * 4, rect.width * 4,
                    rgba.data() + ((rect.y + y) * inputs.outputWidth + rect.x) * 4);
    return rgba;
}

void dependencyScope(const std::filesystem::path& root) {
    auto p = fixture(3);
    auto later = p.timelineClips[0].equationSequence.actions[0];
    later.id = {"later-outline"};
    later.state = {"state2"};
    later.target = {"part2"};
    p.timelineClips[0].equationSequence.actions.push_back(later);
    const auto clip = p.timelineClips[0];
    const auto compiled = app::compileEquationSequence(clip.equationSequence);
    std::string error;
    const auto spec = *app::equationSequenceRenderSpecFor(*compiled.value, error);
    test::FakeMathBackend backend;
    app::MathRasterCache cache("p361", backend.preflight());
    cache.setAuthority(root / "scope-cache", true);
    check(
        pump([&] { return cache.backendState() == app::MathRasterCache::BackendState::Available; }),
        "依存範囲の描画環境を確認");
    cache.requestEquationSequence(spec);
    check(pump([&] {
              return cache.requestEquationSequence(spec).state ==
                     app::MathRasterCache::State::Ready;
          }),
          "三状態の依存範囲対照を準備");
    const auto artifact = cache.readyEquationSequence(spec);
    if (!artifact) {
        check(false, "依存範囲の artifact が存在する");
        return;
    }
    app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;
    request.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(clip);
    const auto full = app::mapTimelineExportPlan(p, request);
    check(full.success, "依存範囲の完全な対照は通る");
    if (!full.success)
        return;
    const auto baseline = full.equationSequences.at(clip.id).snapshot;
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> bytes;
    for (const auto& key : artifact->stateStaticKeys) {
        paths.push_back(root / "scope-cache" / (key + ".png"));
        bytes.push_back(read(paths.back()));
        write(root / ("scope-static-" + std::to_string(bytes.size() - 1) + ".original.png"),
              bytes.back());
    }
    const auto range = [&](int first, int last) {
        auto trimmed = p;
        trimmed.timelineClips[0].sourceInFrame = first;
        trimmed.timelineClips[0].sourceOutFrame = last;
        return trimmed;
    };
    const auto exact = [&](const project::Project& trimmed, const std::string& label) {
        const auto plan = app::mapTimelineExportPlan(trimmed, request);
        check(plan.success, label + ": preflight が通る");
        if (!plan.success)
            return;
        const auto& snapshot = plan.equationSequences.at(clip.id).snapshot;
        std::int64_t output = trimmed.timelineClips[0].timelineStartFrame;
        for (const auto& frame : plan.equationSequences.at(clip.id).frames) {
            std::vector<std::uint8_t> rgba;
            const auto source = frame.sourceFrame;
            check(app::composeEquationExportFrame(snapshot, output, rgba).ready() &&
                      rgba == oracle(baseline, *artifact, source),
                  label + ": 完全な対照と画素一致");
            ++output;
        }
    };
    for (const bool corrupt : {false, true}) {
        if (corrupt)
            write(paths[2], "画面外の静止を壊した対照");
        else
            std::filesystem::rename(paths[2], root / "scope-late-missing.png");
        exact(range(2, 5), "早い action は画面外の後半 static を要求しない");
        exact(range(12, 16), "早い transition は画面外の後半 static を要求しない");
        if (!corrupt) {
            for (const auto& visible : {range(2, 5), range(12, 16)}) {
                auto encoding = request;
                encoding.outputPath =
                    root / (visible.timelineClips[0].sourceInFrame == 2 ? "scope-action.mp4"
                                                                        : "scope-transition.mp4");
                const auto result = app::exportTimeline(visible, encoding);
                check(result.success && std::filesystem::exists(encoding.outputPath),
                      "画面外 static が欠落していても実 export は成功する: " + result.error);
            }
        }
        if (!corrupt)
            std::filesystem::rename(root / "scope-late-missing.png", paths[2]);
        else
            write(paths[2], bytes[2]);
    }
    for (const std::size_t endpoint : {std::size_t{0}, std::size_t{1}}) {
        std::filesystem::rename(paths[endpoint], root / "scope-endpoint-missing.png");
        check(app::mapTimelineExportPlan(range(12, 16), request).equationReadiness.failure ==
                  app::EquationExportFailure::StaticArtifactMissing,
              "可視 transition の source/target static の欠落は拒否");
        if (endpoint == 0) {
            check(app::mapTimelineExportPlan(range(2, 5), request).equationReadiness.failure ==
                      app::EquationExportFailure::StaticArtifactMissing,
                  "可視 action 所有状態の static の欠落は拒否");
            check(app::mapTimelineExportPlan(range(0, 1), request).equationReadiness.failure ==
                      app::EquationExportFailure::StaticArtifactMissing,
                  "可視 hold の static の欠落は拒否");
        }
        std::filesystem::rename(root / "scope-endpoint-missing.png", paths[endpoint]);
    }
    std::filesystem::rename(paths[0], root / "scope-early-missing.png");
    exact(range(34, 37), "state index の穴があっても後半 action は正しい所有状態を使う");
    std::filesystem::rename(root / "scope-early-missing.png", paths[0]);
    cache.setPreflight(test::FakeMathBackend::unavailable("依存範囲試験: backend が消失"));
    cache.startPreflight();
    check(pump([&] {
              return cache.backendState() == app::MathRasterCache::BackendState::Unavailable;
          }),
          "依存範囲対照の backend 消失");
    request.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(clip);
    std::filesystem::rename(paths[2], root / "scope-backend-late-missing.png");
    exact(range(2, 5), "backend 不在でも可視 action の artifact が完全なら通る");
    exact(range(12, 16), "backend 不在でも可視 transition の artifact が完全なら通る");
    std::filesystem::rename(root / "scope-backend-late-missing.png", paths[2]);
    cache.shutdown();
}

void run(const std::filesystem::path& root, const QString& ffprobe) {
    dependencyScope(root);
    auto p = fixture();
    const auto clip = p.timelineClips.front();
    const auto compiled = app::compileEquationSequence(clip.equationSequence);
    std::string error;
    const auto spec = *app::equationSequenceRenderSpecFor(*compiled.value, error);
    test::FakeMathBackend backend;
    app::MathRasterCache cache("p36", backend.preflight());
    cache.setAuthority(root / "cache", true);
    check(
        pump([&] { return cache.backendState() == app::MathRasterCache::BackendState::Available; }),
        "描画環境の確認");
    cache.requestEquationSequence(spec);
    check(pump([&] {
              return cache.requestEquationSequence(spec).state ==
                     app::MathRasterCache::State::Ready;
          }),
          "現在 key の artifact を準備");
    const auto artifact = cache.readyEquationSequence(spec);
    if (!artifact) {
        check(false, "artifact 対照が存在する");
        return;
    }
    cache.setResidentMemoryBudget(0);
    check(cache.residentEquationLayers(
                   spec, {{app::MathRasterCache::EquationLayerRole::TransitionFrame, 0, 2}}, true)
                  .state == app::MathRasterCache::Residency::OverBudget,
          "preview は OverBudget");
    app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;
    request.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(clip);
    auto plan = app::mapTimelineExportPlan(p, request);
    check(plan.success && plan.equationSequences.at(clip.id).frames.size() == 28,
          "RAM 常駐ゼロでも全 28 frame の preflight");
    if (!plan.success) {
        check(false, plan.error);
        return;
    }
    const auto snapshot = plan.equationSequences.at(clip.id).snapshot;
    std::vector<std::vector<std::uint8_t>> expected;
    for (int i = 0; i < 28; ++i)
        expected.push_back(oracle(snapshot, *artifact, i));
    for (int i = 27; i >= 0; --i) {
        std::vector<std::uint8_t> rgba;
        check(app::composeEquationExportFrame(snapshot, i, rgba).ready() &&
                  !expected[static_cast<std::size_t>(i)].empty() &&
                  rgba == expected[static_cast<std::size_t>(i)],
              "逆順の frame " + std::to_string(i) + " は oracle と完全一致");
    }
    std::vector<std::future<bool>> futures;
    for (int i : {14, 3, 8, 0, 15, 9})
        futures.push_back(std::async(std::launch::async, [&, i] {
            std::vector<std::uint8_t> rgba;
            return app::composeEquationExportFrame(snapshot, i, rgba).ready() &&
                   rgba == expected[static_cast<std::size_t>(i)];
        }));
    for (auto& future : futures)
        check(future.get(), "並行取得も同じ frame");
    for (int begin : {0, 1, 3, 8, 13, 16})
        for (int end : {4, 9, 14, 20, 28}) {
            if (begin >= end)
                continue;
            auto trimmed = clip;
            trimmed.sourceInFrame = begin;
            trimmed.sourceOutFrame = end;
            trimmed.timelineStartFrame = 0;
            const auto trimmedPlan =
                app::planEquationSequenceExport(trimmed, 60, 1, 320, 240, snapshot);
            check(trimmedPlan.readiness.ready(), "hold/action/transition で始終する trim");
            std::vector<std::uint8_t> rgba;
            check(app::composeEquationExportFrame(trimmedPlan.snapshot, 0, rgba).ready() &&
                      rgba == expected[static_cast<std::size_t>(begin)],
                  "trim は内部 frame を再開しない");
        }
    auto single = clip;
    single.sourceInFrame = 14;
    single.sourceOutFrame = 15;
    check(app::planEquationSequenceExport(single, 60, 1, 320, 240, snapshot).frames.size() == 1,
          "単一 frame の可視範囲");
    for (int split : {1, 3, 8, 14}) {
        auto divided = p;
        int serial = 0;
        check(project::splitTimelineClips(
                  divided, {clip.id}, split, [&] { return "fresh-" + std::to_string(++serial); },
                  project::LinkMode::Single)
                  .success,
              "hold/outline/pulse/transition 内を既存契約で split");
        for (const auto& piece : divided.timelineClips) {
            const auto splitPlan =
                app::planEquationSequenceExport(piece, 60, 1, 320, 240, snapshot);
            check(splitPlan.readiness.ready(), "fresh ID の split 片も同じ key で有効");
            for (std::size_t i = 0; i < splitPlan.frames.size(); ++i) {
                std::vector<std::uint8_t> rgba;
                const auto frame = piece.timelineStartFrame + static_cast<std::int64_t>(i);
                check(app::composeEquationExportFrame(splitPlan.snapshot, frame, rgba).ready() &&
                          rgba == expected[static_cast<std::size_t>(frame)],
                      "split 境界で frame を重複・欠落しない");
            }
        }
    }
    std::vector<std::uint8_t> rgba;
    auto stale = clip;
    stale.equationSequence.actions[0].duration = 2;
    check(app::planEquationSequenceExport(stale, 60, 1, 320, 240, snapshot).readiness.failure ==
              app::EquationExportFailure::StaleSequenceKey,
          "編集直後に旧 key を拒否");
    auto edited = p;
    edited.timelineClips[0] = stale;
    auto editingRequest = request;
    editingRequest.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(stale);
    check(app::mapTimelineExportPlan(edited, editingRequest).equationReadiness.failure ==
              app::EquationExportFailure::ArtifactPending,
          "旧 animation が disk に残っても新 key pending は拒否");
    const auto editedCompiled = app::compileEquationSequence(stale.equationSequence);
    const auto editedSpec = *app::equationSequenceRenderSpecFor(*editedCompiled.value, error);
    backend.equationGate->store(true);
    cache.requestEquationSequence(editedSpec);
    check(pump([&] { return backend.equationHeld->load(); }), "新 key の renderer を明示的に遅延");
    check(!app::mapTimelineExportPlan(edited, editingRequest).success,
          "遅延中も旧 artifact を現在の入力として受け入れない");
    backend.equationGate->store(false);
    check(pump([&] {
              return cache.equationSequenceEntryOf(editedSpec).state ==
                     app::MathRasterCache::State::Ready;
          }),
          "新 key の background 完了");
    check(app::mapTimelineExportPlan(edited, editingRequest).success,
          "現在 key の準備完了後だけ現在入力の export を許可");
    check(app::composeEquationExportFrame(snapshot, 3, rgba).ready() && rgba == expected[3],
          "新 key の完了は開始済みの旧 Project snapshot の frame を変えない");
    auto changedSource = clip;
    changedSource.equationSequence.states[1].equation.source = "z=b^2-4ac";
    auto changedProject = p;
    changedProject.timelineClips[0] = changedSource;
    editingRequest.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(changedSource);
    check(!app::mapTimelineExportPlan(changedProject, editingRequest).success,
          "旧状態の静止を current source の last-good として使わない");
    auto invalid = clip;
    invalid.equationSequence.states[0].parts[0].binding.status = project::BindingStatus::Invalid;
    check(app::planEquationSequenceExport(invalid, 60, 1, 320, 240, snapshot).readiness.failure ==
              app::EquationExportFailure::InvalidBinding,
          "InvalidBinding を typed 拒否");
    invalid = clip;
    invalid.equationSequence.actions[0].target = {"missing"};
    invalid.equationSequence.actions[0].targetStatus = project::EquationTargetStatus::Missing;
    check(app::planEquationSequenceExport(invalid, 60, 1, 320, 240, snapshot).readiness.failure ==
              app::EquationExportFailure::MissingPart,
          "MissingPart を typed 拒否");
    invalid = clip;
    auto& unsafe = invalid.equationSequence.states[0];
    unsafe.equation.source = "{x+y}";
    unsafe.parts[0].binding = {unsafe.revision, 1, 5, "x+y}", project::BindingStatus::Bound};
    check(app::planEquationSequenceExport(invalid, 60, 1, 320, 240, snapshot).readiness.failure ==
              app::EquationExportFailure::UnsupportedTexBoundary,
          "UnsupportedTexBoundary を typed 拒否");
    auto effectsProject = p;
    effectsProject.timelineClips[0].effects.scaleXPercent = 150;
    effectsProject.timelineClips[0].effects.scaleYPercent = 75;
    effectsProject.timelineClips[0].effects.opacityPercent = 50;
    const auto effectsPlan = app::mapTimelineExportPlan(effectsProject, request);
    check(effectsPlan.success && effectsPlan.clips[0].rectWidth == 480 &&
              effectsPlan.clips[0].rectHeight == 180 &&
              effectsPlan.clips[0].opacityKeys[0].opacity == 0.5,
          "ClipEffects の拡大と不透明度を外側で一度だけ適用 (二重適用なら値が違う)");
    if (effectsPlan.success)
        check(app::composeEquationExportFrame(effectsPlan.equationSequences.at(clip.id).snapshot, 8,
                                              rgba)
                      .ready() &&
                  rgba == expected[8],
              "内部合成には外側 effects を焼き込まない");
    auto withOuter = p;
    project::TimelineTransition outer;
    outer.id = "outer";
    outer.outgoingClipId = clip.id;
    outer.incomingClipId = "other";
    withOuter.timelineTransitions.push_back(outer);
    check(app::mapTimelineExportPlan(withOuter, request).equationReadiness.failure ==
              app::EquationExportFailure::UnsupportedTimelineTransition,
          "外側 TimelineTransition を typed 拒否");
    auto fallback = snapshot;
    fallback.presentation.artifactReady = false;
    check(!app::composeEquationExportFrame(fallback, 12, rgba).ready(),
          "preview の transition 静止代用を export は拒否");
    check(!app::composeEquationExportFrame(fallback, 8, rgba).ready(),
          "preview の action 静止代用を export は拒否");
    check(app::planEquationSequenceExport(clip, 60, 1, 320, 240, snapshot, [] { return true; })
                  .readiness.failure == app::EquationExportFailure::Cancelled,
          "preflight の取消");
    auto refused = snapshot;
    refused.loadLayer = [](const app::EquationPreviewLayerId&, std::vector<std::uint8_t>&) {
        return app::EquationExportReadiness{app::EquationExportFailure::FrameMissing,
                                            app::EquationCompileFailure::None, "試験: 欠落"};
    };
    check(!app::composeEquationExportFrame(refused, 14, rgba).ready(),
          "transition 欠落を静止で代用しない");
    check(!app::composeEquationExportFrame(refused, 8, rgba).ready(),
          "action 欠落を静止で代用しない");
    // 画素の変異を独立 oracle が検出する。許容差は入れない。
    auto color = snapshot;
    color.presentation.transitions[0].colors[2] ^= 0x00010000u;
    check(app::composeEquationExportFrame(color, 14, rgba).ready() && rgba != expected[14],
          "provenance 色の変異を検出");
    auto omitted = snapshot;
    const auto originalLoader = snapshot.loadLayer;
    omitted.loadLayer = [originalLoader](const app::EquationPreviewLayerId& layer,
                                         std::vector<std::uint8_t>& bytes) {
        auto result = originalLoader(layer, bytes);
        if (layer.role == app::EquationPreviewLayerRole::ActionAccent)
            std::fill(bytes.begin(), bytes.end(), 0);
        return result;
    };
    check(app::composeEquationExportFrame(omitted, 8, rgba).ready() && rgba != expected[8],
          "pulse accent の省略を検出");
    auto offset = snapshot;
    offset.loadLayer = [originalLoader](app::EquationPreviewLayerId layer,
                                        std::vector<std::uint8_t>& bytes) {
        if (layer.role == app::EquationPreviewLayerRole::TransitionFrame)
            ++layer.frame;
        return originalLoader(layer, bytes);
    };
    check(app::composeEquationExportFrame(offset, 12, rgba).ready() && rgba != expected[12],
          "(i+1)/N の変異を検出");
    auto shifted = expected[8];
    std::rotate(shifted.begin(), shifted.begin() + 4, shifted.end());
    check(shifted != expected[8], "1 pixel の配置変異を検出");
    // 破損は検証時にも取得時にも拒否し、証拠を消さない。
    for (const auto& pair :
         std::vector<std::pair<app::EquationArtifactFrame, app::EquationExportFailure>>{
             {artifact->transitions[0].frames[2], app::EquationExportFailure::CorruptTransition},
             {artifact->actions[0].base, app::EquationExportFailure::CorruptActionBase},
             {artifact->actions[1].accent[1], app::EquationExportFailure::CorruptActionAccent}}) {
        const auto bytes = read(pair.first.path);
        auto corrupt = bytes;
        corrupt[0] ^= 1;
        write(root / (pair.first.sha256 + ".original"), bytes);
        write(root / (pair.first.sha256 + ".corrupt"), corrupt);
        write(pair.first.path, corrupt);
        const auto broken = app::mapTimelineExportPlan(p, request);
        check(!broken.success && broken.equationReadiness.failure == pair.second,
              "同じ byte 数の破損を SHA で typed 拒否");
        check(std::filesystem::exists(pair.first.path), "read-only 検査は壊れた証拠を削除しない");
        write(pair.first.path, bytes);
    }
    const auto provenance =
        app::equationSequenceProvenancePath(root / "cache", snapshot.sequenceKey);
    const auto saved = read(provenance);
    write(provenance, "stale\n" + saved);
    check(!app::mapTimelineExportPlan(p, request).success, "provenance の改変を拒否");
    write(provenance, saved);
    const auto staticPath = root / "cache" / (artifact->stateStaticKeys[0] + ".png");
    const auto staticBytes = read(staticPath);
    write(root / "static-original.png", staticBytes);
    write(staticPath, "破損した静止の対照");
    check(app::mapTimelineExportPlan(p, request).equationReadiness.failure ==
              app::EquationExportFailure::CorruptStatic,
          "現在の静止 PNG の破損を typed 拒否");
    write(staticPath, staticBytes);
    const auto missing = artifact->transitions[0].frames[2].path;
    std::filesystem::rename(missing, root / "missing-frame-original.a8");
    check(app::mapTimelineExportPlan(p, request).equationReadiness.failure ==
              app::EquationExportFailure::FrameMissing,
          "正確な frame 欠落を出力前に拒否");
    auto holdOnly = p;
    holdOnly.timelineClips[0].sourceInFrame = 16;
    holdOnly.timelineClips[0].sourceOutFrame = 20;
    check(app::mapTimelineExportPlan(holdOnly, request).success,
          "可視範囲外の transition 欠落は hold の export を妨げない");
    auto firstTransitionOnly = p;
    firstTransitionOnly.timelineClips[0].sourceInFrame = 12;
    firstTransitionOnly.timelineClips[0].sourceOutFrame = 13;
    check(app::mapTimelineExportPlan(firstTransitionOnly, request).success,
          "同じ区間でも可視範囲外の frame 欠落は検査依存にしない");
    std::filesystem::rename(root / "missing-frame-original.a8", missing);
    cache.setPreflight(test::FakeMathBackend::unavailable("試験: backend が消失"));
    cache.startPreflight();
    check(pump([&] {
              return cache.backendState() == app::MathRasterCache::BackendState::Unavailable;
          }),
          "backend 消失");
    write(staticPath, "backend 不在時の静止破損");
    auto noBackendRequest = request;
    noBackendRequest.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(clip);
    check(app::mapTimelineExportPlan(p, noBackendRequest).equationReadiness.failure ==
              app::EquationExportFailure::CorruptStatic,
          "backend 不在でも存在する静止の破損を正確に分類");
    write(staticPath, staticBytes);
    request.equationSequences[clip.id] = cache.equationSequenceExportSnapshot(clip);
    check(app::mapTimelineExportPlan(p, request).success,
          "現在の全 artifact が有効なら backend 不在でも通る");
    std::filesystem::rename(provenance, root / "provenance-original.txt");
    check(app::mapTimelineExportPlan(p, request).equationReadiness.failure ==
              app::EquationExportFailure::BackendUnavailable,
          "artifact 不足と backend 不在は出力前に失敗");
    std::filesystem::rename(root / "provenance-original.txt", provenance);
    request.outputPath = root / "equation.mp4";
    const auto exported = app::exportTimeline(p, request);
    check(exported.success && exported.frameCount == 28,
          "既存 timeline exporter で実際に 28 frame を出力: " + exported.error);
    if (exported.success) {
        QProcess probe;
        probe.start(ffprobe, {"-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0",
                              QString::fromStdWString(request.outputPath.wstring())});
        check(probe.waitForFinished(10000) && probe.exitCode() == 0 &&
                  probe.readAllStandardOutput().trimmed() == "video",
              "EquationSequence は audio stream を生成しない");
    }
    request.outputPath = root / "cancelled.mp4";
    request.progress = [](long long completed, long long) { return completed > 0; };
    const auto cancelled = app::exportTimeline(p, request);
    check(cancelled.cancelled && !cancelled.success && !std::filesystem::exists(request.outputPath),
          "encoding 取消は部分出力を公開しない");
    cache.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication gui(argc, argv);
    if (argc != 2)
        return 2;
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0)
        return 1;
    const auto root = std::filesystem::current_path() / "math-p36-export" /
                      QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
    std::filesystem::create_directories(root);
    run(root, QString::fromLocal8Bit(argv[1]));
    mvm_mlt_runtime_shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗。証拠: %s\n", checks, failures,
                 root.string().c_str());
    write(root / "result.txt", std::to_string(checks) + "/" + std::to_string(failures));
    return checks > 0 && failures == 0 ? 0 : 1;
}
