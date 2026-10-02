#include "core/checked_integer.h"
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
#include "media/gpu_preview/still_image_frame.h"
#include "preview_engine/preview_engine_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
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

PreviewError capacityError(PreviewOperation operation, std::string detail) {
    auto error =
        makeError(PreviewErrorCategory::UnsupportedCapability, operation, std::move(detail));
    error.code = PreviewErrorCode::RegistrationCapacityExceeded;
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
    if (descriptor.videoTimelineMappingEnabled &&
        (descriptor.expectedVideoSourceFrameRate.numerator == 0 ||
         descriptor.expectedVideoSourceFrameRate.denominator == 0)) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                      "timeline mappingにはexpected source FPSが必要です"));
    }
    if (descriptor.speedNum <= 0 || descriptor.speedDen <= 0) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource,
                                               "再生速度は正の有理数である必要があります"));
    }
    if (descriptor.videoTimelineMappingEnabled &&
        (descriptor.videoSourceInFrame < 0 ||
         descriptor.videoSourceFrameCount <= descriptor.videoSourceInFrame)) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                      "timeline mappingには素材inより大きい素材frame数が必要です"));
    }
    if (descriptor.videoHoldOutputFrames < 0 ||
        (descriptor.videoHoldOutputFrames > 0 &&
         (!descriptor.videoEnabled || !descriptor.videoTimelineMappingEnabled ||
          descriptor.speedNum != 1 || descriptor.speedDen != 1))) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource,
                                               "フレーム保持の映像設定が不正です"));
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
    (void)numerator;
    (void)denominator;
    (void)outputFrameRate;
    return Result<void>::success();
}

