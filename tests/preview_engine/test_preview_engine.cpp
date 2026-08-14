#include "preview_engine/preview_engine_internal.h"

#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mvm::preview;
using namespace mvm::preview::internal;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename T>
void require(const Result<T>& result, const std::string& message) {
    require(result.hasValue(), message);
}

template<typename T>
void requireFailure(const Result<T>& result, PreviewErrorCategory category, const char* message) {
    require(!result, message);
    require(result.error().category == category, "error categoryが期待値と違います");
}

class ManualDispatcher final : public PreviewEventDispatcher {
public:
    bool post(std::function<void()> callback) override {
        ++postAttemptCount;
        if (throwNextPost) {
            throwNextPost = false;
            throw std::runtime_error("dispatcher post exception");
        }
        if (rejectNextPost) {
            rejectNextPost = false;
            return false;
        }
        if (!acceptPosts) {
            return false;
        }
        tasks.push_back(std::move(callback));
        ++postCount;
        return true;
    }

    void runOne() {
        require(!tasks.empty(), "dispatcher taskがありません");
        std::function<void()> callback = std::move(tasks.front());
        tasks.pop_front();
        callback();
    }

    void runAll() {
        while (!tasks.empty()) {
            runOne();
        }
    }

    bool acceptPosts = true;
    bool rejectNextPost = false;
    bool throwNextPost = false;
    std::size_t postAttemptCount = 0;
    std::size_t postCount = 0;
    std::deque<std::function<void()>> tasks;
};

class ThrowingSink final : public PreviewEventSink {
public:
    void stateChanged(PreviewEngineState) override {
        ++callbackCount;
        throw std::runtime_error("sink callback exception");
    }

    void positionChanged(PreviewPosition) override {}

    void framePresented(PresentedFrameInfo) override {}

    void errorOccurred(PreviewError) override {}

    void deviceChanged(PreviewDeviceInfo) override {}

    std::size_t callbackCount = 0;
};

class RecordingSink final : public PreviewEventSink {
public:
    void stateChanged(PreviewEngineState state) override {
        states.push_back(state);
        if (engine != nullptr) {
            observedStatus = engine->status().state;
        }
    }

    void positionChanged(PreviewPosition position) override { positions.push_back(position); }

    void framePresented(PresentedFrameInfo frame) override { frames.push_back(frame); }

    void errorOccurred(PreviewError error) override { errors.push_back(std::move(error)); }

    void deviceChanged(PreviewDeviceInfo) override { ++deviceChanges; }

    PreviewEngine* engine = nullptr;
    PreviewEngineState observedStatus = PreviewEngineState::Uninitialized;
    std::vector<PreviewEngineState> states;
    std::vector<PreviewPosition> positions;
    std::vector<PresentedFrameInfo> frames;
    std::vector<PreviewError> errors;
    std::size_t deviceChanges = 0;
};

PreviewEngineConfig qualifiedConfig() {
    return {{{60, 1}}};
}

PreviewCompositionLayer layer(std::uint64_t source, PreviewNormalizedRect destination = {},
                              PreviewNormalizedRect sourceRect = {}, float opacity = 1.0F) {
    return {{source}, destination, sourceRect, opacity};
}

std::shared_ptr<const CompositionSnapshot>
snapshot(std::initializer_list<PreviewCompositionLayer> layers) {
    return std::make_shared<const CompositionSnapshot>(
        CompositionSnapshot{std::vector<PreviewCompositionLayer>(layers)});
}

std::unordered_map<std::uint64_t, EligibleSource> twoSources() {
    return {{1, {true}}, {2, {true}}};
}

void frameRateAndDescriptorValidation() {
    const auto valid = validatePreviewFrameRate(120, 2);
    require(valid && valid.value() == PreviewFrameRate{60, 1},
            "valid rationalをcanonicalizeできません");
    requireFailure(validatePreviewFrameRate(0, 1), PreviewErrorCategory::UnsupportedCapability,
                   "zero numeratorをrejectしませんでした");
    requireFailure(validatePreviewFrameRate(1, 0), PreviewErrorCategory::UnsupportedCapability,
                   "zero denominatorをrejectしませんでした");
    requireFailure(
        validatePreviewFrameRate(
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1, 1),
        PreviewErrorCategory::UnsupportedCapability,
        "uint32 boundary overflowをrejectしませんでした");
    const auto validButUnqualified = validatePreviewFrameRate(24, 1);
    require(validButUnqualified, "positive rationalをtype validationでrejectしました");
    require(validateSourceFrameRate(120, 2, {60, 1}), "source frame rateをcanonical比較できません");
    requireFailure(validateSourceFrameRate(30, 1, {60, 1}),
                   PreviewErrorCategory::UnsupportedCapability, "30fps sourceをP5-Cで受理しました");
    requireFailure(validateSourceFrameRate(120, 1, {60, 1}),
                   PreviewErrorCategory::UnsupportedCapability,
                   "120fps sourceをP5-Cで受理しました");

    PreviewEngine unqualifiedEngine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    requireFailure(unqualifiedEngine.initialize({{{24, 1}}}, dispatcher),
                   PreviewErrorCategory::UnsupportedCapability,
                   "unqualified rateをinitializeでrejectしませんでした");
    require(unqualifiedEngine.status().state == PreviewEngineState::Uninitialized,
            "initialize reject後にstateが変化しました");

    PreviewEngine equivalentRateEngine;
    auto equivalentRateDispatcher = std::make_shared<ManualDispatcher>();
    require(equivalentRateEngine.initialize({{{120, 2}}}, equivalentRateDispatcher),
            "60/1と等価な120/2をinitializeで受理しませんでした");
    require(equivalentRateEngine.capabilities().qualifiedOutputFrameRate ==
                    PreviewFrameRate{60, 1} &&
                equivalentRateEngine.capabilities().maxQualifiedActiveAudioSources == 0,
            "等価rationalの受理で公開capabilityを変更しました");
    require(equivalentRateEngine.requestShutdown(),
            "等価rational regression testのshutdownに失敗しました");
    require(equivalentRateEngine.status().state == PreviewEngineState::Shutdown,
            "等価rational regression testがterminal Shutdownへ到達しませんでした");

    requireFailure(validatePreviewSourceDescriptor({}), PreviewErrorCategory::InvalidSource,
                   "empty pathをrejectしませんでした");
    requireFailure(validatePreviewSourceDescriptor({"movie.mp4", false, false}),
                   PreviewErrorCategory::InvalidSource,
                   "audio/video disabledをrejectしませんでした");
    require(validatePreviewSourceDescriptor({"movie.mp4", true, false}),
            "video descriptorをrejectしました");
    require(validatePreviewSourceDescriptor({"voice.wav", false, true}),
            "audio descriptorをrejectしました");
}

