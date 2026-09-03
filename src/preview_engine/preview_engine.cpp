#include "core/checked_output_timebase.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/audio_video_scheduler.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/source_decode_worker.h"
#include "media/gpu_preview/source_registry.h"
#include "preview_engine/preview_engine_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <map>
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

bool validEffectDestination(const PreviewNormalizedRect& rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width > 0.0F && rect.height > 0.0F;
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

std::vector<PreviewEvent> EventMailbox::snapshot() const {
    return {events_.begin(), events_.end()};
}

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
    if (snapshot->layers.size() > capabilities.configuredMaxCompositionLayers) {
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
    if (distinctSources.size() > capabilities.configuredMaxActiveVideoSources) {
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::UnsupportedCapability,
                             "qualified active video source countを超えています"));
    }

    CompositionSnapshot canonical = *snapshot;
    for (PreviewCompositionLayer& layer : canonical.layers) {
        if (!(layer.effectsEnabled ? validEffectDestination(layer.destination)
                                   : validRect(layer.destination)) ||
            !validRect(layer.sourceRect)) {
            return Result<AcceptedComposition>::failure(
                compositionError(PreviewErrorCategory::CompositionFailure,
                                 "normalized rectangleがinvalidです", layer.source));
        }
        if (!std::isfinite(layer.opacity) || layer.opacity < 0.0F || layer.opacity > 1.0F) {
            return Result<AcceptedComposition>::failure(
                compositionError(PreviewErrorCategory::CompositionFailure,
                                 "opacityが[0,1]の範囲外です", layer.source));
        }
        if (!std::isfinite(layer.rotationDegrees) ||
            (layer.effectsEnabled &&
             (layer.sourceInFrame < 0 || layer.sourceDurationFrames <= 0 ||
              layer.fadeInFrames < 0 || layer.fadeOutFrames < 0 ||
              layer.fadeInFrames > layer.sourceDurationFrames ||
              layer.fadeOutFrames > layer.sourceDurationFrames ||
              layer.fadeInFrames > layer.sourceDurationFrames - layer.fadeOutFrames))) {
            return Result<AcceptedComposition>::failure(compositionError(
                PreviewErrorCategory::CompositionFailure,
                "source-native effect timingまたはrotationが不正です", layer.source));
        }
        layer.destination = canonicalRect(layer.destination);
        layer.sourceRect = canonicalRect(layer.sourceRect);
        layer.opacity = canonicalFloat(layer.opacity);
        layer.rotationDegrees = canonicalFloat(layer.rotationDegrees);
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

void CompositionAcceptanceState::markPresented(
    AcceptedComposition composition, std::shared_ptr<const CompositionSnapshot> snapshot) {
    lastPresentedToken_ = composition;
    lastPresentedSnapshot_ = std::move(snapshot);
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

const std::shared_ptr<const CompositionSnapshot>&
CompositionAcceptanceState::lastPresentedSnapshot() const {
    return lastPresentedSnapshot_;
}

bool CompositionAcceptanceState::referencesSource(PreviewSourceId source) const {
    const auto references = [source](const std::shared_ptr<const CompositionSnapshot>& snapshot) {
        if (!snapshot)
            return false;
        for (const PreviewCompositionLayer& layer : snapshot->layers) {
            if (layer.source == source)
                return true;
        }
        return false;
    };
    // pendingとactiveの両方を見る。提示済みのcompositionを差し替えただけでは、
    // 実際に新しいcompositionを提示するまで古い参照は外れていない。
    return references(latestAcceptedSnapshot_) || references(lastPresentedSnapshot_);
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
    // audio transport command (play/pause/stop) を直列化する専用mutex。
    // engine mutexとは別物で、engine mutexを保持したまま取らない。
    // これが無いと、seek resumeのplay()とshutdownのstop()が交錯し、
    // stop済みのworker/sinkをplayが復活させ得る。
    std::mutex audioTransportMutex;
    internal::PreviewStateMachine machine;
    internal::EventMailbox mailbox{32};
    std::shared_ptr<PreviewEventDispatcher> dispatcher;
    std::weak_ptr<PreviewEventSink> sink;
    const std::thread::id controlThread;
    std::uint64_t sinkGeneration = 0;
    bool dispatchScheduled = false;
    PreviewCapabilities capability = [] {
        PreviewCapabilities value;
        value.configuredMaxActiveVideoSources = 2;
        value.configuredMaxCompositionLayers = 2;
        // P5-D2でaudio-master transportを接続したため、qualified audio domainを公開する。
        value.configuredMaxActiveAudioSources = 1;
        value.configuredAudioSampleRate = audio::kInternalSampleRate;
        value.configuredAudioChannelCount = audio::kInternalChannels;
        return value;
    }();
    PreviewTelemetry telemetrySnapshot;
    PreviewDeviceInfo deviceSnapshot;

    // P5-C private backend。public headerへmedia/native型を漏らさない。
    std::unique_ptr<gpu::SharedD3D11Device> renderDevice =
        std::make_unique<gpu::SharedD3D11Device>();
    gpu::SourceRegistry sourceRegistry;
    gpu::ReadbackCounters readbacks;
    std::unique_ptr<gpu::GpuCompositor> compositor;
    std::unordered_map<std::uint64_t, internal::EligibleSource> eligibleSources;
    internal::CompositionAcceptanceState compositionState;

    // P5-E1: video sourceのownership。単数fieldではなくtableで持つ。
    // `std::map`にしているのは走査順を`PreviewSourceId`昇順で決定論的に
    // 固定するためであり、hashの都合でshutdown orderが揺れないようにする。
    struct VideoSourceEntry {
        gpu::SourceId internal{};
        std::unique_ptr<gpu::SourceDecodeWorker> worker;
    };

    std::map<std::uint64_t, VideoSourceEntry> videoSources;

    // P5-E1: compositionのepoch authority。engineは`CompositionEpoch`を
    // 直書きせず、coordinatorが採番した値をそのまま運ぶ。
    // **session中に作り直さない。** 作り直すとinstanceごとに別のepoch
    // namespaceができ、古い`ComposedFrame`のepochと衝突し得る。
    // 参照source集合が変わるcomposition transitionは
    // `adoptCompositionRuntimeSnapshot()`で同一instanceのまま採用する。
    std::unique_ptr<gpu::CompositorCoordinator> coordinator;
    // pairerはbufferをraw pointerで握るので、こちらは参照source集合が
    // 変わるたびに作り直す。epoch authorityではない。
    std::unique_ptr<gpu::ExactFramePairer> pairer;
    // pairerを組んだときの参照source (public ID昇順)。
    std::vector<std::uint64_t> coordinatorSources;
    std::uint64_t staleCompositionEpochRejectCount = 0;
    std::int64_t lastStaleCompositionRejectedFrame = -1;
    // 提示直前のstale epoch拒否を製品経路で検査するためのtest seam。
    // 完成したerrorを注入するのではなく、compose後・validate前に
    // `CompositionEpoch`だけを進める。
    bool compositionEpochAdvanceInjected = false;

    // engine lockを持たない窓をtestから決定論的に止めるbarrier。
    // engine mutexとは別のmutexで守る (止める窓ではengine lockを持っていない)。
    struct TestBarrier {
        std::mutex mutex;
        std::condition_variable changed;
        bool armed = false;
        bool entered = false;
        bool released = false;

        bool arm() {
            std::lock_guard<std::mutex> lock(mutex);
            if (armed)
                return false;
            armed = true;
            entered = false;
            released = false;
            return true;
        }

        // armされていなければ何もしない。既定値では一切動作を変えない。
        void enter() {
            std::unique_lock<std::mutex> lock(mutex);
            if (!armed)
                return;
            entered = true;
            changed.notify_all();
            changed.wait(lock, [this] { return released; });
            armed = false;
            entered = false;
            released = false;
        }

        bool waitEntered(int timeoutMs) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                    [this] { return entered; });
        }

        void release() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                released = true;
            }
            changed.notify_all();
        }
    };

    // `removeSource()`のaudio停止フェーズ (engine lockを持たない窓)。
    TestBarrier removalBarrier;
    // fatalをcommitしてunlockした直後、dispatchをflushする前の窓。
    // state commitとmailbox insertionがlinearizeされていれば、この窓で
    // teardownが先にterminalへ進んでもevent順序は逆転しない。
    TestBarrier fatalPublishBarrier;

    void enterSourceRemovalBarrierForTest() { removalBarrier.enter(); }

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
    // addSource で受け取った audio source の timeline 上の位置ずれ (sample)。
    // output frame <-> audio media sample の換算はこの 1 つの値だけで補正する。
    std::int64_t audioSampleOffset = 0;
    bool audioMasterActive = false;
    bool audioSinkJoined = true;
    bool audioWorkerJoined = true;
    std::uint64_t audioMasterProjectionFailureCount = 0;
    std::uint64_t audioGenerationMismatchCount = 0;
    std::uint64_t audioTransportFailureCount = 0;
    std::uint64_t audioDomainRejectCount = 0;
    // audio source登録中にQPC/wall-clock masterが選ばれた回数。製品経路では常に0である。
    std::uint64_t videoMasterQpcFallbackCount = 0;
    bool audioClockStallInjected = false;
    bool videoMasterQpcFallbackInjected = false;
    bool seekPresentationStallInjected = false;
    bool seekAudioGenerationMismatchInjected = false;
    std::optional<PreviewSourceId> seekVideoGenerationMismatchInjected;
    std::vector<internal::ShutdownStep> shutdownSequence;
    bool renderVisibleWorkersDetached = false;

    // detach後のowner。renderから到達できるfieldではない。
    // memberは宣言の逆順で破棄される。`WasapiAudioSink`はqueue (worker所有) と
    // clockをreferenceで保持し、destructorから`stop()` -> `clock_.stop()`を呼ぶ。
    // したがってsinkが最後に壊れるとuse-after-freeになる。正常teardownの明示reset順
    // (sink -> worker -> clock) と一致するよう、宣言はその逆順に並べる。
    // `Impl`本体のaudio memberも同じ理由で clock -> worker -> sink の順に宣言している。
    struct DetachedWorkers {
        std::vector<std::unique_ptr<gpu::SourceDecodeWorker>> videoWorkers;
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
        for (auto& [publicId, entry] : videoSources) {
            (void)publicId;
            if (entry.worker)
                detachedWorkers.videoWorkers.push_back(std::move(entry.worker));
        }
        // pairerはbufferをraw pointerで握っている。worker本体より先に手放す。
        // coordinatorはpointerを持たず、epoch lineageのownerなので残す。
        pairer.reset();
        coordinatorSources.clear();
        detachedWorkers.audioSink = std::move(audioSink);
        detachedWorkers.audioWorker = std::move(audioWorker);
        detachedWorkers.audioClock = std::move(audioClock);
        renderVisibleWorkersDetached = true;
        noteShutdownStepLocked(internal::ShutdownStep::DetachRenderVisibleWorkerRefs);
    }

    // diagnostics/teardownはdetach後も実体を参照する。render pathはこれを使わない。
    // 走査順は`PreviewSourceId`昇順 -> detach順で決定論的である。
    std::vector<gpu::SourceDecodeWorker*> videoWorkersForTeardown() const {
        std::vector<gpu::SourceDecodeWorker*> workers;
        for (const auto& [publicId, entry] : videoSources) {
            (void)publicId;
            if (entry.worker)
                workers.push_back(entry.worker.get());
        }
        for (const auto& detached : detachedWorkers.videoWorkers) {
            if (detached)
                workers.push_back(detached.get());
        }
        return workers;
    }

    // render pathから見えるworkerだけを返す。detach後は空になる。
    std::vector<gpu::SourceDecodeWorker*> videoWorkersLocked() const {
        std::vector<gpu::SourceDecodeWorker*> workers;
        for (const auto& [publicId, entry] : videoSources) {
            (void)publicId;
            if (entry.worker)
                workers.push_back(entry.worker.get());
        }
        return workers;
    }

    bool hasVideoWorkerLocked() const {
        for (const auto& [publicId, entry] : videoSources) {
            (void)publicId;
            if (entry.worker)
                return true;
        }
        return false;
    }

    // internal `gpu::SourceId`からpublic IDを逆引きする。errorのsource付与に使う。
    std::optional<PreviewSourceId> publicIdForInternalLocked(gpu::SourceId internal) const {
        for (const auto& [publicId, entry] : videoSources) {
            if (entry.internal == internal)
                return PreviewSourceId{publicId};
        }
        return std::nullopt;
    }

    // coordinatorへconfigure済みの参照sourceに対応するworkerを、pairerへ渡した
    // bufferと同じ順序で返す。
    std::vector<gpu::SourceDecodeWorker*> referencedVideoWorkersLocked() const {
        std::vector<gpu::SourceDecodeWorker*> workers;
        for (std::uint64_t publicId : coordinatorSources) {
            const auto entry = videoSources.find(publicId);
            if (entry != videoSources.end() && entry->second.worker)
                workers.push_back(entry->second.worker.get());
        }
        return workers;
    }

    // composeしたframeのsource identityが、engineが登録しているものと一致するか。
    // pairerは自前のbufferしか触らないので構造的には満たされるが、identityの
    // 取り違えは過去に実際に起きた失敗なので製品経路でも検査する。
    bool composedIdentityValidLocked(const gpu::ComposedFrame& composed) const {
        for (const auto& layer : composed.layers) {
            if (!sourceRegistry.contains(layer.frame.sourceId))
                return false;
            if (!publicIdForInternalLocked(layer.frame.sourceId))
                return false;
        }
        return !composed.layers.empty();
    }

    // accepted snapshotをcoordinatorのlayoutへ写す。
    // `CompositionSnapshot::layers`のvector順が背面 -> 前面のz順である
    // (preview-engine-contract.md §7)。public APIへzOrder fieldを増やさない。
    // 未登録sourceを含む場合はnulloptを返す。
    std::optional<std::vector<gpu::LayerLayout>>
    buildLayoutLocked(const CompositionSnapshot& snapshot) const {
        std::vector<gpu::LayerLayout> layout;
        layout.reserve(snapshot.layers.size());
        for (std::size_t i = 0; i < snapshot.layers.size(); ++i) {
            const PreviewCompositionLayer& layer = snapshot.layers[i];
            const auto entry = videoSources.find(layer.source.value);
            if (entry == videoSources.end() || !entry->second.worker)
                return std::nullopt;
            gpu::LayerLayout mapped;
            mapped.sourceId = entry->second.internal;
            mapped.destination = {layer.destination.x, layer.destination.y, layer.destination.width,
                                  layer.destination.height};
            mapped.sourceUv = {layer.sourceRect.x, layer.sourceRect.y, layer.sourceRect.width,
                               layer.sourceRect.height};
            mapped.opacity = layer.opacity;
            mapped.zOrder = static_cast<int>(i);
            mapped.effectsEnabled = layer.effectsEnabled;
            mapped.rotationDegrees = layer.rotationDegrees;
            mapped.sourceInFrame = layer.sourceInFrame;
            mapped.sourceDurationFrames = layer.sourceDurationFrames;
            mapped.fadeInFrames = layer.fadeInFrames;
            mapped.fadeOutFrames = layer.fadeOutFrames;
            layout.push_back(mapped);
        }
        return layout;
    }

    // accepted tokenとsnapshotをcompositionのruntime authorityへ反映する。
    // coordinatorはsession中に作り直さない。source集合ごと変わるtransitionも
    // `adoptCompositionRuntimeSnapshot()`で同一instanceのまま採用するので、
    // `CompositionEpoch`のlineageが切れない。
    // 戻り値は「fatalにすべきerror」。
    std::optional<PreviewError> syncCompositionRuntimeLocked(const AcceptedComposition& token,
                                                             const CompositionSnapshot& snapshot) {
        const auto compositionFailure = [](std::string detail) {
            return makeError(PreviewErrorCategory::CompositionFailure,
                             PreviewOperation::RenderDeviceAttach, std::move(detail),
                             PreviewErrorSeverity::FatalToSession);
        };

        const auto layout = buildLayoutLocked(snapshot);
        if (!layout)
            return compositionFailure("accepted compositionが未登録sourceを参照しています");

        if (snapshot.layers.empty()) {
            pairer.reset();
            coordinatorSources.clear();
            return std::nullopt;
        }

        std::vector<std::uint64_t> referenced;
        for (const auto& layer : snapshot.layers) {
            if (std::find(referenced.begin(), referenced.end(), layer.source.value) ==
                referenced.end()) {
                referenced.push_back(layer.source.value);
            }
        }
        std::sort(referenced.begin(), referenced.end());

        std::map<gpu::SourceId, gpu::SourceGeneration> generations;
        std::vector<gpu::SourceFrameBuffer*> buffers;
        for (std::uint64_t publicId : referenced) {
            const auto entry = videoSources.find(publicId);
            if (entry == videoSources.end() || !entry->second.worker)
                return compositionFailure("accepted compositionが未登録sourceを参照しています");
            gpu::SourceFrameBuffer& buffer = entry->second.worker->buffer();
            generations.emplace(entry->second.internal, buffer.generation());
            buffers.push_back(&buffer);
        }

        if (!coordinator)
            coordinator = std::make_unique<gpu::CompositorCoordinator>();
        if (coordinator->adoptCompositionRuntimeSnapshot(gpu::CompositionStateId{token.id.value},
                                                         *layout, std::move(generations)) ==
            gpu::CompositionStateAdoptionResult::Rejected) {
            return compositionFailure("composition snapshotをcoordinatorへ適用できません");
        }

        if (!pairer || referenced != coordinatorSources) {
            // 参照source集合が変わった。pairerだけを組み直す。
            pairer.reset();
            auto rebuilt =
                std::make_unique<gpu::ExactFramePairer>(std::move(buffers), *coordinator);
            if (!rebuilt->valid())
                return compositionFailure("composition参照sourceのbuffer集合が不正です");
            pairer = std::move(rebuilt);
            coordinatorSources = std::move(referenced);
        }
        return std::nullopt;
    }

    // 提示直前のstale epoch拒否を製品経路で踏ませるためのtest seam。
    // 完成したerrorを注入するのではなく、compose後・validate前に
    // `CompositionEpoch`だけを1つ進める。composition state / layout / generationは
    // 触らないので、engineから見たcompositionは何も変わらない。
    // 進めたepochは次のcompose以降そのまま使われるので、engineは自己回復する。
    bool advanceCompositionEpochForTestLocked() {
        return coordinator && coordinator->advanceCompositionEpochForTest();
    }

    audio::AudioDecodeWorker* audioWorkerForTeardown() const {
        return audioWorker ? audioWorker.get() : detachedWorkers.audioWorker.get();
    }

    audio::WasapiAudioSink* audioSinkForTeardown() const {
        return audioSink ? audioSink.get() : detachedWorkers.audioSink.get();
    }

    // 製品既定は unity。検証アプリだけが下げる。
    float audioSessionVolume = 1.0F;

    // P5-D3: 受理済みseekの進行状態。`seek()`はここへ積むだけで完了を待たない。
    // 完了判定はrender threadが「要求frameを実際に提示できたか」で行う。
    struct PendingSeek {
        bool active = false;
        PreviewPosition target;
        std::int64_t audioSample = 0;
        std::map<std::uint64_t, gpu::SeekTicket> videoTickets;
        std::map<std::uint64_t, std::int64_t> expectedSourceFrames;
        audio::AudioSeekTicket audioTicket;
        bool audioReady = false;
        bool decodeReady = false;
        bool resumePlaying = false;
        std::map<std::uint64_t, gpu::SourceGeneration> expectedVideoGenerations;
        audio::SourceGeneration expectedAudioGeneration{};
        std::chrono::steady_clock::time_point deadline;
    };

    PendingSeek pendingSeek;
    std::uint64_t seekRequestCount = 0;
    std::uint64_t seekVideoRequestAcceptedCount = 0;
    std::uint64_t seekDecodeReadyCount = 0;
    std::uint64_t seekCompletedCount = 0;
    std::uint64_t seekAwaitingPresentationCount = 0;
    std::uint64_t seekStaleGenerationRejectCount = 0;
    std::uint64_t seekCancelledByShutdownCount = 0;
    std::int64_t lastSeekTargetFrame = -1;
    std::int64_t lastSeekPresentedFrame = -1;

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

    // output frameのmasterは`audio::acceptsVideoMasterSource()`だけが決める。判定を
    // ここで再実装しない。projectionが成立しない場合はQPC/steady_clockへ退避せず、
    // `AudioFailure`として表面化する。
    // frame換算そのものは`CheckedOutputTimebase`へ一本化し、ここで再実装しない。
    SchedulerTarget schedulerTargetLocked(std::chrono::steady_clock::time_point now) {
        SchedulerTarget result;
        if (!timebase) {
            result.error =
                makeError(PreviewErrorCategory::InvalidState, PreviewOperation::RenderDeviceAttach,
                          "output timebaseが未確定のままscheduleしようとしました");
            return result;
        }

        // この関数はstateが`Playing`かつscheduler有効なときだけ呼ばれる。したがって
        // audio sourceを登録していればmasterはaudio device clockでなければならない。
        // ここでwall-clockが選ばれることは暗黙fallbackであり、成功へ変えない。
        const audio::VideoMasterSource masterSource =
            (audioMasterActive && !videoMasterQpcFallbackInjected)
                ? audio::VideoMasterSource::AudioDeviceClock
                : audio::VideoMasterSource::Qpc;
        const bool masterAccepted = audio::acceptsVideoMasterSource(masterSource);
        if (publicAudioSource && !masterAccepted) {
            ++videoMasterQpcFallbackCount;
            result.error =
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::RenderDeviceAttach,
                          "audio source登録中にQPC masterへ退避しようとしました",
                          PreviewErrorSeverity::FatalToSession);
            result.error.source = publicAudioSource;
            return result;
        }

        if (masterAccepted) {
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
            if (clock.mediaSamplePosition < audioSampleOffset) {
                projectionFailure("audio master clockがaudio clipの開始より前を指しています");
                return result;
            }
            const auto frame =
                timebase->schedulerOutputFrame(clock.mediaSamplePosition - audioSampleOffset);
            if (!frame) {
                projectionFailure("audio sample positionをoutput frameへ換算できません");
                return result;
            }
            result.valid = true;
            result.frame = frame.value();
            return result;
        }

        // audio sourceが無いvideo-only経路 (P5-C) だけがwall-clockを使う。ここには
        // audio clockが存在しないのでwall-clockがqualified masterであり、退避ではない。
        // したがって`videoMasterQpcFallbackCount`は増やさない。
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
        workerJoined = true;
        for (gpu::SourceDecodeWorker* worker : videoWorkersLocked())
            workerJoined = workerJoined && worker->joined();
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
            std::vector<gpu::SourceDecodeWorker*> workers;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                audioEndpoint = self->audioSink;
                audioDecoder = self->audioWorker;
                workers = self->videoWorkersLocked();
            }
            std::string ignored;
            if (audioEndpoint) {
                // seek resumeのplay()と交錯させない。engine mutexは保持しない。
                std::lock_guard<std::mutex> transport(self->audioTransportMutex);
                audioEndpoint->pause(ignored);
                audioEndpoint->stop();
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->noteShutdownStepLocked(internal::ShutdownStep::StopAudioSink);
            }
            if (audioDecoder) {
                std::lock_guard<std::mutex> transport(self->audioTransportMutex);
                audioDecoder->stop();
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->noteShutdownStepLocked(internal::ShutdownStep::StopAudioDecodeWorker);
            }
            for (gpu::SourceDecodeWorker* worker : workers)
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
                self->workerJoined = true;
                for (gpu::SourceDecodeWorker* worker : workers)
                    self->workerJoined = self->workerJoined && worker->joined();
                self->noteShutdownStepLocked(internal::ShutdownStep::VerifyJoins);
                joinsVerified =
                    self->workerJoined && self->audioSinkJoined && self->audioWorkerJoined;
            }

            // RequestRenderTeardownをpublishすると、render threadがdetached audioを
            // 解放し得る。その前にshutdown thread側のstrong ownerを必ず落とす。
            // detachedWorkersが所有しているので、ここでresetしても実体は生存する。
            audioEndpoint.reset();
            audioDecoder.reset();
            // video workerはdetachedWorkersが所有しており、この生ポインタは
            // teardown後にdanglingになる。publish前に手放す。
            workers.clear();

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

    // seek workerのcompletionを非blockingで回収する。decode readyになっても
    // completeにはしない。completeはexact frameを提示できた時点だけである。
    // 戻り値は「fatalにすべきerror」。
    std::optional<PreviewError> advanceSeekLocked(std::chrono::steady_clock::time_point now) {
        PendingSeek& pending = pendingSeek;
        if (!pending.active)
            return std::nullopt;

        const auto seekFailure = [&](std::string detail,
                                     std::optional<PreviewSourceId> source = std::nullopt) {
            PreviewError failure =
                makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                          std::move(detail), PreviewErrorSeverity::FatalToSession);
            failure.source = source;
            return failure;
        };

        if (!pending.decodeReady) {
            for (const auto& [publicId, ticketValue] : pending.videoTickets) {
                const auto entry = videoSources.find(publicId);
                if (entry == videoSources.end() || !entry->second.worker)
                    return seekFailure("video seek中にsourceが解除されました",
                                       PreviewSourceId{publicId});
                if (pending.expectedVideoGenerations.contains(publicId))
                    continue;
                gpu::SeekCompletion completion;
                const gpu::SeekWaitResult result =
                    entry->second.worker->waitSeek(ticketValue, 0, completion);
                if (result == gpu::SeekWaitResult::StaleTicket)
                    return seekFailure("video seek completionのticketが一致しません",
                                       PreviewSourceId{publicId});
                if (result == gpu::SeekWaitResult::Ready) {
                    if (completion.status != gpu::SeekCompletionStatus::Completed) {
                        return seekFailure("video seekが完了しませんでした: " + completion.error,
                                           PreviewSourceId{publicId});
                    }
                    // decodeしたframeが要求と違えばexact seekではない。
                    const auto expectedFrame = pending.expectedSourceFrames.find(publicId);
                    if (expectedFrame == pending.expectedSourceFrames.end() ||
                        completion.decodedFrameNumber != expectedFrame->second) {
                        return seekFailure(
                            "video seekがrequested frameを返しませんでした (requested=" +
                                (expectedFrame == pending.expectedSourceFrames.end()
                                     ? std::string("missing")
                                     : std::to_string(expectedFrame->second)) +
                                ", decoded=" + std::to_string(completion.decodedFrameNumber) + ")",
                            PreviewSourceId{publicId});
                    }
                    pending.expectedVideoGenerations.emplace(publicId, completion.sourceGeneration);
                }
            }
            if (!pending.audioReady && audioWorker) {
                audio::AudioSeekCompletion completion;
                const audio::AudioSeekWaitResult result =
                    audioWorker->waitSeek(pending.audioTicket, 0, completion);
                if (result == audio::AudioSeekWaitResult::StaleTicket)
                    return seekFailure("audio seek completionのticketが一致しません");
                if (result == audio::AudioSeekWaitResult::Ready) {
                    if (!completion.completed)
                        return seekFailure("audio seekが完了しませんでした: " + completion.error);
                    if (completion.firstOutputSample != pending.audioSample) {
                        return seekFailure(
                            "audio seekがrequested sampleを返しませんでした (requested=" +
                            std::to_string(pending.audioSample) +
                            ", first=" + std::to_string(completion.firstOutputSample) + ")");
                    }
                    pending.expectedAudioGeneration = completion.seekGeneration;
                    pending.audioReady = true;
                }
            }
            if (pending.expectedVideoGenerations.size() == pending.videoTickets.size() &&
                pending.audioReady) {
                pending.decodeReady = true;
                ++seekDecodeReadyCount;
            }
        }

        if (now > pending.deadline) {
            // decode readyでも提示できていなければ、成功にしない。
            return seekFailure(pending.decodeReady
                                   ? "seek対象frameを有限時間内に提示できませんでした"
                                   : "seek decodeが有限時間内に完了しませんでした");
        }
        return std::nullopt;
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

    // mutexを保持したままmailboxへ挿入する。dispatcher postは行わない。
    // state mutationと同じcritical sectionに載せることで、machineの遷移順と
    // mailboxのFIFO順が必ず一致する。分けると、commit後・enqueue前に別threadが
    // stateを進め、stale stateのeventが後から積まれる。
    Result<void> enqueueLocked(internal::PreviewEvent event,
                               std::shared_ptr<PreviewEventDispatcher>& pendingDispatch) {
        Result<void> pushed = mailbox.push(std::move(event));
        if (!pushed) {
            return pushed;
        }
        if (!dispatchScheduled && dispatcher) {
            dispatchScheduled = true;
            pendingDispatch = dispatcher;
        }
        return Result<void>::success();
    }

    // mutexを解放してから呼ぶ。dispatcherへの投函だけを行う。
    Result<void> postDispatch(const std::shared_ptr<PreviewEventDispatcher>& targetDispatcher) {
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

    Result<void> enqueue(internal::PreviewEvent event) {
        std::shared_ptr<PreviewEventDispatcher> pendingDispatch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Result<void> pushed = enqueueLocked(std::move(event), pendingDispatch);
            if (!pushed) {
                return pushed;
            }
        }
        return postDispatch(pendingDispatch);
    }

    void notify(internal::PreviewEvent event) {
        Result<void> notified = enqueue(std::move(event));
        if (!notified) {
            noteEventDeliveryFailure();
        }
    }

    // mutex保持中に呼ぶ。失敗はlocked counterへ直接記録する。
    void notifyLocked(internal::PreviewEvent event,
                      std::shared_ptr<PreviewEventDispatcher>& pendingDispatch) {
        Result<void> pushed = enqueueLocked(std::move(event), pendingDispatch);
        if (!pushed) {
            noteEventDeliveryFailureLocked();
        }
    }

    void flushDispatch(const std::shared_ptr<PreviewEventDispatcher>& pendingDispatch) {
        if (!pendingDispatch)
            return;
        if (!postDispatch(pendingDispatch)) {
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
    if (!core::isConfigurableOutputFrameRate(static_cast<std::int64_t>(rate.value().numerator),
                                             static_cast<std::int64_t>(rate.value().denominator))) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Initialize,
                      "指定frame rateは設定可能な出力rateではありません: " +
                          std::to_string(rate.value().numerator) + "/" +
                          std::to_string(rate.value().denominator)));
    }
    // scheduler / seek / statusが同じ換算を使うよう、timebaseはここで一度だけ確定する。
    const auto timebase = core::CheckedOutputTimebase::createConfigured(
        static_cast<std::int64_t>(rate.value().numerator),
        static_cast<std::int64_t>(rate.value().denominator), audio::kInternalSampleRate);
    if (!timebase) {
        return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                               PreviewOperation::Initialize,
                                               "設定されたoutput timebaseを構築できません"));
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> initialized = impl_->machine.initialize();
        if (!initialized) {
            return initialized;
        }
        impl_->dispatcher = std::move(dispatcher);
        impl_->configuredFrameRate = rate.value();
        // capability が公開する rate は「今回 initialize した rate」である。
        // 固定値を返すと source 側の rate 検査が別の rate を基準にしてしまう。
        impl_->capability.configuredOutputFrameRate = rate.value();
        // 現在の構成が実測 envelope と**組として**一致するかを判定する。
        // rate だけ、layer 数だけ、と軸ごとに合成できると仮定しない。
        const PreviewCapabilities& capability = impl_->capability;
        const MeasuredPreviewEnvelope& envelope = capability.measuredEnvelope;
        impl_->capability.matchesMeasuredEnvelope =
            rate.value() == envelope.outputFrameRate &&
            capability.configuredMaxActiveVideoSources == envelope.maxActiveVideoSources &&
            capability.configuredMaxCompositionLayers == envelope.maxCompositionLayers &&
            capability.configuredMaxActiveAudioSources == envelope.maxActiveAudioSources &&
            capability.configuredAudioSampleRate == envelope.audioSampleRate &&
            capability.configuredAudioChannelCount == envelope.audioChannelCount;
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
    // source-set切替中は旧sourceを新composition提示まで保持する。active compositionの
    // 上限は引き続きcapabilityの2件であり、登録slotだけを最大4件許す。
    constexpr std::size_t maximumRegisteredVideoSources = 4;
    if (descriptor.videoEnabled && impl_->videoSources.size() >= maximumRegisteredVideoSources) {
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::UnsupportedCapability, PreviewOperation::AddSource,
                      "source-set切替用のvideo source登録上限を超えています"));
    }
    if (descriptor.audioEnabled) {
        if (impl_->capability.configuredMaxActiveAudioSources == 0) {
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
            static_cast<int>(impl_->capability.configuredAudioChannelCount), "flt");
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
        impl_->videoSources.emplace(published.value, PreviewEngine::Impl::VideoSourceEntry{
                                                         internalVideo, std::move(newVideoWorker)});
        // source集合が変わったのでpairerは次のcomposeで組み直す。
        // coordinatorはepoch lineageのownerなので作り直さない。
        impl_->pairer.reset();
        impl_->coordinatorSources.clear();
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
        impl_->audioSampleOffset = descriptor.audioSampleOffset;
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

Result<void> PreviewEngine::removeSource(PreviewSourceId source) {
    // audio transportの停止はengine mutexを保持したまま行わない
    // (audioTransportMutexとの取得順を固定するため)。したがって次の3 phaseで行う。
    //
    //   phase 1: engine lock。検査してaudioのshared refだけを取り出す
    //   phase 2: lock無し。audio transportを停止する
    //   phase 3: engine lock。stateを再検証し、video workerを引き直して停止しcommitする
    //
    // phase 1でvideo workerのraw pointerを持ち出さない。phase 2の窓でshutdownが
    // 勝つとownershipが`detachedWorkers`へ移り、raw pointerがdanglingになり得る。
    // phase 3でtableから引き直し、engine lockを保持したまま停止まで行う。
    // ReadyPausedのworkerは既にpause済みなのでjoinは短く、この窓でrender pathは
    // 早期returnするだけである。
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    bool removingAudio = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::RemoveSource);
        if (!affinity) {
            return affinity;
        }
        if (impl_->machine.state() != PreviewEngineState::ReadyPaused) {
            return invalidState(PreviewOperation::RemoveSource,
                                "source removalはReadyPausedでのみ受理します");
        }
        if (impl_->eligibleSources.find(source.value) == impl_->eligibleSources.end()) {
            return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                                   PreviewOperation::RemoveSource,
                                                   "未登録のPreviewSourceIdです"));
        }
        if (impl_->pendingSeek.active) {
            return invalidState(PreviewOperation::RemoveSource,
                                "seek進行中のsourceは削除できません");
        }
        // active (last presented) またはpending (accepted済みで未提示) のcomposition
        // が参照しているsourceは削除しない (preview-engine-contract.md §6)。
        if (impl_->compositionState.referencesSource(source)) {
            return invalidState(
                PreviewOperation::RemoveSource,
                "activeまたはpending compositionが参照しているsourceは削除できません");
        }

        removingAudio = impl_->publicAudioSource && *impl_->publicAudioSource == source;
        if (removingAudio) {
            audioWorker = impl_->audioWorker;
            audioSink = impl_->audioSink;
            // schedulerがまだaudio clockをmasterとして参照しないようにする。
            // ReadyPausedなのでscheduler自体は止まっているが、authorityの
            // 取り下げをstopより先に確定させる。
            impl_->audioMasterActive = false;
        }
    }

    // ownershipは移していない。engine側のfieldは生きたままなので、この窓で
    // fatalが起きてもshutdown経路が同じ実体をstop/joinできる。
    std::string audioError;
    bool audioStopped = true;
    if (audioSink || audioWorker) {
        // shutdown ordering (contract §12) と同じ順で止める: sink -> worker -> clock。
        std::lock_guard<std::mutex> transport(impl_->audioTransportMutex);
        if (audioSink) {
            audioStopped = audioSink->pause(audioError);
            audioSink->stop();
        }
        if (audioWorker)
            audioWorker->stop();
    }
    // transport lockを解放してからbarrierへ入る。保持したまま止まるとshutdown
    // threadがtransport lockで待ち、raceを再現できないままdeadlockする。
    impl_->removalBarrier.enter();

    std::shared_ptr<PreviewEventDispatcher> pendingDispatch;
    std::optional<PreviewError> fatal;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->machine.state() != PreviewEngineState::ReadyPaused) {
            // 停止中にfatal/state変化が入った。removalはcommitせず、
            // 実体のteardownはshutdown経路へ委ねる。
            return invalidState(PreviewOperation::RemoveSource,
                                "removal中にstateが変化したため削除を確定しません");
        }
        if (!audioStopped) {
            // audioを止められないまま登録解除すると、鳴り続けているendpointの
            // ownerが居なくなる。pause()と同じくsession-fatalとして扱う。
            ++impl_->audioTransportFailureCount;
            PreviewError failure =
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::RemoveSource,
                          "WASAPI renderingを停止できません: " + audioError,
                          PreviewErrorSeverity::FatalToSession);
            failure.source = source;
            Result<void> recorded = impl_->machine.recordFatal(failure);
            if (!recorded)
                return recorded;
            fatal = failure;
            impl_->startWorkerShutdown();
            impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
            impl_->telemetrySnapshot.status.lastError = impl_->machine.lastError();
            // state commitとmailbox insertionを同じcritical sectionでlinearizeする。
            // unlock後にnotifyすると、先に進んだteardownの`Error`より後ろへ
            // `ShuttingDown`が入り、event streamの順序が逆転し得る。
            impl_->notifyLocked(internal::ErrorOccurredEvent{failure}, pendingDispatch);
            impl_->notifyLocked(internal::StateChangedEvent{PreviewEngineState::ShuttingDown},
                                pendingDispatch);
        } else {
            // phase 1では持ち出していない。tableから引き直す。
            const auto entry = impl_->videoSources.find(source.value);
            if (entry != impl_->videoSources.end()) {
                if (entry->second.worker)
                    entry->second.worker->stop();
                if (entry->second.internal.value != 0)
                    impl_->sourceRegistry.unregisterSource(entry->second.internal);
                impl_->videoSources.erase(entry);
                // pairerはbufferをraw pointerで握る。source集合が変わったので
                // 必ず組み直す。coordinatorはepoch lineageのownerなので作り直さない。
                impl_->pairer.reset();
                impl_->coordinatorSources.clear();
                if (impl_->videoSources.empty()) {
                    impl_->workerJoined = true;
                    impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
                }
            }
            if (removingAudio) {
                // authoritative audio sourceを安全に削除したのでaudio authorityを
                // 空へ戻す (preview-engine-contract.md §6)。宣言の逆順で解放する。
                impl_->audioSink.reset();
                impl_->audioWorker.reset();
                impl_->audioClock.reset();
                impl_->publicAudioSource.reset();
                impl_->internalAudioSource = audio::SourceId{};
                impl_->resumeAudioSample = 0;
                impl_->audioSampleOffset = 0;
                impl_->audioSinkJoined = true;
                impl_->audioWorkerJoined = true;
                impl_->deviceSnapshot.audioSampleRate = 0;
                impl_->deviceSnapshot.audioChannelCount = 0;
            }
            impl_->eligibleSources.erase(source.value);
        }
    }

    if (fatal) {
        // unlock済みでflush前。ここで止めてもevent順序が壊れないことが、
        // state commitとmailbox insertionをlinearizeしたことの検査になる。
        impl_->fatalPublishBarrier.enter();
        impl_->flushDispatch(pendingDispatch);
        return Result<void>::failure(*fatal);
    }
    return Result<void>::success();
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
        if (!impl_->compositionState.latestAcceptedToken()) {
            return invalidState(PreviewOperation::Play, "playにはaccepted compositionが必要です");
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
        for (gpu::SourceDecodeWorker* worker : impl_->referencedVideoWorkersLocked())
            worker->play();
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
        for (gpu::SourceDecodeWorker* worker : impl_->videoWorkersLocked())
            worker->pause();
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

// `seek()`のreturnはrequest acceptanceである。completionはrender threadが
// 「要求したoutputFrameを実際に提示できたか」で判定する
// (preview-engine-contract.md §10.1)。
Result<void> PreviewEngine::seek(PreviewPosition target) {
    PreviewFrameRequest request;
    request.outputFrameNumber = target.outputFrame;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto snapshot = impl_->compositionState.latestAcceptedSnapshot();
        if (snapshot) {
            for (const auto& layer : snapshot->layers)
                request.sources.push_back({layer.source, target.outputFrame});
        }
    }
    return seekFrameRequest(request);
}

