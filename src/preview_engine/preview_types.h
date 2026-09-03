#ifndef MVM_PREVIEW_ENGINE_PREVIEW_TYPES_H
#define MVM_PREVIEW_ENGINE_PREVIEW_TYPES_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mvm::preview {

struct PreviewSourceId {
    std::uint64_t value = 0;
    bool operator==(const PreviewSourceId&) const = default;
};

struct PreviewFrameRate {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
    bool operator==(const PreviewFrameRate&) const = default;
};

struct PreviewPosition {
    std::int64_t outputFrame = 0;
    bool operator==(const PreviewPosition&) const = default;
};

struct PreviewSourceFrameRequest {
    PreviewSourceId source;
    std::int64_t sourceFrameNumber = -1;
    bool operator==(const PreviewSourceFrameRequest&) const = default;
};

struct PreviewFrameRequest {
    std::int64_t outputFrameNumber = -1;
    std::vector<PreviewSourceFrameRequest> sources;
    bool operator==(const PreviewFrameRequest&) const = default;
};

struct PreviewOutputConfig {
    PreviewFrameRate frameRate;
};

struct PreviewEngineConfig {
    PreviewOutputConfig output;
};

struct PreviewSourceDescriptor {
    std::filesystem::path mediaPath;
    bool videoEnabled = false;
    bool audioEnabled = false;
    // audio source の media sample と output frame の対応をずらす量。
    //   media sample = (output frame を換算した sample) + audioSampleOffset
    // timeline 上で 0 以外の位置に置いた audio clip を鳴らすために使う。
    // videoEnabled のみの source では無視する。
    std::int64_t audioSampleOffset = 0;
};

struct PreviewNormalizedRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 1.0F;
    float height = 1.0F;
    bool operator==(const PreviewNormalizedRect&) const = default;
};

struct PreviewCompositionLayer {
    PreviewSourceId source;
    PreviewNormalizedRect destination;
    PreviewNormalizedRect sourceRect;
    float opacity = 1.0F;
    bool effectsEnabled = false;
    float rotationDegrees = 0.0F;
    std::int64_t sourceInFrame = 0;
    std::int64_t sourceDurationFrames = 0;
    std::int64_t fadeInFrames = 0;
    std::int64_t fadeOutFrames = 0;
    bool operator==(const PreviewCompositionLayer&) const = default;
};

struct CompositionSnapshot {
    std::vector<PreviewCompositionLayer> layers;
    bool operator==(const CompositionSnapshot&) const = default;
};

struct CompositionSnapshotId {
    std::uint64_t value = 0;
    bool operator==(const CompositionSnapshotId&) const = default;
};

struct AcceptedComposition {
    CompositionSnapshotId id;
    std::uint64_t revision = 0;
    bool operator==(const AcceptedComposition&) const = default;
};

struct PresentedFrameInfo {
    std::uint64_t presentationSequence = 0;
    PreviewPosition position;
    AcceptedComposition composition;
    std::uint32_t activeLayerCount = 0;
    bool operator==(const PresentedFrameInfo&) const = default;
};

enum class PreviewEngineState {
    Uninitialized,
    WaitingForRenderDevice,
    ReadyPaused,
    Playing,
    Seeking,
    ShuttingDown,
    Shutdown,
    Error,
};

enum class PreviewErrorCategory {
    InvalidState,
    InvalidSource,
    UnsupportedCapability,
    DecodeFailure,
    AudioFailure,
    DeviceFailure,
    SeekFailure,
    CompositionFailure,
    ShutdownFailure,
};

enum class PreviewErrorSeverity {
    Recoverable,
    FatalToSession,
};

enum class PreviewOperation {
    Initialize,
    AttachEventSink,
    DetachEventSink,
    AddSource,
    RemoveSource,
    SubmitComposition,
    Play,
    Pause,
    Seek,
    Shutdown,
    RenderDeviceAttach,
};

struct PreviewError {
    PreviewErrorCategory category = PreviewErrorCategory::InvalidState;
    PreviewErrorSeverity severity = PreviewErrorSeverity::Recoverable;
    PreviewOperation operation = PreviewOperation::Initialize;
    std::optional<PreviewSourceId> source;
    std::string detail;
    std::optional<std::int64_t> nativeDiagnosticCode;
    bool operator==(const PreviewError&) const = default;
};