void resultAndErrorValues() {
    const auto value = Result<int>::success(42);
    require(value && value.value() == 42, "Result value successが壊れています");
    require(Result<void>::success(), "Result<void> successが壊れています");

    PreviewError error;
    error.category = PreviewErrorCategory::InvalidSource;
    error.severity = PreviewErrorSeverity::Recoverable;
    error.operation = PreviewOperation::AddSource;
    error.source = PreviewSourceId{7};
    error.detail = "入力を拒否しました";
    error.nativeDiagnosticCode = -12;
    const auto failure = Result<int>::failure(error);
    requireFailure(failure, PreviewErrorCategory::InvalidSource,
                   "structured failureがsuccessになりました");
    require(failure.error() == error, "structured PreviewErrorを保持できません");

    bool threw = false;
    try {
        const auto expectedReject = validatePreviewSourceDescriptor({});
        require(!expectedReject, "expected rejectがsuccessになりました");
    } catch (...) {
        threw = true;
    }
    require(!threw, "expected validation rejectがexceptionになりました");
}

void stateMachineLifecycle() {
    PreviewStateMachine state;
    require(state.destructionSafe(), "Uninitializedがdestruction-safeではありません");
    requireFailure(state.play(), PreviewErrorCategory::InvalidState,
                   "illegal play transitionをrejectしませんでした");
    require(state.initialize(), "initialize transitionに失敗しました");
    require(state.state() == PreviewEngineState::WaitingForRenderDevice,
            "initialize後のstateが違います");
    require(state.attachRenderDevice(), "logical render attachに失敗しました");
    require(state.state() == PreviewEngineState::ReadyPaused, "render ready stateが違います");
    require(state.play() && state.state() == PreviewEngineState::Playing,
            "Playing transitionに失敗しました");
    require(state.seek() && state.state() == PreviewEngineState::Seeking,
            "Seeking transitionに失敗しました");
    require(state.completeSeek() && state.state() == PreviewEngineState::Playing,
            "seek後に元のPlayingへ戻りません");
    require(state.pause() && state.state() == PreviewEngineState::ReadyPaused,
            "pause transitionに失敗しました");
    require(state.requestShutdown(), "shutdown requestをrejectしました");
    require(state.state() == PreviewEngineState::ShuttingDown && !state.destructionSafe(),
            "shutdown requestとterminal acknowledgementが分離されていません");
    require(state.requestShutdown() && state.state() == PreviewEngineState::ShuttingDown,
            "ShuttingDownでidempotentではありません");
    require(state.completeTeardown() && state.state() == PreviewEngineState::Shutdown,
            "normal teardown completionがShutdownになりません");
    require(state.destructionSafe() && state.requestShutdown(),
            "Shutdownがsafe/idempotentではありません");

    PreviewStateMachine uninitialized;
    requireFailure(uninitialized.requestShutdown(), PreviewErrorCategory::InvalidState,
                   "Uninitialized shutdownをrejectしませんでした");
    require(uninitialized.state() == PreviewEngineState::Uninitialized,
            "illegal shutdownがstateを変更しました");

    PreviewStateMachine fatal;
    require(fatal.initialize(), "fatal test initializeに失敗しました");
    PreviewError error{PreviewErrorCategory::DeviceFailure,
                       PreviewErrorSeverity::FatalToSession,
                       PreviewOperation::RenderDeviceAttach,
                       std::nullopt,
                       "device failure",
                       5};
    require(fatal.recordFatal(error), "fatal errorをrecordできません");
    require(fatal.state() == PreviewEngineState::ShuttingDown && !fatal.destructionSafe(),
            "fatal検出時にterminal Errorを早期公開しました");
    require(fatal.completeTeardown() && fatal.state() == PreviewEngineState::Error,
            "fatal teardown後にErrorになりません");
    require(fatal.destructionSafe() && fatal.lastError() == error,
            "terminal ErrorにlastErrorが残りません");

    PreviewStateMachine teardownFatal;
    require(teardownFatal.initialize(), "teardown fatal test initializeに失敗しました");
    require(teardownFatal.requestShutdown(), "normal shutdown requestに失敗しました");
    PreviewError shutdownError{PreviewErrorCategory::ShutdownFailure,
                               PreviewErrorSeverity::FatalToSession,
                               PreviewOperation::Shutdown,
                               std::nullopt,
                               "GPU drain failure",
                               9};
    require(teardownFatal.recordFatal(shutdownError), "ShuttingDown中のfatalをrecordできません");
    require(teardownFatal.state() == PreviewEngineState::ShuttingDown,
            "teardown fatalがShuttingDownを維持しません");
    require(teardownFatal.completeTeardown() && teardownFatal.state() == PreviewEngineState::Error,
            "teardown fatalをShutdownへ誤変換しました");
    require(teardownFatal.lastError() == shutdownError, "teardown fatalを保持していません");

    PreviewStateMachine multipleFatal;
    require(multipleFatal.initialize(), "multiple fatal test initializeに失敗しました");
    PreviewError rootError{PreviewErrorCategory::DeviceFailure,
                           PreviewErrorSeverity::FatalToSession,
                           PreviewOperation::RenderDeviceAttach,
                           std::nullopt,
                           "root device failure",
                           10};
    require(multipleFatal.recordFatal(rootError), "root fatalをrecordできません");
    require(multipleFatal.recordFatal(shutdownError), "teardown secondary fatalを拒否しました");
    require(multipleFatal.lastError() == rootError, "secondary fatalがroot fatalを上書きしました");
    require(multipleFatal.completeTeardown() && multipleFatal.state() == PreviewEngineState::Error,
            "multiple fatalがErrorになりません");
    requireFailure(multipleFatal.recordFatal(shutdownError), PreviewErrorCategory::InvalidState,
                   "terminal Errorでfatal mutationを受理しました");
}

