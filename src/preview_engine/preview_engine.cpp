#include "core/checked_output_timebase.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/wasapi_audio_sink.h"
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
#include <cstdio>
#include <limits>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mvm::preview {
namespace {

// internal PCM domainのsample formatは`AudioChunk::pcm`の要素型で決まる。
// runtimeの観測値ではなく型不変条件なので、ここで固定して"flt"の根拠にする。
static_assert(
    std::is_same_v<audio::AudioChunk::PcmSample, float>,
    "internal PCM domainはfloat32である。変更する場合はqualified audio domainも見直すこと");
static_assert(sizeof(audio::AudioChunk::PcmSample) == 4,
              "internal PCM domainのsampleは32 bitである");

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

Result<void> validateSourceFrameRate(long long sourceNumerator, long long sourceDenominator,
                                     PreviewFrameRate outputFrameRate) {
    if (sourceNumerator <= 0 || sourceDenominator <= 0) {
        return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                               PreviewOperation::AddSource,
                                               "sourceのフレームレートが不正です"));
    }

    auto numerator = static_cast<std::uint64_t>(sourceNumerator);
    auto denominator = static_cast<std::uint64_t>(sourceDenominator);
    const std::uint64_t divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator == outputFrameRate.numerator && denominator == outputFrameRate.denominator)
        return Result<void>::success();

    return Result<void>::failure(
        makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                  "P5-Cはoutputと同じフレームレートのsourceだけをsupportします (source=" +
                      std::to_string(sourceNumerator) + "/" + std::to_string(sourceDenominator) +
                      ", output=" + std::to_string(outputFrameRate.numerator) + "/" +
                      std::to_string(outputFrameRate.denominator) + ")"));
}

Result<void> validateQualifiedAudioDomain(int sampleRate, int channels,
                                          const std::string& sampleFormat) {
    if (sampleRate != audio::kInternalSampleRate || channels != audio::kInternalChannels ||
        sampleFormat != "flt") {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "qualified audio domainは48000 Hz / stereo / float32だけです (要求=" +
                          std::to_string(sampleRate) + " Hz / " + std::to_string(channels) +
                          "ch / " + sampleFormat + ")"));
    }
    return Result<void>::success();
}

std::uint64_t skippedSchedulerFrameCount(std::int64_t previousTarget, std::int64_t currentTarget) {
    if (currentTarget <= previousTarget)
        return 0;
    const std::uint64_t distance =
        static_cast<std::uint64_t>(currentTarget) - static_cast<std::uint64_t>(previousTarget);
    return distance > 1 ? distance - 1 : 0;
}

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

void DistinctFrameCounter::note(std::int64_t frame) {
    if (lastFrame_ && frame <= *lastFrame_)
        return;
    lastFrame_ = frame;
    if (count_ != std::numeric_limits<std::uint64_t>::max())
        ++count_;
}

