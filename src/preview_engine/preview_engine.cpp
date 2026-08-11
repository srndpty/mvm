#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/source_decode_worker.h"
#include "media/gpu_preview/source_registry.h"
#include "preview_engine/preview_engine_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace mvm::preview {
namespace {

PreviewError makeError(PreviewErrorCategory category, PreviewOperation operation,
                       std::string detail,
                       PreviewErrorSeverity severity = PreviewErrorSeverity::Recoverable) {
    PreviewError error;
    error.category = category;
    error.severity = severity;
    error.operation = operation;
    error.detail = std::move(detail);
    return error;
}

Result<void> invalidState(PreviewOperation operation, std::string detail) {
    return Result<void>::failure(
        makeError(PreviewErrorCategory::InvalidState, operation, std::move(detail)));
}

bool isActiveState(PreviewEngineState state) {
    return state == PreviewEngineState::WaitingForRenderDevice ||
           state == PreviewEngineState::ReadyPaused || state == PreviewEngineState::Playing ||
           state == PreviewEngineState::Seeking;
}

float canonicalFloat(float value) {
    return value == 0.0F ? 0.0F : value;
}

bool validRect(const PreviewNormalizedRect& rect) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height)) {
        return false;
    }
    if (rect.x < 0.0F || rect.x >= 1.0F || rect.y < 0.0F || rect.y >= 1.0F || rect.width <= 0.0F ||
        rect.width > 1.0F || rect.height <= 0.0F || rect.height > 1.0F) {
        return false;
    }
    return rect.width <= 1.0F - rect.x && rect.height <= 1.0F - rect.y;
}

PreviewNormalizedRect canonicalRect(PreviewNormalizedRect rect) {
    rect.x = canonicalFloat(rect.x);
    rect.y = canonicalFloat(rect.y);
    rect.width = canonicalFloat(rect.width);
    rect.height = canonicalFloat(rect.height);
    return rect;
}

PreviewError compositionError(PreviewErrorCategory category, std::string detail,
                              std::optional<PreviewSourceId> source = std::nullopt) {
    PreviewError error =
        makeError(category, PreviewOperation::SubmitComposition, std::move(detail));
    error.source = source;
    return error;
}

} // namespace

Result<PreviewFrameRate> validatePreviewFrameRate(std::uint64_t numerator,
                                                  std::uint64_t denominator) {
    if (numerator == 0 || denominator == 0) {
        return Result<PreviewFrameRate>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Initialize,
                      "フレームレートの分子と分母は0より大きくなければなりません"));
    }

    const std::uint64_t divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > std::numeric_limits<std::uint32_t>::max() ||
        denominator > std::numeric_limits<std::uint32_t>::max()) {
        return Result<PreviewFrameRate>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Initialize,
                      "フレームレートを公開rationalへ安全に格納できません"));
    }

    return Result<PreviewFrameRate>::success(
        {static_cast<std::uint32_t>(numerator), static_cast<std::uint32_t>(denominator)});
}

Result<void> validatePreviewSourceDescriptor(const PreviewSourceDescriptor& descriptor) {
    if (descriptor.mediaPath.empty()) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource, "mediaPathが空です"));
    }
    if (!descriptor.videoEnabled && !descriptor.audioEnabled) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource,
                                               "videoまたはaudioを有効にしてください"));
    }
    return Result<void>::success();
}

namespace internal {

EventMailbox::EventMailbox(std::size_t capacity) : capacity_(capacity) {}

bool EventMailbox::isTerminal(const PreviewEvent& event) const {
    const auto* state = std::get_if<StateChangedEvent>(&event);
    return state != nullptr && (state->state == PreviewEngineState::Shutdown ||
                                state->state == PreviewEngineState::Error);
}

bool EventMailbox::tryCoalesce(const PreviewEvent& event) {
    if (std::holds_alternative<PositionChangedEvent>(event)) {
        const auto found =
            std::find_if(events_.begin(), events_.end(), [](const PreviewEvent& item) {
                return std::holds_alternative<PositionChangedEvent>(item);
            });
        if (found != events_.end()) {
            *found = event;
            return true;
        }
    }
    if (std::holds_alternative<FramePresentedEvent>(event)) {
        const auto found =
            std::find_if(events_.begin(), events_.end(), [](const PreviewEvent& item) {
                return std::holds_alternative<FramePresentedEvent>(item);
            });
        if (found != events_.end()) {
            *found = event;
            return true;
        }
    }
    return false;
}

Result<void> EventMailbox::push(PreviewEvent event) {
    if (capacity_ == 0) {
        return Result<void>::failure(makeError(PreviewErrorCategory::ShutdownFailure,
                                               PreviewOperation::Shutdown,
                                               "event mailboxの容量が0です"));
    }
    if (tryCoalesce(event)) {
        return Result<void>::success();
    }

    const std::size_t normalCapacity = capacity_ - 1;
    const std::size_t limit = isTerminal(event) ? capacity_ : normalCapacity;
    if (events_.size() >= limit) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::ShutdownFailure, PreviewOperation::Shutdown,
                      "event mailboxの非coalescible event容量を超えました"));
    }
    events_.push_back(std::move(event));
    return Result<void>::success();
}