void mailboxOrderingAndBounds() {
    EventMailbox mailbox(4);
    require(mailbox.push(StateChangedEvent{PreviewEngineState::WaitingForRenderDevice}),
            "state eventをpushできません");
    require(mailbox.push(ErrorOccurredEvent{PreviewError{}}), "error eventをpushできません");
    require(mailbox.push(DeviceChangedEvent{{}}), "device eventをpushできません");
    require(mailbox.size() == 3, "non-coalescible event countが違います");
    requireFailure(mailbox.push(StateChangedEvent{PreviewEngineState::ReadyPaused}),
                   PreviewErrorCategory::ShutdownFailure, "reserved slotへ通常eventを入れました");
    require(mailbox.push(StateChangedEvent{PreviewEngineState::Shutdown}),
            "terminal reserved slotを使用できません");
    require(mailbox.size() == mailbox.capacity(), "mailbox boundが違います");
    require(std::holds_alternative<StateChangedEvent>(*mailbox.pop()),
            "state/error/device orderingが壊れています");
    require(std::holds_alternative<ErrorOccurredEvent>(*mailbox.pop()),
            "error orderingが壊れています");
    require(std::holds_alternative<DeviceChangedEvent>(*mailbox.pop()),
            "device orderingが壊れています");
    const auto terminal = mailbox.pop();
    require(std::get<StateChangedEvent>(*terminal).state == PreviewEngineState::Shutdown,
            "terminal event orderingが壊れています");

    EventMailbox coalescing(3);
    require(coalescing.push(PositionChangedEvent{{1}}), "position pushに失敗しました");
    require(coalescing.push(PositionChangedEvent{{9}}), "position coalesceに失敗しました");
    PresentedFrameInfo first;
    first.presentationSequence = 3;
    PresentedFrameInfo latest;
    latest.presentationSequence = 8;
    require(coalescing.push(FramePresentedEvent{first}), "frame pushに失敗しました");
    require(coalescing.push(FramePresentedEvent{latest}), "frame coalesceに失敗しました");
    require(coalescing.size() == 2, "coalescible eventがbounded一件になっていません");
    require(std::get<PositionChangedEvent>(*coalescing.pop()).position.outputFrame == 9,
            "latest positionを保持していません");
    require(std::get<FramePresentedEvent>(*coalescing.pop()).frame.presentationSequence == 8,
            "latest frame metadataを保持していません");
}