Result<void> PreviewEngine::seekFrameRequest(const PreviewFrameRequest& request) {
    const PreviewPosition target{request.outputFrameNumber};
    std::map<std::uint64_t, std::int64_t> requestedFrames;
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    std::int64_t audioSample = 0;
    bool resumePlaying = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Seek);
        if (!affinity) {
            return affinity;
        }
        const PreviewEngineState state = impl_->machine.state();
        if (state != PreviewEngineState::ReadyPaused && state != PreviewEngineState::Playing) {
            return invalidState(PreviewOperation::Seek, "seekを受理できないstateです");
        }
        // 引数の検査はsource/compositionの有無より先に行う。呼び出し側の誤りを
        // stateの都合で別のerrorへすり替えない。
        if (target.outputFrame < 0) {
            return Result<void>::failure(makeError(PreviewErrorCategory::SeekFailure,
                                                   PreviewOperation::Seek,
                                                   "負のoutputFrameへはseekできません"));
        }
        const auto acceptedSnapshot = impl_->compositionState.latestAcceptedSnapshot();
        if (!impl_->compositionState.latestAcceptedToken() || !acceptedSnapshot) {
            return invalidState(PreviewOperation::Seek, "seekにはaccepted compositionが必要です");
        }
        std::set<std::uint64_t> expectedSources;
        for (const auto& layer : acceptedSnapshot->layers)
            expectedSources.insert(layer.source.value);
        for (const auto& sourceRequest : request.sources) {
            if (sourceRequest.source.value == 0 || sourceRequest.sourceFrameNumber < 0 ||
                !requestedFrames
                     .emplace(sourceRequest.source.value, sourceRequest.sourceFrameNumber)
                     .second) {
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                              "source frame requestがinvalidまたは重複しています"));
            }
        }
        std::set<std::uint64_t> requestedSources;
        for (const auto& [source, frame] : requestedFrames) {
            (void)frame;
            requestedSources.insert(source);
        }
        if (requestedSources != expectedSources) {
            return Result<void>::failure(
                makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                          "source frame requestがaccepted compositionのsource集合と一致しません"));
        }
        if (!impl_->timebase) {
            return invalidState(PreviewOperation::Seek, "output timebaseが未確定です");
        }
        // seek / scheduler / statusは同じ換算authorityを使う。
        const auto sample = impl_->timebase->seekTargetSample(target.outputFrame);
        if (!sample) {
            return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                                   PreviewOperation::Seek,
                                                   "outputFrameをaudio sampleへ換算できません"));
        }
        // output frame -> media sample。audio clip の timeline 位置ずれをここで足す。
        if (impl_->audioSampleOffset > 0 &&
            sample.value() > std::numeric_limits<std::int64_t>::max() - impl_->audioSampleOffset) {
            return Result<void>::failure(makeError(PreviewErrorCategory::SeekFailure,
                                                   PreviewOperation::Seek,
                                                   "audio sample offsetの加算がoverflowしました"));
        }
        audioSample = sample.value() + impl_->audioSampleOffset;
        if (audioSample < 0)
            audioSample = 0;
        resumePlaying = state == PreviewEngineState::Playing;
        audioWorker = impl_->audioWorker;
        audioSink = impl_->audioSink;

        // transportを止めてからrequestする。動作中のschedulerとseekを競合させない。
        impl_->schedulerEnabled = false;
        impl_->audioMasterActive = false;
        for (gpu::SourceDecodeWorker* worker : impl_->videoWorkersLocked())
            worker->pause();
    }

    // audioの停止とendpoint resetはengine lockを保持したまま行わない。
    std::string audioError;
    bool audioPrepared = true;
    if (audioSink) {
        if (!audioSink->pause(audioError))
            audioPrepared = false;
    }
    if (audioWorker)
        audioWorker->pause();
    if (audioPrepared && audioSink) {
        if (!audioSink->resetForSeek(audioError))
            audioPrepared = false;
    }

    std::optional<PreviewError> fatal;
    std::shared_ptr<PreviewEventDispatcher> pendingDispatch;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!audioPrepared) {
            ++impl_->audioTransportFailureCount;
            PreviewError failure =
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Seek,
                          "seekのためにaudio endpointを停止できません: " + audioError,
                          PreviewErrorSeverity::FatalToSession);
            failure.source = impl_->publicAudioSource;
            if (impl_->machine.recordFatal(failure)) {
                impl_->startWorkerShutdown();
                impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
                impl_->telemetrySnapshot.status.lastError = impl_->machine.lastError();
            }
            impl_->notifyLocked(internal::ErrorOccurredEvent{failure}, pendingDispatch);
            impl_->notifyLocked(internal::StateChangedEvent{PreviewEngineState::ShuttingDown},
                                pendingDispatch);
            fatal = failure;
        } else {
            Result<void> moved = impl_->machine.seek();
            if (!moved)
                return moved;

            std::string requestError;
            bool videoRequestsAccepted = true;
            std::optional<PreviewSourceId> rejectedVideoSource;
            impl_->pendingSeek.videoTickets.clear();
            impl_->pendingSeek.expectedSourceFrames.clear();
            for (const auto& [publicId, sourceFrame] : requestedFrames) {
                const auto sourceEntry = impl_->videoSources.find(publicId);
                if (sourceEntry == impl_->videoSources.end() || !sourceEntry->second.worker) {
                    videoRequestsAccepted = false;
                    rejectedVideoSource = PreviewSourceId{publicId};
                    requestError = "要求sourceが登録されていません";
                    break;
                }
                gpu::SeekTicket ticket;
                const gpu::SeekRequestResult seekRequest = sourceEntry->second.worker->requestSeek(
                    sourceFrame, target.outputFrame, ticket, requestError);
                if (seekRequest != gpu::SeekRequestResult::Accepted) {
                    videoRequestsAccepted = false;
                    rejectedVideoSource = PreviewSourceId{publicId};
                    break;
                }
                ++impl_->seekVideoRequestAcceptedCount;
                impl_->pendingSeek.videoTickets.emplace(publicId, ticket);
                impl_->pendingSeek.expectedSourceFrames.emplace(publicId, sourceFrame);
            }
            audio::AudioSeekRequestResult audioRequest = audio::AudioSeekRequestResult::Accepted;
            if (audioWorker) {
                audioRequest = audioWorker->requestSeek(audioSample, impl_->pendingSeek.audioTicket,
                                                        requestError);
            }
            if (!videoRequestsAccepted || audioRequest != audio::AudioSeekRequestResult::Accepted) {
                // 一つでも未受理なら、source間のidentity整合を保証できない。
                PreviewError failure =
                    makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                              "seek requestが受理されませんでした: " + requestError,
                              PreviewErrorSeverity::FatalToSession);
                failure.source = rejectedVideoSource;
                if (impl_->machine.recordFatal(failure)) {
                    impl_->startWorkerShutdown();
                    impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
                    impl_->telemetrySnapshot.status.lastError = impl_->machine.lastError();
                }
                impl_->pendingSeek = Impl::PendingSeek{};
                impl_->notifyLocked(internal::ErrorOccurredEvent{failure}, pendingDispatch);
                impl_->notifyLocked(internal::StateChangedEvent{PreviewEngineState::ShuttingDown},
                                    pendingDispatch);
                fatal = failure;
            } else {
                Impl::PendingSeek& pending = impl_->pendingSeek;
                pending.active = true;
                pending.target = target;
                pending.audioSample = audioSample;
                pending.expectedVideoGenerations.clear();
                pending.audioReady = audioWorker == nullptr;
                pending.decodeReady = false;
                pending.resumePlaying = resumePlaying;
                pending.deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
                ++impl_->seekRequestCount;
                impl_->lastSeekTargetFrame = target.outputFrame;
                impl_->telemetrySnapshot.status.state = PreviewEngineState::Seeking;
                // request acceptanceが確定した成功branchでだけ`Seeking`を公開する。
                // unlock後に積むと、render threadが先にseekを完了/失敗させた場合に
                // stale eventが後ろへ並ぶ。
                impl_->notifyLocked(internal::StateChangedEvent{PreviewEngineState::Seeking},
                                    pendingDispatch);
            }
        }
    }

    impl_->flushDispatch(pendingDispatch);
    if (fatal) {
        return Result<void>::failure(*fatal);
    }
    return Result<void>::success();
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
    if (impl_->audioSink) {
        const audio::WasapiSnapshot endpoint = impl_->audioSink->snapshot();
        result.audioMeterPeakLeft = endpoint.meterPeakLeft;
        result.audioMeterPeakRight = endpoint.meterPeakRight;
    }
    return result;
}