std::optional<PreviewEvent> EventMailbox::pop() {
    if (events_.empty()) {
        return std::nullopt;
    }
    PreviewEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::size_t EventMailbox::size() const {
    return events_.size();
}

std::size_t EventMailbox::capacity() const {
    return capacity_;
}

bool EventMailbox::empty() const {
    return events_.empty();
}

PreviewEngineState PreviewStateMachine::state() const {
    return state_;
}

std::optional<PreviewError> PreviewStateMachine::lastError() const {
    return lastError_;
}

bool PreviewStateMachine::destructionSafe() const {
    return state_ == PreviewEngineState::Uninitialized || state_ == PreviewEngineState::Shutdown ||
           state_ == PreviewEngineState::Error;
}

Result<void> PreviewStateMachine::initialize() {
    if (state_ != PreviewEngineState::Uninitialized) {
        return invalidState(PreviewOperation::Initialize,
                            "initializeはUninitializedでのみ実行できます");
    }
    state_ = PreviewEngineState::WaitingForRenderDevice;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::attachRenderDevice() {
    if (state_ != PreviewEngineState::WaitingForRenderDevice) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "render device attachはWaitingForRenderDeviceでのみ実行できます");
    }
    state_ = PreviewEngineState::ReadyPaused;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::play() {
    if (state_ != PreviewEngineState::ReadyPaused) {
        return invalidState(PreviewOperation::Play, "playはReadyPausedでのみ実行できます");
    }
    state_ = PreviewEngineState::Playing;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::pause() {
    if (state_ != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::Pause, "pauseはPlayingでのみ実行できます");
    }
    state_ = PreviewEngineState::ReadyPaused;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::seek() {
    if (state_ != PreviewEngineState::ReadyPaused && state_ != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::Seek,
                            "seekはReadyPausedまたはPlayingでのみ実行できます");
    }
    stateBeforeSeek_ = state_;
    state_ = PreviewEngineState::Seeking;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::completeSeek() {
    if (state_ != PreviewEngineState::Seeking) {
        return invalidState(PreviewOperation::Seek, "seek completionを受理できないstateです");
    }
    state_ = stateBeforeSeek_;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::requestShutdown() {
    if (state_ == PreviewEngineState::ShuttingDown || state_ == PreviewEngineState::Shutdown ||
        state_ == PreviewEngineState::Error) {
        return Result<void>::success();
    }
    if (!isActiveState(state_)) {
        return invalidState(PreviewOperation::Shutdown,
                            "初期化前のengineへshutdownは要求できません");
    }
    state_ = PreviewEngineState::ShuttingDown;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::recordFatal(PreviewError error) {
    if (!isActiveState(state_) && state_ != PreviewEngineState::ShuttingDown) {
        return invalidState(error.operation, "fatal errorを受理できないstateです");
    }
    error.severity = PreviewErrorSeverity::FatalToSession;
    // teardown中の二次障害でroot causeを上書きしない。履歴は保持せず最初の一件だけを残す。
    if (!lastError_) {
        lastError_ = std::move(error);
    }
    fatalPending_ = true;
    state_ = PreviewEngineState::ShuttingDown;
    return Result<void>::success();
}

Result<void> PreviewStateMachine::completeTeardown() {
    if (state_ != PreviewEngineState::ShuttingDown) {
        return invalidState(PreviewOperation::Shutdown,
                            "teardown completionはShuttingDownでのみ受理できます");
    }
    state_ = fatalPending_ ? PreviewEngineState::Error : PreviewEngineState::Shutdown;
    return Result<void>::success();
}

Result<AcceptedComposition>
CompositionAcceptanceState::submit(const std::shared_ptr<const CompositionSnapshot>& snapshot,
                                   const std::unordered_map<std::uint64_t, EligibleSource>& sources,
                                   const PreviewCapabilities& capabilities) {
    if (!snapshot) {
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::CompositionFailure, "snapshotがnullです"));
    }
    if (snapshot->layers.empty()) {
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::CompositionFailure, "snapshotがemptyです"));
    }
    if (snapshot->layers.size() > capabilities.maxQualifiedCompositionLayers) {
        return Result<AcceptedComposition>::failure(compositionError(
            PreviewErrorCategory::UnsupportedCapability, "qualified layer countを超えています"));
    }

    std::set<std::uint64_t> distinctSources;
    for (const PreviewCompositionLayer& layer : snapshot->layers) {
        const auto source = sources.find(layer.source.value);
        if (source == sources.end() || !source->second.videoEnabled) {
            return Result<AcceptedComposition>::failure(
                compositionError(PreviewErrorCategory::InvalidSource,
                                 "video-enabledでないsourceが参照されています", layer.source));
        }
        if (!distinctSources.insert(layer.source.value).second &&
            !capabilities.duplicateSourceLayersSupported) {
            return Result<AcceptedComposition>::failure(compositionError(
                PreviewErrorCategory::UnsupportedCapability,
                "同一sourceのduplicate layerはqualified capability外です", layer.source));
        }
    }
    if (distinctSources.size() > capabilities.maxQualifiedActiveVideoSources) {
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::UnsupportedCapability,
                             "qualified active video source countを超えています"));
    }

    CompositionSnapshot canonical = *snapshot;
    for (PreviewCompositionLayer& layer : canonical.layers) {
        if (!validRect(layer.destination) || !validRect(layer.sourceRect)) {
            return Result<AcceptedComposition>::failure(
                compositionError(PreviewErrorCategory::CompositionFailure,
                                 "normalized rectangleがinvalidです", layer.source));
        }
        if (!std::isfinite(layer.opacity) || layer.opacity < 0.0F || layer.opacity > 1.0F) {
            return Result<AcceptedComposition>::failure(
                compositionError(PreviewErrorCategory::CompositionFailure,
                                 "opacityが[0,1]の範囲外です", layer.source));
        }
        layer.destination = canonicalRect(layer.destination);
        layer.sourceRect = canonicalRect(layer.sourceRect);
        layer.opacity = canonicalFloat(layer.opacity);
    }

    if (latestAcceptedSnapshot_ && *latestAcceptedSnapshot_ == canonical) {
        return Result<AcceptedComposition>::success(latestAcceptedToken_.value());
    }
    if (nextId_ == 0 || nextRevision_ == 0) {
        return Result<AcceptedComposition>::failure(compositionError(
            PreviewErrorCategory::CompositionFailure, "composition tokenがoverflowしました"));
    }

    const AcceptedComposition accepted{{nextId_}, nextRevision_};
    ++nextId_;
    ++nextRevision_;
    latestAcceptedSnapshot_ = std::make_shared<const CompositionSnapshot>(std::move(canonical));
    latestAcceptedToken_ = accepted;
    return Result<AcceptedComposition>::success(accepted);
}

void CompositionAcceptanceState::markPresented(AcceptedComposition composition) {
    lastPresentedToken_ = composition;
}

std::optional<AcceptedComposition> CompositionAcceptanceState::latestAcceptedToken() const {
    return latestAcceptedToken_;
}

std::optional<AcceptedComposition> CompositionAcceptanceState::lastPresentedToken() const {
    return lastPresentedToken_;
}

const std::shared_ptr<const CompositionSnapshot>&
CompositionAcceptanceState::latestAcceptedSnapshot() const {
    return latestAcceptedSnapshot_;
}

} // namespace internal

struct PreviewEngine::Impl : std::enable_shared_from_this<PreviewEngine::Impl> {
    Impl() : controlThread(std::this_thread::get_id()) {}