void compositionDomains() {
    const auto sources = twoSources();
    const PreviewCapabilities capabilities;
    const std::vector<PreviewNormalizedRect> validRects{
        {0.0F, 0.0F, 1.0F, 1.0F}, {0.25F, 0.25F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F, 0.5F}};
    for (const PreviewNormalizedRect& rect : validRects) {
        CompositionAcceptanceState destinationState;
        require(destinationState.submit(snapshot({layer(1, rect)}), sources, capabilities),
                "valid destination rectangleをrejectしました");
        CompositionAcceptanceState sourceRectState;
        require(sourceRectState.submit(snapshot({layer(1, {}, rect)}), sources, capabilities),
                "valid sourceRectをrejectしました");
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<PreviewNormalizedRect> invalidRects{
        {-0.01F, 0.0F, 0.5F, 0.5F}, {0.0F, -0.01F, 0.5F, 0.5F}, {1.0F, 0.0F, 0.1F, 0.5F},
        {0.0F, 1.0F, 0.5F, 0.1F},   {0.0F, 0.0F, 0.0F, 0.5F},   {0.0F, 0.0F, 0.5F, 0.0F},
        {0.0F, 0.0F, -0.1F, 0.5F},  {0.0F, 0.0F, 0.5F, -0.1F},  {0.0F, 0.0F, 1.1F, 0.5F},
        {0.0F, 0.0F, 0.5F, 1.1F},   {0.75F, 0.0F, 0.5F, 0.5F},  {0.0F, 0.75F, 0.5F, 0.5F},
        {nan, 0.0F, 0.5F, 0.5F},    {inf, 0.0F, 0.5F, 0.5F},    {-inf, 0.0F, 0.5F, 0.5F},
        {0.0F, nan, 0.5F, 0.5F},    {0.0F, 0.0F, inf, 0.5F},    {0.0F, 0.0F, 0.5F, -inf}};
    for (const PreviewNormalizedRect& rect : invalidRects) {
        CompositionAcceptanceState destinationState;
        requireFailure(destinationState.submit(snapshot({layer(1, rect)}), sources, capabilities),
                       PreviewErrorCategory::CompositionFailure,
                       "invalid destination rectangleをacceptしました");
        CompositionAcceptanceState sourceRectState;
        requireFailure(
            sourceRectState.submit(snapshot({layer(1, {}, rect)}), sources, capabilities),
            PreviewErrorCategory::CompositionFailure, "invalid sourceRectをacceptしました");
    }

    for (float opacity : {0.0F, 0.5F, 0.75F, 1.0F}) {
        CompositionAcceptanceState state;
        require(state.submit(snapshot({layer(1, {}, {}, opacity)}), sources, capabilities),
                "valid opacityをrejectしました");
    }
    for (float opacity : {-0.01F, 1.01F, nan, inf, -inf}) {
        CompositionAcceptanceState state;
        requireFailure(state.submit(snapshot({layer(1, {}, {}, opacity)}), sources, capabilities),
                       PreviewErrorCategory::CompositionFailure, "invalid opacityをacceptしました");
    }
}

void compositionIdentityAndCapabilities() {
    const auto sources = twoSources();
    PreviewCapabilities capabilities;
    // acceptance algorithm自体の2層順序・distinct source検査用。product公開値は1層。
    capabilities.maxQualifiedActiveVideoSources = 2;
    capabilities.maxQualifiedCompositionLayers = 2;
    CompositionAcceptanceState state;

    const auto a = snapshot({layer(1)});
    const auto acceptedA = state.submit(a, sources, capabilities);
    require(acceptedA && acceptedA.value() == AcceptedComposition{{1}, 1},
            "最初のtokenが期待値と違います");
    const auto noOpA = state.submit(snapshot({layer(1)}), sources, capabilities);
    require(noOpA && noOpA.value() == acceptedA.value(), "latest desired no-opがtokenを変えました");

    const auto b =
        state.submit(snapshot({layer(1, {0.0F, 0.0F, 0.5F, 1.0F})}), sources, capabilities);
    require(b && b.value() == AcceptedComposition{{2}, 2}, "field差でnew tokenを発行しません");
    state.markPresented(acceptedA.value());
    const auto newA = state.submit(snapshot({layer(1)}), sources, capabilities);
    require(newA && newA.value() == AcceptedComposition{{3}, 3},
            "presented A/latest B/submit Aでold tokenを再利用しました");
    require(state.lastPresentedToken() == acceptedA.value(),
            "desired acceptanceがlast presentedを変更しました");

    CompositionAcceptanceState rejected;
    requireFailure(
        rejected.submit(std::make_shared<const CompositionSnapshot>(), sources, capabilities),
        PreviewErrorCategory::CompositionFailure, "empty snapshotをacceptしました");
    const auto afterReject = rejected.submit(a, sources, capabilities);
    require(afterReject && afterReject.value() == AcceptedComposition{{1}, 1},
            "rejectがID/revisionを消費しました");

    CompositionAcceptanceState negativeZero;
    const auto minusZero = negativeZero.submit(
        snapshot({layer(1, {-0.0F, -0.0F, 1.0F, 1.0F}, {}, -0.0F)}), sources, capabilities);
    const auto plusZero = negativeZero.submit(
        snapshot({layer(1, {0.0F, 0.0F, 1.0F, 1.0F}, {}, 0.0F)}), sources, capabilities);
    require(minusZero && plusZero && minusZero.value() == plusZero.value(),
            "-0.0を+0.0へcanonicalizeしていません");

    CompositionAcceptanceState opacityZero;
    const auto zeroLayer =
        opacityZero.submit(snapshot({layer(1, {}, {}, 0.0F)}), sources, capabilities);
    const auto visibleLayer =
        opacityZero.submit(snapshot({layer(1, {}, {}, 0.5F)}), sources, capabilities);
    require(zeroLayer && visibleLayer && zeroLayer.value() != visibleLayer.value(),
            "opacity 0 layerを構造比較から除外しました");

    PreviewCapabilities oneSource = capabilities;
    oneSource.maxQualifiedActiveVideoSources = 1;
    CompositionAcceptanceState sourceCap;
    requireFailure(
        sourceCap.submit(snapshot({layer(1), layer(2, {}, {}, 0.0F)}), sources, oneSource),
        PreviewErrorCategory::UnsupportedCapability,
        "opacity 0 layerをdistinct source countから除外しました");

    PreviewCapabilities oneLayer = capabilities;
    oneLayer.maxQualifiedCompositionLayers = 1;
    CompositionAcceptanceState layerCap;
    requireFailure(layerCap.submit(snapshot({layer(1), layer(2, {}, {}, 0.0F)}), sources, oneLayer),
                   PreviewErrorCategory::UnsupportedCapability,
                   "opacity 0 layerをlayer countから除外しました");

    CompositionAcceptanceState duplicate;
    requireFailure(duplicate.submit(snapshot({layer(1), layer(1)}), sources, capabilities),
                   PreviewErrorCategory::UnsupportedCapability,
                   "duplicate source layerをacceptしました");

    CompositionAcceptanceState unknown;
    requireFailure(unknown.submit(snapshot({layer(99)}), sources, capabilities),
                   PreviewErrorCategory::InvalidSource, "unknown sourceをacceptしました");
    auto nonVideoSources = sources;
    nonVideoSources.at(1).videoEnabled = false;
    requireFailure(unknown.submit(a, nonVideoSources, capabilities),
                   PreviewErrorCategory::InvalidSource, "non-video sourceをacceptしました");

    CompositionAcceptanceState order;
    const auto order12 = order.submit(snapshot({layer(1), layer(2)}), sources, capabilities);
    const auto order21 = order.submit(snapshot({layer(2), layer(1)}), sources, capabilities);
    require(order12 && order21 && order12.value() != order21.value(),
            "layer orderを構造比較していません");
}

void engineFacadeAndEvents() {
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    auto sink = std::make_shared<RecordingSink>();
    sink->engine = &engine;
    require(engine.initialize(qualifiedConfig(), dispatcher), "engine initializeに失敗しました");
    const PreviewCapabilities productCapabilities = engine.capabilities();
    require(productCapabilities.maxQualifiedActiveVideoSources == 1 &&
                productCapabilities.maxQualifiedCompositionLayers == 1 &&
                productCapabilities.maxQualifiedActiveAudioSources == 0 &&
                productCapabilities.qualifiedAudioSampleRate == 0 &&
                productCapabilities.qualifiedAudioChannelCount == 0,
            "公開capabilityがP5-C product wiringの実装上限と一致しません");
    require(engine.status().state == PreviewEngineState::WaitingForRenderDevice,
            "logical initialize stateが違います");
    require(engine.attachEventSink(sink), "sink attachに失敗しました");
    dispatcher->runAll();
    require(sink->states == std::vector{PreviewEngineState::WaitingForRenderDevice},
            "initialize eventを配送できません");
    require(sink->observedStatus == PreviewEngineState::WaitingForRenderDevice,
            "callbackをengine lock保持中に呼んだ可能性があります");

    require(PreviewRenderPort::attachLogicalDevice(engine), "private render seamに失敗しました");
    dispatcher->runAll();
    require(engine.status().state == PreviewEngineState::ReadyPaused,
            "private render-ready transitionが違います");

    requireFailure(engine.addSource({"movie.mp4", true, false}), PreviewErrorCategory::InvalidState,
                   "native deviceなしでsource IDを発行しました");
    requireFailure(engine.play(), PreviewErrorCategory::InvalidState,
                   "source/compositionなしでPlayingへ遷移しました");

    require(engine.detachEventSink(), "sink detachに失敗しました");
    require(engine.detachEventSink(), "sink detachがidempotentではありません");
    require(engine.requestShutdown(), "shutdown requestに失敗しました");
    require(engine.status().state == PreviewEngineState::ShuttingDown,
            "request returnをterminal acknowledgementにしました");
    require(PreviewRenderPort::completeTeardown(engine), "teardown completionに失敗しました");
    dispatcher->runAll();
    require(engine.status().state == PreviewEngineState::Shutdown, "terminal Shutdownになりません");
    require(sink->states.size() == 2, "detach後にcallbackを開始しました");
    require(engine.requestShutdown(), "Shutdownでidempotentではありません");
}

void eventOwnershipAndFatalPath() {
    PreviewEngine rollback;
    auto rejectedDispatcher = std::make_shared<ManualDispatcher>();
    std::weak_ptr<ManualDispatcher> rejectedWeak = rejectedDispatcher;
    requireFailure(rollback.initialize({{{24, 1}}}, rejectedDispatcher),
                   PreviewErrorCategory::UnsupportedCapability,
                   "invalid initializeをacceptしました");
    rejectedDispatcher.reset();
    require(rejectedWeak.expired(), "transactional rollback後もdispatcherを保持しています");

    PreviewEngine postRollback;
    auto refusingDispatcher = std::make_shared<ManualDispatcher>();
    refusingDispatcher->acceptPosts = false;
    std::weak_ptr<ManualDispatcher> refusingWeak = refusingDispatcher;
    requireFailure(postRollback.initialize(qualifiedConfig(), refusingDispatcher),
                   PreviewErrorCategory::ShutdownFailure,
                   "dispatcher post failureをinitialize successにしました");
    require(postRollback.status().state == PreviewEngineState::Uninitialized,
            "dispatcher post failure後にstateをrollbackしていません");
    refusingDispatcher.reset();
    require(refusingWeak.expired(), "dispatcher post failure後もdispatcherを保持しています");

    PreviewEngine detached;
    auto detachedDispatcher = std::make_shared<ManualDispatcher>();
    auto detachedSink = std::make_shared<RecordingSink>();
    require(detached.initialize(qualifiedConfig(), detachedDispatcher), "initializeに失敗しました");
    require(detached.attachEventSink(detachedSink), "sink attachに失敗しました");
    require(detached.detachEventSink(), "pending callback前のdetachに失敗しました");
    detachedDispatcher->runAll();
    require(detachedSink->states.empty(), "detach return後にpending callbackを開始しました");
    require(detached.requestShutdown(), "shutdownに失敗しました");
    require(detached.status().state == PreviewEngineState::Shutdown,
            "renderer未生成のshutdownを内部完了できませんでした");
    detachedDispatcher->runAll();

    PreviewEngine expired;
    auto expiredDispatcher = std::make_shared<ManualDispatcher>();
    auto expiredSink = std::make_shared<RecordingSink>();
    require(expired.initialize(qualifiedConfig(), expiredDispatcher), "initializeに失敗しました");
    require(expired.attachEventSink(expiredSink), "sink attachに失敗しました");
    expiredSink.reset();
    expiredDispatcher->runAll();
    require(expired.status().state == PreviewEngineState::WaitingForRenderDevice,
            "weak sink失効でstate progressionが止まりました");
    require(expired.requestShutdown(), "shutdownに失敗しました");
    require(expired.status().state == PreviewEngineState::Shutdown,
            "renderer未生成のshutdownを内部完了できませんでした");
    expiredDispatcher->runAll();

    PreviewEngine fatal;
    auto fatalDispatcher = std::make_shared<ManualDispatcher>();
    auto fatalSink = std::make_shared<RecordingSink>();
    require(fatal.initialize(qualifiedConfig(), fatalDispatcher),
            "fatal test initializeに失敗しました");
    require(fatal.attachEventSink(fatalSink), "fatal sink attachに失敗しました");
    fatalDispatcher->runAll();
    PreviewError error{PreviewErrorCategory::DeviceFailure,
                       PreviewErrorSeverity::FatalToSession,
                       PreviewOperation::RenderDeviceAttach,
                       std::nullopt,
                       "render device failure",
                       12};
    require(PreviewRenderPort::injectFatal(fatal, error), "fatal injectionに失敗しました");
    require(fatal.status().state == PreviewEngineState::ShuttingDown,
            "fatal検出直後にErrorを公開しました");
    require(PreviewRenderPort::completeTeardown(fatal), "fatal teardown完了に失敗しました");
    fatalDispatcher->runAll();
    require(fatal.status().state == PreviewEngineState::Error,
            "safe teardown後にErrorを公開しませんでした");
    require(fatal.status().lastError == error, "terminal snapshotにlastErrorがありません");
    require(fatalSink->errors.size() == 1, "errorOccurredを一度配送していません");
    require(fatalSink->states.back() == PreviewEngineState::Error,
            "final Error acknowledgementが最後ではありません");
    const std::size_t callbackCount = fatalSink->states.size() + fatalSink->errors.size();
    require(fatal.requestShutdown(), "Errorでshutdownがidempotentではありません");
    fatalDispatcher->runAll();
    require(fatalSink->states.size() + fatalSink->errors.size() == callbackCount,
            "final terminal acknowledgement後にcallbackを発行しました");
}

void constructorThreadAuthority() {
    PreviewEngine wrongThreadInitialize;
    auto rejectedDispatcher = std::make_shared<ManualDispatcher>();
    std::weak_ptr<ManualDispatcher> rejectedWeak = rejectedDispatcher;
    std::optional<Result<void>> initializeResult;
    std::thread initializeThread([&] {
        initializeResult.emplace(
            wrongThreadInitialize.initialize(qualifiedConfig(), rejectedDispatcher));
    });
    initializeThread.join();
    require(initializeResult.has_value(), "wrong-thread initialize resultがありません");
    requireFailure(*initializeResult, PreviewErrorCategory::InvalidState,
                   "construction thread以外のinitializeを受理しました");
    require(wrongThreadInitialize.status().state == PreviewEngineState::Uninitialized,
            "wrong-thread initializeがstateを変更しました");
    rejectedDispatcher.reset();
    require(rejectedWeak.expired(), "wrong-thread initializeがdispatcherを保持しました");

    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize(qualifiedConfig(), dispatcher),
            "owner-thread initializeに失敗しました");
    dispatcher->runAll();
    auto sink = std::make_shared<RecordingSink>();
    std::optional<Result<void>> controlResult;
    std::thread controlThread([&] {
        controlResult.emplace(engine.attachEventSink(std::weak_ptr<PreviewEventSink>(sink)));
    });
    controlThread.join();
    require(controlResult.has_value(), "wrong-thread control resultがありません");
    requireFailure(*controlResult, PreviewErrorCategory::InvalidState,
                   "construction thread以外のcontrol operationを受理しました");
    require(engine.status().state == PreviewEngineState::WaitingForRenderDevice,
            "wrong-thread control operationがstateを変更しました");
    require(engine.requestShutdown(), "thread authority test shutdownに失敗しました");
    require(engine.status().state == PreviewEngineState::Shutdown,
            "renderer未生成のthread authority testを内部完了できませんでした");
    dispatcher->runAll();
}

