#ifndef MVM_MLT_RGBA_DIAGNOSTIC_H
#define MVM_MLT_RGBA_DIAGNOSTIC_H
#ifdef __cplusplus
extern "C" {
#endif
/* 検証専用。製品と同じ qimage/color/affine service を一段ずつ観測する。
 * stage 0=背景、1=producer、2=合成直前の背景、3=合成直前の素材、4=直後。
 * PNG 配列の順に合成し、callback 中だけ画素を借用する。MLT の型は公開しない。 */
int mvm_mlt_rgba_diagnostic(const char* const* paths, int count, const char* background, int width,
                            int height, double opacity,
                            void (*callback)(int stage, int layer, const unsigned char* rgba,
                                             int width, int height, void* opaque),
                            void* opaque);
#ifdef __cplusplus
}
#endif
#endif