    ~Impl() {
        if (!shutdownThread.joinable())
            return;
        if (shutdownThread.get_id() == std::this_thread::get_id())
            shutdownThread.detach();
        else
            shutdownThread.join();
    }

    mutable std::mutex mutex;
    internal::PreviewStateMachine machine;
    internal::EventMailbox mailbox{32};
    std::shared_ptr<PreviewEventDispatcher> dispatcher;
    std::weak_ptr<PreviewEventSink> sink;
    const std::thread::id controlThread;
    std::uint64_t sinkGeneration = 0;
    bool dispatchScheduled = false;
    PreviewCapabilities capability;
    PreviewTelemetry telemetrySnapshot;
    PreviewDeviceInfo deviceSnapshot;

    // P5-C private backend。public headerへmedia/native型を漏らさない。
    gpu::SharedD3D11Device renderDevice;
    gpu::SourceRegistry sourceRegistry;
    gpu::ReadbackCounters readbacks;
    std::unique_ptr<gpu::SourceDecodeWorker> videoWorker;
    std::unique_ptr<gpu::GpuCompositor> compositor;
    std::unordered_map<std::uint64_t, internal::EligibleSource> eligibleSources;
    internal::CompositionAcceptanceState compositionState;
    std::optional<PreviewSourceId> publicVideoSource;
    gpu::SourceId internalVideoSource{};
    std::uint64_t nextPublicSourceId = 1;
    PreviewFrameRate configuredFrameRate{60, 1};

    std::optional<std::thread::id> renderThread;
    void* nativeDeviceIdentity = nullptr;
    void* nativeContextIdentity = nullptr;
    bool nativeDeviceAttached = false;
    bool schedulerEnabled = false;
    std::chrono::steady_clock::time_point schedulerStart;
    std::int64_t schedulerBaseFrame = 0;
    std::int64_t lastSchedulerTarget = -1;
    std::uint64_t presentationSequence = 0;
    std::unordered_set<std::int64_t> distinctPresentedFrames;
    std::thread shutdownThread;
    bool shutdownWorkerStarted = false;
    bool workerJoined = true;
    bool renderTeardownRequested = false;
    bool gpuDrainStarted = false;
    bool renderTeardownComplete = false;
    bool deviceReleased = true;
    bool forceGpuDrainFailure = false;
    std::uint64_t lifecycleViolationCount = 0;
    std::uint64_t staleSubstitutionCount = 0;
    internal::P5CRuntimeDiagnostics finalRuntimeDiagnostics;

    Result<void> requireControlThread(PreviewOperation operation) const {
        if (controlThread != std::this_thread::get_id()) {
            return invalidState(operation, "control methodが作成時のthread以外から呼ばれました");
        }
        return Result<void>::success();
    }

    std::int64_t schedulerTarget(std::chrono::steady_clock::time_point now) const {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - schedulerStart).count();
        if (elapsed <= 0)
            return schedulerBaseFrame;
        constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
        const auto elapsedValue = static_cast<std::uint64_t>(elapsed);
        const std::uint64_t seconds = elapsedValue / kNanosecondsPerSecond;
        const std::uint64_t nanoseconds = elapsedValue % kNanosecondsPerSecond;
        const std::uint64_t numerator = configuredFrameRate.numerator;
        const std::uint64_t denominator = configuredFrameRate.denominator;
        const std::uint64_t wholeRate = numerator / denominator;
        const std::uint64_t rateRemainder = numerator % denominator;
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (wholeRate != 0 && seconds > maximum / wholeRate)
            return std::numeric_limits<std::int64_t>::max();
        std::uint64_t advanced = seconds * wholeRate;
        const std::uint64_t secondsQuotient = seconds / denominator;
        const std::uint64_t secondsRemainder = seconds % denominator;
        const std::uint64_t secondary =
            secondsQuotient * rateRemainder + (secondsRemainder * rateRemainder) / denominator;
        const std::uint64_t fractionalRemainder = (secondsRemainder * rateRemainder) % denominator;
        const std::uint64_t fractional =
            (fractionalRemainder * kNanosecondsPerSecond + nanoseconds * numerator) /
            (denominator * kNanosecondsPerSecond);
        if (advanced > maximum - secondary || advanced + secondary > maximum - fractional)
            return std::numeric_limits<std::int64_t>::max();
        advanced += secondary + fractional;
        const auto value = static_cast<std::int64_t>(advanced);
        if (schedulerBaseFrame > std::numeric_limits<std::int64_t>::max() - value)
            return std::numeric_limits<std::int64_t>::max();
        return schedulerBaseFrame + value;
    }

    void startWorkerShutdown() {
        if (shutdownWorkerStarted)
            return;
        shutdownWorkerStarted = true;
        workerJoined = videoWorker == nullptr || videoWorker->joined();
        renderTeardownRequested = workerJoined;
        if (workerJoined)
            return;
        const std::shared_ptr<Impl> self = shared_from_this();
        shutdownThread = std::thread([self] {
            gpu::SourceDecodeWorker* worker = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                worker = self->videoWorker.get();
            }
            if (worker)
                worker->stop();
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->workerJoined = worker == nullptr || worker->joined();
                self->renderTeardownRequested = self->workerJoined;
                if (!self->workerJoined)
                    ++self->lifecycleViolationCount;
            }
        });
    }

    void noteEventDeliveryFailureLocked() {
        if (telemetrySnapshot.eventDeliveryFailureCount <
            std::numeric_limits<std::uint64_t>::max()) {
            ++telemetrySnapshot.eventDeliveryFailureCount;
        }
    }

    void noteEventDeliveryFailure() {
        std::lock_guard<std::mutex> lock(mutex);
        noteEventDeliveryFailureLocked();
    }

    void deliver(const internal::PreviewEvent& event,
                 const std::shared_ptr<PreviewEventSink>& target) {
        std::visit(
            [&target](const auto& value) {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, internal::StateChangedEvent>) {
                    target->stateChanged(value.state);
                } else if constexpr (std::is_same_v<Event, internal::PositionChangedEvent>) {
                    target->positionChanged(value.position);
                } else if constexpr (std::is_same_v<Event, internal::FramePresentedEvent>) {
                    target->framePresented(value.frame);
                } else if constexpr (std::is_same_v<Event, internal::ErrorOccurredEvent>) {
                    target->errorOccurred(value.error);
                } else if constexpr (std::is_same_v<Event, internal::DeviceChangedEvent>) {
                    target->deviceChanged(value.device);
                }
            },
            event);
    }

    void dispatchOne() {
        std::optional<internal::PreviewEvent> event;
        std::shared_ptr<PreviewEventSink> target;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            event = mailbox.pop();
            generation = sinkGeneration;
            target = sink.lock();
            if (!event) {
                dispatchScheduled = false;
                return;
            }
        }

        if (target) {
            bool stillAttached = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const std::shared_ptr<PreviewEventSink> current = sink.lock();
                stillAttached = sinkGeneration == generation && current == target;
            }
            if (stillAttached) {
                try {
                    deliver(*event, target);
                } catch (...) {
                    noteEventDeliveryFailure();
                }
            }
        }

        std::shared_ptr<PreviewEventDispatcher> nextDispatcher;
        bool terminal = false;
        if (const auto* state = std::get_if<internal::StateChangedEvent>(&*event)) {
            terminal = state->state == PreviewEngineState::Shutdown ||
                       state->state == PreviewEngineState::Error;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (terminal) {
                dispatchScheduled = false;
                dispatcher.reset();
            } else if (mailbox.empty() || !dispatcher) {
                dispatchScheduled = false;
            } else {
                nextDispatcher = dispatcher;
            }
        }
        if (nextDispatcher) {
            const std::weak_ptr<Impl> weak = shared_from_this();
            bool posted = false;
            try {
                posted = nextDispatcher->post([weak] {
                    if (const std::shared_ptr<Impl> self = weak.lock()) {
                        self->dispatchOne();
                    }
                });
            } catch (...) {
                posted = false;
            }
            if (!posted) {
                std::lock_guard<std::mutex> lock(mutex);
                dispatchScheduled = false;
                noteEventDeliveryFailureLocked();
            }
        }
    }

    Result<void> enqueue(internal::PreviewEvent event) {
        std::shared_ptr<PreviewEventDispatcher> targetDispatcher;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Result<void> pushed = mailbox.push(std::move(event));
            if (!pushed) {
                return pushed;
            }
            if (!dispatchScheduled && dispatcher) {
                dispatchScheduled = true;
                targetDispatcher = dispatcher;
            }
        }
        if (targetDispatcher) {
            const std::weak_ptr<Impl> weak = shared_from_this();
            bool posted = false;
            try {
                posted = targetDispatcher->post([weak] {
                    if (const std::shared_ptr<Impl> self = weak.lock()) {
                        self->dispatchOne();
                    }
                });
            } catch (...) {
                posted = false;
            }
            if (!posted) {
                std::lock_guard<std::mutex> lock(mutex);
                dispatchScheduled = false;
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::ShutdownFailure, PreviewOperation::Shutdown,
                              "dispatcherがevent taskを受理しませんでした"));
            }
        }
        return Result<void>::success();
    }

    void notify(internal::PreviewEvent event) {
        Result<void> notified = enqueue(std::move(event));
        if (!notified) {
            noteEventDeliveryFailure();
        }
    }
};