void dispatcherAndSinkFailureContainment() {
    PreviewEngine rejectedPost;
    auto rejectingDispatcher = std::make_shared<ManualDispatcher>();
    require(rejectedPost.initialize(qualifiedConfig(), rejectingDispatcher),
            "runtime reject test initializeに失敗しました");
    rejectingDispatcher->runAll();
    rejectingDispatcher->rejectNextPost = true;
    require(PreviewRenderPort::attachLogicalDevice(rejectedPost),
            "dispatcher rejectをrender attach rejectionへ変換しました");
    require(rejectedPost.status().state == PreviewEngineState::ReadyPaused,
            "dispatcher rejectがauthoritative stateをrollbackしました");
    require(rejectedPost.telemetry().eventDeliveryFailureCount == 1,
            "runtime dispatcher rejectをdiagnosticへ記録していません");
    require(rejectedPost.requestShutdown(), "dispatcher reject後のshutdownに失敗しました");
    rejectingDispatcher->runAll();
    require(PreviewRenderPort::completeTeardown(rejectedPost),
            "dispatcher reject後のteardownに失敗しました");
    rejectingDispatcher->runAll();

    PreviewEngine throwingPost;
    auto throwingDispatcher = std::make_shared<ManualDispatcher>();
    require(throwingPost.initialize(qualifiedConfig(), throwingDispatcher),
            "runtime throw test initializeに失敗しました");
    throwingDispatcher->runAll();
    throwingDispatcher->throwNextPost = true;
    bool dispatcherExceptionEscaped = false;
    try {
        require(PreviewRenderPort::attachLogicalDevice(throwingPost),
                "dispatcher throwをrender attach rejectionへ変換しました");
    } catch (...) {
        dispatcherExceptionEscaped = true;
    }
    require(!dispatcherExceptionEscaped, "dispatcher post exceptionがengine境界から漏れました");
    require(throwingPost.status().state == PreviewEngineState::ReadyPaused,
            "dispatcher throwがauthoritative stateをrollbackしました");
    require(throwingPost.telemetry().eventDeliveryFailureCount == 1,
            "dispatcher throwをdiagnosticへ記録していません");
    require(throwingPost.requestShutdown(), "dispatcher throw後のshutdownに失敗しました");
    throwingDispatcher->runAll();
    require(PreviewRenderPort::completeTeardown(throwingPost),
            "dispatcher throw後のteardownに失敗しました");
    throwingDispatcher->runAll();

    PreviewEngine throwingSinkEngine;
    auto sinkDispatcher = std::make_shared<ManualDispatcher>();
    auto throwingSink = std::make_shared<ThrowingSink>();
    require(throwingSinkEngine.initialize(qualifiedConfig(), sinkDispatcher),
            "sink throw test initializeに失敗しました");
    require(throwingSinkEngine.attachEventSink(throwingSink), "throwing sink attachに失敗しました");
    bool sinkExceptionEscaped = false;
    try {
        sinkDispatcher->runAll();
    } catch (...) {
        sinkExceptionEscaped = true;
    }
    require(!sinkExceptionEscaped, "sink callback exceptionがdispatcherへ漏れました");
    require(throwingSink->callbackCount == 1, "throwing sink callbackを実行していません");
    require(throwingSinkEngine.telemetry().eventDeliveryFailureCount == 1,
            "sink callback exceptionをdiagnosticへ記録していません");
    require(throwingSinkEngine.requestShutdown(), "sink throw後のshutdownに失敗しました");
    require(throwingSinkEngine.status().state == PreviewEngineState::Shutdown,
            "renderer未生成のsink throw testを内部完了できませんでした");
    sinkDispatcher->runAll();
    require(throwingSinkEngine.status().state == PreviewEngineState::Shutdown,
            "sink throw後にterminal Shutdownへ到達しません");
}

