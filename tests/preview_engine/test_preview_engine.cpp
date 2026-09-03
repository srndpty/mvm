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

// 実測済み (measured) の構成。60/1 は measuredOutputFrameRates の唯一の要素。
PreviewEngineConfig measuredConfig() {
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

// matchesMeasuredEnvelope は保存値ではなく derived であること。
// 保存値にすると configured を書き換える経路ごとに再計算が要り、
// 書き忘れると stale な true が残る。
void measuredEnvelopeIsDerived() {
    // 第0状態: 構成が未確定なら、値が envelope と一致していても measured にしない。
    // configured* の既定値は envelope の既定値と同じ組なので、この検査が無いと
    // 「initialize していない engine が measured」を通してしまう。
    PreviewCapabilities unconfigured;
    unconfigured.configuredOutputFrameRate = MeasuredPreviewEnvelope{}.outputFrameRate;
    unconfigured.configuredMaxActiveVideoSources = MeasuredPreviewEnvelope{}.maxActiveVideoSources;
    unconfigured.configuredMaxCompositionLayers = MeasuredPreviewEnvelope{}.maxCompositionLayers;
    unconfigured.configuredMaxActiveAudioSources = MeasuredPreviewEnvelope{}.maxActiveAudioSources;
    unconfigured.configuredAudioSampleRate = MeasuredPreviewEnvelope{}.audioSampleRate;
    unconfigured.configuredAudioChannelCount = MeasuredPreviewEnvelope{}.audioChannelCount;
    require(!unconfigured.hasConfiguredEnvelope, "構成未確定が既定値になっていません");
    require(!unconfigured.matchesMeasuredEnvelope(),
            "構成未確定のcapabilityをmeasured一致として返しました");

    PreviewCapabilities capability;
    capability.hasConfiguredEnvelope = true;
    const MeasuredPreviewEnvelope envelope;
    capability.configuredOutputFrameRate = envelope.outputFrameRate;
    capability.configuredMaxActiveVideoSources = envelope.maxActiveVideoSources;
    capability.configuredMaxCompositionLayers = envelope.maxCompositionLayers;
    capability.configuredMaxActiveAudioSources = envelope.maxActiveAudioSources;
    capability.configuredAudioSampleRate = envelope.audioSampleRate;
    capability.configuredAudioChannelCount = envelope.audioChannelCount;
    require(capability.matchesMeasuredEnvelope(), "envelopeと同じ構成を一致として返しません");

    // 軸ごとに 1 つずつ崩す。どれが崩れても一致にしない。
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredOutputFrameRate = PreviewFrameRate{24, 1};
        require(!mutated.matchesMeasuredEnvelope(), "output rateの相違を無視しました");
    }
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredMaxActiveVideoSources = envelope.maxActiveVideoSources - 1;
        require(!mutated.matchesMeasuredEnvelope(), "video source上限の相違を無視しました");
    }
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredMaxCompositionLayers = envelope.maxCompositionLayers + 1;
        require(!mutated.matchesMeasuredEnvelope(), "layer上限の相違を無視しました");
    }
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredMaxActiveAudioSources = envelope.maxActiveAudioSources + 1;
        require(!mutated.matchesMeasuredEnvelope(), "audio source上限の相違を無視しました");
    }
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredAudioSampleRate = 44100;
        require(!mutated.matchesMeasuredEnvelope(), "audio sample rateの相違を無視しました");
    }
    {
        PreviewCapabilities mutated = capability;
        mutated.configuredAudioChannelCount = 1;
        require(!mutated.matchesMeasuredEnvelope(), "audio channel数の相違を無視しました");
    }

    // 崩してから戻せば一致へ復帰する (保存値なら stale のまま)。
    PreviewCapabilities restored = capability;
    restored.configuredMaxActiveVideoSources = envelope.maxActiveVideoSources - 1;
    require(!restored.matchesMeasuredEnvelope(), "崩した構成を一致として返しました");
    restored.configuredMaxActiveVideoSources = envelope.maxActiveVideoSources;
    require(restored.matchesMeasuredEnvelope(), "戻した構成が一致へ復帰しません");
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
    const auto validButUnconfigurable = validatePreviewFrameRate(48, 1);
    require(validButUnconfigurable, "positive rationalをtype validationでrejectしました");
    require(validateSourceFrameRate(120, 2, {60, 1}), "source frame rateをcanonical比較できません");
    requireFailure(validateSourceFrameRate(30, 1, {60, 1}),
                   PreviewErrorCategory::UnsupportedCapability, "30fps sourceをP5-Cで受理しました");
    requireFailure(validateSourceFrameRate(120, 1, {60, 1}),
                   PreviewErrorCategory::UnsupportedCapability,
                   "120fps sourceをP5-Cで受理しました");

    PreviewEngine unconfigurableEngine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    // initialize 前の engine は measured ではない。configured* の既定値が
    // measured envelope の既定値と同じ組であることに引きずられない。
    require(!unconfigurableEngine.capabilities().matchesMeasuredEnvelope(),
            "initialize前のengineをmeasured一致として公開しました");
    require(!unconfigurableEngine.capabilities().hasConfiguredEnvelope,
            "initialize前のengineが構成確定済みになっています");
    // 48/1 は configurable 表に無い。表の中身はここへ literal で書き、実装の
    // 判定関数を呼ばない。
    requireFailure(unconfigurableEngine.initialize({{{48, 1}}}, dispatcher),
                   PreviewErrorCategory::UnsupportedCapability,
                   "configurableでないrateをinitializeでrejectしませんでした");
    require(unconfigurableEngine.status().state == PreviewEngineState::Uninitialized,
            "initialize reject後にstateが変化しました");
    // initialize に失敗した engine も measured ではない。
    require(!unconfigurableEngine.capabilities().matchesMeasuredEnvelope(),
            "initialize失敗後のengineをmeasured一致として公開しました");

    // 60/1 以外の configurable rate (= 未計測) を受理し、capability がその rate を
    // 公開すること。受理は qualification ではない。
    // 固定値 60/1 を返すと source の rate 検査が別の基準で動いてしまう。
    const std::pair<std::uint32_t, std::uint32_t> configurableOnlyRates[] = {
        {24, 1}, {24000, 1001}, {25, 1}, {30, 1}, {30000, 1001}, {50, 1}, {60000, 1001}};
    for (const auto& [numerator, denominator] : configurableOnlyRates) {
        PreviewEngine rateEngine;
        auto rateDispatcher = std::make_shared<ManualDispatcher>();
        require(rateEngine.initialize({{{numerator, denominator}}}, rateDispatcher),
                "configurable rateをinitializeでrejectしました");
        require(rateEngine.capabilities().configuredOutputFrameRate ==
                    PreviewFrameRate{numerator, denominator},
                "capabilityがinitializeしたoutput rateを公開していません");
        // 受理できることと計測済みであることを混ぜない。60/1 以外は未計測である。
        require(!rateEngine.capabilities().matchesMeasuredEnvelope(),
                "未計測の構成をmeasured envelope一致として公開しました");
        // envelope そのものは 60/1 cohort のまま動かない。現在の rate へ追従させると
        // 「測っていない構成を測ったことにする」になる。
        require(rateEngine.capabilities().measuredEnvelope.outputFrameRate ==
                    PreviewFrameRate{60, 1},
                "measured envelopeがinitializeしたrateへ追従しました");
        require(rateEngine.capabilities().measuredEnvelope.maxCompositionLayers == 2,
                "measured envelopeのlayer上限が変化しました");
        // source の rate 検査が output rate を基準にしていること。
        require(validateSourceFrameRate(numerator, denominator,
                                        rateEngine.capabilities().configuredOutputFrameRate),
                "output rateと同じsource rateをrejectしました");
        requireFailure(validateSourceFrameRate(60, 1, {numerator, denominator}),
                       PreviewErrorCategory::UnsupportedCapability,
                       "output rateと異なるsource rateを受理しました");
        require(rateEngine.requestShutdown(), "configurable rate engineのshutdownに失敗しました");
        require(rateEngine.status().state == PreviewEngineState::Shutdown,
                "configurable rate engineがterminal Shutdownへ到達しませんでした");
    }

    PreviewEngine equivalentRateEngine;
    auto equivalentRateDispatcher = std::make_shared<ManualDispatcher>();
    require(equivalentRateEngine.initialize({{{120, 2}}}, equivalentRateDispatcher),
            "60/1と等価な120/2をinitializeで受理しませんでした");
    // P5-D2でaudio-master transportを接続したため、configured audio sourceは1件である。
    // 等価rationalの受理がcapabilityを書き換えないことを、ここで固定する。
    require(equivalentRateEngine.capabilities().matchesMeasuredEnvelope(),
            "60/1構成をmeasured envelope一致として公開していません");
    require(equivalentRateEngine.capabilities().configuredOutputFrameRate ==
                    PreviewFrameRate{60, 1} &&
                equivalentRateEngine.capabilities().configuredMaxActiveAudioSources == 1 &&
                equivalentRateEngine.capabilities().configuredAudioSampleRate == 48000 &&
                equivalentRateEngine.capabilities().configuredAudioChannelCount == 2,
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

    PreviewCompositionLayer effectLayer =
        layer(1, {-0.25F, 0.1F, 0.8F, 0.8F}, {0.1F, 0.2F, 0.7F, 0.6F}, 0.5F);
    effectLayer.effectsEnabled = true;
    effectLayer.rotationDegrees = 30.0F;
    effectLayer.sourceInFrame = 100;
    effectLayer.sourceDurationFrames = 60;
    effectLayer.fadeInFrames = 10;
    effectLayer.fadeOutFrames = 12;
    CompositionAcceptanceState effectState;
    require(effectState.submit(snapshot({effectLayer}), sources, capabilities),
            "M7a effect layerの画面外destinationとsource-native timingをrejectしました");
    effectLayer.fadeInFrames = 50;
    effectLayer.fadeOutFrames = 20;
    CompositionAcceptanceState invalidEffectState;
    requireFailure(invalidEffectState.submit(snapshot({effectLayer}), sources, capabilities),
                   PreviewErrorCategory::CompositionFailure,
                   "重なるsource-native FadeをPreview mappingがacceptしました");

    // 各fieldを独立に壊す。代表fieldだけの検査では、他fieldの有限性検査漏れを検出できない。
    for (int field = 0; field < 4; ++field) {
        for (float value : {nan, inf, -inf}) {
            PreviewNormalizedRect rect;
            switch (field) {
            case 0:
                rect.x = value;
                break;
            case 1:
                rect.y = value;
                break;
            case 2:
                rect.width = value;
                break;
            default:
                rect.height = value;
                break;
            }
            CompositionAcceptanceState destinationState;
            requireFailure(
                destinationState.submit(snapshot({layer(1, rect)}), sources, capabilities),
                PreviewErrorCategory::CompositionFailure,
                "destinationの非有限fieldをacceptしました");
            CompositionAcceptanceState sourceRectState;
            requireFailure(
                sourceRectState.submit(snapshot({layer(1, {}, rect)}), sources, capabilities),
                PreviewErrorCategory::CompositionFailure,
                "sourceRectの非有限fieldをacceptしました");
        }
    }
}