Result<void> validateExpectedSourceFrameRate(long long actualNumerator, long long actualDenominator,
                                             PreviewFrameRate expectedFrameRate) {
    if (actualNumerator <= 0 || actualDenominator <= 0 || expectedFrameRate.numerator == 0 ||
        expectedFrameRate.denominator == 0) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource,
                                               "source FPS authorityが不正です"));
    }
    auto actualNum = static_cast<std::uint64_t>(actualNumerator);
    auto actualDen = static_cast<std::uint64_t>(actualDenominator);
    const std::uint64_t actualDivisor = std::gcd(actualNum, actualDen);
    actualNum /= actualDivisor;
    actualDen /= actualDivisor;
    auto expectedNum = static_cast<std::uint64_t>(expectedFrameRate.numerator);
    auto expectedDen = static_cast<std::uint64_t>(expectedFrameRate.denominator);
    const std::uint64_t expectedDivisor = std::gcd(expectedNum, expectedDen);
    expectedNum /= expectedDivisor;
    expectedDen /= expectedDivisor;
    if (actualNum != expectedNum || actualDen != expectedDen) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                      "Projectのsource FPSと実ファイルのFPSが一致しません"));
    }
    return Result<void>::success();
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
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::UnsupportedCapability,
                             "設定されたcomposition layer countを超えています"));
    }

    std::set<std::uint64_t> distinctSources;
    for (const PreviewCompositionLayer& layer : snapshot->layers) {
        if (layer.stillImage) {
            // 静止画 layer は decode source を持たない。画素と配置だけを検査する。
            const PreviewStillImage& image = *layer.stillImage;
            if (layer.source.value != 0 || image.width <= 0 || image.height <= 0 ||
                image.rgba.size() != static_cast<std::size_t>(image.width) *
                                         static_cast<std::size_t>(image.height) * 4U) {
                return Result<AcceptedComposition>::failure(
                    compositionError(PreviewErrorCategory::CompositionFailure,
                                     "静止画 layerの画素または寸法が不正です"));
            }
            // 静止画には decode した source frame が無く、compositor は fade を source frame
            // 番号から評価する。fade は呼び出し側が opacity へ評価済みの値で渡す。
            if (layer.effectsEnabled && (layer.fadeInFrames != 0 || layer.fadeOutFrames != 0)) {
                return Result<AcceptedComposition>::failure(compositionError(
                    PreviewErrorCategory::CompositionFailure,
                    "静止画 layerのfadeは受理しません。opacityへ評価済みの値を渡してください"));
            }
            continue;
        }
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
                "同一sourceのduplicate layerは現在の構成では扱えません", layer.source));
        }
    }
    if (distinctSources.size() > capabilities.configuredMaxActiveVideoSources) {
        return Result<AcceptedComposition>::failure(
            compositionError(PreviewErrorCategory::UnsupportedCapability,
                             "設定されたactive video source countを超えています"));
    }
    // decode source を持たない composition (静止画だけ、または空) の提示の authority は
    // scheduler の時計 (音声があれば audio master) である。docs/preview-engine-contract.md。

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
        if (layer.opaqueBackdrop && (layer.stillImage || layer.rotationDegrees != 0.0F)) {
            return Result<AcceptedComposition>::failure(compositionError(
                PreviewErrorCategory::CompositionFailure,
                "opaque backdropは回転の無いvideo layerだけに使えます", layer.source));
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
        // 通常は requestShutdown が準備を取り消して join 済み。経路を問わず thread を残さない。
        for (auto& [id, preparation] : preparations) {
            (void)id;
            preparation->cancelled.store(true, std::memory_order_release);
        }
        preparationHold->wakeAll();
        for (auto& [id, preparation] : preparations) {
            (void)id;
            finish(preparation->thread);
        }
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
        // decode する video source と合成する layer の上限は別の値である。
        // 文字などの静止画 layer は decode source を増やさずに layer だけを増やす。
        // ここは「現在の構成で受理できる上限」であって qualification ではない。
        // 実測した組は measuredEnvelope (既定値 = 60/1 cohort) が持ち、この値はその外である。
        value.configuredMaxActiveVideoSources = kProductMaxActiveVideoSources;
        value.configuredMaxCompositionLayers = kProductMaxCompositionLayers;
        // P5-D2でaudio-master transportを接続したため、audio domainを公開する。
        value.configuredMaxActiveAudioSources = kProductMaxActiveAudioSources;
        value.configuredAudioSampleRate = audio::kInternalSampleRate;
        value.configuredAudioChannelCount = audio::kInternalChannels;
        return value;
    }();
    PreviewTelemetry telemetrySnapshot;
    std::array<std::int64_t, 256> presentedOutputFrames{};
    // presentedOutputFrames と同じ位置に、その frame で提示した composition の layer 数と
    // 最前面 layer の不透明度を持つ。再生中の不透明度の変化が提示に届いたかを試験が見る。
    std::array<std::uint32_t, 256> presentedLayerCounts{};
    std::array<float, 256> presentedTopLayerOpacities{};
    // 最背面の decode layer の public source ID と素材 frame (decode layer が無ければ 0 / -1)。
    std::array<std::uint64_t, 256> presentedBaseSources{};
    std::array<std::int64_t, 256> presentedBaseSourceFrames{};
    std::size_t presentedOutputFrameCount = 0;
    std::size_t presentedOutputFrameNext = 0;
    std::array<std::int64_t, 256> unpairedOutputFrames{};
    std::size_t unpairedOutputFrameCount = 0;
    std::size_t unpairedOutputFrameNext = 0;

    void notePresentedOutputFrameLocked(std::int64_t frame, const CompositionSnapshot& snapshot) {
        presentedOutputFrames[presentedOutputFrameNext] = frame;
        presentedLayerCounts[presentedOutputFrameNext] =
            static_cast<std::uint32_t>(snapshot.layers.size());
        presentedTopLayerOpacities[presentedOutputFrameNext] =
            snapshot.layers.empty() ? 0.0F : snapshot.layers.back().opacity;
        presentedBaseSources[presentedOutputFrameNext] = 0;
        presentedBaseSourceFrames[presentedOutputFrameNext] = -1;
        presentedOutputFrameNext = (presentedOutputFrameNext + 1) % presentedOutputFrames.size();
        presentedOutputFrameCount =
            std::min(presentedOutputFrameCount + 1, presentedOutputFrames.size());
    }

    // notePresentedOutputFrameLocked の直後に呼び、同じ位置へ最背面の decode layer を記録する。
    void notePresentedBaseLayerLocked(const gpu::ComposedFrame& composed) {
        const std::size_t at = (presentedOutputFrameNext + presentedOutputFrames.size() - 1) %
                               presentedOutputFrames.size();
        for (const auto& layer : composed.layers) {
            if (layer.frame.pixelFormat == gpu::GpuPixelFormat::RGBA8 &&
                !layer.frame.sourceId.value)
                continue;
            const auto publicSource = publicIdForInternalLocked(layer.frame.sourceId);
            presentedBaseSources[at] = publicSource ? publicSource->value : 0;
            presentedBaseSourceFrames[at] = layer.frame.frameNumber;
            return;
        }
    }

    void noteUnpairedOutputFrameLocked(std::int64_t frame) {
        unpairedOutputFrames[unpairedOutputFrameNext] = frame;
        unpairedOutputFrameNext = (unpairedOutputFrameNext + 1) % unpairedOutputFrames.size();
        unpairedOutputFrameCount =
            std::min(unpairedOutputFrameCount + 1, unpairedOutputFrames.size());
    }

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
    std::size_t registeredVideoSourceLimit = 2 * std::size_t{kProductMaxActiveVideoSources} + 1;

    // 静止画 layer の GPU frame。render thread だけが作り、捨てる。
    // key の pointer が再利用されないよう、image 本体の所有権も entry が持つ。
    struct StillImageEntry {
        std::shared_ptr<const PreviewStillImage> image;
        gpu::DecodedGpuFrame frame;
    };

    std::map<const PreviewStillImage*, StillImageEntry> stillImages;

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

    // 再生中の audio source の位置。addSource と先読みの準備が、要求した時点と公開する時点で
    // 同じ規則で求める。
    struct AudioPlacement {
        std::int64_t timelineSample = 0;
        // 初期 seek 先の素材 sample (0 未満は 0)。
        std::int64_t mediaSample = 0;
        // audio sink がまだ無く、この source の区間がこれから始まる。sink は主入力を外し、
        // mix 入力として offset 付きで鳴らす。
        bool futureFirst = false;
        std::int64_t activationFrame = -1;
    };

    // source の登録の途中の状態。addSource と先読みの準備が同じ段 (begin / run / publish) を通る。
    //   begin   engine lock 内。検査し、worker を作り、再生位置から決まる値を確定する
    //   run     engine lock 外。decoder の open と初期 seek を待つ (先読みでは準備用の thread)
    //   publish engine lock 内。state を検査し直し、audio mix / sink へ繋いで公開する
    struct SourceWork {
        PreviewSourceDescriptor descriptor;
        bool whilePlaying = false;
        std::string path;
        PreviewFrameRate configuredFrameRate{};
        // begin で予約した public source ID。公開しなかったら releaseReservedIdLocked で戻す。
        std::uint64_t reservedId = 0;
        gpu::SourceId internalVideo{};
        std::unique_ptr<gpu::SourceDecodeWorker> videoWorker;
        long long videoFirstSourceFrame = 0;
        long long videoFirstOutputFrame = 0;
        audio::SourceId internalAudio{};
        std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
        AudioPlacement audioPlacement;
        // run が decoder の open で失敗した (telemetry の decodeFailureCount へ数える)。
        bool decodeFailed = false;
    };

    // 準備用の thread を試験から止める。止めている間に pause / seek などを起こし、古くなった
    // 完了が公開されないことを決定論的に確かめる。取り消した準備は止めない。
    struct PreparationHold {
        std::mutex mutex;
        std::condition_variable changed;
        bool held = false;

        void wakeAll() {
            {
                std::lock_guard<std::mutex> lock(mutex);
            }
            changed.notify_all();
        }
    };

    struct SourcePreparation {
        std::uint64_t id = 0;
        // 要求した時点の transportGeneration。公開する時点と違えば古い。
        std::uint64_t generation = 0;
        SourceWork work;
        std::thread thread;
        std::atomic<bool> cancelled{false};
        // 完了を待たれている。試験用の hold でも止めない (待つ側と hold を外す側が同じ
        // control thread なので、止めると待ち続ける)。
        std::atomic<bool> awaited{false};
        std::atomic<bool> done{false};
        // thread が書き、done を立てた後に control thread が読む。
        std::optional<Result<void>> result;
    };

    std::map<std::uint64_t, std::shared_ptr<SourcePreparation>> preparations;
    std::uint64_t nextPreparationId = 1;
    // pause / seek / shutdown で進める。要求の後に transport が変わった準備を見分ける。
    std::uint64_t transportGeneration = 0;
    std::uint64_t staleSourcePreparationRejectCount = 0;
    // 再生中に最初の audio を公開したときに、control thread で endpoint の open から再生開始まで
    // に掛かった時間 (WASAPI は COM を初期化した thread で扱うので準備用の thread へ移せない)。
    std::uint64_t playingAudioEndpointOpenCount = 0;
    double maxPlayingAudioEndpointOpenMs = 0.0;
    // 試験用: 次に要求する準備を、取り消しの効かない段でこの時間止める。
    std::chrono::milliseconds nextPreparationBlockForTest{0};
    // 再生中の endpoint の open を試みた回数・失敗した回数・掛かった時間の最大 (成功・失敗とも)。
    std::uint64_t playingAudioEndpointOpenAttemptCount = 0;
    std::uint64_t playingAudioEndpointOpenFailureCount = 0;
    std::uint64_t playingAudioTransportStartFailureCount = 0;
    double maxPlayingAudioEndpointOpenAttemptMs = 0.0;
    // 試験用: 次の再生中の endpoint の open を、この時間待ってから失敗させる。
    std::optional<std::chrono::milliseconds> failNextPlayingEndpointOpenForTest;
    // 試験用: 次の再生中の endpoint の open は成功させ、再生開始を失敗させる。
    bool failNextPlayingTransportStartForTest = false;
    std::shared_ptr<PreparationHold> preparationHold = std::make_shared<PreparationHold>();

    Result<AudioPlacement> audioPlacementLocked(const PreviewSourceDescriptor& descriptor) const;
    Result<void> beginSourceWorkLocked(const PreviewSourceDescriptor& descriptor, SourceWork& work);
    Result<PreviewSourceId>
    publishSourceWorkLocked(SourceWork& work, std::unique_lock<std::mutex>& lock, bool deferred);
    void rollbackSourceWorkLocked(SourceWork& work);
    Result<PreviewSourceId> finishPreparationLocked(SourcePreparation& preparation,
                                                    std::unique_lock<std::mutex>& lock);
    static Result<void> runSourceWork(SourceWork& work, const std::atomic<bool>* cancelled);

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
    std::int64_t primaryAudioSampleOffset = 0;
    bool primaryAudioAsMix = false;
    std::int64_t primaryAudioActivationFrame = -1;
    std::optional<std::int64_t> pendingAudioActivationFrame;

    struct ExtraAudioSourceEntry {
        audio::SourceId internal;
        std::shared_ptr<audio::AudioDecodeWorker> worker;
        std::int64_t sampleOffset = 0;
    };

    std::map<std::uint64_t, ExtraAudioSourceEntry> extraAudioSources;
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
        std::vector<std::shared_ptr<audio::AudioDecodeWorker>> extraAudioWorkers;

        // 宣言順という規約に依存すると、将来の並べ替えで無言のuse-after-freeへ
        // 戻り得る (このUAFはcrashしないため、testでも捕まえられない)。
        // 依存の逆順をdestructorで固定し、宣言順に関係なく安全にする。
        ~DetachedWorkers() {
            audioSink.reset();
            audioWorker.reset();
            extraAudioWorkers.clear();
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
        for (auto& [id, entry] : extraAudioSources) {
            (void)id;
            detachedWorkers.extraAudioWorkers.push_back(std::move(entry.worker));
        }
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
            // 静止画 layer は pairing の対象外。zOrder は snapshot 全体の添字のまま
            // 使うので、後で差し込む静止画との前後関係が崩れない。
            if (layer.stillImage)
                continue;
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
            mapped.opaqueBackdrop = layer.opaqueBackdrop;
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
            if (layer.stillImage)
                continue;
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

    // pairing 済みの video layer へ静止画 layer を差し込み、snapshot の z 順に並べる。
    // texture は初回だけ作る。現在の snapshot が参照しない静止画は compositor の
    // SRV cache から外して捨てる (GPU 完了までは retirement が保持する)。
    // render thread で、pairing と提示直前の検証を終えた後に呼ぶ。
    std::optional<std::string> addStillLayersLocked(const CompositionSnapshot& snapshot,
                                                    gpu::ComposedFrame& composed) {
        std::set<const PreviewStillImage*> referenced;
        for (std::size_t i = 0; i < snapshot.layers.size(); ++i) {
            const PreviewCompositionLayer& layer = snapshot.layers[i];
            if (!layer.stillImage)
                continue;
            const PreviewStillImage* key = layer.stillImage.get();
            referenced.insert(key);
            auto entry = stillImages.find(key);
            if (entry == stillImages.end()) {
                StillImageEntry created;
                created.image = layer.stillImage;
                std::string error;
                if (!gpu::makeStillImageFrame(*renderDevice, key->width, key->height,
                                              key->rgba.data(), key->rgba.size(), gpu::SourceId{},
                                              created.frame, error))
                    return error;
                entry = stillImages.emplace(key, std::move(created)).first;
            }
            gpu::CompositionLayerFrame still;
            still.frame = entry->second.frame;
            still.destination = {layer.destination.x, layer.destination.y, layer.destination.width,
                                 layer.destination.height};
            still.sourceUv = {layer.sourceRect.x, layer.sourceRect.y, layer.sourceRect.width,
                              layer.sourceRect.height};
            still.opacity = layer.opacity;
            still.zOrder = static_cast<int>(i);
            still.effectsEnabled = layer.effectsEnabled;
            still.rotationDegrees = layer.rotationDegrees;
            composed.layers.push_back(std::move(still));
        }
        std::stable_sort(composed.layers.begin(), composed.layers.end(),
                         gpu::deterministicLayerLess);
        for (auto entry = stillImages.begin(); entry != stillImages.end();) {
            if (referenced.contains(entry->first)) {
                ++entry;
                continue;
            }
            if (compositor)
                compositor->retireLayerTexture(entry->second.frame.texture);
            entry = stillImages.erase(entry);
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
        std::int64_t primaryAudioWorkerSample = 0;
        std::map<std::uint64_t, gpu::SeekTicket> videoTickets;
        std::map<std::uint64_t, std::int64_t> expectedSourceFrames;
        audio::AudioSeekTicket audioTicket;
        std::map<std::uint64_t, audio::AudioSeekTicket> extraAudioTickets;
        std::map<std::uint64_t, std::int64_t> extraAudioSamples;
        std::map<std::uint64_t, audio::SourceGeneration> expectedExtraAudioGenerations;
        bool audioReady = false;
        bool decodeReady = false;
        bool resumePlaying = false;
        std::map<std::uint64_t, gpu::SourceGeneration> expectedVideoGenerations;
        audio::SourceGeneration expectedAudioGeneration{};
        std::chrono::steady_clock::time_point deadline;
        // renderFrameDue が完了の回収中に見つけた失敗。renderFrame が引き取って表面化する
        // (完了は一度しか回収できないので、ここに置かないと失敗を取りこぼす)。
        std::optional<PreviewError> dueFailure;
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
        if (publicAudioSource && !masterAccepted && !pendingAudioActivationFrame) {
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
        if (pendingAudioActivationFrame && result.frame >= *pendingAudioActivationFrame) {
            pendingAudioActivationFrame.reset();
            audioMasterActive = true;
            return schedulerTargetLocked(now);
        }
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
        for (const auto& [id, entry] : extraAudioSources) {
            (void)id;
            audioWorkerJoined =
                audioWorkerJoined && entry.worker && entry.worker->snapshot().joined;
        }
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
            std::vector<std::shared_ptr<audio::AudioDecodeWorker>> extraAudioDecoders;
            std::vector<gpu::SourceDecodeWorker*> workers;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                audioEndpoint = self->audioSink;
                audioDecoder = self->audioWorker;
                for (const auto& [id, entry] : self->extraAudioSources) {
                    (void)id;
                    extraAudioDecoders.push_back(entry.worker);
                }
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
            {
                std::lock_guard<std::mutex> transport(self->audioTransportMutex);
                if (audioDecoder)
                    audioDecoder->stop();
                for (const auto& decoder : extraAudioDecoders)
                    decoder->stop();
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
                for (const auto& decoder : extraAudioDecoders)
                    self->audioWorkerJoined = self->audioWorkerJoined && decoder->snapshot().joined;
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
            extraAudioDecoders.clear();
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
                    if (completion.firstOutputSample != pending.primaryAudioWorkerSample) {
                        return seekFailure(
                            "audio seekがrequested sampleを返しませんでした (requested=" +
                            std::to_string(pending.primaryAudioWorkerSample) +
                            ", first=" + std::to_string(completion.firstOutputSample) + ")");
                    }
                    if (primaryAudioAsMix && audioSink) {
                        std::string mixError;
                        if (!audioSink->updateMixInput(audioWorker->queue(),
                                                       completion.seekGeneration,
                                                       primaryAudioSampleOffset, mixError))
                            return seekFailure("audio mix inputを更新できません: " + mixError);
                    }
                    pending.expectedAudioGeneration = completion.seekGeneration;
                    pending.audioReady = true;
                }
            }
            for (const auto& [publicId, ticketValue] : pending.extraAudioTickets) {
                if (pending.expectedExtraAudioGenerations.contains(publicId))
                    continue;
                const auto entry = extraAudioSources.find(publicId);
                if (entry == extraAudioSources.end() || !entry->second.worker)
                    return seekFailure("audio mix seek中にsourceが解除されました",
                                       PreviewSourceId{publicId});
                audio::AudioSeekCompletion completion;
                const audio::AudioSeekWaitResult result =
                    entry->second.worker->waitSeek(ticketValue, 0, completion);
                if (result == audio::AudioSeekWaitResult::StaleTicket)
                    return seekFailure("audio mix seek completionのticketが一致しません",
                                       PreviewSourceId{publicId});
                if (result == audio::AudioSeekWaitResult::Ready) {
                    const auto expected = pending.extraAudioSamples.find(publicId);
                    if (!completion.completed || expected == pending.extraAudioSamples.end() ||
                        completion.firstOutputSample != expected->second) {
                        return seekFailure("audio mix seekがrequested sampleを返しませんでした",
                                           PreviewSourceId{publicId});
                    }
                    std::string mixError;
                    std::int64_t offsetDelta = 0;
                    if (!core::checkedSubtract(entry->second.sampleOffset, audioSampleOffset,
                                               offsetDelta)) {
                        mixError = "audio mix offsetの差がint64範囲を超えています";
                    }
                    if (!audioSink || !mixError.empty() ||
                        !audioSink->updateMixInput(entry->second.worker->queue(),
                                                   completion.seekGeneration, offsetDelta,
                                                   mixError)) {
                        return seekFailure("audio mix generationを更新できません: " + mixError,
                                           PreviewSourceId{publicId});
                    }
                    pending.expectedExtraAudioGenerations.emplace(publicId,
                                                                  completion.seekGeneration);
                }
            }
            if (pending.expectedVideoGenerations.size() == pending.videoTickets.size() &&
                pending.audioReady &&
                pending.expectedExtraAudioGenerations.size() == pending.extraAudioTickets.size()) {
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
        // ここで初めて構成が確定する。measured envelope との一致は derived getter が
        // 返すが、この flag が無いと未初期化の既定値が envelope と一致してしまう。
        impl_->capability.hasConfiguredEnvelope = true;
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
        // 構成は確定しなかった。measured 判定を false positive にしない。
        impl_->capability.hasConfiguredEnvelope = false;
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

namespace {

// begin で予約した ID を、公開しなかったときに戻す。後に別の予約があれば戻さない (欠番になる)。
void releaseReservedId(std::uint64_t& next, std::uint64_t reserved) {
    if (reserved != 0 && next == reserved + 1)
        next = reserved;
}

PreviewError stalePreparationError(std::string detail) {
    auto error = makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                           std::move(detail));
    error.code = PreviewErrorCode::PreparationStale;
    return error;
}

} // namespace

// decoder の open と初期 seek。engine lock を持たずに呼ぶ。engine の状態には触れない
// (先読みでは準備用の thread で走る)。cancelled が立てば段の間で止める。
Result<void> PreviewEngine::Impl::runSourceWork(SourceWork& work,
                                                const std::atomic<bool>* cancelled) {
    const auto stopped = [cancelled] {
        return cancelled && cancelled->load(std::memory_order_acquire);
    };
    const auto& descriptor = work.descriptor;
    if (work.videoWorker) {
        std::string openError;
        if (stopped())
            return Result<void>::failure(stalePreparationError("準備を取り消しました"));
        if (!work.videoWorker->start(work.path, openError)) {
            work.decodeFailed = true;
            return Result<void>::failure(
                makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::AddSource,
                          "D3D11VA video sourceをopenできません: " + openError));
        }
        const gpu::SourceDecoderSnapshot opened = work.videoWorker->snapshot();
        Result<void> supportedRate = internal::validateSourceFrameRate(
            opened.info.frameRate.num, opened.info.frameRate.den, work.configuredFrameRate);
        if (!supportedRate)
            return supportedRate;
        if (descriptor.videoTimelineMappingEnabled) {
            Result<void> expectedRate = internal::validateExpectedSourceFrameRate(
                opened.info.frameRate.num, opened.info.frameRate.den,
                descriptor.expectedVideoSourceFrameRate);
            if (!expectedRate)
                return expectedRate;
        }
        if (work.whilePlaying) {
            if (stopped())
                return Result<void>::failure(stalePreparationError("準備を取り消しました"));
            double decodeReadyMs = 0.0;
            if (!work.videoWorker->seekBlocking(work.videoFirstSourceFrame,
                                                work.videoFirstOutputFrame, decodeReadyMs,
                                                openError)) {
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::DecodeFailure, PreviewOperation::AddSource,
                              "再生中のvideo source初期seekに失敗しました: " + openError));
            }
        }
    }
    if (work.audioWorker) {
        std::string audioError;
        if (stopped())
            return Result<void>::failure(stalePreparationError("準備を取り消しました"));
        if (!work.audioWorker->setPlaybackSpeed(descriptor.speedNum, descriptor.speedDen,
                                                descriptor.audioPreservePitch, audioError) ||
            !work.audioWorker->start(work.path, audioError)) {
            work.decodeFailed = true;
            return Result<void>::failure(makeError(PreviewErrorCategory::DecodeFailure,
                                                   PreviewOperation::AddSource,
                                                   "audio sourceをopenできません: " + audioError));
        }
        if (work.whilePlaying) {
            const std::int64_t mediaSample = work.audioPlacement.mediaSample;
            audio::AudioSeekTicket ticket;
            if (work.audioWorker->requestSeek(mediaSample, ticket, audioError) !=
                audio::AudioSeekRequestResult::Accepted) {
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                              "再生中のaudio source初期seekを受理できません: " + audioError));
            }
            audio::AudioSeekCompletion completion;
            const auto waitResult =
                work.audioWorker->waitSeek(ticket, audio::kPrerollTimeoutMs, completion);
            if (waitResult != audio::AudioSeekWaitResult::Ready || !completion.completed ||
                completion.firstOutputSample != mediaSample) {
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                              "再生中のaudio source初期seekを完了できません"));
            }
            work.audioWorker->play();
        }
    }
    return Result<void>::success();
}