PreviewEngine::PreviewEngine() : impl_(std::make_shared<Impl>()) {}

PreviewEngine::~PreviewEngine() {
    bool safe = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        safe = impl_->machine.destructionSafe();
    }
    if (!safe) {
        std::terminate();
    }
}

Result<void> PreviewEngine::initialize(const PreviewEngineConfig& config,
                                       std::shared_ptr<PreviewEventDispatcher> dispatcher) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Initialize);
        if (!affinity) {
            return affinity;
        }
    }
    if (!dispatcher) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidState,
                                               PreviewOperation::Initialize,
                                               "dispatcherがnullです"));
    }
    Result<PreviewFrameRate> rate = validatePreviewFrameRate(config.output.frameRate.numerator,
                                                             config.output.frameRate.denominator);
    if (!rate) {
        return Result<void>::failure(rate.error());
    }
    if (!(rate.value() == impl_->capability.qualifiedOutputFrameRate)) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Initialize,
                      "指定frame rateは現在qualifiedな60/1ではありません"));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> initialized = impl_->machine.initialize();
        if (!initialized) {
            return initialized;
        }
        impl_->dispatcher = std::move(dispatcher);
        impl_->configuredFrameRate = rate.value();
        impl_->telemetrySnapshot.status.state = impl_->machine.state();
    }
    Result<void> posted =
        impl_->enqueue(internal::StateChangedEvent{PreviewEngineState::WaitingForRenderDevice});
    if (!posted) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->machine = internal::PreviewStateMachine{};
        impl_->mailbox = internal::EventMailbox{32};
        impl_->dispatcher.reset();
        impl_->sink.reset();
        ++impl_->sinkGeneration;
        impl_->dispatchScheduled = false;
        impl_->telemetrySnapshot = PreviewTelemetry{};
        return posted;
    }
    return Result<void>::success();
}

Result<void> PreviewEngine::attachEventSink(std::weak_ptr<PreviewEventSink> sink) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::AttachEventSink);
    if (!affinity) {
        return affinity;
    }
    const PreviewEngineState state = impl_->machine.state();
    if (!isActiveState(state)) {
        return invalidState(PreviewOperation::AttachEventSink,
                            "event sinkをattachできないstateです");
    }
    ++impl_->sinkGeneration;
    impl_->sink = std::move(sink);
    return Result<void>::success();
}

Result<void> PreviewEngine::detachEventSink() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::DetachEventSink);
    if (!affinity) {
        return affinity;
    }
    if (impl_->machine.state() == PreviewEngineState::Uninitialized) {
        return invalidState(PreviewOperation::DetachEventSink,
                            "初期化前のengineからsinkはdetachできません");
    }
    ++impl_->sinkGeneration;
    impl_->sink.reset();
    return Result<void>::success();
}