void compositionStructuralEqualityLiterals() {
    const auto sources = twoSources();
    PreviewCapabilities capabilities;
    capabilities.configuredMaxActiveVideoSources = 2;
    capabilities.configuredMaxCompositionLayers = 2;

    const PreviewNormalizedRect baseRect{0.1F, 0.1F, 0.5F, 0.5F};
    const auto expectDifferent = [&](std::shared_ptr<const CompositionSnapshot> first,
                                     std::shared_ptr<const CompositionSnapshot> second,
                                     const char* message) {
        CompositionAcceptanceState state;
        const auto acceptedFirst = state.submit(std::move(first), sources, capabilities);
        const auto acceptedSecond = state.submit(std::move(second), sources, capabilities);
        require(acceptedFirst && acceptedFirst.value() == AcceptedComposition{{1}, 1},
                "構造比較の基準tokenがliteralと違います");
        require(acceptedSecond && acceptedSecond.value() == AcceptedComposition{{2}, 2}, message);
    };

    expectDifferent(snapshot({layer(1, baseRect)}),
                    snapshot({layer(1, baseRect), layer(2, baseRect)}),
                    "layer count差を構造比較していません");
    expectDifferent(snapshot({layer(1, baseRect), layer(2, baseRect)}),
                    snapshot({layer(2, baseRect), layer(1, baseRect)}),
                    "layer order差を構造比較していません");
    expectDifferent(snapshot({layer(1, baseRect)}), snapshot({layer(2, baseRect)}),
                    "source ID差を構造比較していません");

    for (int field = 0; field < 4; ++field) {
        PreviewNormalizedRect changed = baseRect;
        switch (field) {
        case 0:
            changed.x = 0.2F;
            break;
        case 1:
            changed.y = 0.2F;
            break;
        case 2:
            changed.width = 0.4F;
            break;
        default:
            changed.height = 0.4F;
            break;
        }
        expectDifferent(snapshot({layer(1, baseRect)}), snapshot({layer(1, changed)}),
                        "destination field差を構造比較していません");
        expectDifferent(snapshot({layer(1, {}, baseRect)}), snapshot({layer(1, {}, changed)}),
                        "sourceRect field差を構造比較していません");
    }
    expectDifferent(snapshot({layer(1, baseRect, baseRect, 0.75F)}),
                    snapshot({layer(1, baseRect, baseRect, 0.5F)}),
                    "opacity差を構造比較していません");

    const std::vector<std::shared_ptr<const CompositionSnapshot>> invalidSnapshots{
        snapshot({layer(1, {-0.1F, 0.0F, 1.0F, 1.0F})}),
        snapshot({layer(1, {}, {0.0F, 0.0F, 0.0F, 1.0F})}), snapshot({layer(1, {}, {}, 1.1F)})};
    for (const auto& invalid : invalidSnapshots) {
        CompositionAcceptanceState state;
        const auto accepted = state.submit(snapshot({layer(1)}), sources, capabilities);
        require(accepted && accepted.value() == AcceptedComposition{{1}, 1},
                "reject不変条件の基準tokenがliteralと違います");
        requireFailure(state.submit(invalid, sources, capabilities),
                       PreviewErrorCategory::CompositionFailure,
                       "invalid compositionをacceptしました");
        const auto noOp = state.submit(snapshot({layer(1)}), sources, capabilities);
        require(noOp && noOp.value() == AcceptedComposition{{1}, 1},
                "rejectがlatest desiredまたはtokenを変更しました");
        const auto changed = state.submit(snapshot({layer(1, baseRect)}), sources, capabilities);
        require(changed && changed.value() == AcceptedComposition{{2}, 2},
                "rejectがIDまたはrevisionを消費しました");
    }
}