std::uint64_t DistinctFrameCounter::count() const {
    return count_;
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
        const auto finish = [](std::thread& thread) {
            if (!thread.joinable())
                return;
            if (thread.get_id() == std::this_thread::get_id())
                thread.detach();
            else
                thread.join();
        };
        finish(shutdownThread);
        finish(detachedTeardownThread);
    }

    mutable std::mutex mutex;
    internal::PreviewStateMachine machine;
    internal::EventMailbox mailbox{32};
    std::shared_ptr<PreviewEventDispatcher> dispatcher;
    std::weak_ptr<PreviewEventSink> sink;
    const std::thread::id controlThread;
    std::uint64_t sinkGeneration = 0;
    bool dispatchScheduled = false;
    PreviewCapabilities capability = [] {
        PreviewCapabilities value;
        // P5-D2でaudio-master transportを接続したため、qualified audio domainを公開する。
        value.maxQualifiedActiveAudioSources = 1;
        value.qualifiedAudioSampleRate = audio::kInternalSampleRate;
        value.qualifiedAudioChannelCount = audio::kInternalChannels;
        return value;
    }();
    PreviewTelemetry telemetrySnapshot;
    PreviewDeviceInfo deviceSnapshot;

    // P5-C private backend。public headerへmedia/native型を漏らさない。
    std::unique_ptr<gpu::SharedD3D11Device> renderDevice =
        std::make_unique<gpu::SharedD3D11Device>();
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

    // P5-D2 private audio backend。public headerへWASAPI/FFmpeg型を漏らさない。
    std::optional<core::CheckedOutputTimebase> timebase;
    std::shared_ptr<audio::AudioMasterClock> audioClock;
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    std::optional<PreviewSourceId> publicAudioSource;
    audio::SourceId internalAudioSource{};
    std::int64_t resumeAudioSample = 0;
    bool audioMasterActive = false;
    bool audioSinkJoined = true;
    bool audioWorkerJoined = true;
    std::uint64_t audioMasterProjectionFailureCount = 0;
    std::uint64_t audioGenerationMismatchCount = 0;
    std::uint64_t audioTransportFailureCount = 0;
    std::uint64_t audioDomainRejectCount = 0;
    bool audioClockStallInjected = false;
    std::vector<internal::ShutdownStep> shutdownSequence;
    bool renderVisibleWorkersDetached = false;

    // detach後のowner。renderから到達できるfieldではない。
    // memberは宣言の逆順で破棄される。`WasapiAudioSink`はqueue (worker所有) と
    // clockをreferenceで保持し、destructorから`stop()` -> `clock_.stop()`を呼ぶ。
    // したがってsinkが最後に壊れるとuse-after-freeになる。正常teardownの明示reset順
    // (sink -> worker -> clock) と一致するよう、宣言はその逆順に並べる。
    // `Impl`本体のaudio memberも同じ理由で clock -> worker -> sink の順に宣言している。
    struct DetachedWorkers {
        std::unique_ptr<gpu::SourceDecodeWorker> videoWorker;
        std::shared_ptr<audio::AudioMasterClock> audioClock;
        std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
        std::shared_ptr<audio::WasapiAudioSink> audioSink;

        // 宣言順という規約に依存すると、将来の並べ替えで無言のuse-after-freeへ
        // 戻り得る (このUAFはcrashしないため、testでも捕まえられない)。
        // 依存の逆順をdestructorで固定し、宣言順に関係なく安全にする。
        ~DetachedWorkers() {
            audioSink.reset();
            audioWorker.reset();
            audioClock.reset();
        }

        DetachedWorkers() = default;
        DetachedWorkers(const DetachedWorkers&) = delete;
        DetachedWorkers& operator=(const DetachedWorkers&) = delete;
    };

    DetachedWorkers detachedWorkers;

    // 実行順をそのまま積む。重複を畳むと、誤った再実行や並べ替えがexact比較を
    // すり抜けるため、畳まずに残したうえでviolationとして数える。
    void noteShutdownStepLocked(internal::ShutdownStep step) {
        if (std::find(shutdownSequence.begin(), shutdownSequence.end(), step) !=
            shutdownSequence.end()) {
            ++lifecycleViolationCount;
        }
        shutdownSequence.push_back(step);
    }

    // renderから到達できるfieldを実際に空にする。bookkeepingだけでは
    // 「detach済みと言いながら参照が残っている」状態になるため、ownershipを
    // shutdown専用のholderへ移す。ここを通るまでrender teardownへ進まない。
    void detachRenderVisibleWorkerRefsLocked() {
        detachedWorkers.videoWorker = std::move(videoWorker);
        detachedWorkers.audioSink = std::move(audioSink);
        detachedWorkers.audioWorker = std::move(audioWorker);
        detachedWorkers.audioClock = std::move(audioClock);
        renderVisibleWorkersDetached = true;
        noteShutdownStepLocked(internal::ShutdownStep::DetachRenderVisibleWorkerRefs);
    }

    // diagnostics/teardownはdetach後も実体を参照する。render pathはこれを使わない。
    gpu::SourceDecodeWorker* videoWorkerForTeardown() const {
        return videoWorker ? videoWorker.get() : detachedWorkers.videoWorker.get();
    }

    audio::AudioDecodeWorker* audioWorkerForTeardown() const {
        return audioWorker ? audioWorker.get() : detachedWorkers.audioWorker.get();
    }

    audio::WasapiAudioSink* audioSinkForTeardown() const {
        return audioSink ? audioSink.get() : detachedWorkers.audioSink.get();
    }

    // 製品既定は unity。検証アプリだけが下げる。
    float audioSessionVolume = 1.0F;

    std::optional<std::thread::id> renderThread;
    void* nativeDeviceIdentity = nullptr;
    void* nativeContextIdentity = nullptr;
    bool nativeDeviceAttached = false;
    bool schedulerEnabled = false;
    std::chrono::steady_clock::time_point schedulerStart;
    std::int64_t schedulerBaseFrame = 0;
    std::int64_t lastSchedulerTarget = -1;
    std::uint64_t presentationSequence = 0;
    internal::DistinctFrameCounter distinctPresentedFrames;
    std::thread shutdownThread;
    std::thread detachedTeardownThread;
    bool rendererDetached = false;
    bool detachedTeardownStarted = false;
    bool shutdownWorkerStarted = false;
    bool workerJoined = true;
    bool renderTeardownRequested = false;
    bool gpuDrainStarted = false;
    bool renderTeardownComplete = false;
    bool deviceReleased = true;
    bool unsafeGpuResourcesRetained = false;
    std::uint64_t lifecycleViolationCount = 0;
    std::uint64_t staleSubstitutionCount = 0;
    std::uint64_t deviceLostCount = 0;
    internal::P5CRuntimeDiagnostics finalRuntimeDiagnostics;

    Result<void> requireControlThread(PreviewOperation operation) const {
        if (controlThread != std::this_thread::get_id()) {
            return invalidState(operation, "control methodが作成時のthread以外から呼ばれました");
        }
        return Result<void>::success();
    }

    struct SchedulerTarget {
        bool valid = false;
        std::int64_t frame = 0;
        PreviewError error;
    };

    // output frameのmasterはaudio source登録時のみ`IAudioClock`である。projectionが
    // 成立しない場合はQPC/steady_clockへ退避せず、`AudioFailure`として表面化する。
    // frame換算そのものは`CheckedOutputTimebase`へ一本化し、ここで再実装しない。
    SchedulerTarget schedulerTargetLocked(std::chrono::steady_clock::time_point now) {
        SchedulerTarget result;
        if (!timebase) {
            result.error =
                makeError(PreviewErrorCategory::InvalidState, PreviewOperation::RenderDeviceAttach,
                          "output timebaseが未確定のままscheduleしようとしました");
            return result;
        }

        if (audioMasterActive) {
            const auto audioFailure = [&](std::string detail) {
                result.error = makeError(PreviewErrorCategory::AudioFailure,
                                         PreviewOperation::RenderDeviceAttach, std::move(detail),
                                         PreviewErrorSeverity::FatalToSession);
                result.error.source = publicAudioSource;
            };
            const auto projectionFailure = [&](std::string detail) {
                ++audioMasterProjectionFailureCount;
                audioFailure(std::move(detail));
            };
            // WASAPI sinkのruntime failureは、clock停止として誤診断する前に検知する。
            if (audioSink) {
                const audio::WasapiSnapshot endpoint = audioSink->snapshot();
                if (endpoint.deviceFailureCount != 0) {
                    audioFailure("WASAPI renderingがruntime failureで停止しました: " +
                                 endpoint.lastError);
                    return result;
                }
            }
            if (audioClockStallInjected || !audioClock) {
                projectionFailure("audio master clockが利用できません");
                return result;
            }
            const audio::AudioClockSnapshot clock = audioClock->snapshot();
            if (!clock.running) {
                projectionFailure("audio master clockが停止しています");
                return result;
            }
            const auto frame = timebase->schedulerOutputFrame(clock.mediaSamplePosition);
            if (!frame) {
                projectionFailure("audio sample positionをoutput frameへ換算できません");
                return result;
            }
            result.valid = true;
            result.frame = frame.value();
            return result;
        }

        // audio sourceが無いvideo-only経路 (P5-C) だけがwall-clockを使う。
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - schedulerStart).count();
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        std::int64_t advanced = 0;
        if (elapsed > 0) {
            const auto frames = timebase->outputFrameForNanoseconds(elapsed);
            advanced = frames ? frames.value() : maximum;
        }
        result.valid = true;
        result.frame =
            schedulerBaseFrame > maximum - advanced ? maximum : schedulerBaseFrame + advanced;
        return result;
    }

    void startWorkerShutdown() {
        if (shutdownWorkerStarted)
            return;
        shutdownWorkerStarted = true;
        // preview-engine-contract.md §12の固定順:
        // DisableSchedulers -> StopAudioSink -> StopAudioDecodeWorker -> StopVideoWorkers
        // -> DetachRenderVisibleWorkerRefs -> VerifyJoins -> RequestRenderTeardown
        audioMasterActive = false;
        noteShutdownStepLocked(internal::ShutdownStep::DisableSchedulers);
        audioSinkJoined = audioSink == nullptr;
        audioWorkerJoined = audioWorker == nullptr;
        workerJoined = videoWorker == nullptr || videoWorker->joined();
        renderTeardownRequested = workerJoined && audioSinkJoined && audioWorkerJoined;
        if (renderTeardownRequested) {
            // 停止対象が無い場合もstepの順序は同じ形で残す。
            noteShutdownStepLocked(internal::ShutdownStep::StopAudioSink);
            noteShutdownStepLocked(internal::ShutdownStep::StopAudioDecodeWorker);
            noteShutdownStepLocked(internal::ShutdownStep::StopVideoWorkers);
            detachRenderVisibleWorkerRefsLocked();
            noteShutdownStepLocked(internal::ShutdownStep::VerifyJoins);
            noteShutdownStepLocked(internal::ShutdownStep::RequestRenderTeardown);
            return;
        }
        const std::shared_ptr<Impl> self = shared_from_this();
        shutdownThread = std::thread([self] {
            std::shared_ptr<audio::WasapiAudioSink> audioEndpoint;
            std::shared_ptr<audio::AudioDecodeWorker> audioDecoder;
            gpu::SourceDecodeWorker* worker = nullptr;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                audioEndpoint = self->audioSink;
                audioDecoder = self->audioWorker;
                worker = self->videoWorker.get();
            }
            std::string ignored;
            if (audioEndpoint) {
                audioEndpoint->pause(ignored);
                audioEndpoint->stop();
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->noteShutdownStepLocked(internal::ShutdownStep::StopAudioSink);
            }
            if (audioDecoder)
                audioDecoder->stop();
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->noteShutdownStepLocked(internal::ShutdownStep::StopAudioDecodeWorker);
            }
            if (worker)
                worker->stop();
            bool joinsVerified = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->noteShutdownStepLocked(internal::ShutdownStep::StopVideoWorkers);
                self->detachRenderVisibleWorkerRefsLocked();
                self->audioSinkJoined =
                    audioEndpoint == nullptr || audioEndpoint->snapshot().joined;
                self->audioWorkerJoined =
                    audioDecoder == nullptr || audioDecoder->snapshot().joined;
                self->workerJoined = worker == nullptr || worker->joined();
                self->noteShutdownStepLocked(internal::ShutdownStep::VerifyJoins);
                joinsVerified =
                    self->workerJoined && self->audioSinkJoined && self->audioWorkerJoined;
            }

            // RequestRenderTeardownをpublishすると、render threadがdetached audioを
            // 解放し得る。その前にshutdown thread側のstrong ownerを必ず落とす。
            // detachedWorkersが所有しているので、ここでresetしても実体は生存する。
            audioEndpoint.reset();
            audioDecoder.reset();
            // videoWorkerはdetachedWorkersが所有しており、この生ポインタは
            // teardown後にdanglingになる。publish前に手放す。
            worker = nullptr;

            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->renderTeardownRequested = joinsVerified;
                if (!self->renderTeardownRequested)
                    ++self->lifecycleViolationCount;
                else
                    self->noteShutdownStepLocked(internal::ShutdownStep::RequestRenderTeardown);
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
    // scheduler / seek / statusが同じ換算を使うよう、timebaseはここで一度だけ確定する。
    const auto timebase = core::CheckedOutputTimebase::createQualified(
        static_cast<std::int64_t>(config.output.frameRate.numerator),
        static_cast<std::int64_t>(config.output.frameRate.denominator), audio::kInternalSampleRate);
    if (!timebase) {
        return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                               PreviewOperation::Initialize,
                                               "qualified output timebaseを構築できません"));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> initialized = impl_->machine.initialize();
        if (!initialized) {
            return initialized;
        }
        impl_->dispatcher = std::move(dispatcher);
        impl_->configuredFrameRate = rate.value();
        impl_->timebase = timebase.value();
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
        impl_->timebase.reset();
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
    if (!impl_->nativeDeviceAttached || !impl_->compositor || !impl_->renderDevice->valid()) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "native render deviceの準備前にsourceを登録できません"));
    }
    if (descriptor.videoEnabled && impl_->publicVideoSource) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "P5-D product wiringはvideo sourceを1件だけ受理します"));
    }
    if (descriptor.audioEnabled) {
        if (impl_->capability.maxQualifiedActiveAudioSources == 0) {
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                          "audio sourceはqualified capabilityに含まれていません"));
        }
        if (impl_->publicAudioSource) {
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                          "P5-D product wiringはactive audio sourceを1件だけ受理します"));
        }
    }
    if (impl_->nextPublicSourceId == 0) {
        return Result<PreviewSourceId>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                                          PreviewOperation::AddSource,
                                                          "PreviewSourceIdがoverflowしました"));
    }

    const auto utf8Path = descriptor.mediaPath.u8string();
    const std::string path(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());

    // video registration。失敗時はsource tableとregistryを呼び出し前へ戻す。
    gpu::SourceId internalVideo{};
    std::unique_ptr<gpu::SourceDecodeWorker> newVideoWorker;
    if (descriptor.videoEnabled) {
        internalVideo = impl_->sourceRegistry.registerSource();
        newVideoWorker = std::make_unique<gpu::SourceDecodeWorker>(
            internalVideo, *impl_->renderDevice, impl_->readbacks, 6);
        std::string openError;
        if (!newVideoWorker->start(path, openError)) {
            newVideoWorker->stop();
            impl_->sourceRegistry.unregisterSource(internalVideo);
            ++impl_->telemetrySnapshot.decodeFailureCount;
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::AddSource,
                          "D3D11VA video sourceをopenできません: " + openError));
        }
        const gpu::SourceDecoderSnapshot opened = newVideoWorker->snapshot();
        Result<void> supportedRate = internal::validateSourceFrameRate(
            opened.info.frameRate.num, opened.info.frameRate.den, impl_->configuredFrameRate);
        if (!supportedRate) {
            newVideoWorker->stop();
            impl_->sourceRegistry.unregisterSource(internalVideo);
            return Result<PreviewSourceId>::failure(supportedRate.error());
        }
    }

    // audio registration。sinkはworkerのqueueとclockを参照するため、この順で組む。
    std::shared_ptr<audio::AudioMasterClock> newAudioClock;
    std::shared_ptr<audio::AudioDecodeWorker> newAudioWorker;
    std::shared_ptr<audio::WasapiAudioSink> newAudioSink;
    audio::SourceId internalAudio{};
    if (descriptor.audioEnabled) {
        const auto rollbackVideo = [&] {
            if (!newVideoWorker)
                return;
            newVideoWorker->stop();
            impl_->sourceRegistry.unregisterSource(internalVideo);
        };
        internalAudio = audio::SourceId{impl_->nextPublicSourceId};
        newAudioClock = std::make_shared<audio::AudioMasterClock>();
        newAudioWorker = std::make_shared<audio::AudioDecodeWorker>(internalAudio);
        std::string audioError;
        if (!newAudioWorker->start(path, audioError)) {
            newAudioWorker->stop();
            rollbackVideo();
            ++impl_->telemetrySnapshot.decodeFailureCount;
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::AddSource,
                          "audio sourceをopenできません: " + audioError));
        }
        // ここはengine configの検査である。sample rateはtimebaseが保持する実際の値、
        // channelはcapabilityが公開している実際の値を使う。decode出力そのものの
        // domainは、実データが出そろうplay()側で観測値を使って検査する。
        const std::int64_t configuredSampleRate =
            impl_->timebase ? impl_->timebase->audioSampleRate() : 0;
        Result<void> domain = internal::validateQualifiedAudioDomain(
            static_cast<int>(configuredSampleRate),
            static_cast<int>(impl_->capability.qualifiedAudioChannelCount), "flt");
        if (!domain) {
            newAudioWorker->stop();
            rollbackVideo();
            return Result<PreviewSourceId>::failure(domain.error());
        }
        newAudioSink =
            std::make_shared<audio::WasapiAudioSink>(newAudioWorker->queue(), *newAudioClock);
        if (!newAudioSink->open(audioError, impl_->audioSessionVolume)) {
            newAudioSink->stop();
            newAudioWorker->stop();
            rollbackVideo();
            ++impl_->audioTransportFailureCount;
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                          "WASAPI shared event-driven endpointをopenできません: " + audioError));
        }
    }

    const PreviewSourceId published{impl_->nextPublicSourceId++};
    if (descriptor.videoEnabled) {
        impl_->internalVideoSource = internalVideo;
        impl_->publicVideoSource = published;
        impl_->videoWorker = std::move(newVideoWorker);
        impl_->workerJoined = false;
        impl_->deviceReleased = false;
        impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
    }
    if (descriptor.audioEnabled) {
        impl_->internalAudioSource = internalAudio;
        impl_->publicAudioSource = published;
        impl_->audioClock = std::move(newAudioClock);
        impl_->audioWorker = std::move(newAudioWorker);
        impl_->audioSink = std::move(newAudioSink);
        impl_->audioSinkJoined = false;
        impl_->audioWorkerJoined = false;
        impl_->resumeAudioSample = 0;
        const audio::WasapiSnapshot endpoint = impl_->audioSink->snapshot();
        impl_->deviceSnapshot.audioSampleRate =
            static_cast<std::uint32_t>(endpoint.deviceFormat.sampleRate);
        impl_->deviceSnapshot.audioChannelCount =
            static_cast<std::uint32_t>(endpoint.deviceFormat.channels);
    }
    impl_->eligibleSources.emplace(
        published.value,
        internal::EligibleSource{descriptor.videoEnabled, descriptor.audioEnabled});
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
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    std::int64_t startSample = 0;
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
        audioWorker = impl_->audioWorker;
        audioSink = impl_->audioSink;
        startSample = impl_->resumeAudioSample;
    }

    // audio decode preroll と WASAPI start は engine lockを保持したまま待たない。
    // control methodは同一threadからしか呼べないため、この窓でstateは動かない。
    if (audioWorker && audioSink) {
        audioWorker->play();
        if (!audioWorker->queue().waitForSamples(audio::kAudioPrerollSamples,
                                                 audio::kPrerollTimeoutMs)) {
            // 全chunkがdomain不一致でrejectされた場合もprerollは埋まらない。
            // transport failureとして丸めず、authorityを見分けて分類する。
            const audio::AudioQueueSnapshot timedOut = audioWorker->queue().snapshot();
            audioWorker->pause();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (timedOut.invalidRejectCount != 0) {
                ++impl_->audioDomainRejectCount;
                return Result<void>::failure(makeError(
                    PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Play,
                    "audio decode出力がqualified PCM domainと一致せずprerollを満たせません "
                    "(invalid reject=" +
                        std::to_string(timedOut.invalidRejectCount) + ")"));
            }
            ++impl_->audioTransportFailureCount;
            return Result<void>::failure(
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Play,
                          "audio prerollを満たせないままplayを開始できません"));
        }
        // decode workerが実際に出したPCM domainを観測値として検査する。期待値を
        // そのまま渡すと原理的に失敗できない検査になるため、queueが受理したchunkの
        // 実測sample rate / channel数を使う。invalid rejectが残っている場合も、
        // qualified domain以外を暗黙に鳴らさずfail-closedにする。
        const audio::AudioQueueSnapshot prerolled = audioWorker->queue().snapshot();
        Result<void> observedDomain = internal::validateQualifiedAudioDomain(
            prerolled.observedSampleRate, prerolled.observedChannels, "flt");
        if (prerolled.invalidRejectCount != 0 || !observedDomain) {
            audioWorker->pause();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->audioDomainRejectCount;
            if (!observedDomain)
                return Result<void>::failure(observedDomain.error());
            return Result<void>::failure(
                makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Play,
                          "audio decode出力がqualified PCM domainと一致しません (invalid reject=" +
                              std::to_string(prerolled.invalidRejectCount) + ")"));
        }
        std::string error;
        if (!audioSink->play(startSample, audioWorker->snapshot().sourceGeneration, error)) {
            audioWorker->pause();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->audioTransportFailureCount;
            return Result<void>::failure(makeError(PreviewErrorCategory::AudioFailure,
                                                   PreviewOperation::Play,
                                                   "WASAPI renderingを開始できません: " + error));
        }
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> played = impl_->machine.play();
        if (!played) {
            if (audioWorker)
                audioWorker->pause();
            if (audioSink) {
                std::string ignored;
                audioSink->pause(ignored);
            }
            return played;
        }
        impl_->schedulerEnabled = true;
        // audio sourceが登録されている場合だけ`IAudioClock`がmasterになる。
        impl_->audioMasterActive = impl_->audioSink != nullptr && impl_->audioClock != nullptr;
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
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Pause);
        if (!affinity)
            return affinity;
        if (impl_->machine.state() != PreviewEngineState::Playing)
            return invalidState(PreviewOperation::Pause, "pauseを受理できないstateです");
        // 提示だけ先に止める。audio clockを止める前にschedulerを黙らせないと、
        // render threadがclock停止をprojection失敗として誤検出する。
        // ただしtransport stateはまだcommitしない (sink停止を確認するまでPlayingのまま)。
        impl_->schedulerEnabled = false;
        impl_->audioMasterActive = false;
        if (impl_->videoWorker)
            impl_->videoWorker->pause();
        audioWorker = impl_->audioWorker;
        audioSink = impl_->audioSink;
    }

    std::string sinkError;
    const bool sinkPaused = audioSink == nullptr || audioSink->pause(sinkError);
    if (audioWorker)
        audioWorker->pause();

    std::optional<PreviewError> fatal;
    PreviewEngineState published = PreviewEngineState::ReadyPaused;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!sinkPaused) {
            // audio sinkを止められないまま`ReadyPaused`を公開しない。videoだけ
            // 停止してaudioが鳴り続ける状態はsession-fatalとして扱う。
            ++impl_->audioTransportFailureCount;
            PreviewError failure =
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Pause,
                          "WASAPI renderingを停止できません: " + sinkError,
                          PreviewErrorSeverity::FatalToSession);
            failure.source = impl_->publicAudioSource;
            Result<void> recorded = impl_->machine.recordFatal(failure);
            if (!recorded)
                return recorded;
            fatal = failure;
            impl_->startWorkerShutdown();
            published = PreviewEngineState::ShuttingDown;
            impl_->telemetrySnapshot.status.state = published;
            impl_->telemetrySnapshot.status.lastError = impl_->machine.lastError();
        } else {
            Result<void> paused = impl_->machine.pause();
            if (!paused)
                return paused;
            // resume時のmedia positionは、audio clockが確定した値だけを使う。
            if (impl_->audioClock)
                impl_->resumeAudioSample = impl_->audioClock->snapshot().mediaSamplePosition;
            impl_->telemetrySnapshot.status.state = published;
        }
    }

    if (fatal) {
        impl_->notify(internal::ErrorOccurredEvent{*fatal});
        impl_->notify(internal::StateChangedEvent{published});
        return Result<void>::failure(*fatal);
    }
    impl_->notify(internal::StateChangedEvent{published});
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
    bool completeWithoutRuntime = false;
    bool startDetachedTeardown = false;
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
        impl_->audioMasterActive = false;
        if (impl_->videoWorker)
            impl_->videoWorker->pause();
        if (impl_->nativeDeviceAttached)
            impl_->startWorkerShutdown();
        completeWithoutRuntime =
            before == PreviewEngineState::WaitingForRenderDevice && !impl_->nativeDeviceAttached;
        if (impl_->rendererDetached && impl_->nativeDeviceAttached &&
            !impl_->detachedTeardownStarted) {
            impl_->detachedTeardownStarted = true;
            startDetachedTeardown = true;
        }
        impl_->telemetrySnapshot.status.state = after;
    }
    if (before != after) {
        impl_->notify(internal::StateChangedEvent{after});
    }
    if (completeWithoutRuntime)
        return internal::PreviewRenderPort::completeTeardown(*this);
    if (startDetachedTeardown) {
        const std::shared_ptr<Impl> retained = impl_;
        retained->detachedTeardownThread = std::thread([retained] {
            PreviewEngine authority;
            authority.impl_ = retained;
            {
                std::lock_guard<std::mutex> lock(retained->mutex);
                retained->renderThread = std::this_thread::get_id();
            }
            for (;;) {
                Result<bool> completed =
                    internal::PreviewRenderPort::completeRuntimeTeardown(authority);
                if (!completed || completed.value())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
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
    std::optional<PreviewError> failure;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native device attachは登録済みrender threadで実行してください");
        }
        if (engine.impl_->machine.state() != PreviewEngineState::WaitingForRenderDevice) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native device attachはdevice attach待ちでのみ実行できます");
        }
        if (!device || !context) {
            failure =
                makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                          "D3D11 device/contextがnullです");
        } else {
            auto* nativeDevice = static_cast<ID3D11Device*>(device);
            auto* nativeContext = static_cast<ID3D11DeviceContext*>(context);
            ID3D11Device* contextDevice = nullptr;
            nativeContext->GetDevice(&contextDevice);
            const bool sameDevice = contextDevice == nativeDevice;
            if (contextDevice)
                contextDevice->Release();
            if (!sameDevice) {
                failure = makeError(PreviewErrorCategory::DeviceFailure,
                                    PreviewOperation::RenderDeviceAttach,
                                    "contextとdeviceの実体が一致しません");
            } else {
                std::string error;
                if (!engine.impl_->renderDevice->adopt(nativeDevice, nativeContext, error)) {
                    failure = makeError(PreviewErrorCategory::DeviceFailure,
                                        PreviewOperation::RenderDeviceAttach,
                                        "共有D3D11 deviceをadoptできません: " + error);
                } else {
                    auto compositor = std::make_unique<gpu::GpuCompositor>();
                    if (!compositor->initializeExternal(*engine.impl_->renderDevice,
                                                        engine.impl_->readbacks, error)) {
                        engine.impl_->renderDevice->release();
                        failure = makeError(PreviewErrorCategory::DeviceFailure,
                                            PreviewOperation::RenderDeviceAttach,
                                            "product compositorを初期化できません: " + error);
                    } else {
                        Result<void> attached = engine.impl_->machine.attachRenderDevice();
                        if (!attached) {
                            std::string ignored;
                            compositor->shutdown(2000, ignored);
                            engine.impl_->renderDevice->release();
                            failure = attached.error();
                        } else {
                            engine.impl_->compositor = std::move(compositor);
                            engine.impl_->nativeDeviceIdentity = device;
                            engine.impl_->nativeContextIdentity = context;
                            engine.impl_->nativeDeviceAttached = true;
                            engine.impl_->deviceReleased = false;
                            const gpu::AdapterInfo& adapter = engine.impl_->renderDevice->adapter();
                            info.adapterDescription = adapter.description;
                            info.adapterLuidLow = adapter.luidLow;
                            info.adapterLuidHigh = adapter.luidHigh;
                            engine.impl_->deviceSnapshot = info;
                            engine.impl_->telemetrySnapshot.status.state =
                                PreviewEngineState::ReadyPaused;
                        }
                    }
                }
            }
        }

        if (failure) {
            failure->severity = PreviewErrorSeverity::FatalToSession;
            Result<void> recorded = engine.impl_->machine.recordFatal(*failure);
            if (!recorded)
                return recorded;
            engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
            engine.impl_->telemetrySnapshot.status.lastError = engine.impl_->machine.lastError();
        }
    }
    if (failure) {
        engine.impl_->notify(ErrorOccurredEvent{*failure});
        engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});
        return Result<void>::failure(*failure);
    }
    engine.impl_->notify(DeviceChangedEvent{info});
    engine.impl_->notify(StateChangedEvent{PreviewEngineState::ReadyPaused});
    return Result<void>::success();
}