PreviewDeviceInfo PreviewEngine::deviceInfo() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->deviceSnapshot;
}

Result<void> PreviewEngine::requestShutdown() {
    std::shared_ptr<PreviewEventDispatcher> pendingDispatch;
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
        for (gpu::SourceDecodeWorker* worker : impl_->videoWorkersLocked())
            worker->pause();
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
        if (before != after) {
            // stateの遷移とeventの挿入を分けると、commit後・enqueue前に
            // render threadがstale stateのeventを先に積み得る。
            impl_->notifyLocked(internal::StateChangedEvent{after}, pendingDispatch);
        }
    }
    impl_->flushDispatch(pendingDispatch);
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
    bool seeking = false;
    bool seekResumePlaying = false;
    bool seekAwaitingResume = false;
    bool fatalPublished = false;
    std::shared_ptr<PreviewEventDispatcher> fatalDispatch;
    std::shared_ptr<PreviewEventDispatcher> seekDispatch;
    std::int64_t seekResumeSample = 0;
    audio::SourceGeneration seekResumeAudioGeneration{};
    std::optional<PreviewEngineState> seekCompletedState;
    std::shared_ptr<audio::AudioDecodeWorker> seekAudioWorker;
    std::shared_ptr<audio::WasapiAudioSink> seekAudioSink;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        if (!engine.impl_->renderThread ||
            *engine.impl_->renderThread != std::this_thread::get_id()) {
            return Result<RenderFrameResult>::failure(
                invalidState(PreviewOperation::RenderDeviceAttach,
                             "renderは登録済みrender threadで実行してください")
                    .error());
        }
        const PreviewEngineState renderState = engine.impl_->machine.state();
        seeking = renderState == PreviewEngineState::Seeking && engine.impl_->pendingSeek.active;
        if (!seeking &&
            (renderState != PreviewEngineState::Playing || !engine.impl_->schedulerEnabled)) {
            return Result<RenderFrameResult>::success(result);
        }
        if (!renderTargetView || width <= 0 || height <= 0 || !engine.impl_->compositor) {
            return Result<RenderFrameResult>::failure(
                makeError(PreviewErrorCategory::InvalidState, PreviewOperation::RenderDeviceAttach,
                          "render targetまたはproduct runtimeが未準備です"));
        }

        const auto renderNow = std::chrono::steady_clock::now();
        std::int64_t target = 0;
        bool proceed = false;
        if (seeking) {
            // decode completionを非blockingで回収する。decode readyでも
            // exact frameを提示するまでcompleteにしない。
            std::optional<PreviewError> seekFatal = engine.impl_->advanceSeekLocked(renderNow);
            if (seekFatal) {
                fatal = *seekFatal;
            } else if (engine.impl_->pendingSeek.decodeReady) {
                if (engine.impl_->seekPresentationStallInjected) {
                    // decodeは完了しているが提示できない状況。ここでcompleteに
                    // しないことがseek contractの核心であり、deadlineで失敗する。
                    ++engine.impl_->seekAwaitingPresentationCount;
                    return Result<RenderFrameResult>::success(result);
                }
                target = engine.impl_->pendingSeek.target.outputFrame;
                proceed = true;
            } else {
                return Result<RenderFrameResult>::success(result);
            }
        } else {
            // audio masterが成立しない場合、QPC/steady_clockへ退避せずfatalとして表面化する。
            const auto scheduled = engine.impl_->schedulerTargetLocked(renderNow);
            if (!scheduled.valid)
                fatal = scheduled.error;
            else {
                target = scheduled.frame;
                if (target <= engine.impl_->lastSchedulerTarget)
                    return Result<RenderFrameResult>::success(result);
                proceed = true;
            }
        }
        if (proceed && !seeking) {
            const std::uint64_t skipped =
                internal::skippedSchedulerFrameCount(engine.impl_->lastSchedulerTarget, target);
            const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
            if (skipped > maximum - engine.impl_->telemetrySnapshot.droppedFrameCount)
                engine.impl_->telemetrySnapshot.droppedFrameCount = maximum;
            else
                engine.impl_->telemetrySnapshot.droppedFrameCount += skipped;
            engine.impl_->lastSchedulerTarget = target;
        }
        if (proceed) {
            // compositionのruntime authorityを先に確定させる。layoutとstateが
            // 決まっていないとexact pairingの対象sourceも決まらない。
            const auto token = engine.impl_->compositionState.latestAcceptedToken();
            const auto snapshot = engine.impl_->compositionState.latestAcceptedSnapshot();
            if (!token || !snapshot) {
                return Result<RenderFrameResult>::failure(makeError(
                    PreviewErrorCategory::CompositionFailure, PreviewOperation::RenderDeviceAttach,
                    "accepted compositionが見つかりません"));
            }
            if (snapshot->layers.empty()) {
                // gapはrender passの既存background clearをそのまま表示する。
                // decode sourceをpresentation identityの代用にしない。
                engine.impl_->pairer.reset();
                engine.impl_->coordinatorSources.clear();
                engine.impl_->compositionState.markPresented(*token, snapshot);
                engine.impl_->distinctPresentedFrames.note(target);
                ++engine.impl_->presentationSequence;
                ++engine.impl_->telemetrySnapshot.presentedFrameCount;
                engine.impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
                engine.impl_->telemetrySnapshot.status.position = {target};
                engine.impl_->telemetrySnapshot.status.lastPresentedComposition = *token;
                result.presented = true;
                result.sourceFrame = -1;
                result.frame = {engine.impl_->presentationSequence, {target}, *token, 0};
                if (seeking) {
                    ++engine.impl_->seekCompletedCount;
                    engine.impl_->lastSeekPresentedFrame = target;
                    seekResumePlaying = engine.impl_->pendingSeek.resumePlaying;
                    seekResumeSample = engine.impl_->pendingSeek.audioSample;
                    seekResumeAudioGeneration = engine.impl_->pendingSeek.expectedAudioGeneration;
                    engine.impl_->pendingSeek = PreviewEngine::Impl::PendingSeek{};
                    engine.impl_->lastSchedulerTarget = target;
                    engine.impl_->schedulerBaseFrame = target + 1;
                    engine.impl_->resumeAudioSample = seekResumeSample;
                    if (seekResumePlaying) {
                        seekAwaitingResume = true;
                    } else {
                        Result<void> completed = engine.impl_->machine.completeSeek();
                        if (!completed)
                            fatal = completed.error();
                        else {
                            engine.impl_->telemetrySnapshot.status.state =
                                engine.impl_->machine.state();
                            seekCompletedState = engine.impl_->machine.state();
                            engine.impl_->notifyLocked(StateChangedEvent{*seekCompletedState},
                                                       seekDispatch);
                        }
                    }
                }
            } else if (std::optional<PreviewError> syncFailure =
                           engine.impl_->syncCompositionRuntimeLocked(*token, *snapshot)) {
                fatal = std::move(*syncFailure);
            } else {
                gpu::ExactFramePairer& pairer = *engine.impl_->pairer;
                const std::vector<gpu::SourceDecodeWorker*> workers =
                    engine.impl_->referencedVideoWorkersLocked();
                const auto queueDepth = [&workers]() -> std::uint32_t {
                    std::size_t deepest = 0;
                    for (gpu::SourceDecodeWorker* worker : workers)
                        deepest = std::max(deepest, worker->buffer().depth());
                    return static_cast<std::uint32_t>(deepest);
                };

                gpu::ComposedFrame composed;
                const gpu::PairResult paired = pairer.tryPair(target, composed);
                if (paired == gpu::PairResult::StaleGeneration ||
                    paired == gpu::PairResult::FutureGeneration) {
                    // generationが揃わないframeをold/latestで代用しない。
                    // ここはsubstitutionではなくrejectなので、禁止fallbackの
                    // counterである staleSubstitutionCount は増やさない。
                    // generationが動くのはseekのときだけなので、counterの
                    // authorityもseek経路に限る。それ以外で起きたなら
                    // 単なるdropとして数え、seekの証拠に混ぜない。
                    if (seeking)
                        ++engine.impl_->seekStaleGenerationRejectCount;
                    else if (engine.impl_->telemetrySnapshot.droppedFrameCount !=
                             std::numeric_limits<std::uint64_t>::max())
                        ++engine.impl_->telemetrySnapshot.droppedFrameCount;
                    engine.impl_->telemetrySnapshot.currentSourceQueueDepth = queueDepth();
                } else if (paired != gpu::PairResult::Paired) {
                    bool anyFatal = false;
                    bool anyEof = false;
                    std::string fatalDetail;
                    std::optional<PreviewSourceId> fatalSource;
                    for (gpu::SourceDecodeWorker* worker : workers) {
                        const gpu::SourceDecoderSnapshot state = worker->snapshot();
                        if (state.fatal && !anyFatal) {
                            anyFatal = true;
                            fatalDetail = state.lastError;
                            fatalSource = engine.impl_->publicIdForInternalLocked(state.sourceId);
                        }
                        anyEof = anyEof || state.eof;
                    }
                    if (anyFatal) {
                        PreviewError failure = makeError(
                            PreviewErrorCategory::DecodeFailure,
                            PreviewOperation::RenderDeviceAttach,
                            fatalDetail.empty()
                                ? "video decode workerがfatal終了しました"
                                : "video decode workerがfatal終了しました: " + fatalDetail);
                        failure.severity = PreviewErrorSeverity::FatalToSession;
                        failure.source = fatalSource;
                        fatal = std::move(failure);
                        decoderFatal = true;
                    } else if (anyEof) {
                        Result<void> shutdown = engine.impl_->machine.requestShutdown();
                        if (shutdown) {
                            engine.impl_->schedulerEnabled = false;
                            engine.impl_->startWorkerShutdown();
                            engine.impl_->telemetrySnapshot.status.state =
                                PreviewEngineState::ShuttingDown;
                            playbackEnded = true;
                        }
                    } else if (seeking) {
                        // decode readyでもexact frameをまだ提示できていない。
                        // ここでcompleteにしないことがseek contractの核心である。
                        ++engine.impl_->seekAwaitingPresentationCount;
                        engine.impl_->telemetrySnapshot.currentSourceQueueDepth = queueDepth();
                    } else {
                        if (engine.impl_->telemetrySnapshot.droppedFrameCount !=
                            std::numeric_limits<std::uint64_t>::max()) {
                            ++engine.impl_->telemetrySnapshot.droppedFrameCount;
                        }
                        engine.impl_->telemetrySnapshot.currentSourceQueueDepth = queueDepth();
                    }
                } else if (!engine.impl_->composedIdentityValidLocked(composed)) {
                    PreviewError error = makeError(PreviewErrorCategory::DecodeFailure,
                                                   PreviewOperation::RenderDeviceAttach,
                                                   "decode frameのsource identityが一致しません");
                    error.severity = PreviewErrorSeverity::FatalToSession;
                    fatal = error;
                } else {
                    bool rejected = false;
                    if (seeking && !engine.impl_->pendingSeek.expectedVideoGenerations.empty()) {
                        for (const auto& layer : composed.layers) {
                            const auto publicSource =
                                engine.impl_->publicIdForInternalLocked(layer.frame.sourceId);
                            const auto expected =
                                publicSource
                                    ? engine.impl_->pendingSeek.expectedVideoGenerations.find(
                                          publicSource->value)
                                    : engine.impl_->pendingSeek.expectedVideoGenerations.end();
                            gpu::SourceGeneration required =
                                expected == engine.impl_->pendingSeek.expectedVideoGenerations.end()
                                    ? gpu::SourceGeneration{}
                                    : expected->second;
                            if (publicSource &&
                                engine.impl_->seekVideoGenerationMismatchInjected == publicSource) {
                                ++required.value;
                            }
                            if (!publicSource ||
                                expected ==
                                    engine.impl_->pendingSeek.expectedVideoGenerations.end() ||
                                !(layer.frame.sourceGeneration == required)) {
                                rejected = true;
                                break;
                            }
                        }
                        if (rejected)
                            ++engine.impl_->seekStaleGenerationRejectCount;
                    }
                    if (!rejected && seeking && engine.impl_->audioWorker &&
                        engine.impl_->pendingSeek.expectedAudioGeneration.value != 0) {
                        // seek completionで得たaudio identityをそのままauthorityにする。
                        // decoderとqueueの双方が揃うまで提示しない。
                        const audio::AudioDecoderSnapshot decoder =
                            engine.impl_->audioWorker->snapshot();
                        const audio::SourceGeneration expected =
                            engine.impl_->seekAudioGenerationMismatchInjected
                                ? audio::SourceGeneration{engine.impl_->pendingSeek
                                                              .expectedAudioGeneration.value +
                                                          1}
                                : engine.impl_->pendingSeek.expectedAudioGeneration;
                        if (!(decoder.sourceGeneration == expected) ||
                            !(engine.impl_->audioWorker->queue().generation() == expected)) {
                            ++engine.impl_->seekStaleGenerationRejectCount;
                            rejected = true;
                        }
                    }
                    if (!rejected && engine.impl_->audioMasterActive && engine.impl_->audioWorker) {
                        const audio::AudioDecoderSnapshot decoder =
                            engine.impl_->audioWorker->snapshot();
                        if (!(decoder.sourceGeneration ==
                              engine.impl_->audioWorker->queue().generation())) {
                            // audio generationが揃わないframeをlatest/staleで代用しない。
                            ++engine.impl_->audioGenerationMismatchCount;
                            ++engine.impl_->staleSubstitutionCount;
                            rejected = true;
                        }
                    }
                    if (!rejected && engine.impl_->compositionEpochAdvanceInjected) {
                        // supersedeを製品経路で再現する。ここで進めた epoch は
                        // 直後の validateForDisplay が必ず弾く。
                        engine.impl_->compositionEpochAdvanceInjected = false;
                        if (!engine.impl_->advanceCompositionEpochForTestLocked()) {
                            PreviewError failure =
                                makeError(PreviewErrorCategory::CompositionFailure,
                                          PreviewOperation::RenderDeviceAttach,
                                          "composition epoch advance seamが成立しませんでした",
                                          PreviewErrorSeverity::FatalToSession);
                            fatal = failure;
                            rejected = true;
                        }
                    }
                    if (!rejected && engine.impl_->coordinator->validateForDisplay(composed) !=
                                         gpu::CompositionResult::Accepted) {
                        // 提示直前のre-validation。supersedeされたcomposition epochや
                        // generationのframeをGPUへ出さない。
                        // 現在のrender pathはcompose -> validate -> drawを同じ
                        // engine lock内で行うため、ここが反応するのは
                        // compositionのownerが壊れている場合だけである。
                        // したがってrejectはlifecycle violationとしても数える。
                        ++engine.impl_->staleCompositionEpochRejectCount;
                        ++engine.impl_->lifecycleViolationCount;
                        // 拒否したframe identityを残す。counterだけでは
                        // 「数えたうえでそのまま描画した」bugを観測できない。
                        engine.impl_->lastStaleCompositionRejectedFrame = target;
                        rejected = true;
                    }

                    if (rejected)
                        return Result<RenderFrameResult>::success(result);

                    gpu::ExternalCompositionTarget targetView{
                        static_cast<ID3D11RenderTargetView*>(renderTargetView), width, height};
                    std::string error;
                    // 期待layer数のauthorityはaccepted snapshotであり、
                    // compose結果そのものではない。自己参照にすると層数の
                    // boundary checkがtautologyになる。
                    const std::size_t expectedLayerCount = snapshot->layers.size();
                    if (!engine.impl_->compositor->composeLayersToTarget(
                            composed, targetView, expectedLayerCount, error)) {
                        PreviewError failure = makeError(PreviewErrorCategory::DeviceFailure,
                                                         PreviewOperation::RenderDeviceAttach,
                                                         "GPU compositionに失敗しました: " + error);
                        failure.severity = PreviewErrorSeverity::FatalToSession;
                        fatal = failure;
                    } else {
                        for (gpu::SourceDecodeWorker* worker : workers)
                            worker->buffer().noteDisplayed(target);
                        engine.impl_->compositionState.markPresented(*token, snapshot);
                        engine.impl_->distinctPresentedFrames.note(target);
                        ++engine.impl_->presentationSequence;
                        ++engine.impl_->telemetrySnapshot.presentedFrameCount;
                        engine.impl_->telemetrySnapshot.currentSourceQueueDepth = queueDepth();
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
                        result.frame = {engine.impl_->presentationSequence,
                                        {target},
                                        *token,
                                        static_cast<std::uint32_t>(composed.layers.size())};
                        if (seeking) {
                            // actual requested frameを提示できた時点だけがseek completionである。
                            ++engine.impl_->seekCompletedCount;
                            engine.impl_->lastSeekPresentedFrame = target;
                            seekResumePlaying = engine.impl_->pendingSeek.resumePlaying;
                            seekResumeSample = engine.impl_->pendingSeek.audioSample;
                            seekResumeAudioGeneration =
                                engine.impl_->pendingSeek.expectedAudioGeneration;
                            engine.impl_->pendingSeek = PreviewEngine::Impl::PendingSeek{};
                            engine.impl_->lastSchedulerTarget = target;
                            engine.impl_->schedulerBaseFrame = target + 1;
                            engine.impl_->resumeAudioSample = seekResumeSample;
                            seekAudioWorker = engine.impl_->audioWorker;
                            seekAudioSink = engine.impl_->audioSink;
                            if (seekResumePlaying) {
                                // transportを再開できる前に`Playing`を公開しない。
                                // stateはSeekingのまま保持し、resume成功後にcommitする。
                                seekAwaitingResume = true;
                            } else {
                                Result<void> completed = engine.impl_->machine.completeSeek();
                                if (!completed) {
                                    fatal = completed.error();
                                } else {
                                    engine.impl_->telemetrySnapshot.status.state =
                                        engine.impl_->machine.state();
                                    seekCompletedState = engine.impl_->machine.state();
                                    // paused originのseekも、commitとeventの挿入を
                                    // 同じcritical sectionに収める。
                                    engine.impl_->notifyLocked(
                                        StateChangedEvent{*seekCompletedState}, seekDispatch);
                                }
                            }
                        }
                    }
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
                for (gpu::SourceDecodeWorker* worker : engine.impl_->videoWorkersLocked())
                    worker->pause();
                engine.impl_->startWorkerShutdown();
                engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
                engine.impl_->telemetrySnapshot.status.lastError =
                    engine.impl_->machine.lastError();
                // seek resume failureと同じ理由で、commitとeventの挿入を分けない。
                engine.impl_->notifyLocked(ErrorOccurredEvent{*fatal}, fatalDispatch);
                engine.impl_->notifyLocked(StateChangedEvent{PreviewEngineState::ShuttingDown},
                                           fatalDispatch);
                fatalPublished = true;
            }
        }
    }
    if (fatal) {
        if (fatalPublished) {
            engine.impl_->flushDispatch(fatalDispatch);
        } else {
            engine.impl_->notify(ErrorOccurredEvent{*fatal});
            engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});
        }
        return Result<RenderFrameResult>::failure(*fatal);
    }
    if (playbackEnded)
        engine.impl_->notify(StateChangedEvent{PreviewEngineState::ShuttingDown});

    // seek完了後のtransport復帰。engine lockを保持したままaudioを触らない。
    // Playing originのseekは、ここが成功するまで`Playing`を公開しない。
    if (seekAwaitingResume) {
        std::optional<PreviewError> resumeFailure;
        if (seekAudioWorker && seekAudioSink) {
            // shutdownのstop()と直列化する。engine mutexは保持しない。
            std::lock_guard<std::mutex> transport(engine.impl_->audioTransportMutex);
            seekAudioWorker->play();
            std::string error;
            // seek completionが返したidentityをそのまま運ぶ。
            if (!seekAudioSink->play(seekResumeSample, seekResumeAudioGeneration, error)) {
                seekAudioWorker->pause();
                resumeFailure =
                    makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Seek,
                              "seek後にWASAPI renderingを再開できません: " + error,
                              PreviewErrorSeverity::FatalToSession);
            }
        }
        // shutdownの再確認とcompleteSeek()/recordFatal()を1つのcritical sectionに
        // 収める。分けると「Seekingを確認 -> requestShutdown -> completeSeek」で
        // 正常なshutdownがInvalidState経由でErrorへ化ける窓が残る。
        bool cancelledByShutdown = false;
        {
            std::lock_guard<std::mutex> lock(engine.impl_->mutex);
            if (engine.impl_->machine.state() != PreviewEngineState::Seeking) {
                // resume中にrequestShutdown()がcommitされていた。これはseek failure
                // ではなくshutdownによるcancellationなので、completeSeek()も
                // recordFatal()も行わない。resumeが失敗していても同じである。
                ++engine.impl_->seekCancelledByShutdownCount;
                cancelledByShutdown = true;
            } else if (!resumeFailure) {
                Result<void> completed = engine.impl_->machine.completeSeek();
                if (!completed) {
                    resumeFailure = completed.error();
                } else {
                    // seek()でpauseしたvideo decodeも再開する。ここを忘れるとbufferが
                    // 補充されず、Playingに戻ってもframeを提示できない。
                    for (gpu::SourceDecodeWorker* worker : engine.impl_->videoWorkersLocked())
                        worker->play();
                    engine.impl_->schedulerEnabled = true;
                    engine.impl_->audioMasterActive =
                        engine.impl_->audioSink != nullptr && engine.impl_->audioClock != nullptr;
                    engine.impl_->schedulerStart = std::chrono::steady_clock::now();
                    engine.impl_->telemetrySnapshot.status.state = engine.impl_->machine.state();
                    seekCompletedState = engine.impl_->machine.state();
                    // Playingのcommitと同じcritical sectionでeventを積む。
                    // 分けるとcommit後にrequestShutdown()が割り込み、
                    // ShuttingDownの後にstaleなPlayingが並ぶ。
                    engine.impl_->notifyLocked(StateChangedEvent{*seekCompletedState},
                                               seekDispatch);
                }
            }
            if (!cancelledByShutdown && resumeFailure) {
                ++engine.impl_->audioTransportFailureCount;
                if (engine.impl_->machine.recordFatal(*resumeFailure)) {
                    engine.impl_->schedulerEnabled = false;
                    engine.impl_->audioMasterActive = false;
                    engine.impl_->startWorkerShutdown();
                    engine.impl_->telemetrySnapshot.status.state = PreviewEngineState::ShuttingDown;
                    engine.impl_->telemetrySnapshot.status.lastError =
                        engine.impl_->machine.lastError();
                }
                // startWorkerShutdown()はこのlock中に開始しているため、unlock直後から
                // shutdown workerがteardownを進めterminal eventを積み得る。
                // ShuttingDownのcommitとeventの挿入を同じlock区間に収める。
                engine.impl_->notifyLocked(ErrorOccurredEvent{*resumeFailure}, seekDispatch);
                engine.impl_->notifyLocked(StateChangedEvent{PreviewEngineState::ShuttingDown},
                                           seekDispatch);
            }
        }
        if (cancelledByShutdown)
            return Result<RenderFrameResult>::success(result);
        if (resumeFailure) {
            engine.impl_->flushDispatch(seekDispatch);
            return Result<RenderFrameResult>::failure(*resumeFailure);
        }
    }
    // state eventはcommitと同じcritical sectionで積んである。ここでは投函だけ。
    engine.impl_->flushDispatch(seekDispatch);

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
        const bool detachViolation = engine.impl_->hasVideoWorkerLocked() || engine.impl_->pairer ||
                                     engine.impl_->audioSink || engine.impl_->audioWorker ||
                                     engine.impl_->audioClock;
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
        engine.impl_->finalRuntimeDiagnostics.staleCompositionEpochRejectCount =
            engine.impl_->staleCompositionEpochRejectCount;
        engine.impl_->finalRuntimeDiagnostics.lastStaleCompositionRejectedFrame =
            engine.impl_->lastStaleCompositionRejectedFrame;
        for (gpu::SourceDecodeWorker* teardownVideo : engine.impl_->videoWorkersForTeardown()) {
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
        engine.impl_->finalRuntimeDiagnostics.videoMasterQpcFallbackCount =
            engine.impl_->videoMasterQpcFallbackCount;
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
            // pairerはbufferをraw pointerで握る。worker本体より先に手放す。
            engine.impl_->pairer.reset();
            engine.impl_->coordinator.reset();
            engine.impl_->coordinatorSources.clear();
            for (auto& [publicId, entry] : engine.impl_->videoSources) {
                (void)publicId;
                entry.worker.reset();
            }
            engine.impl_->detachedWorkers.videoWorkers.clear();
            // audio sink は queue/clock を参照するため、参照する側から解放する。
            engine.impl_->audioSink.reset();
            engine.impl_->audioWorker.reset();
            engine.impl_->audioClock.reset();
            engine.impl_->detachedWorkers.audioSink.reset();
            engine.impl_->detachedWorkers.audioWorker.reset();
            engine.impl_->detachedWorkers.audioClock.reset();
            // unregisterは`PreviewSourceId`昇順で決定論的に行う。
            for (const auto& [publicId, entry] : engine.impl_->videoSources) {
                (void)publicId;
                if (entry.internal.value != 0)
                    engine.impl_->sourceRegistry.unregisterSource(entry.internal);
            }
            engine.impl_->videoSources.clear();
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
            engine.impl_->pairer.reset();
            engine.impl_->coordinator.reset();
            engine.impl_->coordinatorSources.clear();
            for (auto& [publicId, entry] : engine.impl_->videoSources) {
                (void)publicId;
                if (entry.worker)
                    retainedWorkers->push_back(std::move(entry.worker));
            }
            for (auto& detached : engine.impl_->detachedWorkers.videoWorkers) {
                if (detached)
                    retainedWorkers->push_back(std::move(detached));
            }
            engine.impl_->detachedWorkers.videoWorkers.clear();
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
        for (gpu::SourceDecodeWorker* worker : engine.impl_->videoWorkersLocked())
            worker->pause();
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
    const std::vector<gpu::SourceDecodeWorker*> workers = engine.impl_->videoWorkersLocked();
    if (workers.empty() || engine.impl_->machine.state() != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "decoder fatal faultは再生中のvideo workerにのみ設定できます");
    }
    // faultは先頭sourceだけに入れる。全sourceへ入れると「1本の失敗が
    // session全体をfatalにする」ことの検査にならない。
    workers.front()->injectFatalForTest(detail);
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectDecoderEofForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    const std::vector<gpu::SourceDecodeWorker*> workers = engine.impl_->videoWorkersLocked();
    if (workers.empty() || engine.impl_->machine.state() != PreviewEngineState::Playing) {
        return invalidState(PreviewOperation::RenderDeviceAttach,
                            "decoder EOF faultは再生中のvideo workerにのみ設定できます");
    }
    workers.front()->injectEofForTest();
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
    for (const auto& [publicId, entry] : engine.impl_->videoSources) {
        if (entry.worker) {
            result.videoSourceGenerations.emplace(publicId,
                                                  entry.worker->snapshot().sourceGeneration.value);
        }
    }
    result.distinctPresentedSourceFrameCount = engine.impl_->distinctPresentedFrames.count();
    result.staleSubstitutionCount = engine.impl_->staleSubstitutionCount;
    result.staleCompositionEpochRejectCount = engine.impl_->staleCompositionEpochRejectCount;
    result.lastStaleCompositionRejectedFrame = engine.impl_->lastStaleCompositionRejectedFrame;
    result.lifecycleViolationCount = engine.impl_->lifecycleViolationCount;
    result.shutdownSequence = engine.impl_->shutdownSequence;
    result.audioMasterActive = engine.impl_->audioMasterActive;
    result.renderVisibleWorkersDetached = engine.impl_->renderVisibleWorkersDetached;
    result.audioSinkJoined = engine.impl_->audioSinkJoined;
    result.audioWorkerJoined = engine.impl_->audioWorkerJoined;
    result.registeredAudioSourceCount = engine.impl_->publicAudioSource ? 1U : 0U;
    result.audioMasterProjectionFailureCount = engine.impl_->audioMasterProjectionFailureCount;
    result.videoMasterQpcFallbackCount = engine.impl_->videoMasterQpcFallbackCount;
    result.audioGenerationMismatchCount = engine.impl_->audioGenerationMismatchCount;
    result.seekRequestCount = engine.impl_->seekRequestCount;
    result.seekVideoRequestAcceptedCount = engine.impl_->seekVideoRequestAcceptedCount;
    result.seekDecodeReadyCount = engine.impl_->seekDecodeReadyCount;
    result.seekCompletedCount = engine.impl_->seekCompletedCount;
    result.seekAwaitingPresentationCount = engine.impl_->seekAwaitingPresentationCount;
    result.seekStaleGenerationRejectCount = engine.impl_->seekStaleGenerationRejectCount;
    result.seekCancelledByShutdownCount = engine.impl_->seekCancelledByShutdownCount;
    result.lastSeekTargetFrame = engine.impl_->lastSeekTargetFrame;
    result.lastSeekPresentedFrame = engine.impl_->lastSeekPresentedFrame;
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
    // 全video sourceを畳んで報告する。1本でもsoftware decodeやdevice mismatchを
    // 起こしていれば、それがそのまま診断値になる。
    const std::vector<gpu::SourceDecodeWorker*> diagVideos =
        engine.impl_->videoWorkersForTeardown();
    if (!diagVideos.empty()) {
        result.d3d11vaActive = true;
        result.decodeRenderSameDevice = true;
        result.softwareFallbackCount = 0;
    }
    for (gpu::SourceDecodeWorker* diagVideo : diagVideos) {
        const gpu::SourceDecoderSnapshot decoder = diagVideo->snapshot();
        result.d3d11vaActive =
            result.d3d11vaActive && decoder.open && decoder.softwareFrameRejectCount == 0;
        result.decodeRenderSameDevice =
            result.decodeRenderSameDevice &&
            decoder.decodeDevicePointer ==
                reinterpret_cast<std::uintptr_t>(engine.impl_->renderDevice->device());
        result.softwareFallbackCount +=
            static_cast<std::uint64_t>(decoder.softwareFrameRejectCount);
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

Result<void> PreviewRenderPort::armAudioPlayBarrierForTest(PreviewEngine& engine) {
    std::shared_ptr<audio::WasapiAudioSink> sink;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        sink = engine.impl_->audioSink;
    }
    if (!sink) {
        return invalidState(PreviewOperation::Seek,
                            "audio sourceが未登録のためplay barrierを設定できません");
    }
    sink->armPlayBarrierForTest();
    return Result<void>::success();
}