Result<PreviewEngine::Impl::AudioPlacement>
PreviewEngine::Impl::audioPlacementLocked(const PreviewSourceDescriptor& descriptor) const {
    AudioPlacement placement;
    const std::int64_t position = telemetrySnapshot.status.position.outputFrame;
    const auto unconvertible = [] {
        return Result<AudioPlacement>::failure(
            makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                      "再生中のaudio source位置を換算できません"));
    };
    if (!timebase)
        return unconvertible();
    const auto timelineSample = timebase->seekTargetSample(position + 1);
    std::int64_t mediaSample = 0;
    if (!timelineSample ||
        !core::checkedAdd(timelineSample.value(), descriptor.audioSampleOffset, mediaSample))
        return unconvertible();
    placement.timelineSample = timelineSample.value();
    if (!audioSink) {
        placement.activationFrame = descriptor.audioTimelineStartFrame;
        if (placement.activationFrame < 0 && mediaSample < 0) {
            std::int64_t firstTimelineSample = 0;
            if (!core::checkedSubtract(std::int64_t{0}, descriptor.audioSampleOffset,
                                       firstTimelineSample)) {
                return Result<AudioPlacement>::failure(
                    makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                              "audio sourceの開始sampleを表せません"));
            }
            const auto firstFrame = timebase->schedulerOutputFrame(firstTimelineSample);
            if (!firstFrame) {
                return Result<AudioPlacement>::failure(
                    makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                              "audio sourceの開始frameを換算できません"));
            }
            placement.activationFrame = firstFrame.value();
        }
        placement.futureFirst = placement.activationFrame > position + 1;
    }
    placement.mediaSample = std::max<std::int64_t>(0, mediaSample);
    return Result<AudioPlacement>::success(placement);
}