Result<void> PreviewRenderPort::acquireNativeD3D11Device(PreviewEngine& engine, void* device,
                                                         void* context) {
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        attached = engine.impl_->nativeDeviceAttached;
        if (attached) {
            // renderer再生成後は既存runtimeの所有render threadを新rendererへ移す。
            engine.impl_->rendererDetached = false;
            engine.impl_->renderThread = std::this_thread::get_id();
            if (engine.impl_->nativeDeviceIdentity == device &&
                engine.impl_->nativeContextIdentity == context) {
                return Result<void>::success();
            }
        }
    }
    if (attached) {
        PreviewError failure =
            makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                      "renderer再生成時にQRhiのD3D11 device/context identityが差し替わりました");
        failure.severity = PreviewErrorSeverity::FatalToSession;
        Result<void> recorded = injectFatal(engine, failure);
        if (!recorded)
            return recorded;
        return Result<void>::failure(std::move(failure));
    }
    Result<void> bound = bindRenderThread(engine);
    if (!bound)
        return bound;
    return attachNativeD3D11Device(engine, device, context);
}

Result<void> PreviewRenderPort::validateNativeD3D11Device(PreviewEngine& engine, void* device,
                                                          void* context) {
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native device検証は登録済みrender threadで実行してください");
        }
        if (!engine.impl_->nativeDeviceAttached) {
            return invalidState(PreviewOperation::RenderDeviceAttach,
                                "native device未attachのためidentityを検証できません");
        }
        if (engine.impl_->nativeDeviceIdentity == device &&
            engine.impl_->nativeContextIdentity == context) {
            return Result<void>::success();
        }
    }

    PreviewError failure =
        makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                  "QRhiのD3D11 device/context identityが差し替わりました");
    failure.severity = PreviewErrorSeverity::FatalToSession;
    Result<void> recorded = injectFatal(engine, failure);
    if (!recorded)
        return recorded;
    return Result<void>::failure(std::move(failure));
}