bool PreviewRenderPort::waitAudioPlayBarrierEnteredForTest(PreviewEngine& engine, int timeoutMs) {
    std::shared_ptr<audio::WasapiAudioSink> sink;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        sink = engine.impl_->audioSink;
    }
    return sink != nullptr && sink->waitPlayBarrierEnteredForTest(timeoutMs);
}

void PreviewRenderPort::releaseAudioPlayBarrierForTest(PreviewEngine& engine) {
    std::shared_ptr<audio::WasapiAudioSink> sink;
    {
        std::lock_guard<std::mutex> lock(engine.impl_->mutex);
        sink = engine.impl_->audioSink;
    }
    if (sink)
        sink->releasePlayBarrierForTest();
}

Result<void> PreviewRenderPort::injectAudioSinkPlayFaultForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->audioSink) {
        return invalidState(PreviewOperation::Seek,
                            "audio sourceが未登録のためplay faultを注入できません");
    }
    engine.impl_->audioSink->injectPlayFaultForTest();
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectSeekAudioGenerationMismatchForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->audioWorker) {
        return invalidState(PreviewOperation::Seek,
                            "audio sourceが未登録のためgeneration mismatchを注入できません");
    }
    engine.impl_->seekAudioGenerationMismatchInjected = true;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectCompositionEpochAdvanceForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->coordinator || !engine.impl_->pairer) {
        return invalidState(PreviewOperation::SubmitComposition,
                            "composition runtimeが未構成のためepoch advanceを注入できません");
    }
    engine.impl_->compositionEpochAdvanceInjected = true;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::armSourceRemovalBarrierForTest(PreviewEngine& engine) {
    if (!engine.impl_->removalBarrier.arm()) {
        return invalidState(PreviewOperation::RemoveSource,
                            "source removal barrierは既に設定されています");
    }
    return Result<void>::success();
}