Result<PreviewSourceId> PreviewEngine::addSource(const PreviewSourceDescriptor& descriptor) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::AddSource);
    if (!affinity) {
        return Result<PreviewSourceId>::failure(affinity.error());
    }
    Result<void> valid = validatePreviewSourceDescriptor(descriptor);
    if (!valid) {
        return Result<PreviewSourceId>::failure(valid.error());
    }
    if (impl_->machine.state() != PreviewEngineState::ReadyPaused) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "source registrationはReadyPausedでのみ受理します"));
    }
    if (descriptor.audioEnabled) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "P5-CではaudioEnabled sourceをsupportしません"));
    }
    if (!descriptor.videoEnabled) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "P5-CではvideoEnabled sourceだけをsupportします"));
    }
    if (!impl_->nativeDeviceAttached || !impl_->compositor || !impl_->renderDevice.valid()) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "native render deviceの準備前にsourceを登録できません"));
    }
    if (impl_->publicVideoSource) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "P5-C product wiringはvideo sourceを1件だけ受理します"));
    }

    const gpu::SourceId internal = impl_->sourceRegistry.registerSource();
    auto worker = std::make_unique<gpu::SourceDecodeWorker>(internal, impl_->renderDevice,
                                                            impl_->readbacks, 6);
    std::string openError;
    const auto utf8Path = descriptor.mediaPath.u8string();
    const std::string path(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
    if (!worker->start(path, openError)) {
        worker->stop();
        impl_->sourceRegistry.unregisterSource(internal);
        ++impl_->telemetrySnapshot.decodeFailureCount;
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::AddSource,
                      "D3D11VA video sourceをopenできません: " + openError));
    }

    if (impl_->nextPublicSourceId == 0) {
        worker->stop();
        impl_->sourceRegistry.unregisterSource(internal);
        return Result<PreviewSourceId>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                                          PreviewOperation::AddSource,
                                                          "PreviewSourceIdがoverflowしました"));
    }
    const PreviewSourceId published{impl_->nextPublicSourceId++};
    impl_->internalVideoSource = internal;
    impl_->publicVideoSource = published;
    impl_->eligibleSources.emplace(published.value, internal::EligibleSource{true});
    impl_->videoWorker = std::move(worker);
    impl_->workerJoined = false;
    impl_->deviceReleased = false;
    impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
    return Result<PreviewSourceId>::success(published);
}

Result<void> PreviewEngine::removeSource(PreviewSourceId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::RemoveSource);
    if (!affinity) {
        return affinity;
    }
    if (impl_->machine.state() != PreviewEngineState::ReadyPaused) {
        return invalidState(PreviewOperation::RemoveSource,
                            "source removalはReadyPausedでのみ受理します");
    }
    return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                           PreviewOperation::RemoveSource,
                                           "P5-Bではsource tableを接続していません"));
}

Result<AcceptedComposition>
PreviewEngine::submitComposition(std::shared_ptr<const CompositionSnapshot> snapshot) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::SubmitComposition);
    if (!affinity) {
        return Result<AcceptedComposition>::failure(affinity.error());
    }
    const PreviewEngineState state = impl_->machine.state();
    if (state != PreviewEngineState::ReadyPaused && state != PreviewEngineState::Playing) {
        return Result<AcceptedComposition>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::SubmitComposition,
                      "composition submissionを受理できないstateです"));
    }
    if (snapshot && snapshot->layers.size() > 1) {
        return Result<AcceptedComposition>::failure(makeError(
            PreviewErrorCategory::UnsupportedCapability, PreviewOperation::SubmitComposition,
            "P5-C product wiringはexactly 1 layerだけを受理します"));
    }
    Result<AcceptedComposition> accepted =
        impl_->compositionState.submit(snapshot, impl_->eligibleSources, impl_->capability);
    if (!accepted)
        return accepted;
    impl_->telemetrySnapshot.status.latestAcceptedDesiredComposition = accepted.value();
    return accepted;
}

Result<void> PreviewEngine::play() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Play);
        if (!affinity)
            return affinity;
        if (impl_->machine.state() != PreviewEngineState::ReadyPaused)
            return invalidState(PreviewOperation::Play, "playを受理できないstateです");
        if (!impl_->videoWorker || !impl_->publicVideoSource ||
            !impl_->compositionState.latestAcceptedToken()) {
            return invalidState(PreviewOperation::Play,
                                "playには登録済みvideo sourceとaccepted compositionが必要です");
        }
        Result<void> played = impl_->machine.play();
        if (!played)
            return played;
        impl_->schedulerEnabled = true;
        impl_->schedulerStart = std::chrono::steady_clock::now();
        impl_->schedulerBaseFrame = impl_->telemetrySnapshot.presentedFrameCount == 0
                                        ? 0
                                        : impl_->telemetrySnapshot.status.position.outputFrame + 1;
        impl_->lastSchedulerTarget = impl_->schedulerBaseFrame - 1;
        impl_->videoWorker->play();
        impl_->telemetrySnapshot.status.state = PreviewEngineState::Playing;
    }
    impl_->notify(internal::StateChangedEvent{PreviewEngineState::Playing});
    return Result<void>::success();
}

Result<void> PreviewEngine::pause() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Pause);
        if (!affinity)
            return affinity;
        if (impl_->machine.state() != PreviewEngineState::Playing)
            return invalidState(PreviewOperation::Pause, "pauseを受理できないstateです");
        Result<void> paused = impl_->machine.pause();
        if (!paused)
            return paused;
        impl_->schedulerEnabled = false;
        if (impl_->videoWorker)
            impl_->videoWorker->pause();
        impl_->telemetrySnapshot.status.state = PreviewEngineState::ReadyPaused;
    }
    impl_->notify(internal::StateChangedEvent{PreviewEngineState::ReadyPaused});
    return Result<void>::success();
}

Result<void> PreviewEngine::seek(PreviewPosition) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::Seek);
    if (!affinity) {
        return affinity;
    }
    const PreviewEngineState state = impl_->machine.state();
    if (state != PreviewEngineState::ReadyPaused && state != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::Seek, "seekを受理できないstateです");
    }
    return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                           PreviewOperation::Seek,
                                           "P5-Bではtransportを接続していません"));
}

PreviewStatus PreviewEngine::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    PreviewStatus result = impl_->telemetrySnapshot.status;
    result.state = impl_->machine.state();
    result.lastError = impl_->machine.lastError();
    return result;
}

PreviewCapabilities PreviewEngine::capabilities() const {
    return impl_->capability;
}

PreviewTelemetry PreviewEngine::telemetry() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    PreviewTelemetry result = impl_->telemetrySnapshot;
    result.status.state = impl_->machine.state();
    result.status.lastError = impl_->machine.lastError();
    return result;
}

PreviewDeviceInfo PreviewEngine::deviceInfo() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->deviceSnapshot;
}