Result<RenderFrameResult> PreviewRenderPort::renderFrame(PreviewEngine& engine,
                                                         void* renderTargetView, int width,
                                                         int height) {
    RenderFrameResult result;
    std::optional<PreviewError> fatal;
    bool decoderFatal = false;
    bool playbackEnded = false;
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

        // audio masterが成立しない場合、QPC/steady_clockへ退避せずfatalとして表面化する。
        const auto scheduled =
            engine.impl_->schedulerTargetLocked(std::chrono::steady_clock::now());
        if (!scheduled.valid)
            fatal = scheduled.error;
        else {
            const std::int64_t target = scheduled.frame;
            if (target <= engine.impl_->lastSchedulerTarget)
                return Result<RenderFrameResult>::success(result);
            const std::uint64_t skipped =
                internal::skippedSchedulerFrameCount(engine.impl_->lastSchedulerTarget, target);
            const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
            if (skipped > maximum - engine.impl_->telemetrySnapshot.droppedFrameCount)
                engine.impl_->telemetrySnapshot.droppedFrameCount = maximum;
            else
                engine.impl_->telemetrySnapshot.droppedFrameCount += skipped;
            engine.impl_->lastSchedulerTarget = target;

            gpu::SourceFrameBuffer& buffer = engine.impl_->videoWorker->buffer();
            buffer.discardBefore(target);
            gpu::DecodedGpuFrame decoded;
            if (!buffer.takeExact(target, decoded)) {
                const gpu::SourceDecoderSnapshot worker = engine.impl_->videoWorker->snapshot();
                if (worker.fatal) {
                    PreviewError failure = makeError(
                        PreviewErrorCategory::DecodeFailure, PreviewOperation::RenderDeviceAttach,
                        worker.lastError.empty()
                            ? "video decode workerがfatal終了しました"
                            : "video decode workerがfatal終了しました: " + worker.lastError);
                    failure.severity = PreviewErrorSeverity::FatalToSession;
                    failure.source = engine.impl_->publicVideoSource;
                    fatal = std::move(failure);
                    decoderFatal = true;
                } else if (worker.eof) {
                    Result<void> shutdown = engine.impl_->machine.requestShutdown();
                    if (shutdown) {
                        engine.impl_->schedulerEnabled = false;
                        engine.impl_->startWorkerShutdown();
                        engine.impl_->telemetrySnapshot.status.state =
                            PreviewEngineState::ShuttingDown;
                        playbackEnded = true;
                    }
                } else {
                    if (engine.impl_->telemetrySnapshot.droppedFrameCount !=
                        std::numeric_limits<std::uint64_t>::max()) {
                        ++engine.impl_->telemetrySnapshot.droppedFrameCount;
                    }
                    engine.impl_->telemetrySnapshot.currentSourceQueueDepth =
                        static_cast<std::uint32_t>(buffer.depth());
                }
            } else if (decoded.sourceId != engine.impl_->internalVideoSource ||
                       !engine.impl_->sourceRegistry.contains(decoded.sourceId)) {
                PreviewError error = makeError(PreviewErrorCategory::DecodeFailure,
                                               PreviewOperation::RenderDeviceAttach,
                                               "decode frameのsource identityが一致しません");
                error.severity = PreviewErrorSeverity::FatalToSession;
                fatal = error;
            } else {
                const auto token = engine.impl_->compositionState.latestAcceptedToken();
                const auto snapshot = engine.impl_->compositionState.latestAcceptedSnapshot();
                if (!token || !snapshot || snapshot->layers.size() != 1) {
                    return Result<RenderFrameResult>::failure(
                        makeError(PreviewErrorCategory::CompositionFailure,
                                  PreviewOperation::RenderDeviceAttach,
                                  "accepted single-layer compositionが見つかりません"));
                }
                if (engine.impl_->audioMasterActive && engine.impl_->audioWorker) {
                    const audio::AudioDecoderSnapshot decoder =
                        engine.impl_->audioWorker->snapshot();
                    if (!(decoder.sourceGeneration ==
                          engine.impl_->audioWorker->queue().generation())) {
                        // audio generationが揃わないframeをlatest/staleで代用しない。
                        ++engine.impl_->audioGenerationMismatchCount;
                        ++engine.impl_->staleSubstitutionCount;
                        return Result<RenderFrameResult>::success(result);
                    }
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
                    engine.impl_->distinctPresentedFrames.note(target);
                    ++engine.impl_->presentationSequence;
                    ++engine.impl_->telemetrySnapshot.presentedFrameCount;
                    engine.impl_->telemetrySnapshot.currentSourceQueueDepth =
                        static_cast<std::uint32_t>(buffer.depth());
                    const auto& counters = engine.impl_->compositor->counters();
                    engine.impl_->telemetrySnapshot.gpuRetirementCurrentDepth =
                        static_cast<std::uint32_t>(counters.retirementDepthAfterDrain);
                    engine.impl_->telemetrySnapshot.gpuRetirementPeakDepth =
                        static_cast<std::uint32_t>(counters.retirementDepthPeak);
                    if (engine.impl_->audioWorker) {
                        engine.impl_->telemetrySnapshot.audioUnderflowCount =
                            engine.impl_->audioWorker->queue().snapshot().underflowCount;
                    }
                    engine.impl_->telemetrySnapshot.status.position = {target};
                    engine.impl_->telemetrySnapshot.status.lastPresentedComposition = *token;
                    result.presented = true;
                    result.sourceFrame = target;
                    result.frame = {engine.impl_->presentationSequence, {target}, *token, 1};
                }
            }
        }

        if (fatal) {
            Result<void> accepted = engine.impl_->machine.recordFatal(*fatal);
            if (accepted) {
                if (decoderFatal && engine.impl_->telemetrySnapshot.decodeFailureCount !=
                                        std::numeric_limits<std::uint64_t>::max()) {
                    ++engine.impl_->telemetrySnapshot.decodeFailureCount;
                }
                engine.impl_->schedulerEnabled = false;
                engine.impl_->audioMasterActive = false;
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
    if (playbackEnded)
        engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});
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
        // render可視worker参照のdetachと、audio sink / audio worker / video workerの
        // joinを確認できなければrender teardownへ進まない (contract §12)。
        if (!engine.impl_->renderTeardownRequested || !engine.impl_->renderVisibleWorkersDetached ||
            !engine.impl_->workerJoined || !engine.impl_->audioSinkJoined ||
            !engine.impl_->audioWorkerJoined)
            return Result<bool>::success(false);

        // detach済みと記録しながらrender可視fieldに参照が残っている状態を成功にしない。
        // これが無いと`DetachRenderVisibleWorkerRefs`はbookkeepingだけで通ってしまう。
        // renderがまだ参照し得るresourceは解放せず、GPU完了未確認と同じ扱いにする。
        const bool detachViolation = engine.impl_->videoWorker || engine.impl_->audioSink ||
                                     engine.impl_->audioWorker || engine.impl_->audioClock;
        if (detachViolation)
            ++engine.impl_->lifecycleViolationCount;

        std::string error;
        bool drained = true;
        // pollingで複数回入るが、drainを開始するのは一度だけである。
        const bool startingDrain = !engine.impl_->gpuDrainStarted;
        engine.impl_->gpuDrainStarted = true;
        if (startingDrain) {
            engine.impl_->noteShutdownStepLocked(internal::ShutdownStep::FiniteGpuRetirementDrain);
        }
        if (engine.impl_->compositor) {
            if (startingDrain) {
                if (!engine.impl_->compositor->beginShutdown(2000, error))
                    drained = false;
            }
            if (drained) {
                const gpu::GpuCompositorShutdownStatus drainStatus =
                    engine.impl_->compositor->pollShutdown(error);
                if (drainStatus == gpu::GpuCompositorShutdownStatus::Pending)
                    return Result<bool>::success(false);
                drained = drainStatus == gpu::GpuCompositorShutdownStatus::Complete;
            }
        }
        if (detachViolation)
            drained = false;
        if (!drained) {
            PreviewError failure = makeError(
                PreviewErrorCategory::ShutdownFailure, PreviewOperation::Shutdown,
                detachViolation ? "detach済みと記録されているのにrender可視worker参照が残っています"
                : error.empty() ? "GPU retirement drainのtest faultを検出しました"
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
        engine.impl_->finalRuntimeDiagnostics.deviceLostCount = engine.impl_->deviceLostCount;
        if (gpu::SourceDecodeWorker* teardownVideo = engine.impl_->videoWorkerForTeardown()) {
            const auto mismatchCount =
                static_cast<std::uint64_t>(teardownVideo->snapshot().deviceMismatchCount);
            const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
            if (mismatchCount > maximum - engine.impl_->finalRuntimeDiagnostics.deviceLostCount)
                engine.impl_->finalRuntimeDiagnostics.deviceLostCount = maximum;
            else
                engine.impl_->finalRuntimeDiagnostics.deviceLostCount += mismatchCount;
        }
        if (audio::AudioDecodeWorker* teardownAudio = engine.impl_->audioWorkerForTeardown()) {
            const audio::AudioQueueSnapshot queue = teardownAudio->queue().snapshot();
            engine.impl_->telemetrySnapshot.audioUnderflowCount = queue.underflowCount;
            engine.impl_->finalRuntimeDiagnostics.audioUnderflowCount = queue.underflowCount;
        }
        if (audio::WasapiAudioSink* teardownSink = engine.impl_->audioSinkForTeardown()) {
            const audio::WasapiSnapshot endpoint = teardownSink->snapshot();
            // sink snapshotがdevice failureのauthority。engine側counterを足さない。
            engine.impl_->finalRuntimeDiagnostics.audioSinkDeviceFailureCount =
                endpoint.deviceFailureCount;
            engine.impl_->finalRuntimeDiagnostics.audioSessionVolume = endpoint.sessionVolume;
        }
        engine.impl_->finalRuntimeDiagnostics.audioTransportFailureCount =
            engine.impl_->audioTransportFailureCount;
        engine.impl_->finalRuntimeDiagnostics.audioDomainRejectCount =
            engine.impl_->audioDomainRejectCount;
        engine.impl_->finalRuntimeDiagnostics.audioMasterProjectionFailureCount =
            engine.impl_->audioMasterProjectionFailureCount;
        engine.impl_->finalRuntimeDiagnostics.audioGenerationMismatchCount =
            engine.impl_->audioGenerationMismatchCount;
        engine.impl_->finalRuntimeDiagnostics.renderVisibleWorkersDetached =
            engine.impl_->renderVisibleWorkersDetached;
        engine.impl_->finalRuntimeDiagnostics.audioSinkJoined = engine.impl_->audioSinkJoined;
        engine.impl_->finalRuntimeDiagnostics.audioWorkerJoined = engine.impl_->audioWorkerJoined;
        engine.impl_->finalRuntimeDiagnostics.registeredAudioSourceCount =
            engine.impl_->publicAudioSource ? 1U : 0U;
        if (drained) {
            engine.impl_->compositor.reset();
            engine.impl_->videoWorker.reset();
            engine.impl_->detachedWorkers.videoWorker.reset();
            // audio sink は queue/clock を参照するため、参照する側から解放する。
            engine.impl_->audioSink.reset();
            engine.impl_->audioWorker.reset();
            engine.impl_->audioClock.reset();
            engine.impl_->detachedWorkers.audioSink.reset();
            engine.impl_->detachedWorkers.audioWorker.reset();
            engine.impl_->detachedWorkers.audioClock.reset();
            if (engine.impl_->internalVideoSource.value != 0)
                engine.impl_->sourceRegistry.unregisterSource(engine.impl_->internalVideoSource);
            engine.impl_->renderDevice->release();
            engine.impl_->noteShutdownStepLocked(internal::ShutdownStep::ReleaseRenderTargetDevice);
            engine.impl_->nativeDeviceAttached = false;
            engine.impl_->deviceReleased = true;
            engine.impl_->renderTeardownComplete = true;
        } else {
            // GPU完了を確認できないresourceは解放しない。engineの論理lifecycleだけを
            // terminalへ進め、native runtime全体をprocess lifetimeまで隔離する。
            engine.impl_->unsafeGpuResourcesRetained = true;
            static auto* quarantineMutex = new std::mutex;
            static auto* retainedCompositors = new std::vector<std::unique_ptr<gpu::GpuCompositor>>;
            static auto* retainedWorkers =
                new std::vector<std::unique_ptr<gpu::SourceDecodeWorker>>;
            static auto* retainedDevices = new std::vector<std::unique_ptr<gpu::SharedD3D11Device>>;
            std::lock_guard<std::mutex> quarantineLock(*quarantineMutex);
            retainedCompositors->push_back(std::move(engine.impl_->compositor));
            // detach済みかどうかでownerが変わる。両方見るが、空のentryは積まない。
            if (engine.impl_->videoWorker)
                retainedWorkers->push_back(std::move(engine.impl_->videoWorker));
            if (engine.impl_->detachedWorkers.videoWorker)
                retainedWorkers->push_back(std::move(engine.impl_->detachedWorkers.videoWorker));
            retainedDevices->push_back(std::move(engine.impl_->renderDevice));
            // audio sink/worker/clockはjoin確認済みで、GPU completionに紐づかない。
            // holderの破棄順に頼らず、依存の逆順で明示的に解放する。
            engine.impl_->audioSink.reset();
            engine.impl_->audioWorker.reset();
            engine.impl_->audioClock.reset();
            engine.impl_->detachedWorkers.audioSink.reset();
            engine.impl_->detachedWorkers.audioWorker.reset();
            engine.impl_->detachedWorkers.audioClock.reset();
            engine.impl_->nativeDeviceAttached = false;
        }
        engine.impl_->finalRuntimeDiagnostics.workerJoined = engine.impl_->workerJoined;
        engine.impl_->finalRuntimeDiagnostics.renderTeardownComplete =
            engine.impl_->renderTeardownComplete;
        engine.impl_->finalRuntimeDiagnostics.deviceReleased = engine.impl_->deviceReleased;
        engine.impl_->finalRuntimeDiagnostics.unsafeGpuResourcesRetained =
            engine.impl_->unsafeGpuResourcesRetained;
        engine.impl_->finalRuntimeDiagnostics.distinctPresentedSourceFrameCount =
            engine.impl_->distinctPresentedFrames.count();
        engine.impl_->finalRuntimeDiagnostics.fullCpuReadbackCount =
            static_cast<std::uint64_t>(engine.impl_->readbacks.fullFrameReadbacks());
        Result<void> completed = engine.impl_->machine.completeTeardown();
        if (!completed)
            return Result<bool>::failure(completed.error());
        terminal = engine.impl_->machine.state();
        engine.impl_->noteShutdownStepLocked(internal::ShutdownStep::PublishShutdownComplete);
        engine.impl_->finalRuntimeDiagnostics.shutdownSequence = engine.impl_->shutdownSequence;
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

Result<void> PreviewRenderPort::completeRendererDetach(PreviewEngine& engine) {
    bool nativeRuntimeAttached = false;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        const PreviewEngineState state = engine.impl_->machine.state();
        if (state == PreviewEngineState::Shutdown || state == PreviewEngineState::Error)
            return Result<void>::success();

        // state判定とdetach公開を同じcritical sectionに置く。これによりshutdown側は、
        // renderer authorityが残る状態かstandby authorityが必要な状態かを必ず判別できる。
        if (state != PreviewEngineState::ShuttingDown) {
            engine.impl_->rendererDetached = true;
            engine.impl_->renderThread.reset();
            return Result<void>::success();
        }
        nativeRuntimeAttached = engine.impl_->nativeDeviceAttached;
    }

    if (!nativeRuntimeAttached)
        return completeTeardown(engine);

    for (;;) {
        Result<bool> completed = completeRuntimeTeardown(engine);
        if (!completed)
            return Result<void>::failure(completed.error());
        if (completed.value())
            return Result<void>::success();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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

Result<void> PreviewRenderPort::injectFatal(PreviewEngine& engine, PreviewError error,
                                            FatalDiagnostic diagnostic) {
    PreviewError recorded = error;
    recorded.severity = PreviewErrorSeverity::FatalToSession;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        Result<void> accepted = engine.impl_->machine.recordFatal(recorded);
        if (!accepted) {
            return accepted;
        }
        if (diagnostic == FatalDiagnostic::DeviceLost &&
            engine.impl_->deviceLostCount != std::numeric_limits<std::uint64_t>::max()) {
            ++engine.impl_->deviceLostCount;
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

Result<void> PreviewRenderPort::reportRenderTargetFailure(PreviewEngine& engine, long hresult) {
    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "QRhi render target viewを生成できませんでした (HRESULT=0x%08lX)",
                  static_cast<unsigned long>(hresult));
    PreviewError error = makeError(PreviewErrorCategory::DeviceFailure,
                                   PreviewOperation::RenderDeviceAttach, detail);
    error.severity = PreviewErrorSeverity::FatalToSession;
    return injectFatal(engine, std::move(error));
}

Result<void> PreviewRenderPort::reportDeviceLost(PreviewEngine& engine, long hresult) {
    char detail[160];
    std::snprintf(detail, sizeof detail,
                  "D3D11 device lostを検出しました (GetDeviceRemovedReason=0x%08lX)",
                  static_cast<unsigned long>(hresult));
    PreviewError error = makeError(PreviewErrorCategory::DeviceFailure,
                                   PreviewOperation::RenderDeviceAttach, detail);
    error.severity = PreviewErrorSeverity::FatalToSession;
    return injectFatal(engine, std::move(error), FatalDiagnostic::DeviceLost);
}

Result<void> PreviewRenderPort::reportUnsupportedRenderBackend(PreviewEngine& engine) {
    PreviewError error =
        makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                  "preview engineはQRhi D3D11 backendを必須とします");
    error.severity = PreviewErrorSeverity::FatalToSession;
    return injectFatal(engine, std::move(error));
}

Result<void> PreviewRenderPort::reportMissingNativeD3D11Handles(PreviewEngine& engine) {
    PreviewError error =
        makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                  "QRhi D3D11 native handlesからdevice/contextを取得できませんでした");
    error.severity = PreviewErrorSeverity::FatalToSession;
    return injectFatal(engine, std::move(error));
}

Result<void> PreviewRenderPort::reportEngineReplacement(PreviewEngine& engine) {
    PreviewError error =
        makeError(PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                  "attach済みrendererのengine差し替えを検出しました");
    error.severity = PreviewErrorSeverity::FatalToSession;
    return injectFatal(engine, std::move(error));
}

bool PreviewRenderPort::nativeRuntimeAttached(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    return engine.impl_->nativeDeviceAttached;
}

Result<void> PreviewRenderPort::injectGpuDrainFailureForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->nativeDeviceAttached || engine.impl_->renderTeardownComplete) {
        return invalidState(PreviewOperation::Shutdown,
                            "GPU drain faultはactive native runtimeでのみ設定できます");
    }
    engine.impl_->compositor->setTestFaults(
        {gpu::GpuCompositorInitializeFault::None, -1, false, true});
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectDecoderFatalForTest(PreviewEngine& engine,
                                                          std::string detail) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->videoWorker ||
        engine.impl_->machine.state() != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "decoder fatal faultは再生中のvideo workerにのみ設定できます");
    }
    engine.impl_->videoWorker->injectFatalForTest(detail);
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectDecoderEofForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->videoWorker ||
        engine.impl_->machine.state() != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "decoder EOF faultは再生中のvideo workerにのみ設定できます");
    }
    engine.impl_->videoWorker->injectEofForTest();
    return Result<void>::success();
}