bool PreviewRenderPort::waitSourceRemovalBarrierEnteredForTest(PreviewEngine& engine,
                                                               int timeoutMs) {
    return engine.impl_->removalBarrier.waitEntered(timeoutMs);
}

void PreviewRenderPort::releaseSourceRemovalBarrierForTest(PreviewEngine& engine) {
    engine.impl_->removalBarrier.release();
}

Result<void> PreviewRenderPort::armFatalPublishBarrierForTest(PreviewEngine& engine) {
    if (!engine.impl_->fatalPublishBarrier.arm()) {
        return invalidState(PreviewOperation::RemoveSource,
                            "fatal publish barrierは既に設定されています");
    }
    return Result<void>::success();
}

bool PreviewRenderPort::waitFatalPublishBarrierEnteredForTest(PreviewEngine& engine,
                                                              int timeoutMs) {
    return engine.impl_->fatalPublishBarrier.waitEntered(timeoutMs);
}

void PreviewRenderPort::releaseFatalPublishBarrierForTest(PreviewEngine& engine) {
    engine.impl_->fatalPublishBarrier.release();
}

Result<void> PreviewRenderPort::injectSeekPresentationStallForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->hasVideoWorkerLocked()) {
        return invalidState(PreviewOperation::Seek,
                            "video sourceが未登録のためseek stallを注入できません");
    }
    engine.impl_->seekPresentationStallInjected = true;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::suspendVideoSourceForTest(PreviewEngine& engine,
                                                          PreviewSourceId source) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    const auto entry = engine.impl_->videoSources.find(source.value);
    if (entry == engine.impl_->videoSources.end() || !entry->second.worker) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::RenderDeviceAttach,
                                               "停止対象のvideo sourceが登録されていません"));
    }
    entry->second.worker->pause();
    entry->second.worker->buffer().clear();
    return Result<void>::success();
}