void dispatcherContinuationFailure() {
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize(qualifiedConfig(), dispatcher),
            "continuation test initializeに失敗しました");
    require(PreviewRenderPort::attachLogicalDevice(engine),
            "continuation test render attachに失敗しました");
    require(PreviewRenderPort::mailboxSizeForTest(engine) == 2,
            "continuation test backlogを構築できません");

    dispatcher->rejectNextPost = true;
    dispatcher->runOne();
    require(engine.telemetry().eventDeliveryFailureCount == 1,
            "continuation post failureをdiagnosticへ記録していません");
    require(PreviewRenderPort::mailboxSizeForTest(engine) == 1,
            "continuation failure時のpending event数が違います");
    require(dispatcher->tasks.empty(), "continuation failureで無限retry taskを生成しました");
    require(engine.status().state == PreviewEngineState::ReadyPaused,
            "continuation failureがauthoritative stateを変更しました");

    require(engine.requestShutdown(), "later enqueueによるretry開始に失敗しました");
    dispatcher->runAll();
    require(PreviewRenderPort::mailboxSizeForTest(engine) == 0,
            "later enqueueでstranded eventを再配送できません");
    require(PreviewRenderPort::completeTeardown(engine),
            "continuation failure後のteardownに失敗しました");
    dispatcher->runAll();
}

