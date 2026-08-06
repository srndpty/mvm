#include "media/gpu_preview/gpu_frame.h"

#include "media/gpu_preview/preview_surface.h"
#include "media/gpu_preview/readback_counter.h"

namespace mvm::gpu {

const char* toString(GpuPixelFormat f) {
    switch (f) {
    case GpuPixelFormat::NV12:
        return "nv12";
    case GpuPixelFormat::P010:
        return "p010";
    case GpuPixelFormat::Unknown:
        break;
    }
    return "unknown";
}

const char* toString(ColorSpace c) {
    switch (c) {
    case ColorSpace::BT601:
        return "bt601";
    case ColorSpace::BT709:
        return "bt709";
    case ColorSpace::BT2020NCL:
        return "bt2020ncl";
    case ColorSpace::Unknown:
        break;
    }
    return "unknown";
}

const char* toString(ColorRange r) {
    switch (r) {
    case ColorRange::Limited:
        return "limited";
    case ColorRange::Full:
        return "full";
    case ColorRange::Unknown:
        break;
    }
    return "unknown";
}

const char* toString(SubmitResult r) {
    switch (r) {
    case SubmitResult::Accepted:
        return "accepted";
    case SubmitResult::RejectedStaleGeneration:
        return "rejected_stale_generation";
    case SubmitResult::RejectedInvalidFrame:
        return "rejected_invalid_frame";
    case SubmitResult::RejectedDeviceMismatch:
        return "rejected_device_mismatch";
    case SubmitResult::RejectedNotReady:
        return "rejected_not_ready";
    case SubmitResult::RejectedQueueFull:
        return "rejected_queue_full";
    }
    return "unknown";
}

ReadbackCounters& globalReadbackCounters() {
    static ReadbackCounters counters;
    return counters;
}

} // namespace mvm::gpu