P5CRuntimeDiagnostics PreviewRenderPort::runtimeDiagnostics(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    P5CRuntimeDiagnostics result = engine.impl_->finalRuntimeDiagnostics;
    result.nativeDeviceAttached = engine.impl_->nativeDeviceAttached;
    result.workerJoined = engine.impl_->workerJoined;
    result.renderTeardownComplete = engine.impl_->renderTeardownComplete;
    result.deviceReleased = engine.impl_->deviceReleased;
    result.unsafeGpuResourcesRetained = engine.impl_->unsafeGpuResourcesRetained;
    result.registeredVideoSourceCount = engine.impl_->sourceRegistry.registeredSourceCount();
    result.distinctPresentedSourceFrameCount = engine.impl_->distinctPresentedFrames.count();
    result.staleSubstitutionCount = engine.impl_->staleSubstitutionCount;
    result.lifecycleViolationCount = engine.impl_->lifecycleViolationCount;
    result.shutdownSequence = engine.impl_->shutdownSequence;
    result.audioMasterActive = engine.impl_->audioMasterActive;
    result.renderVisibleWorkersDetached = engine.impl_->renderVisibleWorkersDetached;
    result.audioSinkJoined = engine.impl_->audioSinkJoined;
    result.audioWorkerJoined = engine.impl_->audioWorkerJoined;
    result.registeredAudioSourceCount = engine.impl_->publicAudioSource ? 1U : 0U;
    result.audioMasterProjectionFailureCount = engine.impl_->audioMasterProjectionFailureCount;
    result.audioGenerationMismatchCount = engine.impl_->audioGenerationMismatchCount;
    result.audioTransportFailureCount = engine.impl_->audioTransportFailureCount;
    result.audioDomainRejectCount = engine.impl_->audioDomainRejectCount;
    if (audio::AudioDecodeWorker* diagAudio = engine.impl_->audioWorkerForTeardown()) {
        result.audioUnderflowCount = diagAudio->queue().snapshot().underflowCount;
    }
    if (audio::WasapiAudioSink* diagSink = engine.impl_->audioSinkForTeardown()) {
        // 解放後はfinal snapshotが確定値を持つため、そのまま残す。
        const audio::WasapiSnapshot endpoint = diagSink->snapshot();
        result.audioSinkDeviceFailureCount = endpoint.deviceFailureCount;
        result.audioSessionVolume = endpoint.sessionVolume;
    }
    result.fullCpuReadbackCount =
        static_cast<std::uint64_t>(engine.impl_->readbacks.fullFrameReadbacks());
    result.deviceLostCount = std::max(result.deviceLostCount, engine.impl_->deviceLostCount);
    if (gpu::SourceDecodeWorker* diagVideo = engine.impl_->videoWorkerForTeardown()) {
        const gpu::SourceDecoderSnapshot decoder = diagVideo->snapshot();
        result.d3d11vaActive = decoder.open && decoder.softwareFrameRejectCount == 0;
        result.decodeRenderSameDevice =
            decoder.decodeDevicePointer ==
            reinterpret_cast<std::uintptr_t>(engine.impl_->renderDevice->device());
        result.softwareFallbackCount = static_cast<std::uint64_t>(decoder.softwareFrameRejectCount);
        const auto mismatchCount = static_cast<std::uint64_t>(decoder.deviceMismatchCount);
        const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
        if (mismatchCount > maximum - result.deviceLostCount)
            result.deviceLostCount = maximum;
        else
            result.deviceLostCount += mismatchCount;
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

Result<void> PreviewRenderPort::setVerificationAudioVolume(PreviewEngine& engine, float volume) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!(volume >= 0.0F) || volume > 1.0F) {
        return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                               PreviewOperation::Initialize,
                                               "session volumeは0.0〜1.0で指定してください"));
    }
    if (engine.impl_->audioSink) {
        // endpointのopen後に変えても適用されない。黙って無視しない。
        return invalidState(PreviewOperation::Initialize,
                            "audio source登録後にsession volumeを変更できません");
    }
    engine.impl_->audioSessionVolume = volume;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectAudioSinkPauseFaultForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->audioSink) {
        return invalidState(PreviewOperation::Pause,
                            "audio sourceが未登録のためsink pause faultを注入できません");
    }
    engine.impl_->audioSink->injectPauseFaultForTest();
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectAudioClockStallForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->audioSink || !engine.impl_->audioClock) {
        return invalidState(PreviewOperation::Play,
                            "audio sourceが未登録のためaudio clock stallを注入できません");
    }
    engine.impl_->audioClockStallInjected = true;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectAudioSinkRenderFaultForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->audioSink) {
        return invalidState(PreviewOperation::Play,
                            "audio sourceが未登録のためsink render faultを注入できません");
    }
    // engineへ完成したerrorを渡さない。sink自身にdevice failureを起こさせ、
    // product側のpolling経路が昇格できるかどうかを検査する。
    engine.impl_->audioSink->injectRenderFaultForTest();
    return Result<void>::success();
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