Result<void> PreviewEngine::Impl::beginSourceWorkLocked(const PreviewSourceDescriptor& descriptor,
                                                        SourceWork& work) {
    Result<void> valid = validatePreviewSourceDescriptor(descriptor);
    if (!valid)
        return valid;
    const bool addingWhilePlaying = machine.state() == PreviewEngineState::Playing;
    if (machine.state() != PreviewEngineState::ReadyPaused && !addingWhilePlaying) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidState,
                                               PreviewOperation::AddSource,
                                               "source registrationを受理できないstateです"));
    }
    if (!nativeDeviceAttached || !compositor || !renderDevice->valid()) {
        return Result<void>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "native render deviceの準備前にsourceを登録できません"));
    }
    // 準備中 (未公開) の source も登録枠を使う。公開した時点で枠を超えないように数える。
    std::size_t pendingVideo = 0;
    std::size_t pendingAudio = 0;
    for (const auto& [id, preparation] : preparations) {
        (void)id;
        pendingVideo += preparation->work.descriptor.videoEnabled ? 1U : 0U;
        pendingAudio += preparation->work.descriptor.audioEnabled ? 1U : 0U;
    }
    // source-set切替中は旧sourceを新composition提示まで保持する。active compositionの
    // 上限は引き続きcapabilityの値であり、登録slotだけを旧set + 新set + slip preview 1本まで許す。
    if (descriptor.videoEnabled &&
        videoSources.size() + pendingVideo >= registeredVideoSourceLimit) {
        return Result<void>::failure(capacityError(
            PreviewOperation::AddSource, "source-set切替用のvideo source登録上限を超えています"));
    }
    if (descriptor.audioEnabled) {
        if (capability.configuredMaxActiveAudioSources == 0) {
            return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                                   PreviewOperation::AddSource,
                                                   "audio sourceは現在の構成では扱えません"));
        }
        const std::size_t registeredAudioCount =
            (publicAudioSource ? 1U : 0U) + extraAudioSources.size() + pendingAudio;
        if (registeredAudioCount >= capability.configuredMaxActiveAudioSources) {
            return Result<void>::failure(capacityError(
                PreviewOperation::AddSource, "active audio sourceの登録上限を超えています"));
        }
    }
    if (nextPublicSourceId == 0) {
        return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                               PreviewOperation::AddSource,
                                               "PreviewSourceIdがoverflowしました"));
    }

    work.descriptor = descriptor;
    work.whilePlaying = addingWhilePlaying;
    const auto utf8Path = descriptor.mediaPath.u8string();
    work.path.assign(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
    work.configuredFrameRate = configuredFrameRate;
    // 準備が並行しても audio worker の内部 ID が重ならないよう、公開する ID を先に予約する。
    work.reservedId = nextPublicSourceId++;

    if (descriptor.videoEnabled) {
        work.internalVideo = sourceRegistry.registerSource();
        work.videoWorker = std::make_unique<gpu::SourceDecodeWorker>(work.internalVideo,
                                                                     *renderDevice, readbacks, 6);
        std::string openError;
        if (descriptor.videoTimelineMappingEnabled &&
            !work.videoWorker->configureOutputMapping(
                descriptor.videoSourceInFrame, descriptor.videoSourceFrameCount,
                {descriptor.speedNum, descriptor.speedDen}, descriptor.videoTimelineStartFrame,
                {static_cast<long long>(configuredFrameRate.numerator),
                 static_cast<long long>(configuredFrameRate.denominator)},
                descriptor.videoHoldOutputFrames, openError)) {
            rollbackSourceWorkLocked(work);
            return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                                   PreviewOperation::AddSource, openError));
        }
        if (addingWhilePlaying) {
            work.videoFirstSourceFrame = descriptor.videoTimelineMappingEnabled
                                             ? descriptor.videoSourceInFrame
                                             : telemetrySnapshot.status.position.outputFrame + 1;
            work.videoFirstOutputFrame = descriptor.videoTimelineMappingEnabled
                                             ? descriptor.videoTimelineStartFrame
                                             : work.videoFirstSourceFrame;
        }
    }
    if (descriptor.audioEnabled) {
        work.internalAudio = audio::SourceId{work.reservedId};
        work.audioWorker = std::make_shared<audio::AudioDecodeWorker>(work.internalAudio);
        work.audioWorker->queue().setGainAtSample(descriptor.audioGainAtMediaSample);
        if (addingWhilePlaying) {
            auto placement = audioPlacementLocked(descriptor);
            if (!placement) {
                rollbackSourceWorkLocked(work);
                return Result<void>::failure(placement.error());
            }
            work.audioPlacement = placement.value();
        }
    }
    return Result<void>::success();
}

void PreviewEngine::Impl::rollbackSourceWorkLocked(SourceWork& work) {
    if (work.audioWorker) {
        work.audioWorker->stop();
        work.audioWorker.reset();
    }
    if (work.videoWorker) {
        work.videoWorker->stop();
        work.videoWorker.reset();
    }
    if (work.internalVideo.value) {
        sourceRegistry.unregisterSource(work.internalVideo);
        work.internalVideo = {};
    }
    if (work.decodeFailed)
        ++telemetrySnapshot.decodeFailureCount;
    work.decodeFailed = false;
    releaseReservedId(nextPublicSourceId, work.reservedId);
    work.reservedId = 0;
}

Result<PreviewSourceId>
PreviewEngine::Impl::publishSourceWorkLocked(SourceWork& work, std::unique_lock<std::mutex>& lock,
                                             bool deferred) {
    const auto& descriptor = work.descriptor;
    const bool addingWhilePlaying = work.whilePlaying;
    AudioPlacement placement = work.audioPlacement;
    if (deferred && descriptor.audioEnabled) {
        // 準備している間に再生位置が進んだ。sink を作るか (主入力)、mix へ足すかは公開する
        // 時点で決め直す。主入力は sample の連続を要求するので、区間が既に始まっていたら
        // 要求した時点の seek 先では鳴らせない。
        auto current = audioPlacementLocked(descriptor);
        if (!current) {
            rollbackSourceWorkLocked(work);
            return Result<PreviewSourceId>::failure(current.error());
        }
        placement.timelineSample = current.value().timelineSample;
        placement.futureFirst = current.value().futureFirst;
        placement.activationFrame = current.value().activationFrame;
        if (!audioSink && !placement.futureFirst) {
            rollbackSourceWorkLocked(work);
            ++staleSourcePreparationRejectCount;
            return Result<PreviewSourceId>::failure(
                stalePreparationError("先読みした主audioの区間が準備の間に始まりました"));
        }
    }
    const bool futureFirstAudio = placement.futureFirst;
    const std::int64_t newAudioStartSample =
        futureFirstAudio ? placement.timelineSample : placement.mediaSample;

    std::shared_ptr<audio::AudioMasterClock> newAudioClock;
    std::shared_ptr<audio::WasapiAudioSink> newAudioSink;
    const auto rollback = [&] {
        if (newAudioSink)
            newAudioSink->stop();
        rollbackSourceWorkLocked(work);
    };
    if (descriptor.audioEnabled) {
        // ここはengine configの検査である。sample rateはtimebaseが保持する実際の値、
        // channelはcapabilityが公開している実際の値を使う。decode出力そのものの
        // domainは、実データが出そろうplay()側で観測値を使って検査する。
        const std::int64_t configuredSampleRate = timebase ? timebase->audioSampleRate() : 0;
        Result<void> domain = internal::validateQualifiedAudioDomain(
            static_cast<int>(configuredSampleRate),
            static_cast<int>(capability.configuredAudioChannelCount), "flt");
        if (!domain) {
            rollback();
            return Result<PreviewSourceId>::failure(domain.error());
        }
        if (!audioSink) {
            // WASAPI endpoint は COM を初期化した thread で open / 解放する必要があるので、
            // 先読みでも control thread のここで open する。
            std::string audioError;
            const auto endpointBegan = std::chrono::steady_clock::now();

            // 再生中の open は、成功・失敗にかかわらず control thread を止めた時間を記録する
            // (失敗した open が遅い機器を見落とさない)。抜けるときは必ず engine lock を持つ。
            struct EndpointAttempt {
                Impl& impl;
                bool playing;
                std::chrono::steady_clock::time_point began;
                // endpoint の open が成功した。その後の mix への接続・再生開始の失敗は open の
                // 失敗と分けて数える (open 自体が遅い・失敗する機器の診断と混ぜない)。
                bool opened = false;
                bool succeeded = false;

                ~EndpointAttempt() {
                    if (!playing)
                        return;
                    ++impl.playingAudioEndpointOpenAttemptCount;
                    if (!opened)
                        ++impl.playingAudioEndpointOpenFailureCount;
                    else if (!succeeded)
                        ++impl.playingAudioTransportStartFailureCount;
                    impl.maxPlayingAudioEndpointOpenAttemptMs =
                        std::max(impl.maxPlayingAudioEndpointOpenAttemptMs,
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - began)
                                     .count());
                }
            } attempt{*this, addingWhilePlaying, endpointBegan};

            newAudioClock = std::make_shared<audio::AudioMasterClock>();
            newAudioSink =
                std::make_shared<audio::WasapiAudioSink>(work.audioWorker->queue(), *newAudioClock);
            const auto injectedFailure =
                addingWhilePlaying ? std::exchange(failNextPlayingEndpointOpenForTest, std::nullopt)
                                   : std::nullopt;
            const bool injectedStartFailure =
                addingWhilePlaying && std::exchange(failNextPlayingTransportStartForTest, false);
            lock.unlock();
            bool endpointOpened = false;
            if (injectedFailure) {
                std::this_thread::sleep_for(*injectedFailure);
                audioError = "試験で open を失敗させました";
            } else {
                endpointOpened = newAudioSink->open(audioError, audioSessionVolume);
            }
            lock.lock();
            if (!endpointOpened) {
                rollback();
                ++audioTransportFailureCount;
                return Result<PreviewSourceId>::failure(makeError(
                    PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                    "WASAPI shared event-driven endpointをopenできません: " + audioError));
            }
            attempt.opened = true;
            if (addingWhilePlaying) {
                if (futureFirstAudio) {
                    if (!newAudioSink->detachPrimaryInput(work.audioWorker->queue(), audioError) ||
                        !newAudioSink->addMixInput(
                            work.audioWorker->queue(), descriptor.audioSampleOffset,
                            work.audioWorker->snapshot().sourceGeneration, audioError)) {
                        rollback();
                        return Result<PreviewSourceId>::failure(makeError(
                            PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                            "先読みaudio inputを登録できません: " + audioError));
                    }
                }
                lock.unlock();
                bool endpointPlaying = false;
                if (injectedStartFailure)
                    audioError = "試験で再生開始を失敗させました";
                else
                    endpointPlaying = newAudioSink->play(
                        newAudioStartSample, work.audioWorker->snapshot().sourceGeneration,
                        audioError);
                lock.lock();
                if (!endpointPlaying) {
                    rollback();
                    return Result<PreviewSourceId>::failure(
                        makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                                  "再生中のaudio transportを開始できません: " + audioError));
                }
                attempt.succeeded = true;
                ++playingAudioEndpointOpenCount;
                maxPlayingAudioEndpointOpenMs =
                    std::max(maxPlayingAudioEndpointOpenMs,
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - endpointBegan)
                                 .count());
            }
        }
    }

    if (machine.state() !=
            (addingWhilePlaying ? PreviewEngineState::Playing : PreviewEngineState::ReadyPaused) ||
        !nativeDeviceAttached) {
        rollback();
        return Result<PreviewSourceId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "sourceの準備中にengine stateが変わりました"));
    }

    // 複合 source はaudio mixへの登録まで成功してから公開する。先にvideo tableへ
    // 入れると、offset換算失敗時にvideoだけ残ってしまう。
    if (descriptor.audioEnabled && audioSink) {
        std::int64_t delta = 0;
        if (!core::checkedSubtract(descriptor.audioSampleOffset, audioSampleOffset, delta)) {
            rollback();
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource,
                          "audio mix offsetの差がint64範囲を超えています"));
        }
        std::string mixError;
        if (!audioSink->addMixInput(work.audioWorker->queue(), delta,
                                    work.audioWorker->snapshot().sourceGeneration, mixError)) {
            rollback();
            return Result<PreviewSourceId>::failure(
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::AddSource,
                          "audio mix inputを登録できません: " + mixError));
        }
    }

    const PreviewSourceId published{work.reservedId};
    work.reservedId = 0;
    if (descriptor.videoEnabled) {
        videoSources.emplace(published.value,
                             VideoSourceEntry{work.internalVideo, std::move(work.videoWorker)});
        work.internalVideo = {};
        if (addingWhilePlaying)
            videoSources.at(published.value).worker->play();
        workerJoined = false;
        deviceReleased = false;
        telemetrySnapshot.currentSourceQueueDepth = 0;
    }
    if (descriptor.audioEnabled && !audioSink) {
        internalAudioSource = work.internalAudio;
        publicAudioSource = published;
        audioClock = std::move(newAudioClock);
        audioWorker = std::move(work.audioWorker);
        audioSink = std::move(newAudioSink);
        audioSinkJoined = false;
        audioWorkerJoined = false;
        resumeAudioSample = 0;
        audioSampleOffset = futureFirstAudio ? 0 : descriptor.audioSampleOffset;
        primaryAudioSampleOffset = descriptor.audioSampleOffset;
        primaryAudioAsMix = futureFirstAudio;
        if (futureFirstAudio) {
            primaryAudioActivationFrame = placement.activationFrame;
            pendingAudioActivationFrame = placement.activationFrame;
        }
        if (addingWhilePlaying)
            audioMasterActive = !futureFirstAudio;
        const audio::WasapiSnapshot endpoint = audioSink->snapshot();
        deviceSnapshot.audioSampleRate =
            static_cast<std::uint32_t>(endpoint.deviceFormat.sampleRate);
        deviceSnapshot.audioChannelCount =
            static_cast<std::uint32_t>(endpoint.deviceFormat.channels);
    } else if (descriptor.audioEnabled) {
        extraAudioSources.emplace(
            published.value, ExtraAudioSourceEntry{work.internalAudio, std::move(work.audioWorker),
                                                   descriptor.audioSampleOffset});
    }
    eligibleSources.emplace(published.value, internal::EligibleSource{descriptor.videoEnabled,
                                                                      descriptor.audioEnabled});
    return Result<PreviewSourceId>::success(published);
}