Result<void> PreviewEngine::requestShutdown() {
    PreviewEngineState before;
    PreviewEngineState after;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Shutdown);
        if (!affinity) {
            return affinity;
        }
        before = impl_->machine.state();
        Result<void> requested = impl_->machine.requestShutdown();
        if (!requested) {
            return requested;
        }
        after = impl_->machine.state();
        impl_->schedulerEnabled = false;
        if (impl_->videoWorker)
            impl_->videoWorker->pause();
        if (impl_->nativeDeviceAttached)
            impl_->startWorkerShutdown();
        impl_->telemetrySnapshot.status.state = after;
    }
    if (before != after) {
        impl_->notify(internal::StateChangedEvent{after});
    }
    return Result<void>::success();
}

namespace internal {

Result<void> PreviewRenderPort::bindRenderThread(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (engine.impl_->machine.state() != PreviewEngineState::WaitingForRenderDevice) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "render threadはdevice attach待ちでのみ登録できます");
    }
    if (engine.impl_->renderThread) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "render threadは既に登録されています");
    }
    engine.impl_->renderThread = std::this_thread::get_id();
    return Result<void>::success();
}

Result<void> PreviewRenderPort::attachNativeD3D11Device(PreviewEngine& engine, void* device,
                                                        void* context) {
    PreviewDeviceInfo info;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native device attachは登録済みrender threadで実行してください");
        }
        if (!device || !context) {
            return Result<void>::failure(makeError(PreviewErrorCategory::DeviceFailure,
                                                   PreviewOperation::RenderDeviceAttach,
                                                   "D3D11 device/contextがnullです"));
        }
        if (engine.impl_->nativeDeviceAttached) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native render deviceは既にattachされています");
        }

        auto* nativeDevice = static_cast<ID3D11Device*>(device);
        auto* nativeContext = static_cast<ID3D11DeviceContext*>(context);
        ID3D11Device* contextDevice = nullptr;
        nativeContext->GetDevice(&contextDevice);
        const bool sameDevice = contextDevice == nativeDevice;
        if (contextDevice)
            contextDevice->Release();
        if (!sameDevice) {
            return Result<void>::failure(makeError(PreviewErrorCategory::DeviceFailure,
                                                   PreviewOperation::RenderDeviceAttach,
                                                   "contextとdeviceの実体が一致しません"));
        }

        std::string error;
        if (!engine.impl_->renderDevice.adopt(nativeDevice, nativeContext, error)) {
            return Result<void>::failure(makeError(PreviewErrorCategory::DeviceFailure,
                                                   PreviewOperation::RenderDeviceAttach,
                                                   "共有D3D11 deviceをadoptできません: " + error));
        }
        auto compositor = std::make_unique<gpu::GpuCompositor>();
        if (!compositor->initializeExternal(engine.impl_->renderDevice, engine.impl_->readbacks,
                                            error)) {
            engine.impl_->renderDevice.release();
            return Result<void>::failure(
                makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                          "product compositorを初期化できません: " + error));
        }
        Result<void> attached = engine.impl_->machine.attachRenderDevice();
        if (!attached) {
            std::string ignored;
            compositor->shutdown(2000, ignored);
            engine.impl_->renderDevice.release();
            return attached;
        }

        engine.impl_->compositor = std::move(compositor);
        engine.impl_->nativeDeviceIdentity = device;
        engine.impl_->nativeContextIdentity = context;
        engine.impl_->nativeDeviceAttached = true;
        engine.impl_->deviceReleased = false;
        const gpu::AdapterInfo& adapter = engine.impl_->renderDevice.adapter();
        info.adapterDescription = adapter.description;
        info.adapterLuidLow = adapter.luidLow;
        info.adapterLuidHigh = adapter.luidHigh;
        engine.impl_->deviceSnapshot = info;
        engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ReadyPaused;
    }
    engine.impl_->notify(DeviceChangedEvent{info});
    engine.impl_->notify(StateChangedEvent{PreviewEngineState::ReadyPaused});
    return Result<void>::success();
}

