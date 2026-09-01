#ifndef MVM_MLT_OVERLAY_P0_H
#define MVM_MLT_OVERLAY_P0_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_M7B_P0_MAX_CLIPS 2
#define MVM_M7B_P0_MAX_KEYFRAMES 8

typedef struct {
    int position;
    double opacity;
} MvmM7bP0OpacityKeyframe;

typedef struct {
    int timeline_start;
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
    int transition_in;
    int transition_out;
    const MvmM7bP0OpacityKeyframe* opacity_keyframes;
    int opacity_keyframe_count;
} MvmM7bP0OverlayClip;

typedef struct {
    const char* v1_source_path;
    const char* v2_source_path;
    const char* output_path;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int total_duration;
    const MvmM7bP0OverlayClip* v2_clips;
    int v2_clip_count;
    int timeout_ms;
} MvmM7bP0RenderRequest;

typedef struct {
    long long frame_count;
    int playlist_count;
    int transition_count;
    int crop_pair_count;
    int keyframes_verified;
    int opaque_black_affine_filter_count;
} MvmM7bP0RenderResult;

/* M7b-P0専用。製品export経路からは呼ばない。 */
int mvm_m7b_p0_render(const MvmM7bP0RenderRequest* request, MvmM7bP0RenderResult* result,
                      char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif // MVM_MLT_OVERLAY_P0_H
