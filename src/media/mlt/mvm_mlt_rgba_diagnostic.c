#include "mvm_mlt_rgba_diagnostic.h"

#include <framework/mlt.h>

static int observe(mlt_frame frame, int stage, int layer, int width, int height,
                   void (*callback)(int, int, const unsigned char*, int, int, void*),
                   void* opaque) {
    uint8_t* image = NULL;
    mlt_image_format format = mlt_image_rgba;
    int w = width, h = height;
    if (mlt_frame_get_image(frame, &image, &format, &w, &h, 1) || !image ||
        format != mlt_image_rgba || w != width || h != height)
        return 1;
    callback(stage, layer, image, w, h, opaque);
    return 0;
}

int mvm_mlt_rgba_diagnostic(const char* const* paths, int count, const char* background, int width,
                            int height, double opacity,
                            void (*callback)(int, int, const unsigned char*, int, int, void*),
                            void* opaque) {
    if (!paths || count < 1 || !callback || width < 1 || height < 1)
        return 1;
    int status = 1;
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer base = NULL;
    mlt_frame destination = NULL;
    if (!profile)
        return 1;
    profile->width = width;
    profile->height = height;
    profile->frame_rate_num = 30;
    profile->frame_rate_den = 1;
    profile->sample_aspect_num = profile->sample_aspect_den = 1;
    profile->display_aspect_num = width;
    profile->display_aspect_den = height;
    profile->progressive = 1;
    profile->is_explicit = 1;
    base = mlt_factory_producer(profile, "color", background);
    if (!base || mlt_service_get_frame(MLT_PRODUCER_SERVICE(base), &destination, 0) ||
        observe(destination, 0, -1, width, height, callback, opaque))
        goto cleanup;
    for (int layer = 0; layer < count; ++layer) {
        mlt_producer source = mlt_factory_producer(profile, "qimage", paths[layer]);
        mlt_transition transition = mlt_factory_transition(profile, "affine", NULL);
        mlt_frame input = NULL;
        int failed = !source || !transition;
        if (!failed)
            failed = mlt_service_get_frame(MLT_PRODUCER_SERVICE(source), &input, 0);
        if (!failed) {
            mlt_properties props = MLT_TRANSITION_PROPERTIES(transition);
            mlt_properties_set_int(props, "fill", 1);
            mlt_properties_set_int(props, "distort", 1);
            mlt_properties_set_int(props, "b_alpha", 0);
            mlt_properties_set_int(props, "keyed", 0);
            mlt_properties_set_int(props, "repeat_off", 1);
            mlt_properties_set_int(props, "mirror_off", 1);
            mlt_properties_set(props, "halign", "center");
            mlt_properties_set(props, "valign", "middle");
            mlt_rect rect = {0, 0, width, height, opacity};
            mlt_properties_set_rect(props, "rect", rect);
            failed = observe(input, 1, layer, width, height, callback, opaque) ||
                     observe(destination, 2, layer, width, height, callback, opaque) ||
                     observe(input, 3, layer, width, height, callback, opaque);
            if (!failed) {
                /* 関数ポインタは factory が実 DLL から取得する。oracle はここを呼ばない。 */
                transition->process(transition, destination, input);
                failed = observe(destination, 4, layer, width, height, callback, opaque);
            }
        }
        if (input)
            mlt_frame_close(input);
        if (transition)
            mlt_transition_close(transition);
        if (source)
            mlt_producer_close(source);
        if (failed)
            goto cleanup;
    }
    status = 0;
cleanup:
    if (destination)
        mlt_frame_close(destination);
    if (base)
        mlt_producer_close(base);
    mlt_profile_close(profile);
    return status;
}