Result<RenderFrameResult> PreviewRenderPort::renderFrame(PreviewEngine& engine,
                                                         void* renderTargetView, int width,
                                                         int height) {
    RenderFrameResult result;
    std::optional<PreviewError> fatal;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return Result<RenderFrameResult>::failure(
                invalidState(PreviewOperation::RenderDeviceAttach,
                             "renderは登録済みrender threadで実行してください")
                    .error());
        }
        if (engine.impl_->machine.state() != PreviewEngineState::Playing ||
            !engine.impl_->schedulerEnabled) {
            return Result<RenderFrameResult>::success(result);
        }
        if (!renderTargetView || width <= 0 || height <= 0 || !engine.impl_->videoWorker ||
            !engine.impl_->compositor) {
            return Result<RenderFrameResult>::failure(
                makeError(PreviewErrorCategory::InvalidState, PreviewOperation::RenderDeviceAttach,
                          "render targetまたはproduct runtimeが未準備です"));
        }

        const std::int64_t target = engine.impl_->schedulerTarget(std::chrono::steady_clock::now());
        if (target <= engine.impl_->lastSchedulerTarget)
            return Result<RenderFrameResult>::success(result);
        engine.impl_->lastSchedulerTarget = target;

        gpu::SourceFrameBuffer& buffer = engine.impl_->videoWorker->buffer();
        buffer.discardBefore(target);
        gpu::DecodedGpuFrame decoded;
        if (!buffer.takeExact(target, decoded)) {
            ++engine.impl_->telemetrySnapshot.droppedFrameCount;
            engine.impl_->telemetrySnapshot.currentSourceQueueDepth =
                static_cast<std::uint32_t>(buffer.depth());
            return Result<RenderFrameResult>::success(result);
        }
        if (decoded.sourceId != engine.impl_->internalVideoSource ||
            !engine.impl_->sourceRegistry.contains(decoded.sourceId)) {
            PreviewError error =
                makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::RenderDeviceAttach,
                          "decode frameのsource identityが一致しません");
            error.severity = PreviewErrorSeverity::FatalToSession;
            fatal = error;
        } else {
            const auto token = engine.impl_->compositionState.latestAcceptedToken();
            const auto snapshot = engine.impl_->compositionState.latestAcceptedSnapshot();
            if (!token || !snapshot || snapshot->layers.size() != 1) {
                return Result<RenderFrameResult>::failure(makeError(
                    PreviewErrorCategory::CompositionFailure, PreviewOperation::RenderDeviceAttach,
                    "accepted single-layer compositionが見つかりません"));
            }
            const PreviewCompositionLayer& sourceLayer = snapshot->layers.front();
            gpu::CompositionLayerFrame layer;
            layer.frame = std::move(decoded);
            layer.destination = {sourceLayer.destination.x, sourceLayer.destination.y,
                                 sourceLayer.destination.width, sourceLayer.destination.height};
            layer.sourceUv = {sourceLayer.sourceRect.x, sourceLayer.sourceRect.y,
                              sourceLayer.sourceRect.width, sourceLayer.sourceRect.height};
            layer.opacity = sourceLayer.opacity;
            gpu::ComposedFrame composed;
            composed.outputFrameNumber = target;
            composed.compositionEpoch = {token->revision};
            composed.compositionState = {token->id.value};
            composed.layers.push_back(std::move(layer));
            gpu::ExternalCompositionTarget targetView{
                static_cast<ID3D11RenderTargetView*>(renderTargetView), width, height};
            std::string error;
            if (!engine.impl_->compositor->composeSingleLayerToTarget(composed, targetView,
                                                                      error)) {
                PreviewError failure = makeError(
                    PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                    "single-layer GPU compositionに失敗しました: " + error);
                failure.severity = PreviewErrorSeverity::FatalToSession;
                fatal = failure;
            } else {
                buffer.noteDisplayed(target);
                engine.impl_->compositionState.markPresented(*token);
                engine.impl_->distinctPresentedFrames.insert(target);
                ++engine.impl_->presentationSequence;
                ++engine.impl_->telemetrySnapshot.presentedFrameCount;
                engine.impl_->telemetrySnapshot.currentSourceQueueDepth =
                    static_cast<std::uint32_t>(buffer.depth());
                const auto& counters = engine.impl_->compositor->counters();
                engine.impl_->telemetrySnapshot.gpuRetirementCurrentDepth =
                    static_cast<std::uint32_t>(counters.retirementDepthAfterDrain);
                engine.impl_->telemetrySnapshot.gpuRetirementPeakDepth =
                    static_cast<std::uint32_t>(counters.retirementDepthPeak);
                engine.impl_->telemetrySnapshot.status.position = {target};
                engine.impl_->telemetrySnapshot.status.lastPresentedComposition = *token;
                result.presented = true;
                result.sourceFrame = target;
                result.frame = {engine.impl_->presentationSequence, {target}, *token, 1};
            }
        }

        if (fatal) {
            Result<void> accepted = engine.impl_->machine.recordFatal(*fatal);
            if (accepted) {
                engine.impl_->schedulerEnabled = false;
                if (engine.impl_->videoWorker)
                    engine.impl_->videoWorker->pause();
                engine.impl_->startWorkerShutdown();
                engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
                engine.impl_->telemetrySnapshot.status.lastError =
                    engine.impl_->machine.lastError();
            }
        }
    }
    if (fatal) {
        engine.impl_->notify(ErrorOccurredEvent{*fatal});
        engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});
        return Result<RenderFrameResult>::failure(*fatal);
    }
    if (result.presented) {
        engine.impl_->notify(PositionChangedEvent{result.frame.position});
        engine.impl_->notify(FramePresentedEvent{result.frame});
    }
    return Result<RenderFrameResult>::success(result);
}

Result<void> PreviewRenderPort::attachLogicalDevice(PreviewEngine& engine) {
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        Result<void> attached = engine.impl_->machine.attachRenderDevice();
        if (!attached) {
            return attached;
        }
        engine.impl_->telemetrySnapshot.status.state = engine.impl_->machine.state();
    }
    engine.impl_->notify(StateChangedEvent{PreviewEngineState::ReadyPaused});
    return Result<void>::success();
}

Result<bool> PreviewRenderPort::completeRuntimeTeardown(PreviewEngine& engine) {
    PreviewEngineState terminal = PreviewEngineState::ShuttingDown;
    std::optional<PreviewError> drainFailure;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return Result<bool>::failure(
                invalidState(PreviewOperation::Shutdown,
                             "runtime teardownは登録済みrender threadで実行してください")
                    .error());
        }
        if (engine.impl_->machine.state() != PreviewEngineState::ShuttingDown) {
            return Result<bool>::failure(
                invalidState(PreviewOperation::Shutdown,
                             "runtime teardownはShuttingDownでのみ実行できます")
                    .error());
        }
        if (!engine.impl_->renderTeardownRequested || !engine.impl_->workerJoined)
            return Result<bool>::success(false);

        std::string error;
        bool drained = true;
        if (engine.impl_->compositor) {
            if (!engine.impl_->gpuDrainStarted) {
                if (!engine.impl_->compositor->beginShutdown(2000, error))
                    drained = false;
                engine.impl_->gpuDrainStarted = true;
            }
            if (drained) {
                const gpu::GpuCompositorShutdownStatus drainStatus =
                    engine.impl_->compositor->pollShutdown(error);
                if (drainStatus == gpu::GpuCompositorShutdownStatus::Pending)
                    return Result<bool>::success(false);
                drained = drainStatus == gpu::GpuCompositorShutdownStatus::Complete;
            }
        }
        if (engine.impl_->forceGpuDrainFailure)
            drained = false;
        if (!drained) {
            PreviewError failure =
                makeError(PreviewErrorCategory::ShutdownFailure, PreviewOperation::Shutdown,
                          error.empty() ? "GPU retirement drainのtest faultを検出しました"
                                        : "GPU retirement drainに失敗しました: " + error);
            failure.severity = PreviewErrorSeverity::FatalToSession;
            Result<void> recorded = engine.impl_->machine.recordFatal(failure);
            if (recorded)
                drainFailure = failure;
        }

        if (engine.impl_->compositor) {
            const auto& counters = engine.impl_->compositor->counters();
            engine.impl_->finalRuntimeDiagnostics.untrackedSubmissionCount =
                static_cast<std::uint64_t>(counters.untrackedSubmissionCount);
            engine.impl_->finalRuntimeDiagnostics.earlyPayloadReleaseCount =
                static_cast<std::uint64_t>(counters.payloadsReleasedBeforeCompletion);
            engine.impl_->finalRuntimeDiagnostics.retirementTimeoutCount =
                static_cast<std::uint64_t>(counters.retirementTimeoutCount);
            engine.impl_->finalRuntimeDiagnostics.gpuCompositionPassCount =
                static_cast<std::uint64_t>(counters.compositionDrawnCount);
            engine.impl_->finalRuntimeDiagnostics.fullFrameGpuCopyCount =
                static_cast<std::uint64_t>(counters.fullFrameGpuCopyCount);
        }
        engine.impl_->compositor.reset();
        engine.impl_->videoWorker.reset();
        if (engine.impl_->internalVideoSource.value != 0)
            engine.impl_->sourceRegistry.unregisterSource(engine.impl_->internalVideoSource);
        engine.impl_->renderDevice.release();
        engine.impl_->nativeDeviceAttached = false;
        engine.impl_->deviceReleased = true;
        engine.impl_->renderTeardownComplete = true;
        engine.impl_->finalRuntimeDiagnostics.workerJoined = engine.impl_->workerJoined;
        engine.impl_->finalRuntimeDiagnostics.renderTeardownComplete = true;
        engine.impl_->finalRuntimeDiagnostics.deviceReleased = true;
        engine.impl_->finalRuntimeDiagnostics.distinctPresentedSourceFrameCount =
            engine.impl_->distinctPresentedFrames.size();
        engine.impl_->finalRuntimeDiagnostics.fullCpuReadbackCount =
            static_cast<std::uint64_t>(engine.impl_->readbacks.fullFrameReadbacks());
        Result<void> completed = engine.impl_->machine.completeTeardown();
        if (!completed)
            return Result<bool>::failure(completed.error());
        terminal = engine.impl_->machine.state();
        engine.impl_->telemetrySnapshot.status.state = terminal;
        engine.impl_->telemetrySnapshot.status.lastError = engine.impl_->machine.lastError();
    }
    if (engine.impl_->shutdownThread.joinable() &&
        engine.impl_->shutdownThread.get_id() != std::this_thread::get_id())
        engine.impl_->shutdownThread.join();
    if (drainFailure)
        engine.impl_->notify(ErrorOccurredEvent{*drainFailure});
    engine.impl_->notify(StateChangedEvent{terminal});
    return Result<bool>::success(true);
}

