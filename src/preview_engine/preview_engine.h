#ifndef MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_H
#define MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_H

#include "preview_engine/preview_result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

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

// 完了した先読みの準備。source は公開した source の番号か、公開しなかった理由。
struct PreviewPreparationOutcome {
    PreviewPreparationId preparation;
    Result<PreviewSourceId> source;
};

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
    // 再生中に、これから始まる区間の source を準備する。decoder の open と初期 seek は準備用の
    // thread で行い、呼び出し側 (control thread) は待たない。完了は
    // takeCompletedSourcePreparations / waitSourcePreparation が control thread で公開する。
    // 要求した後に pause / seek / shutdown があった準備や、取り消した準備は公開しない
    // (PreviewErrorCode::PreparationStale)。公開した source は addSource と同じく removeSource
    // で外す。
    Result<PreviewPreparationId>
    requestSourcePreparation(const PreviewSourceDescriptor& descriptor);
    // 完了した準備をすべて公開して返す。未完了のものは返さない。
    std::vector<PreviewPreparationOutcome> takeCompletedSourcePreparations();
    // 指定した準備の完了を待って公開する。clip 境界までに終わらなかった準備に使う。
    Result<PreviewSourceId> waitSourcePreparation(PreviewPreparationId preparation);
    // 準備を取り消す。完了を待たない。完了した時点で公開せずに捨てる。
    Result<void> cancelSourcePreparation(PreviewPreparationId preparation);
    Result<void> removeSource(PreviewSourceId source);
    Result<AcceptedComposition>
    submitComposition(std::shared_ptr<const CompositionSnapshot> snapshot);
    Result<void> play();
    Result<void> pause();
    Result<void> seek(PreviewPosition target);
    Result<void> seekFrameRequest(const PreviewFrameRequest& request);
    // preview endpoint の master volume。再生中にも即時反映する。
    Result<void> setMasterVolume(float volume);
    void clearAudioMeterClip();

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