void fillNonTerminalMailbox(PreviewEngine& engine) {
    while (PreviewRenderPort::mailboxSizeForTest(engine) < 31) {
        PreviewRenderPort::enqueueEventForTest(engine,
                                               StateChangedEvent{PreviewEngineState::ReadyPaused});
    }
}

void mailboxSaturationLifecycle() {
    PreviewEngine normal;
    auto normalDispatcher = std::make_shared<ManualDispatcher>();
    require(normal.initialize(qualifiedConfig(), normalDispatcher),
            "mailbox normal test initializeに失敗しました");
    normalDispatcher->runAll();
    normalDispatcher->acceptPosts = false;
    fillNonTerminalMailbox(normal);
    const std::uint64_t failuresBeforeShutdown = normal.telemetry().eventDeliveryFailureCount;
    require(normal.requestShutdown(), "mailbox-fullでshutdown requestをfalse failureにしました");
    require(normal.status().state == PreviewEngineState::Shutdown,
            "mailbox-fullでrenderer未生成shutdownを内部完了できませんでした");
    require(normal.telemetry().eventDeliveryFailureCount > failuresBeforeShutdown,
            "mailbox-full shutdown notification failureを記録していません");
    require(PreviewRenderPort::mailboxSizeForTest(normal) == 32,
            "terminal reserved slotを使用していません");

    PreviewEngine fatal;
    auto fatalDispatcher = std::make_shared<ManualDispatcher>();
    require(fatal.initialize(qualifiedConfig(), fatalDispatcher),
            "mailbox fatal test initializeに失敗しました");
    fatalDispatcher->runAll();
    fatalDispatcher->acceptPosts = false;
    fillNonTerminalMailbox(fatal);
    PreviewError fatalError{PreviewErrorCategory::DeviceFailure,
                            PreviewErrorSeverity::FatalToSession,
                            PreviewOperation::RenderDeviceAttach,
                            std::nullopt,
                            "mailbox saturated fatal",
                            77};
    const std::uint64_t failuresBeforeFatal = fatal.telemetry().eventDeliveryFailureCount;
    require(PreviewRenderPort::injectFatal(fatal, fatalError),
            "mailbox-fullでfatal injectionをfalse failureにしました");
    require(fatal.status().state == PreviewEngineState::ShuttingDown,
            "mailbox-full fatalがShuttingDownへ進みません");
    require(fatal.telemetry().eventDeliveryFailureCount >= failuresBeforeFatal + 2,
            "fatal event overflowを全てdiagnosticへ記録していません");
    require(PreviewRenderPort::completeTeardown(fatal),
            "mailbox-full fatal completionをfalse failureにしました");
    require(fatal.status().state == PreviewEngineState::Error,
            "mailbox-full fatal teardownがErrorになりません");
    require(fatal.status().lastError == fatalError,
            "mailbox-full fatalでauthoritative lastErrorを失いました");
}

void dispatcherRetention() {
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    std::weak_ptr<ManualDispatcher> weak = dispatcher;
    require(engine.initialize(qualifiedConfig(), dispatcher), "initializeに失敗しました");
    dispatcher.reset();
    require(!weak.expired(), "terminal acknowledgement前にdispatcherを解放しました");
    auto retained = weak.lock();
    require(retained != nullptr, "retained dispatcherを取得できません");
    retained->runAll();
    require(engine.requestShutdown(), "shutdown requestに失敗しました");
    require(engine.status().state == PreviewEngineState::Shutdown,
            "renderer未生成のshutdownを内部完了できませんでした");
    retained->runAll();
    retained.reset();
    require(weak.expired(), "terminal acknowledgement後もdispatcherを保持しています");
}

