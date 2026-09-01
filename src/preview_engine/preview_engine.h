#ifndef MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_H
#define MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_H

#include "preview_engine/preview_result.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace mvm::preview {

class PreviewEventDispatcher {
public:
    virtual ~PreviewEventDispatcher() = default;
    virtual bool post(std::function<void()> callback) = 0;
};

class PreviewEventSink {
public:
    virtual ~PreviewEventSink() = default;
    virtual void stateChanged(PreviewEngineState state) = 0;
    virtual void positionChanged(PreviewPosition position) = 0;
    virtual void framePresented(PresentedFrameInfo frame) = 0;
    virtual void errorOccurred(PreviewError error) = 0;
    virtual void deviceChanged(PreviewDeviceInfo device) = 0;
};

Result<PreviewFrameRate> validatePreviewFrameRate(std::uint64_t numerator,
                                                  std::uint64_t denominator);
Result<void> validatePreviewSourceDescriptor(const PreviewSourceDescriptor& descriptor);

namespace internal {
class PreviewRenderPort;
}

class PreviewEngine {
public:
    PreviewEngine();
    ~PreviewEngine();

    PreviewEngine(const PreviewEngine&) = delete;
    PreviewEngine& operator=(const PreviewEngine&) = delete;

    Result<void> initialize(const PreviewEngineConfig& config,
                            std::shared_ptr<PreviewEventDispatcher> dispatcher);
    Result<void> attachEventSink(std::weak_ptr<PreviewEventSink> sink);
    Result<void> detachEventSink();

    Result<PreviewSourceId> addSource(const PreviewSourceDescriptor& descriptor);
    Result<void> removeSource(PreviewSourceId source);
    Result<AcceptedComposition>
    submitComposition(std::shared_ptr<const CompositionSnapshot> snapshot);
    Result<void> play();
    Result<void> pause();
    Result<void> seek(PreviewPosition target);
    Result<void> seekFrameRequest(const PreviewFrameRequest& request);

    PreviewStatus status() const;
    PreviewCapabilities capabilities() const;
    PreviewTelemetry telemetry() const;
    PreviewDeviceInfo deviceInfo() const;

    Result<void> requestShutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    friend class internal::PreviewRenderPort;
};

} // namespace mvm::preview

#endif // MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_H