Result<PreviewSourceId>
PreviewEngine::Impl::finishPreparationLocked(SourcePreparation& preparation,
                                             std::unique_lock<std::mutex>& lock) {
    // 取り消した準備、要求の後に transport が変わった準備は公開しない。open / seek が失敗して
    // いても古さを理由にする (呼び出し側はもう使わないので、失敗として数えさせない)。
    if (preparation.cancelled.load(std::memory_order_acquire) ||
        preparation.generation != transportGeneration ||
        machine.state() != PreviewEngineState::Playing) {
        rollbackSourceWorkLocked(preparation.work);
        ++staleSourcePreparationRejectCount;
        return Result<PreviewSourceId>::failure(
            stalePreparationError(preparation.cancelled.load(std::memory_order_acquire)
                                      ? "準備を取り消しました"
                                      : "準備の間にpause / seek / shutdownがありました"));
    }
    if (!preparation.result || !*preparation.result) {
        rollbackSourceWorkLocked(preparation.work);
        return Result<PreviewSourceId>::failure(
            preparation.result ? preparation.result->error()
                               : makeError(PreviewErrorCategory::InvalidState,
                                           PreviewOperation::AddSource, "準備が完了していません"));
    }
    return publishSourceWorkLocked(preparation.work, lock, true);
}

Result<PreviewSourceId> PreviewEngine::addSource(const PreviewSourceDescriptor& descriptor) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::AddSource);
    if (!affinity) {
        return Result<PreviewSourceId>::failure(affinity.error());
    }
    Impl::SourceWork work;
    Result<void> begun = impl_->beginSourceWorkLocked(descriptor, work);
    if (!begun)
        return Result<PreviewSourceId>::failure(begun.error());
    lock.unlock();
    Result<void> ran = Impl::runSourceWork(work, nullptr);
    lock.lock();
    if (!ran) {
        impl_->rollbackSourceWorkLocked(work);
        return Result<PreviewSourceId>::failure(ran.error());
    }
    return impl_->publishSourceWorkLocked(work, lock, false);
}

Result<PreviewPreparationId>
PreviewEngine::requestSourcePreparation(const PreviewSourceDescriptor& descriptor) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::AddSource);
    if (!affinity)
        return Result<PreviewPreparationId>::failure(affinity.error());
    if (impl_->machine.state() != PreviewEngineState::Playing) {
        return Result<PreviewPreparationId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "先読みの準備は再生中だけ受理します"));
    }
    auto preparation = std::make_shared<Impl::SourcePreparation>();
    Result<void> begun = impl_->beginSourceWorkLocked(descriptor, preparation->work);
    if (!begun)
        return Result<PreviewPreparationId>::failure(begun.error());
    // 主入力の audio (sink がまだ無い) は sample の連続を要求する。区間が既に始まっている
    // ものを先読みすると、公開した時点では要求した時点の seek 先が古くなっている。
    if (descriptor.audioEnabled && !impl_->audioSink &&
        !preparation->work.audioPlacement.futureFirst) {
        impl_->rollbackSourceWorkLocked(preparation->work);
        return Result<PreviewPreparationId>::failure(
            makeError(PreviewErrorCategory::InvalidState, PreviewOperation::AddSource,
                      "再生中に始まっている主audioは先読みで準備できません"));
    }
    preparation->id = impl_->nextPreparationId++;
    preparation->generation = impl_->transportGeneration;
    const auto hold = impl_->preparationHold;
    const auto blockedFor = std::exchange(impl_->nextPreparationBlockForTest, {});
    preparation->thread = std::thread([preparation, hold, blockedFor] {
        {
            std::unique_lock<std::mutex> holdLock(hold->mutex);
            hold->changed.wait(holdLock, [&] {
                return !hold->held || preparation->cancelled.load(std::memory_order_acquire) ||
                       preparation->awaited.load(std::memory_order_acquire);
            });
        }
        // 試験用: decoder の seek の途中のように、取り消しも待ちも効かない段で止まる。
        if (blockedFor.count() > 0)
            std::this_thread::sleep_for(blockedFor);
        preparation->result = Impl::runSourceWork(preparation->work, &preparation->cancelled);
        preparation->done.store(true, std::memory_order_release);
    });
    impl_->preparations.emplace(preparation->id, preparation);
    return Result<PreviewPreparationId>::success(PreviewPreparationId{preparation->id});
}

std::vector<PreviewPreparationOutcome> PreviewEngine::takeCompletedSourcePreparations() {
    std::vector<std::shared_ptr<Impl::SourcePreparation>> completed;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!impl_->requireControlThread(PreviewOperation::AddSource))
        return {};
    for (auto entry = impl_->preparations.begin(); entry != impl_->preparations.end();) {
        if (entry->second->done.load(std::memory_order_acquire)) {
            completed.push_back(entry->second);
            entry = impl_->preparations.erase(entry);
        } else {
            ++entry;
        }
    }
    if (completed.empty())
        return {};
    lock.unlock();
    for (const auto& preparation : completed)
        preparation->thread.join();
    lock.lock();
    std::vector<PreviewPreparationOutcome> outcomes;
    outcomes.reserve(completed.size());
    for (const auto& preparation : completed)
        outcomes.push_back({PreviewPreparationId{preparation->id},
                            impl_->finishPreparationLocked(*preparation, lock)});
    return outcomes;
}

Result<PreviewSourceId> PreviewEngine::waitSourcePreparation(PreviewPreparationId preparationId) {
    std::shared_ptr<Impl::SourcePreparation> preparation;
    std::unique_lock<std::mutex> lock(impl_->mutex);
    Result<void> affinity = impl_->requireControlThread(PreviewOperation::AddSource);
    if (!affinity)
        return Result<PreviewSourceId>::failure(affinity.error());
    const auto found = impl_->preparations.find(preparationId.value);
    if (found == impl_->preparations.end()) {
        return Result<PreviewSourceId>::failure(makeError(
            PreviewErrorCategory::InvalidSource, PreviewOperation::AddSource, "未登録の準備です"));
    }
    preparation = found->second;
    impl_->preparations.erase(found);
    lock.unlock();
    preparation->awaited.store(true, std::memory_order_release);
    impl_->preparationHold->wakeAll();
    preparation->thread.join();
    lock.lock();
    return impl_->finishPreparationLocked(*preparation, lock);
}

