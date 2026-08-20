#ifndef MVM_GPU_PREVIEW_GPU_COMPOSITOR_H
#define MVM_GPU_PREVIEW_GPU_COMPOSITOR_H

#include "media/gpu_preview/composed_frame.h"
#include "media/gpu_preview/gpu_completion.h"
#include "media/gpu_preview/nv12_converter.h"

namespace mvm::gpu {

struct GpuCompositorCounters {
    long long compositionRequestedCount = 0;
    long long compositionDrawnCount = 0;
    long long layerDrawCount = 0;
    long long clearCount = 0;
    long long deviceMismatchCount = 0;
    long long gpuSubmissionCount = 0;
    long long untrackedSubmissionCount = 0;
    long long completionPollFailureCount = 0;
    size_t retirementDepthPeak = 0;
    size_t retirementDepthAfterDrain = 0;
    long long payloadsReleasedBeforeCompletion = 0;
    long long retirementTimeoutCount = 0;
    long long partialGpuIssueFailureCount = 0;
    long long composeAfterFatalRejectedCount = 0;
    long long fullFrameGpuCopyCount = 0;
};

struct ExternalCompositionTarget {
    ID3D11RenderTargetView* rtv = nullptr; // 借用。GpuCompositor は Release しない。
    int width = 0;
    int height = 0;
};

struct GpuCompositorStageTiming {
    double prepareUs = 0.0;
    double issueUs = 0.0;
    double completionPollUs = 0.0;
};

enum class GpuCompositorInitializeFault {
    None = 0,
    Completion,
    TargetTexture,
    TargetRtv,
    TargetSrv,
};

struct GpuCompositorTestFaults {
    GpuCompositorInitializeFault initialize = GpuCompositorInitializeFault::None;
    int failBeforeLayerDraw = -1;
    bool failCompletionPoll = false;
    bool failShutdownCompletionPoll = false;
};

enum class GpuCompositorShutdownStatus { Pending = 0, Complete, Failed };

// Qt非依存のoffscreen compositor。全layerを事前検証し、1 clear + N drawを
// 1 submission serialとして追跡する。
class GpuCompositor {
public:
    ~GpuCompositor();
    bool initialize(SharedD3D11Device& device, ReadbackCounters& readbacks, int width, int height,
                    std::string& err, GpuCompletionBackend backend = GpuCompletionBackend::Fence);
    bool initializeExternal(SharedD3D11Device& device, ReadbackCounters& readbacks,
                            std::string& err,
                            GpuCompletionBackend backend = GpuCompletionBackend::Fence);
    bool compose(const ComposedFrame& frame, std::string& err);
    // P5-C0: productのone-video slice専用。exactly 1 layerだけを受理する。
    // arbitrary-Nや既存2-layer product/formal契約の代替にはしない。
    bool composeSingleLayerToTarget(const ComposedFrame& frame,
                                    const ExternalCompositionTarget& target, std::string& err);
    // P5-E1: product composition経路。呼び出し側がqualified capabilityで検証済みの
    // layer数を明示し、その数と一致しない`ComposedFrame`を拒否する。
    // 層数を暗黙に受け入れないことで、composition acceptanceとGPU描画の
    // 食い違いを黙って通さない。
    bool composeLayersToTarget(const ComposedFrame& frame, const ExternalCompositionTarget& target,
                               size_t expectedLayerCount, std::string& err);
    bool composeToTarget(const ComposedFrame& frame, const ExternalCompositionTarget& target,
                         std::string& err);
    // P2-D3診断専用。1または2 layerを許可し、formal契約には使用しない。
    bool composeDiagnosticToTarget(const ComposedFrame& frame,
                                   const ExternalCompositionTarget& target,
                                   GpuCompositorStageTiming& timing, std::string& err);
    bool poll(std::string& err);
    bool shutdown(int timeoutMs, std::string& err);
    bool beginShutdown(int timeoutMs, std::string& err);
    GpuCompositorShutdownStatus pollShutdown(std::string& err);
    bool readOutputProbe(int x, int y, int width, int height, std::vector<unsigned char>& rgba,
                         std::string& err);
    bool readExternalOutputProbe(ID3D11Texture2D* texture, int x, int y, int width, int height,
                                 std::vector<unsigned char>& rgba, std::string& err);
    bool readSourceProbe(const DecodedGpuFrame& frame, float u, float v,
                         std::vector<unsigned char>& rgba, std::string& err);
    bool readSourceMarker(const DecodedGpuFrame& frame, int width, int height,
                          std::vector<unsigned char>& rgba, std::string& err);

    ID3D11Texture2D* outputTexture() const { return target_; }

    ID3D11ShaderResourceView* outputSrv() const { return targetSrv_; }

    const GpuCompositorCounters& counters() const { return counters_; }

    GpuCompletionBackend completionBackend() const { return completion_.backend(); }

    bool fatal() const { return fatal_; }

    const std::string& fatalReason() const { return fatalReason_; }

    bool ready() const { return ready_; }

    // 決定論的negative test専用。既定値では一切動作を変えない。
    void setTestFaults(GpuCompositorTestFaults faults) { testFaults_ = faults; }

private:
    void releaseTarget();
    void rollbackInitialize();
    void finishShutdown();
    void enterFatal(const std::string& reason);
    GpuCompositorShutdownStatus failShutdownCompletionPoll(const std::string& reason,
                                                           std::string& err);
    bool composeProductToTarget(const ComposedFrame& frame, const ExternalCompositionTarget& target,
                                size_t expectedLayerCount, const char* layerCountError,
                                std::string& err);
    bool prepareComposition(const ComposedFrame& frame, const ExternalCompositionTarget& target,
                            size_t expectedLayerCount, std::string& err);
    bool issueComposition(const ComposedFrame& frame, const ExternalCompositionTarget& target,
                          bool clearTarget, GpuCompositorStageTiming* timing, std::string& err);

    SharedD3D11Device* shared_ = nullptr;
    Nv12Converter converter_;
    GpuCompletionTracker completion_;
    GpuRetirementQueue retirement_;
    ID3D11Texture2D* target_ = nullptr;
    ID3D11RenderTargetView* targetRtv_ = nullptr;
    ID3D11ShaderResourceView* targetSrv_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
    bool fatal_ = false;
    std::string fatalReason_;
    GpuCompositorCounters counters_;
    GpuCompositorTestFaults testFaults_;
    bool shutdownStarted_ = false;
    bool shutdownFailed_ = false;
    long long shutdownDeadlineQpc_ = 0;
};

} // namespace mvm::gpu
#endif
