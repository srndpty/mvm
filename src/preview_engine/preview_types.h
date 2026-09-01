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

struct PreviewCapabilities {
    std::uint32_t maxQualifiedActiveVideoSources = 1;
    std::uint32_t maxQualifiedCompositionLayers = 1;
    std::uint32_t maxQualifiedActiveAudioSources = 0;
    PreviewFrameRate qualifiedOutputFrameRate{60, 1};
    std::uint32_t qualifiedAudioSampleRate = 0;
    std::uint32_t qualifiedAudioChannelCount = 0;
    bool duplicateSourceLayersSupported = false;
    bool deviceRecoverySupported = false;
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
    PreviewStatus status;
};

} // namespace mvm::preview

#endif // MVM_PREVIEW_ENGINE_PREVIEW_TYPES_H