Result<void> PreviewEngine::cancelSourcePreparation(PreviewPreparationId preparationId) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::AddSource);
        if (!affinity)
            return affinity;
        const auto found = impl_->preparations.find(preparationId.value);
        if (found == impl_->preparations.end()) {
            return Result<void>::failure(makeError(PreviewErrorCategory::InvalidSource,
                                                   PreviewOperation::AddSource,
                                                   "未登録の準備です"));
        }
        found->second->cancelled.store(true, std::memory_order_release);
    }
    impl_->preparationHold->wakeAll();
    return Result<void>::success();
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
    bool removingExtraAudio = false;
    bool removingWhilePlaying = false;
    bool removingPrimaryAsMix = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::RemoveSource);
        if (!affinity) {
            return affinity;
        }
        removingWhilePlaying = impl_->machine.state() == PreviewEngineState::Playing;
        if (impl_->machine.state() != PreviewEngineState::ReadyPaused && !removingWhilePlaying) {
            return invalidState(PreviewOperation::RemoveSource,
                                "source removalを受理できないstateです");
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
            removingPrimaryAsMix = impl_->primaryAudioAsMix;
            if (!removingWhilePlaying && !impl_->extraAudioSources.empty()) {
                return invalidState(PreviewOperation::RemoveSource,
                                    "mix inputより先にmaster audio sourceを削除できません");
            }
            audioWorker = impl_->audioWorker;
            audioSink = impl_->audioSink;
            // schedulerがまだaudio clockをmasterとして参照しないようにする。
            // ReadyPausedなのでscheduler自体は止まっているが、authorityの
            // 取り下げをstopより先に確定させる。
            if (!removingWhilePlaying)
                impl_->audioMasterActive = false;
        } else if (const auto extra = impl_->extraAudioSources.find(source.value);
                   extra != impl_->extraAudioSources.end()) {
            removingExtraAudio = true;
            audioWorker = extra->second.worker;
            audioSink = impl_->audioSink;
        }
    }

    // ownershipは移していない。engine側のfieldは生きたままなので、この窓で
    // fatalが起きてもshutdown経路が同じ実体をstop/joinできる。
    std::string audioError;
    bool audioStopped = true;
    if (audioSink || audioWorker) {
        // shutdown ordering (contract §12) と同じ順で止める: sink -> worker -> clock。
        std::lock_guard<std::mutex> transport(impl_->audioTransportMutex);
        if (removingExtraAudio && audioSink && audioWorker) {
            audioStopped = audioSink->removeMixInput(audioWorker->queue(), audioError);
        } else if (removingAudio && removingWhilePlaying && audioSink && audioWorker) {
            audioStopped = removingPrimaryAsMix
                               ? audioSink->removeMixInput(audioWorker->queue(), audioError)
                               : audioSink->detachPrimaryInput(audioWorker->queue(), audioError);
        } else if (audioSink) {
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
        if (impl_->machine.state() != (removingWhilePlaying ? PreviewEngineState::Playing
                                                            : PreviewEngineState::ReadyPaused)) {
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
                // 参照中のworkerは事前に拒否済み。未参照sourceの削除で現在の
                // pairerを作り直すと、再生中のframe提示を止めてしまう。
                if (impl_->videoSources.empty()) {
                    impl_->workerJoined = true;
                    impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
                }
            }
            if (removingAudio) {
                // authoritative audio sourceを安全に削除したのでaudio authorityを
                // 空へ戻す (preview-engine-contract.md §6)。宣言の逆順で解放する。
                if (!removingWhilePlaying)
                    impl_->audioSink.reset();
                impl_->audioWorker.reset();
                if (!removingWhilePlaying)
                    impl_->audioClock.reset();
                impl_->publicAudioSource.reset();
                impl_->internalAudioSource = audio::SourceId{};
                impl_->primaryAudioAsMix = false;
                impl_->primaryAudioSampleOffset = 0;
                impl_->primaryAudioActivationFrame = -1;
                impl_->pendingAudioActivationFrame.reset();
                if (removingWhilePlaying)
                    impl_->audioMasterActive = true;
                if (!removingWhilePlaying) {
                    impl_->resumeAudioSample = 0;
                    impl_->audioSampleOffset = 0;
                    impl_->audioSinkJoined = true;
                }
                impl_->audioWorkerJoined = true;
                if (!removingWhilePlaying) {
                    impl_->deviceSnapshot.audioSampleRate = 0;
                    impl_->deviceSnapshot.audioChannelCount = 0;
                }
            } else if (removingExtraAudio) {
                impl_->extraAudioSources.erase(source.value);
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
    std::vector<std::shared_ptr<audio::AudioDecodeWorker>> extraAudioWorkers;
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
        for (const auto& [id, entry] : impl_->extraAudioSources) {
            (void)id;
            extraAudioWorkers.push_back(entry.worker);
        }
        startSample = impl_->resumeAudioSample;
    }

    // audio decode preroll と WASAPI start は engine lockを保持したまま待たない。
    // control methodは同一threadからしか呼べないため、この窓でstateは動かない。
    if (audioSink) {
        if (audioWorker)
            audioWorker->play();
        for (const auto& worker : extraAudioWorkers)
            worker->play();
        if (audioWorker && !audioWorker->queue().waitForSamples(audio::kAudioPrerollSamples,
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
        for (const auto& worker : extraAudioWorkers) {
            if (!worker->queue().waitForSamples(audio::kAudioPrerollSamples,
                                                audio::kPrerollTimeoutMs)) {
                if (audioWorker)
                    audioWorker->pause();
                for (const auto& started : extraAudioWorkers)
                    started->pause();
                std::lock_guard<std::mutex> lock(impl_->mutex);
                ++impl_->audioTransportFailureCount;
                return Result<void>::failure(makeError(PreviewErrorCategory::AudioFailure,
                                                       PreviewOperation::Play,
                                                       "audio mix inputのprerollを満たせません"));
            }
        }
        // decode workerが実際に出したPCM domainを観測値として検査する。期待値を
        // そのまま渡すと原理的に失敗できない検査になるため、queueが受理したchunkの
        // 実測sample rate / channel数を使う。invalid rejectが残っている場合も、
        // qualified domain以外を暗黙に鳴らさずfail-closedにする。
        if (audioWorker) {
            const audio::AudioQueueSnapshot prerolled = audioWorker->queue().snapshot();
            Result<void> observedDomain = internal::validateQualifiedAudioDomain(
                prerolled.observedSampleRate, prerolled.observedChannels, "flt");
            if (prerolled.invalidRejectCount != 0 || !observedDomain) {
                audioWorker->pause();
                std::lock_guard<std::mutex> lock(impl_->mutex);
                ++impl_->audioDomainRejectCount;
                if (!observedDomain)
                    return Result<void>::failure(observedDomain.error());
                return Result<void>::failure(makeError(
                    PreviewErrorCategory::UnsupportedCapability, PreviewOperation::Play,
                    "audio decode出力がqualified PCM domainと一致しません (invalid reject=" +
                        std::to_string(prerolled.invalidRejectCount) + ")"));
            }
        }
        std::string error;
        const auto generation = audioWorker ? audioWorker->snapshot().sourceGeneration
                                            : impl_->audioClock->snapshot().generation;
        if (!audioSink->play(startSample, generation, error)) {
            if (audioWorker)
                audioWorker->pause();
            for (const auto& worker : extraAudioWorkers)
                worker->pause();
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
            for (const auto& worker : extraAudioWorkers)
                worker->pause();
            if (audioSink) {
                std::string ignored;
                audioSink->pause(ignored);
            }
            return played;
        }
        impl_->schedulerEnabled = true;
        // audio sourceが登録されている場合だけ`IAudioClock`がmasterになる。
        impl_->audioMasterActive = impl_->audioSink != nullptr && impl_->audioClock != nullptr &&
                                   !impl_->pendingAudioActivationFrame;
        impl_->schedulerStart = std::chrono::steady_clock::now();
        impl_->schedulerBaseFrame = impl_->telemetrySnapshot.presentedFrameCount == 0
                                        ? 0
                                        : impl_->telemetrySnapshot.status.position.outputFrame + 1;
        impl_->lastSchedulerTarget = impl_->schedulerBaseFrame - 1;
        // 初回renderより前はpairer/coordinator source setがまだ同期されていない。
        // referencedだけを起こすと0件になり、最初のframeが永久に供給されない。
        for (gpu::SourceDecodeWorker* worker : impl_->videoWorkersLocked())
            worker->play();
        impl_->telemetrySnapshot.status.state = PreviewEngineState::Playing;
    }
    impl_->notify(internal::StateChangedEvent{PreviewEngineState::Playing});
    return Result<void>::success();
}

Result<void> PreviewEngine::pause() {
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker;
    std::shared_ptr<audio::WasapiAudioSink> audioSink;
    std::vector<std::shared_ptr<audio::AudioDecodeWorker>> extraAudioWorkers;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Result<void> affinity = impl_->requireControlThread(PreviewOperation::Pause);
        if (!affinity)
            return affinity;
        if (impl_->machine.state() != PreviewEngineState::Playing)
            return invalidState(PreviewOperation::Pause, "pauseを受理できないstateです");
        // 再生中に要求した先読みの準備は、この後に完了しても公開しない。
        ++impl_->transportGeneration;
        // 提示だけ先に止める。audio clockを止める前にschedulerを黙らせないと、
        // render threadがclock停止をprojection失敗として誤検出する。
        // ただしtransport stateはまだcommitしない (sink停止を確認するまでPlayingのまま)。
        impl_->schedulerEnabled = false;
        impl_->audioMasterActive = false;
        for (gpu::SourceDecodeWorker* worker : impl_->videoWorkersLocked())
            worker->pause();
        audioWorker = impl_->audioWorker;
        audioSink = impl_->audioSink;
        for (const auto& [id, entry] : impl_->extraAudioSources) {
            (void)id;
            extraAudioWorkers.push_back(entry.worker);
        }
    }

    std::string sinkError;
    const bool sinkPaused = audioSink == nullptr || audioSink->pause(sinkError);
    if (audioWorker)
        audioWorker->pause();
    for (const auto& worker : extraAudioWorkers)
        worker->pause();

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
                if (!layer.stillImage)
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

    struct ExtraAudioSeekTarget {
        std::uint64_t publicId = 0;
        std::shared_ptr<audio::AudioDecodeWorker> worker;
        std::int64_t sample = 0;
    };

    std::vector<ExtraAudioSeekTarget> extraAudioTargets;
    std::int64_t audioSample = 0;
    std::int64_t audioClockSample = 0;
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
            if (!layer.stillImage)
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
        // workerは素材sample、sinkのclockは先読みaudioではtimeline sampleを指す。
        if (!core::checkedAdd(sample.value(), impl_->primaryAudioSampleOffset, audioSample)) {
            return Result<void>::failure(makeError(PreviewErrorCategory::SeekFailure,
                                                   PreviewOperation::Seek,
                                                   "audio sample offsetの加算がoverflowしました"));
        }
        if (audioSample < 0)
            audioSample = 0;
        if (!core::checkedAdd(sample.value(), impl_->audioSampleOffset, audioClockSample)) {
            return Result<void>::failure(
                makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                          "audio clock sample offsetの加算がoverflowしました"));
        }
        audioClockSample = std::max<std::int64_t>(0, audioClockSample);
        resumePlaying = state == PreviewEngineState::Playing;
        audioWorker = impl_->audioWorker;
        audioSink = impl_->audioSink;
        const std::int64_t timelineSample = sample.value();
        for (const auto& [publicId, entry] : impl_->extraAudioSources) {
            std::int64_t extraSample = 0;
            if (!core::checkedAdd(timelineSample, entry.sampleOffset, extraSample)) {
                return Result<void>::failure(
                    makeError(PreviewErrorCategory::SeekFailure, PreviewOperation::Seek,
                              "audio mix sample offsetの加算がoverflowしました"));
            }
            if (extraSample < 0)
                extraSample = 0;
            extraAudioTargets.push_back({publicId, entry.worker, extraSample});
        }

        // seek より前の位置で要求した先読みの準備は、この後に完了しても公開しない。
        ++impl_->transportGeneration;
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
    for (const auto& extraTarget : extraAudioTargets)
        extraTarget.worker->pause();
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
            if (impl_->primaryAudioAsMix) {
                if (target.outputFrame < impl_->primaryAudioActivationFrame)
                    impl_->pendingAudioActivationFrame = impl_->primaryAudioActivationFrame;
                else
                    impl_->pendingAudioActivationFrame.reset();
            }

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
            impl_->pendingSeek.extraAudioTickets.clear();
            impl_->pendingSeek.extraAudioSamples.clear();
            impl_->pendingSeek.expectedExtraAudioGenerations.clear();
            for (const auto& extraTarget : extraAudioTargets) {
                audio::AudioSeekTicket ticket;
                const auto accepted =
                    extraTarget.worker->requestSeek(extraTarget.sample, ticket, requestError);
                if (accepted != audio::AudioSeekRequestResult::Accepted) {
                    audioRequest = accepted;
                    break;
                }
                impl_->pendingSeek.extraAudioTickets.emplace(extraTarget.publicId, ticket);
                impl_->pendingSeek.extraAudioSamples.emplace(extraTarget.publicId,
                                                             extraTarget.sample);
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
                pending.audioSample = audioClockSample;
                pending.primaryAudioWorkerSample = audioSample;
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
    // 先読みの準備用の thread は render device を使う。teardown が device を手放す前に、
    // 取り消して join し、作りかけの worker を止める。
    {
        std::vector<std::shared_ptr<Impl::SourcePreparation>> pending;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            Result<void> affinity = impl_->requireControlThread(PreviewOperation::Shutdown);
            if (!affinity)
                return affinity;
            ++impl_->transportGeneration;
            for (auto& [id, preparation] : impl_->preparations) {
                (void)id;
                preparation->cancelled.store(true, std::memory_order_release);
                pending.push_back(preparation);
            }
            impl_->preparations.clear();
        }
        impl_->preparationHold->wakeAll();
        for (const auto& preparation : pending)
            preparation->thread.join();
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& preparation : pending) {
            impl_->rollbackSourceWorkLocked(preparation->work);
            ++impl_->staleSourcePreparationRejectCount;
        }
    }
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

Result<void> PreviewEngine::setMasterVolume(float volume) {
    if (!(volume >= 0.0F) || volume > 1.0F) {
        return Result<void>::failure(makeError(PreviewErrorCategory::UnsupportedCapability,
                                               PreviewOperation::Initialize,
                                               "master volumeは0.0〜1.0で指定してください"));
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->audioSink) {
        std::string error;
        if (!impl_->audioSink->setSessionVolume(volume, error)) {
            return Result<void>::failure(
                makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Initialize, error));
        }
    }
    impl_->audioSessionVolume = volume;
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
    std::vector<std::shared_ptr<audio::AudioDecodeWorker>> seekExtraAudioWorkers;
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
            auto& dueFailure = engine.impl_->pendingSeek.dueFailure;
            std::optional<PreviewError> seekFatal =
                dueFailure ? std::exchange(dueFailure, std::nullopt)
                           : engine.impl_->advanceSeekLocked(renderNow);
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
                // 最初の1枚を提示する前にclockだけが進むと、decoder queueより常に
                // 先のframeを要求し続けて永久に追いつけない。初回だけbaseを固定する。
                target = engine.impl_->telemetrySnapshot.presentedFrameCount == 0
                             ? engine.impl_->schedulerBaseFrame
                             : scheduled.frame;
                if (target <= engine.impl_->lastSchedulerTarget)
                    return Result<RenderFrameResult>::success(result);
                proceed = true;
            }
        }
        const auto commitSchedulerTarget = [&] {
            if (seeking) {
                engine.impl_->lastSchedulerTarget = target;
                return;
            }
            const std::uint64_t skipped =
                internal::skippedSchedulerFrameCount(engine.impl_->lastSchedulerTarget, target);
            const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
            if (skipped > maximum - engine.impl_->telemetrySnapshot.droppedFrameCount)
                engine.impl_->telemetrySnapshot.droppedFrameCount = maximum;
            else
                engine.impl_->telemetrySnapshot.droppedFrameCount += skipped;
            engine.impl_->lastSchedulerTarget = target;
        };
        // seek 完了後に再生を再開する音声 transport を、lock の外で使うために控える。
        // decode layer の有無に関わらず同じにする。控え忘れると、音声だけの区間で
        // seek してから再開したときに音が戻らない。
        const auto captureSeekAudioResume = [&] {
            seekAudioWorker = engine.impl_->audioWorker;
            seekAudioSink = engine.impl_->audioSink;
            for (const auto& [id, entry] : engine.impl_->extraAudioSources) {
                (void)id;
                seekExtraAudioWorkers.push_back(entry.worker);
            }
        };
        if (proceed) {
            // compositionのruntime authorityを先に確定させる。layoutとstateが
            // 決まっていないとexact pairingの対象sourceも決まらない。
            auto token = engine.impl_->compositionState.latestAcceptedToken();
            auto snapshot = engine.impl_->compositionState.latestAcceptedSnapshot();
            if (!seeking && snapshot && snapshot->activationOutputFrame >= 0 &&
                target < snapshot->activationOutputFrame &&
                engine.impl_->compositionState.lastPresentedToken()) {
                token = engine.impl_->compositionState.lastPresentedToken();
                snapshot = engine.impl_->compositionState.lastPresentedSnapshot();
            }
            if (!token || !snapshot) {
                return Result<RenderFrameResult>::failure(makeError(
                    PreviewErrorCategory::CompositionFailure, PreviewOperation::RenderDeviceAttach,
                    "accepted compositionが見つかりません"));
            }
            const bool hasDecodeLayer =
                std::any_of(snapshot->layers.begin(), snapshot->layers.end(),
                            [](const PreviewCompositionLayer& layer) { return !layer.stillImage; });
            std::optional<std::string> sourcelessFailure;
            if (!hasDecodeLayer) {
                // decode layer の無い composition (gap、または静止画だけ)。背景は render pass の
                // 既存の clear をそのまま使い、静止画があればその上に描く。
                // 提示の authority は scheduler の時計であり、decode source を代用にしない。
                gpu::ComposedFrame composed;
                sourcelessFailure = engine.impl_->addStillLayersLocked(*snapshot, composed);
                if (!sourcelessFailure && !composed.layers.empty()) {
                    gpu::ExternalCompositionTarget targetView{
                        static_cast<ID3D11RenderTargetView*>(renderTargetView), width, height};
                    std::string error;
                    if (!engine.impl_->compositor->composeLayersToTarget(
                            composed, targetView, snapshot->layers.size(), error))
                        sourcelessFailure = "GPU compositionに失敗しました: " + error;
                }
            }
            if (sourcelessFailure) {
                PreviewError failure = makeError(
                    PreviewErrorCategory::DeviceFailure, PreviewOperation::RenderDeviceAttach,
                    "静止画 layerを描画できません: " + *sourcelessFailure);
                failure.severity = PreviewErrorSeverity::FatalToSession;
                fatal = failure;
            } else if (!hasDecodeLayer) {
                engine.impl_->pairer.reset();
                engine.impl_->coordinatorSources.clear();
                engine.impl_->compositionState.markPresented(*token, snapshot);
                engine.impl_->notePresentedOutputFrameLocked(target, *snapshot);
                commitSchedulerTarget();
                engine.impl_->distinctPresentedFrames.note(target);
                ++engine.impl_->presentationSequence;
                ++engine.impl_->telemetrySnapshot.presentedFrameCount;
                engine.impl_->telemetrySnapshot.currentSourceQueueDepth = 0;
                engine.impl_->telemetrySnapshot.status.position = {target};
                engine.impl_->telemetrySnapshot.status.lastPresentedComposition = *token;
                result.presented = true;
                result.sourceFrame = -1;
                // layer 数は描いた静止画の枚数 (gap なら 0)。decode layer は無い。
                result.frame = {engine.impl_->presentationSequence,
                                {target},
                                *token,
                                static_cast<std::uint32_t>(snapshot->layers.size())};
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
                    captureSeekAudioResume();
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
                    if (!seeking)
                        engine.impl_->noteUnpairedOutputFrameLocked(target);
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
                        engine.impl_->noteUnpairedOutputFrameLocked(target);
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

                    // 静止画 layer は pairing と提示直前の検証の対象外なので、
                    // 検証を通った後で snapshot の z 順どおりに差し込む。
                    const std::optional<std::string> stillFailure =
                        engine.impl_->addStillLayersLocked(*snapshot, composed);
                    gpu::ExternalCompositionTarget targetView{
                        static_cast<ID3D11RenderTargetView*>(renderTargetView), width, height};
                    std::string error;
                    // 期待layer数のauthorityはaccepted snapshotであり、
                    // compose結果そのものではない。自己参照にすると層数の
                    // boundary checkがtautologyになる。
                    const std::size_t expectedLayerCount = snapshot->layers.size();
                    if (stillFailure || !engine.impl_->compositor->composeLayersToTarget(
                                            composed, targetView, expectedLayerCount, error)) {
                        PreviewError failure = makeError(
                            PreviewErrorCategory::DeviceFailure,
                            PreviewOperation::RenderDeviceAttach,
                            stillFailure ? "静止画 layerを準備できません: " + *stillFailure
                                         : "GPU compositionに失敗しました: " + error);
                        failure.severity = PreviewErrorSeverity::FatalToSession;
                        fatal = failure;
                    } else {
                        for (gpu::SourceDecodeWorker* worker : workers)
                            worker->buffer().noteDisplayed(target);
                        engine.impl_->compositionState.markPresented(*token, snapshot);
                        engine.impl_->notePresentedOutputFrameLocked(target, *snapshot);
                        engine.impl_->notePresentedBaseLayerLocked(composed);
                        commitSchedulerTarget();
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
                            captureSeekAudioResume();
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
            for (const auto& worker : seekExtraAudioWorkers)
                worker->play();
            std::string error;
            for (const auto& worker : seekExtraAudioWorkers) {
                if (!worker->queue().waitForSamples(audio::kAudioPrerollSamples,
                                                    audio::kPrerollTimeoutMs)) {
                    error = "audio mix inputのseek後prerollを満たせません";
                    resumeFailure =
                        makeError(PreviewErrorCategory::AudioFailure, PreviewOperation::Seek, error,
                                  PreviewErrorSeverity::FatalToSession);
                    break;
                }
            }
            if (resumeFailure) {
                seekAudioWorker->pause();
                for (const auto& worker : seekExtraAudioWorkers)
                    worker->pause();
            }
            // seek completionが返したidentityをそのまま運ぶ。
            if (!resumeFailure &&
                !seekAudioSink->play(seekResumeSample, seekResumeAudioGeneration, error)) {
                seekAudioWorker->pause();
                for (const auto& worker : seekExtraAudioWorkers)
                    worker->pause();
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
                    engine.impl_->audioMasterActive = engine.impl_->audioSink != nullptr &&
                                                      engine.impl_->audioClock != nullptr &&
                                                      !engine.impl_->pendingAudioActivationFrame;
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

bool PreviewRenderPort::renderFrameDue(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    const PreviewEngineState state = engine.impl_->machine.state();
    if (state == PreviewEngineState::Seeking) {
        auto& pending = engine.impl_->pendingSeek;
        if (!pending.active)
            return false;
        // decode の完了前に render pass を始めると、何も描かないまま背景色で clear した
        // frame が提示される。effect の編集は同じ frame への seek を繰り返すので、そのたびに
        // 画面が黒く瞬く。完了 (または失敗・期限切れ) を回収できた時だけ描く。
        if (!pending.decodeReady && !pending.dueFailure)
            pending.dueFailure = engine.impl_->advanceSeekLocked(std::chrono::steady_clock::now());
        return pending.decodeReady || pending.dueFailure.has_value();
    }
    if (state != PreviewEngineState::Playing || !engine.impl_->schedulerEnabled)
        return false;
    const auto scheduled = engine.impl_->schedulerTargetLocked(std::chrono::steady_clock::now());
    // masterが成立しない (audio clock停止、sink failure、QPC退避) ならrenderFrameへ回し、
    // そこでfatalとして表面化させる。ここでfalseを返すとrenderが止まるだけでPlayingが
    // 続き、fail-closedであるべき失敗が永久に報告されない。
    if (!scheduled.valid)
        return true;
    return scheduled.frame > engine.impl_->lastSchedulerTarget;
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
            engine.impl_->finalRuntimeDiagnostics.audioEndpointVolume = endpoint.endpointVolume;
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
            (engine.impl_->publicAudioSource ? 1U : 0U) +
            static_cast<std::uint32_t>(engine.impl_->extraAudioSources.size());
        if (drained) {
            // 静止画の texture は compositor の SRV cache と一緒に手放す。
            engine.impl_->stillImages.clear();
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
            engine.impl_->detachedWorkers.extraAudioWorkers.clear();
            engine.impl_->detachedWorkers.audioClock.reset();
            engine.impl_->extraAudioSources.clear();
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
            engine.impl_->detachedWorkers.extraAudioWorkers.clear();
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
    result.pendingSourcePreparationCount = engine.impl_->preparations.size();
    result.staleSourcePreparationRejectCount = engine.impl_->staleSourcePreparationRejectCount;
    result.playingAudioEndpointOpenCount = engine.impl_->playingAudioEndpointOpenCount;
    result.maxPlayingAudioEndpointOpenMs = engine.impl_->maxPlayingAudioEndpointOpenMs;
    result.playingAudioEndpointOpenAttemptCount =
        engine.impl_->playingAudioEndpointOpenAttemptCount;
    result.playingAudioEndpointOpenFailureCount =
        engine.impl_->playingAudioEndpointOpenFailureCount;
    result.playingAudioTransportStartFailureCount =
        engine.impl_->playingAudioTransportStartFailureCount;
    result.maxPlayingAudioEndpointOpenAttemptMs =
        engine.impl_->maxPlayingAudioEndpointOpenAttemptMs;
    result.publishedSourceCount = engine.impl_->eligibleSources.size();
    const std::size_t first =
        (engine.impl_->presentedOutputFrameNext + engine.impl_->presentedOutputFrames.size() -
         engine.impl_->presentedOutputFrameCount) %
        engine.impl_->presentedOutputFrames.size();
    for (std::size_t index = 0; index < engine.impl_->presentedOutputFrameCount; ++index) {
        const std::size_t at = (first + index) % engine.impl_->presentedOutputFrames.size();
        result.recentPresentedOutputFrames.push_back(engine.impl_->presentedOutputFrames[at]);
        result.recentPresentedLayerCounts.push_back(engine.impl_->presentedLayerCounts[at]);
        result.recentPresentedTopLayerOpacities.push_back(
            engine.impl_->presentedTopLayerOpacities[at]);
        result.recentPresentedBaseSources.push_back(engine.impl_->presentedBaseSources[at]);
        result.recentPresentedBaseSourceFrames.push_back(
            engine.impl_->presentedBaseSourceFrames[at]);
    }
    const std::size_t firstUnpaired =
        (engine.impl_->unpairedOutputFrameNext + engine.impl_->unpairedOutputFrames.size() -
         engine.impl_->unpairedOutputFrameCount) %
        engine.impl_->unpairedOutputFrames.size();
    for (std::size_t index = 0; index < engine.impl_->unpairedOutputFrameCount; ++index)
        result.recentUnpairedOutputFrames.push_back(
            engine.impl_->unpairedOutputFrames[(firstUnpaired + index) %
                                               engine.impl_->unpairedOutputFrames.size()]);
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
        result.audioEndpointVolume = endpoint.endpointVolume;
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

Result<void> PreviewRenderPort::setRegisteredVideoSourceLimitForTest(PreviewEngine& engine,
                                                                     std::size_t limit) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (engine.impl_->machine.state() != PreviewEngineState::ReadyPaused ||
        limit < engine.impl_->videoSources.size() || limit == 0)
        return invalidState(PreviewOperation::AddSource,
                            "登録上限はReadyPausedで既存source数以上に設定してください");
    engine.impl_->registeredVideoSourceLimit = limit;
    return Result<void>::success();
}

void PreviewRenderPort::failNextPlayingAudioTransportStartForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    engine.impl_->failNextPlayingTransportStartForTest = true;
}

void PreviewRenderPort::failNextPlayingAudioEndpointOpenForTest(PreviewEngine& engine,
                                                                int delayMilliseconds) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    engine.impl_->failNextPlayingEndpointOpenForTest = std::chrono::milliseconds(delayMilliseconds);
}

void PreviewRenderPort::blockNextSourcePreparationForTest(PreviewEngine& engine, int milliseconds) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    engine.impl_->nextPreparationBlockForTest = std::chrono::milliseconds(milliseconds);
}

void PreviewRenderPort::holdSourcePreparationsForTest(PreviewEngine& engine, bool held) {
    const auto hold = engine.impl_->preparationHold;
    {
        std::lock_guard<std::mutex> lock(hold->mutex);
        hold->held = held;
    }
    hold->changed.notify_all();
}

Result<void> PreviewRenderPort::disableAudioSourcesForTest(PreviewEngine& engine) {
    std::lock_guard<std::mutex> lock(engine.impl_->mutex);
    if (engine.impl_->machine.state() != PreviewEngineState::ReadyPaused ||
        engine.impl_->publicAudioSource || !engine.impl_->extraAudioSources.empty())
        return invalidState(
            PreviewOperation::AddSource,
            "audio sourceの無効化はReadyPausedかつaudio source未登録時に行ってください");
    engine.impl_->capability.configuredMaxActiveAudioSources = 0;
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