Result<void> PreviewRenderPort::completeTeardown(PreviewEngine& engine) {
    PreviewEngineState terminal;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        Result<void> completed = engine.impl_->machine.completeTeardown();
        if (!completed) {
            return completed;
        }
        terminal = engine.impl_->machine.state();
        engine.impl_->telemetrySnapshot.status.state = terminal;
        engine.impl_->telemetrySnapshot.status.lastError = engine.impl_->machine.lastError();
    }
    engine.impl_->notify(StateChangedEvent{terminal});
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectFatal(PreviewEngine& engine, PreviewError error) {
    PreviewError recorded = error;
    recorded.severity = PreviewErrorSeverity::FatalToSession;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        Result<void> accepted = engine.impl_->machine.recordFatal(recorded);
        if (!accepted) {
            return accepted;
        }
        engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
        engine.impl_->telemetrySnapshot.status.lastError = engine.impl_->machine.lastError();
        engine.impl_->schedulerEnabled = false;
        if (engine.impl_->videoWorker)
            engine.impl_->videoWorker->pause();
        if (engine.impl_->nativeDeviceAttached)
            engine.impl_->startWorkerShutdown();
    }
    engine.impl_->notify(ErrorOccurredEvent{recorded});
    engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectGpuDrainFailureForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->nativeDeviceAttached || engine.impl_->renderTeardownComplete) {
        return invalidState(PreviewOperation::Shutdown,
                            "GPU drain faultはactive native runtimeでのみ設定できます");
    }
    engine.impl_->forceGpuDrainFailure = true;
    return Result<void>::success();
}

P5CRuntimeDiagnostics PreviewRenderPort::runtimeDiagnostics(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    P5CRuntimeDiagnostics result = engine.impl_->finalRuntimeDiagnostics;
    result.nativeDeviceAttached = engine.impl_->nativeDeviceAttached;
    result.workerJoined = engine.impl_->workerJoined;
    result.renderTeardownComplete = engine.impl_->renderTeardownComplete;
    result.deviceReleased = engine.impl_->deviceReleased;
    result.registeredVideoSourceCount = engine.impl_->sourceRegistry.registeredSourceCount();
    result.distinctPresentedSourceFrameCount = engine.impl_->distinctPresentedFrames.size();
    result.staleSubstitutionCount = engine.impl_->staleSubstitutionCount;
    result.lifecycleViolationCount = engine.impl_->lifecycleViolationCount;
    result.fullCpuReadbackCount =
        static_cast<std::uint64_t>(engine.impl_->readbacks.fullFrameReadbacks());
    if (engine.impl_->videoWorker) {
        const gpu::SourceDecoderSnapshot decoder = engine.impl_->videoWorker->snapshot();
        result.d3d11vaActive = decoder.open && decoder.softwareFrameRejectCount == 0;
        result.decodeRenderSameDevice =
            decoder.decodeDevicePointer ==
            reinterpret_cast<std::uintptr_t>(engine.impl_->renderDevice.device());
        result.softwareFallbackCount = static_cast<std::uint64_t>(decoder.softwareFrameRejectCount);
        result.deviceLostCount = static_cast<std::uint64_t>(decoder.deviceMismatchCount);
    }
    if (engine.impl_->compositor) {
        const auto& counters = engine.impl_->compositor->counters();
        result.untrackedSubmissionCount =
            static_cast<std::uint64_t>(counters.untrackedSubmissionCount);
        result.earlyPayloadReleaseCount =
            static_cast<std::uint64_t>(counters.payloadsReleasedBeforeCompletion);
        result.retirementTimeoutCount = static_cast<std::uint64_t>(counters.retirementTimeoutCount);
        result.gpuCompositionPassCount = static_cast<std::uint64_t>(counters.compositionDrawnCount);
        result.fullFrameGpuCopyCount = static_cast<std::uint64_t>(counters.fullFrameGpuCopyCount);
    }
    return result;
}

void PreviewRenderPort::enqueueEventForTest(PreviewEngine& engine, PreviewEvent event) {
    engine.impl_->notify(std::move(event));
}

std::size_t PreviewRenderPort::mailboxSizeForTest(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    return engine.impl_->mailbox.size();
}

} // namespace internal
} // namespace mvm::preview