void safeDestruction() {
    {
        PreviewEngine engine;
    }
    {
        PreviewEngine engine;
        auto dispatcher = std::make_shared<ManualDispatcher>();
        require(engine.initialize(qualifiedConfig(), dispatcher), "initializeに失敗しました");
        require(engine.requestShutdown(), "shutdown requestに失敗しました");
        require(engine.status().state == PreviewEngineState::Shutdown,
                "renderer未生成のshutdownを内部完了できませんでした");
    }
    {
        PreviewEngine engine;
        auto dispatcher = std::make_shared<ManualDispatcher>();
        require(engine.initialize(qualifiedConfig(), dispatcher), "initializeに失敗しました");
        PreviewError error{PreviewErrorCategory::DeviceFailure,
                           PreviewErrorSeverity::FatalToSession,
                           PreviewOperation::RenderDeviceAttach,
                           std::nullopt,
                           "fatal",
                           std::nullopt};
        require(PreviewRenderPort::injectFatal(engine, error), "fatal injectionに失敗しました");
        require(PreviewRenderPort::completeTeardown(engine), "Error completionに失敗しました");
    }
}

void p5cControlAndRenderNegatives() {
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize(qualifiedConfig(), dispatcher), "initializeに失敗しました");
    requireFailure(engine.play(), PreviewErrorCategory::InvalidState,
                   "render attach前のplayを受理しました");
    requireFailure(engine.addSource({"movie.mp4", true, false}), PreviewErrorCategory::InvalidState,
                   "ReadyPaused前のaddSourceを受理しました");
    require(PreviewRenderPort::bindRenderThread(engine), "render thread bindに失敗しました");
    requireFailure(PreviewRenderPort::bindRenderThread(engine), PreviewErrorCategory::InvalidState,
                   "duplicate render thread bindを受理しました");

    std::optional<Result<void>> wrongThreadAttach;
    std::thread wrongThread([&] {
        wrongThreadAttach.emplace(
            PreviewRenderPort::attachNativeD3D11Device(engine, nullptr, nullptr));
    });
    wrongThread.join();
    require(wrongThreadAttach.has_value(), "wrong-thread attach resultがありません");
    requireFailure(*wrongThreadAttach, PreviewErrorCategory::InvalidState,
                   "wrong-thread native attachを受理しました");

    require(PreviewRenderPort::attachLogicalDevice(engine), "logical test seamに失敗しました");
    requireFailure(engine.addSource({"movie.mp4", true, true}),
                   PreviewErrorCategory::UnsupportedCapability,
                   "audioEnabled sourceを受理しました");
    requireFailure(engine.submitComposition(std::make_shared<const CompositionSnapshot>()),
                   PreviewErrorCategory::CompositionFailure, "empty compositionを受理しました");
    requireFailure(engine.submitComposition(snapshot({layer(99)})),
                   PreviewErrorCategory::InvalidSource, "unknown sourceを受理しました");
    requireFailure(engine.submitComposition(snapshot({layer(1), layer(2)})),
                   PreviewErrorCategory::UnsupportedCapability,
                   "P5-Cでtwo-layer product submissionを受理しました");
    requireFailure(engine.pause(), PreviewErrorCategory::InvalidState,
                   "Playing以外のpauseを受理しました");
    requireFailure(engine.seek({0}), PreviewErrorCategory::UnsupportedCapability,
                   "P5-Cでseekを受理しました");
    require(engine.requestShutdown(), "shutdown requestに失敗しました");
    require(PreviewRenderPort::completeTeardown(engine), "teardown completionに失敗しました");
}

void distinctFrameCounterIsBounded() {
    DistinctFrameCounter counter;
    require(counter.count() == 0, "distinct frame counterの初期値が0ではありません");
    counter.note(10);
    counter.note(10);
    counter.note(9);
    counter.note(11);
    require(counter.count() == 2, "distinct frame counterが重複または逆行frameを加算しました");
}

void schedulerSkippedFramesAreCounted() {
    require(skippedSchedulerFrameCount(10, 11) == 0, "連続scheduler targetをdropとして数えました");
    require(skippedSchedulerFrameCount(10, 13) == 2,
            "schedulerが飛ばしたoutput frameを数えていません");
    require(skippedSchedulerFrameCount(-1, 2) == 2,
            "再生開始直後に飛ばしたoutput frameを数えていません");
    require(skippedSchedulerFrameCount(13, 13) == 0, "同一scheduler targetをdropとして数えました");
}

void unsafeDestructionProcess() {
    std::set_terminate([] { std::_Exit(86); });
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize(qualifiedConfig(), dispatcher), "initializeに失敗しました");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--unsafe-destructor") {
        unsafeDestructionProcess();
        return 0;
    }

    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"frame rate / descriptor", frameRateAndDescriptorValidation},
        {"Result / error", resultAndErrorValues},
        {"state machine", stateMachineLifecycle},
        {"event mailbox", mailboxOrderingAndBounds},
        {"composition domain", compositionDomains},
        {"composition identity", compositionIdentityAndCapabilities},
        {"engine façade / events", engineFacadeAndEvents},
        {"event ownership / fatal", eventOwnershipAndFatalPath},
        {"constructor thread authority", constructorThreadAuthority},
        {"dispatcher / sink failure containment", dispatcherAndSinkFailureContainment},
        {"dispatcher continuation failure", dispatcherContinuationFailure},
        {"mailbox saturation lifecycle", mailboxSaturationLifecycle},
        {"dispatcher retention", dispatcherRetention},
        {"safe destruction", safeDestruction},
        {"P5-C control / render negatives", p5cControlAndRenderNegatives},
        {"bounded distinct frame counter", distinctFrameCounterIsBounded},
        {"scheduler skipped frame count", schedulerSkippedFramesAreCounted},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
