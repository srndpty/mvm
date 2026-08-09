#ifndef MVM_GPU_PREVIEW_VALIDATION_REFERENCE_H
#define MVM_GPU_PREVIEW_VALIDATION_REFERENCE_H

namespace mvm::gpu {

// Phase 2 / Phase 4 の検証だけが使う、Qt 非依存の独立 reference 値型。
// product shader / Nv12Converter の実装とは共有しない。
struct Rgba8 {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 255;
    bool operator==(const Rgba8&) const = default;
};

Rgba8 bt709Limited(double y, double u, double v);
Rgba8 straightAlphaBlend(Rgba8 source, Rgba8 destination, double opacity);
bool probeWithinTolerance(const Rgba8& actual, const Rgba8& expected, int rgbTolerance = 3);

} // namespace mvm::gpu

#endif