Result<void> PreviewRenderPort::injectSeekVideoGenerationMismatchForTest(PreviewEngine& engine,
                                                                         PreviewSourceId source) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    const auto entry = engine.impl_->videoSources.find(source.value);
    if (entry == engine.impl_->videoSources.end() || !entry->second.worker) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::Seek,
                      "generation mismatch対象のvideo sourceが未登録です"));
    }
    engine.impl_->seekVideoGenerationMismatchInjected = source;
    return Result<void>::success();
}

Result<void> PreviewRenderPort::armVideoSeekRequestForTest(PreviewEngine& engine,
                                                           PreviewSourceId source,
                                                           PreviewPosition target) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    const auto entry = engine.impl_->videoSources.find(source.value);
    if (entry == engine.impl_->videoSources.end() || !entry->second.worker) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::Seek,
                                               "seek request対象のvideo sourceが未登録です"));
    }
    gpu::SeekTicket ticket;
    std::string error;
    if (entry->second.worker->requestSeek(target.outputFrame, ticket, error) !=
        gpu::SeekRequestResult::Accepted) {
        return invalidState(PreviewOperation::Seek,
                            "video sourceのseek mailboxをbusyにできません: " + error);
    }
    return Result<void>::success();
}

Result<void> PreviewRenderPort::setVideoSourceLimitForTest(PreviewEngine& engine,
                                                           std::uint32_t limit) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (engine.impl_->machine.state() != PreviewEngineState::ReadyPaused ||
        !engine.impl_->videoSources.empty() || limit < 1) {
        return invalidState(PreviewOperation::AddSource,
                            "video source上限はReadyPausedかつsource未登録時に設定してください");
    }
    engine.impl_->capability.configuredMaxActiveVideoSources = limit;
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

Result<void> PreviewRenderPort::injectVideoMasterQpcFallbackForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (!engine.impl_->publicAudioSource) {
        return invalidState(PreviewOperation::Play,
                            "audio sourceが未登録のためQPC master退避を注入できません");
    }
    // 完成したerrorを注入しない。master選択だけを誤らせ、product側の検査が
    // 実際にQPC退避を捕まえるかどうかを通常のscheduler経路で確かめる。
    engine.impl_->videoMasterQpcFallbackInjected = true;
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

std::vector<PreviewEvent> PreviewRenderPort::mailboxEventsForTest(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    return engine.impl_->mailbox.snapshot();
}

std::size_t PreviewRenderPort::mailboxSizeForTest(const PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    return engine.impl_->mailbox.size();
}

} // namespace internal
} // namespace mvm::preview