void compositionIdentityAndCapabilities() {
    const auto sources = twoSources();
    PreviewCapabilities capabilities;
    // P5-E closureのproduct capability 2/2と同じenvelopeで、
    // acceptance identity、order、distinct-source semanticsを固定する。
    capabilities.configuredMaxActiveVideoSources = 2;
    capabilities.configuredMaxCompositionLayers = 2;
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
    state.markPresented(acceptedA.value(), a);
    const auto newA = state.submit(snapshot({layer(1)}), sources, capabilities);
    require(newA && newA.value() == AcceptedComposition{{3}, 3},
            "presented A/latest B/submit Aでold tokenを再利用しました");
    require(state.lastPresentedToken() == acceptedA.value(),
            "desired acceptanceがlast presentedを変更しました");
    require(state.lastPresentedSnapshot() && *state.lastPresentedSnapshot() == *a,
            "last presented snapshotがpresented tokenと対応していません");

    CompositionAcceptanceState rejected;
    const auto emptyAccepted =
        rejected.submit(std::make_shared<const CompositionSnapshot>(), sources, capabilities);
    require(emptyAccepted && emptyAccepted.value() == AcceptedComposition{{1}, 1},
            "gap用empty snapshotをacceptしませんでした");
    const auto afterReject = rejected.submit(a, sources, capabilities);
    require(afterReject && afterReject.value() == AcceptedComposition{{2}, 2},
            "emptyからvideo compositionへのrevisionが不正です");

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
    oneSource.configuredMaxActiveVideoSources = 1;
    CompositionAcceptanceState sourceCap;
    requireFailure(
        sourceCap.submit(snapshot({layer(1), layer(2, {}, {}, 0.0F)}), sources, oneSource),
        PreviewErrorCategory::UnsupportedCapability,
        "opacity 0 layerをdistinct source countから除外しました");

    PreviewCapabilities oneLayer = capabilities;
    oneLayer.configuredMaxCompositionLayers = 1;
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
    require(engine.initialize(measuredConfig(), dispatcher), "engine initializeに失敗しました");
    const PreviewCapabilities productCapabilities = engine.capabilities();
    require(productCapabilities.configuredMaxActiveVideoSources == 2 &&
                productCapabilities.configuredMaxCompositionLayers == 2 &&
                productCapabilities.configuredMaxActiveAudioSources == 1 &&
                productCapabilities.configuredAudioSampleRate == 48000 &&
                productCapabilities.configuredAudioChannelCount == 2,
            "公開capabilityがP5-E3 product wiringの実装上限と一致しません");
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
    requireFailure(rollback.initialize({{{48, 1}}}, rejectedDispatcher),
                   PreviewErrorCategory::UnsupportedCapability,
                   "invalid initializeをacceptしました");
    rejectedDispatcher.reset();
    require(rejectedWeak.expired(), "transactional rollback後もdispatcherを保持しています");

    PreviewEngine postRollback;
    auto refusingDispatcher = std::make_shared<ManualDispatcher>();
    refusingDispatcher->acceptPosts = false;
    std::weak_ptr<ManualDispatcher> refusingWeak = refusingDispatcher;
    requireFailure(postRollback.initialize(measuredConfig(), refusingDispatcher),
                   PreviewErrorCategory::ShutdownFailure,
                   "dispatcher post failureをinitialize successにしました");
    require(postRollback.status().state == PreviewEngineState::Uninitialized,
            "dispatcher post failure後にstateをrollbackしていません");
    // rollback した engine の構成は確定していない。measured へ戻さない。
    require(!postRollback.capabilities().hasConfiguredEnvelope,
            "rollback後も構成確定済みのままです");
    require(!postRollback.capabilities().matchesMeasuredEnvelope(),
            "rollbackしたengineをmeasured一致として公開しました");
    refusingDispatcher.reset();
    require(refusingWeak.expired(), "dispatcher post failure後もdispatcherを保持しています");

    PreviewEngine detached;
    auto detachedDispatcher = std::make_shared<ManualDispatcher>();
    auto detachedSink = std::make_shared<RecordingSink>();
    require(detached.initialize(measuredConfig(), detachedDispatcher), "initializeに失敗しました");
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
    require(expired.initialize(measuredConfig(), expiredDispatcher), "initializeに失敗しました");
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
    require(fatal.initialize(measuredConfig(), fatalDispatcher),
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
            wrongThreadInitialize.initialize(measuredConfig(), rejectedDispatcher));
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
    require(engine.initialize(measuredConfig(), dispatcher),
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
    require(rejectedPost.initialize(measuredConfig(), rejectingDispatcher),
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
    require(throwingPost.initialize(measuredConfig(), throwingDispatcher),
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
    require(throwingSinkEngine.initialize(measuredConfig(), sinkDispatcher),
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
    require(engine.initialize(measuredConfig(), dispatcher),
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
    require(normal.initialize(measuredConfig(), normalDispatcher),
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
    require(fatal.initialize(measuredConfig(), fatalDispatcher),
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
    require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
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
        require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
        require(engine.requestShutdown(), "shutdown requestに失敗しました");
        require(engine.status().state == PreviewEngineState::Shutdown,
                "renderer未生成のshutdownを内部完了できませんでした");
    }
    {
        PreviewEngine engine;
        auto dispatcher = std::make_shared<ManualDispatcher>();
        require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
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
    require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
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
    // P5-D2でaudioEnabled sourceは受理対象になったが、native render device
    // (と WASAPI endpoint) の準備前は video と同じくfail-closedで拒否する。
    requireFailure(engine.addSource({"movie.mp4", true, true}), PreviewErrorCategory::InvalidState,
                   "native device準備前のaudioEnabled sourceを受理しました");
    requireFailure(engine.addSource({"voice.wav", false, true}), PreviewErrorCategory::InvalidState,
                   "native device準備前のaudio-only sourceを受理しました");
    // source/compositionなしのseekはgap composition受理前に検査する。
    requireFailure(engine.seek({0}), PreviewErrorCategory::InvalidState,
                   "source/compositionなしのseekを受理しました");
    require(engine.submitComposition(std::make_shared<const CompositionSnapshot>()),
            "gap用empty compositionを受理できません");
    requireFailure(engine.submitComposition(snapshot({layer(99)})),
                   PreviewErrorCategory::InvalidSource, "unknown sourceを受理しました");
    requireFailure(engine.submitComposition(snapshot({layer(1), layer(2)})),
                   PreviewErrorCategory::InvalidSource,
                   "未登録sourceを含むtwo-layer submissionを受理しました");
    requireFailure(engine.pause(), PreviewErrorCategory::InvalidState,
                   "Playing以外のpauseを受理しました");
    // P5-D3でseekは受理対象になった。ただし引数検査はsource/compositionの有無より
    // 先に行い、呼び出し側の誤りをstateの都合で別errorへすり替えない。
    requireFailure(engine.seek({-1}), PreviewErrorCategory::SeekFailure,
                   "負のoutputFrameへのseekを受理しました");
    require(engine.requestShutdown(), "shutdown requestに失敗しました");
    require(PreviewRenderPort::completeTeardown(engine), "teardown completionに失敗しました");
}

// P5-E2: source removal guard。参照が外れていないsourceを削除させない。
// engineの経路 (native device必須) はp5e product testが駆動する。ここでは
// acceptance stateが「参照が外れたか」をどう判定するかを固定する。
void p5eCompositionSourceReferences() {
    std::unordered_map<std::uint64_t, EligibleSource> sources;
    sources.emplace(1, EligibleSource{true, false});
    sources.emplace(2, EligibleSource{true, false});
    PreviewCapabilities capabilities;
    capabilities.configuredMaxActiveVideoSources = 2;
    capabilities.configuredMaxCompositionLayers = 2;

    CompositionAcceptanceState state;
    require(!state.referencesSource({1}), "compositionが無い状態で参照ありと判定しました");

    const auto a = snapshot({layer(1)});
    const auto acceptedA = state.submit(a, sources, capabilities);
    require(acceptedA, "source Aのcompositionをacceptできません");
    // acceptedだが未提示 = pending。この時点でAの参照は外れていない。
    require(state.referencesSource({1}), "pending compositionの参照を見落としました");
    require(!state.referencesSource({2}), "参照していないsourceを参照ありと判定しました");

    state.markPresented(acceptedA.value(), a);
    require(state.referencesSource({1}), "active compositionの参照を見落としました");

    // Bへ差し替えてacceptしただけでは、まだAを提示中である。
    // ここでAを解放可能と判定すると、提示中のsourceを削除できてしまう。
    const auto b = snapshot({layer(2)});
    const auto acceptedB = state.submit(b, sources, capabilities);
    require(acceptedB, "source Bのcompositionをacceptできません");
    require(state.referencesSource({1}),
            "acceptしただけでactive compositionの参照が外れたと判定しました");
    require(state.referencesSource({2}), "新しいpending compositionの参照を見落としました");

    // Bを実際に提示して初めてAの参照が外れる。
    state.markPresented(acceptedB.value(), b);
    require(!state.referencesSource({1}), "提示完了後もAの参照が残っていると判定しました");
    require(state.referencesSource({2}), "提示中のBを参照なしと判定しました");

    // opacity 0のlayerも参照である。描画に寄与しないことと、
    // sourceが解放可能であることは別問題である。
    CompositionAcceptanceState transparent;
    auto invisible = snapshot({layer(1)});
    const_cast<CompositionSnapshot&>(*invisible).layers[0].opacity = 0.0F;
    require(transparent.submit(invisible, sources, capabilities),
            "opacity 0のcompositionをacceptできません");
    require(transparent.referencesSource({1}), "opacity 0のlayerを参照から外しました");
}

// P5-E2: engineの`removeSource()`のうち、native deviceを必要としない検査。
void p5eRemoveSourceNegatives() {
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
    // ReadyPaused以外はstateで拒否する。
    requireFailure(engine.removeSource({1}), PreviewErrorCategory::InvalidState,
                   "ReadyPaused前のremoveSourceを受理しました");

    require(PreviewRenderPort::bindRenderThread(engine), "render thread bindに失敗しました");
    require(PreviewRenderPort::attachLogicalDevice(engine), "logical test seamに失敗しました");
    require(engine.status().state == PreviewEngineState::ReadyPaused,
            "logical attach後のstateが違います");
    // 未登録IDは`InvalidSource`。stateの都合で別のerrorへすり替えない。
    requireFailure(engine.removeSource({1}), PreviewErrorCategory::InvalidSource,
                   "未登録のPreviewSourceIdを受理しました");
    requireFailure(engine.removeSource({0}), PreviewErrorCategory::InvalidSource,
                   "0番のPreviewSourceIdを受理しました");

    std::optional<Result<void>> wrongThreadRemove;
    std::thread wrongThread([&] { wrongThreadRemove.emplace(engine.removeSource({1})); });
    wrongThread.join();
    require(wrongThreadRemove.has_value(), "wrong-thread remove resultがありません");
    requireFailure(*wrongThreadRemove, PreviewErrorCategory::InvalidState,
                   "control thread以外からのremoveSourceを受理しました");

    require(engine.requestShutdown(), "shutdown requestに失敗しました");
    requireFailure(engine.removeSource({1}), PreviewErrorCategory::InvalidState,
                   "ShuttingDown中のremoveSourceを受理しました");
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
    require(engine.initialize(measuredConfig(), dispatcher), "initializeに失敗しました");
}

// P5-D2: audio-master transportの公開contract。期待値はproduct helperを呼ばず
// 独立したliteralで与える。
void p5dAudioDomainAndCapabilities() {
    // 現在の構成の audio domain は 48000 Hz / stereo / float32 だけである。
    require(validateQualifiedAudioDomain(48000, 2, "flt"),
            "configured audio domainを受理しませんでした");
    requireFailure(validateQualifiedAudioDomain(44100, 2, "flt"),
                   PreviewErrorCategory::UnsupportedCapability,
                   "44100 Hzを暗黙のresampleで受理しました");
    requireFailure(validateQualifiedAudioDomain(96000, 2, "flt"),
                   PreviewErrorCategory::UnsupportedCapability,
                   "96000 Hzを暗黙のresampleで受理しました");
    requireFailure(validateQualifiedAudioDomain(48000, 1, "flt"),
                   PreviewErrorCategory::UnsupportedCapability,
                   "monoを暗黙のchannel変換で受理しました");
    requireFailure(validateQualifiedAudioDomain(48000, 6, "flt"),
                   PreviewErrorCategory::UnsupportedCapability,
                   "5.1chを暗黙のdownmixで受理しました");
    requireFailure(validateQualifiedAudioDomain(48000, 2, "s16"),
                   PreviewErrorCategory::UnsupportedCapability,
                   "s16を暗黙のformat変換で受理しました");
    requireFailure(validateQualifiedAudioDomain(0, 0, ""),
                   PreviewErrorCategory::UnsupportedCapability,
                   "未確定のaudio domainを受理しました");

    // audio統合後もcapabilityは実体を報告する。
    PreviewEngine engine;
    auto dispatcher = std::make_shared<ManualDispatcher>();
    require(engine.initialize({{{60, 1}}}, dispatcher), "initializeに失敗しました");
    const PreviewCapabilities capabilities = engine.capabilities();
    require(capabilities.configuredMaxActiveAudioSources == 1,
            "configured audio source数が1として公開されていません");
    require(capabilities.configuredAudioSampleRate == 48000,
            "configured audio sample rateが48000として公開されていません");
    require(capabilities.configuredAudioChannelCount == 2,
            "configured audio channel数が2として公開されていません");
    require(!capabilities.deviceRecoverySupported,
            "device recoveryをsupport済みとして公開しました");
    // P5-E3 capability確定。active source数とlayer数は独立したliteralで固定する。
    // active source数とlayer数は別capabilityとして検査する (contract §21)。
    require(capabilities.configuredMaxActiveVideoSources == 2,
            "configured active video source数が2として公開されていません");
    require(capabilities.configuredMaxCompositionLayers == 2,
            "configured composition layer数が2として公開されていません");
    require(!capabilities.duplicateSourceLayersSupported,
            "同一sourceの複数layer配置をsupport済みとして公開しました");
    require(capabilities.configuredOutputFrameRate.numerator == 60 &&
                capabilities.configuredOutputFrameRate.denominator == 1,
            "configured output frame rateが60/1として公開されていません");

    // descriptor validatorだけでなく、addSource()経路でも空descriptorを拒否する。
    // video/audioのどちらも無効なsourceにpublic IDを発行しない。
    requireFailure(engine.addSource({"movie.mp4", false, false}),
                   PreviewErrorCategory::InvalidSource,
                   "video/audioともに無効なsourceをaddSourceが受理しました");
    requireFailure(engine.addSource({"", false, false}), PreviewErrorCategory::InvalidSource,
                   "空pathかつvideo/audio無効のsourceをaddSourceが受理しました");
    requireFailure(engine.addSource({}), PreviewErrorCategory::InvalidSource,
                   "既定構築descriptorをaddSourceが受理しました");

    // render device attach前はaudio sourceもfail-closedで拒否する。
    requireFailure(engine.addSource({"movie.mp4", false, true}), PreviewErrorCategory::InvalidState,
                   "device attach前にaudio sourceを受理しました");
    // audio sourceを登録していないengineでは、audio seamも成立しない。
    requireFailure(PreviewRenderPort::injectAudioClockStallForTest(engine),
                   PreviewErrorCategory::InvalidState,
                   "audio未登録engineでclock stallを注入できてしまいました");
    requireFailure(PreviewRenderPort::injectVideoMasterQpcFallbackForTest(engine),
                   PreviewErrorCategory::InvalidState,
                   "audio未登録engineでQPC master退避を注入できてしまいました");

    const P5CRuntimeDiagnostics diagnostics = PreviewRenderPort::runtimeDiagnostics(engine);
    require(!diagnostics.audioMasterActive, "audio未登録なのにaudio masterがactiveです");
    require(diagnostics.registeredAudioSourceCount == 0,
            "audio未登録なのにaudio source数が0ではありません");
    require(diagnostics.audioMasterProjectionFailureCount == 0,
            "初期状態でaudio master projection失敗が記録されています");
    require(diagnostics.videoMasterQpcFallbackCount == 0,
            "初期状態でQPC master退避が記録されています");
    require(diagnostics.audioUnderflowCount == 0, "初期状態でunderflowが記録されています");

    require(engine.requestShutdown(), "shutdownに失敗しました");
    dispatcher->runAll();
    require(engine.status().state == PreviewEngineState::Shutdown,
            "terminal Shutdownへ到達しませんでした");
    // teardown後もaudio診断はfail-closedのまま (joinを未確認にしない)。
    const P5CRuntimeDiagnostics terminal = PreviewRenderPort::runtimeDiagnostics(engine);
    require(terminal.audioSinkJoined && terminal.audioWorkerJoined,
            "audio未登録のteardownでjoin未確認を報告しました");
    require(terminal.audioMasterProjectionFailureCount == 0,
            "shutdownまでにaudio master projection失敗が発生しました");
}

} // namespace

int main(int argc, char** argv) {
    // crashしたtestを特定できるよう、PASS行をbufferに溜めない。
    std::cout << std::unitbuf;
    if (argc == 2 && std::string(argv[1]) == "--unsafe-destructor") {
        unsafeDestructionProcess();
        return 0;
    }

    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"measured envelope derived", measuredEnvelopeIsDerived},
        {"frame rate / descriptor", frameRateAndDescriptorValidation},
        {"Result / error", resultAndErrorValues},
        {"state machine", stateMachineLifecycle},
        {"event mailbox", mailboxOrderingAndBounds},
        {"composition domain", compositionDomains},
        {"composition identity", compositionIdentityAndCapabilities},
        {"composition structural equality literals", compositionStructuralEqualityLiterals},
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
        {"P5-D audio domain / capabilities", p5dAudioDomainAndCapabilities},
        {"P5-E composition source references", p5eCompositionSourceReferences},
        {"P5-E removeSource negatives", p5eRemoveSourceNegatives},
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