// canonical workload を実測した「構成の組」。
//
// **qualification の各軸を独立に合成できると仮定しない。** 2 layer が qualified
// なのは 60/1 cohort で測ったからであり、24/1 構成で 2 layer が qualified である
// ことは意味しない。したがって measured 側は個別の上限値ではなく envelope
// (tuple) として持ち、現在の構成が envelope と一致するかどうかだけを公開する。
struct MeasuredPreviewEnvelope {
    PreviewFrameRate outputFrameRate{60, 1};
    std::uint32_t maxActiveVideoSources = 2;
    std::uint32_t maxCompositionLayers = 2;
    std::uint32_t maxActiveAudioSources = 1;
    std::uint32_t audioSampleRate = 48000;
    std::uint32_t audioChannelCount = 2;
    bool operator==(const MeasuredPreviewEnvelope&) const = default;
};

struct PreviewCapabilities {
    // ---- 現在の構成で実際に受理できる上限。measured とは限らない ----
    std::uint32_t configuredMaxActiveVideoSources = 1;
    std::uint32_t configuredMaxCompositionLayers = 1;
    std::uint32_t configuredMaxActiveAudioSources = 0;
    // initialize() で確定した output frame rate。
    // 受理されたことは qualify されたことではない。
    PreviewFrameRate configuredOutputFrameRate{60, 1};
    std::uint32_t configuredAudioSampleRate = 0;
    std::uint32_t configuredAudioChannelCount = 0;

    // configured* が「実際に確定した構成」かどうか。
    //
    // **これは derived value の cache ではなく一次 state である。**
    //
    // この struct 自体の既定値 (1 / 1 / 0 / 60-1 / 0 / 0) は measuredEnvelope と
    // 一致しない。問題になるのは `PreviewEngine` が initialize 前に持つ product
    // capability であり、そちらは 2 / 2 / 1 / 48000 / 2 へ上書きされているため
    // measuredEnvelope と同値の組になる。この flag が無いと、initialize を
    // 通していない engine が tuple equality だけで一致してしまう
    // (「実測 authority の false positive」)。
    //
    // initialize が最後まで成功したときにだけ true にし、rollback では false へ戻す。
    bool hasConfiguredEnvelope = false;

    // ---- 実測済みの構成 ----
    MeasuredPreviewEnvelope measuredEnvelope;

    bool duplicateSourceLayersSupported = false;
    bool deviceRecoverySupported = false;

    // 現在の構成が measuredEnvelope と**組として**一致するか。
    // 保存値にすると configured field を書き換える経路 (test hook を含む) ごとに
    // 再計算が要り、書き忘れると stale な true が残る。derived にして
    // 「派生値が元と食い違う」状態そのものを作らない。
    //
    // 構成が未確定 (hasConfiguredEnvelope == false) のときは常に false を返す。
    // 「まだ分からない」を「測定済み」にしない。
    bool matchesMeasuredEnvelope() const {
        return hasConfiguredEnvelope &&
               configuredOutputFrameRate == measuredEnvelope.outputFrameRate &&
               configuredMaxActiveVideoSources == measuredEnvelope.maxActiveVideoSources &&
               configuredMaxCompositionLayers == measuredEnvelope.maxCompositionLayers &&
               configuredMaxActiveAudioSources == measuredEnvelope.maxActiveAudioSources &&
               configuredAudioSampleRate == measuredEnvelope.audioSampleRate &&
               configuredAudioChannelCount == measuredEnvelope.audioChannelCount;
    }
};

struct PreviewDeviceInfo {
    std::string adapterDescription;
    std::uint64_t adapterLuidLow = 0;
    std::int64_t adapterLuidHigh = 0;
    std::string audioEndpointId;
    std::string audioEndpointName;
    std::uint32_t audioSampleRate = 0;
    std::uint32_t audioChannelCount = 0;
};

struct PreviewStatus {
    PreviewEngineState state = PreviewEngineState::Uninitialized;
    PreviewPosition position;
    std::optional<AcceptedComposition> latestAcceptedDesiredComposition;
    std::optional<AcceptedComposition> lastPresentedComposition;
    std::optional<PreviewError> lastError;
};

struct PreviewTelemetry {
    std::uint64_t presentedFrameCount = 0;
    std::uint64_t droppedFrameCount = 0;
    std::uint64_t audioUnderflowCount = 0;
    std::uint64_t decodeFailureCount = 0;
    std::uint64_t eventDeliveryFailureCount = 0;
    // 現在のaccepted compositionが参照するvideo source群の最大queue depth。
    std::uint32_t currentSourceQueueDepth = 0;
    std::uint32_t gpuRetirementCurrentDepth = 0;
    std::uint32_t gpuRetirementPeakDepth = 0;
    // endpoint へ送った PCM の channel peak (linear, 0..1)。audio source が
    // 無い間は 0。dB への換算は表示側の責務にする。
    float audioMeterPeakLeft = 0.0F;
    float audioMeterPeakRight = 0.0F;
    PreviewStatus status;
};

} // namespace mvm::preview

#endif // MVM_PREVIEW_ENGINE_PREVIEW_TYPES_H
