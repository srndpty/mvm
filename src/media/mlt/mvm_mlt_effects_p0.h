#ifndef MVM_MLT_EFFECTS_P0_H
#define MVM_MLT_EFFECTS_P0_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_M7A_P0_MAX_KEYFRAMES 8

typedef struct {
    int local_frame;
    double opacity;
} MvmM7aP0OpacityKeyframe;

typedef struct {
    const char* source_path;
    const char* output_path;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int source_in;
    int duration;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;
    double rect_x;
    double rect_y;
    double rect_width;
    double rect_height;
    double rotation_degrees;
    int b_alpha;
    int crop_on_parent;
    int attach_affine_first;
    const MvmM7aP0OpacityKeyframe* opacity_keyframes;
    int opacity_keyframe_count;
    int timeout_ms;
} MvmM7aP0RenderRequest;

typedef struct {
    long long frame_count;
    int keyframes_verified;
    int crop_attached;
    int affine_attached;
} MvmM7aP0RenderResult;

/* M7a-P0専用。製品export経路からは呼ばない。 */
int mvm_m7a_p0_render(const MvmM7aP0RenderRequest* request, MvmM7aP0RenderResult* result,
                      char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif // MVM_MLT_EFFECTS_P0_H
