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
};

// Qt非依存のoffscreen compositor。全layerを事前検証し、1 clear + N drawを
// 1 submission serialとして追跡する。
class GpuCompositor {
public:
    ~GpuCompositor();
    bool initialize(SharedD3D11Device& device, ReadbackCounters& readbacks, int width, int height,
                    std::string& err, GpuCompletionBackend backend = GpuCompletionBackend::Fence);
    bool compose(const ComposedFrame& frame, std::string& err);
    bool poll(std::string& err);
    bool shutdown(int timeoutMs, std::string& err);
    bool readOutputProbe(int x, int y, int width, int height, std::vector<unsigned char>& rgba,
                         std::string& err);
    bool readSourceMarker(const DecodedGpuFrame& frame, int width, int height,
                          std::vector<unsigned char>& rgba, std::string& err);

    ID3D11Texture2D* outputTexture() const { return target_; }

    ID3D11ShaderResourceView* outputSrv() const { return targetSrv_; }

    const GpuCompositorCounters& counters() const { return counters_; }

    GpuCompletionBackend completionBackend() const { return completion_.backend(); }

private:
    void releaseTarget();
    bool validateAllLayers(const ComposedFrame& frame, std::string& err);

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
    GpuCompositorCounters counters_;
};

} // namespace mvm::gpu
#endif
