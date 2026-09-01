#include "mvm_mlt_effects_p0.h"

#include "mvm_mlt_probe.h"
#include "mvm_mlt_runtime.h"

#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <framework/mlt.h>

static void set_error(char* error, size_t size, const char* format, ...) {
    if (!error || size == 0)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, size, format, arguments);
    va_end(arguments);
}

static int service_exists(mlt_properties services, const char* name) {
    if (!services || !name)
        return 0;
    const int count = mlt_properties_count(services);
    for (int index = 0; index < count; ++index) {
        const char* actual = mlt_properties_get_name(services, index);
        if (actual && strcmp(actual, name) == 0)
            return 1;
    }
    return 0;
}

static int attach_crop(mlt_profile profile, mlt_producer cut, const MvmM7aP0RenderRequest* request,
                       char* error, size_t error_size) {
    /* cropは2段で動く。passive instanceがframeのcrop.*とmeta.media.*を設定し、
     * active instanceがその値を読んで実画素を切る。active側へleft等を設定しても
     * 実装は読まないため、1 instanceだけではproperty読み戻しだけ成功して空振りする。 */
    mlt_filter parameters = mlt_factory_filter(profile, "crop", NULL);
    mlt_filter active = mlt_factory_filter(profile, "crop", NULL);
    if (!parameters || !active) {
        if (parameters)
            mlt_filter_close(parameters);
        if (active)
            mlt_filter_close(active);
        set_error(error, error_size, "必須filter 'crop'を2 instance作れません");
        return 1;
    }
    mlt_properties properties = MLT_FILTER_PROPERTIES(parameters);
    mlt_properties_set_int(properties, "active", 0);
    mlt_properties_set_int(properties, "use_profile", 1);
    mlt_properties_set_int(properties, "left", request->crop_left);
    mlt_properties_set_int(properties, "top", request->crop_top);
    mlt_properties_set_int(properties, "right", request->crop_right);
    mlt_properties_set_int(properties, "bottom", request->crop_bottom);
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(active), "active", 1);
    if (mlt_producer_attach(cut, parameters) != 0 || mlt_producer_attach(cut, active) != 0) {
        mlt_filter_close(parameters);
        mlt_filter_close(active);
        set_error(error, error_size, "crop filter pairをproducerへattachできません");
        return 1;
    }
    mlt_filter_close(parameters);
    mlt_filter_close(active);
    return 0;
}

static int attach_affine(mlt_profile profile, mlt_producer cut,
                         const MvmM7aP0RenderRequest* request, int* verified, char* error,
                         size_t error_size) {
    mlt_filter filter = mlt_factory_filter(profile, "affine", NULL);
    if (!filter) {
        set_error(error, error_size, "必須filter 'affine'を作れません");
        return 1;
    }
    mlt_properties properties = MLT_FILTER_PROPERTIES(filter);
    /* transparent backgroundではMP4がalphaを捨てた後もRGBが元の明るさで残る。
     * opacity/fadeの実画素を検証するためopaque blackを明示する。 */
    mlt_properties_set(properties, "background", "colour:#000000");
    mlt_properties_set_int(properties, "transition.fill", 1);
    mlt_properties_set_int(properties, "transition.distort", 1);
    mlt_properties_set_int(properties, "transition.b_alpha", request->b_alpha);
    mlt_properties_set_int(properties, "transition.repeat_off", 1);
    mlt_properties_set_int(properties, "transition.mirror_off", 1);
    mlt_properties_set(properties, "transition.halign", "center");
    mlt_properties_set(properties, "transition.valign", "middle");
    mlt_properties_set_double(properties, "transition.fix_rotate_z", request->rotation_degrees);
    /* cutのframeはparent producerのpositionを保持する。filter rangeを明示的な
     * source rangeへ合わせ、mlt_filter_get_position()が返す値をclip-localへする。 */
    mlt_filter_set_in_and_out(filter, request->source_in,
                              request->source_in + request->duration - 1);

    for (int index = 0; index < request->opacity_keyframe_count; ++index) {
        const MvmM7aP0OpacityKeyframe* key = &request->opacity_keyframes[index];
        mlt_rect rectangle = {request->rect_x, request->rect_y, request->rect_width,
                              request->rect_height, key->opacity};
        if (mlt_properties_anim_set_rect(properties, "transition.rect", rectangle, key->local_frame,
                                         request->duration, mlt_keyframe_linear) != 0) {
            mlt_filter_close(filter);
            set_error(error, error_size, "transition.rect keyframe %dを設定できません", index);
            return 1;
        }
    }

    *verified = 0;
    for (int index = 0; index < request->opacity_keyframe_count; ++index) {
        const MvmM7aP0OpacityKeyframe* key = &request->opacity_keyframes[index];
        const mlt_rect actual = mlt_properties_anim_get_rect(properties, "transition.rect",
                                                             key->local_frame, request->duration);
        if (fabs(actual.x - request->rect_x) > 0.001 || fabs(actual.y - request->rect_y) > 0.001 ||
            fabs(actual.w - request->rect_width) > 0.001 ||
            fabs(actual.h - request->rect_height) > 0.001 ||
            fabs(actual.o - key->opacity) > 0.001) {
            mlt_filter_close(filter);
            set_error(error, error_size,
                      "transition.rect keyframe %dの読み戻しが設定値と一致しません", index);
            return 1;
        }
        ++*verified;
    }

    if (mlt_producer_attach(cut, filter) != 0) {
        mlt_filter_close(filter);
        set_error(error, error_size, "affine filterをcut producerへattachできません");
        return 1;
    }
    mlt_filter_close(filter);
    return 0;
}

int mvm_m7a_p0_render(const MvmM7aP0RenderRequest* request, MvmM7aP0RenderResult* result,
                      char* error, size_t error_size) {
    mlt_profile profile = NULL;
    mlt_producer parent = NULL;
    mlt_producer cut = NULL;
    mlt_playlist playlist = NULL;
    mlt_consumer consumer = NULL;
    int failed = 1;

    if (result)
        memset(result, 0, sizeof(*result));
    if (!mvm_mlt_runtime_is_ready() || !request || !result || !request->source_path ||
        !request->output_path || request->width <= 0 || request->height <= 0 ||
        request->fps_num <= 0 || request->fps_den <= 0 || request->source_in < 0 ||
        request->duration <= 0 || request->opacity_keyframe_count <= 0 ||
        request->opacity_keyframe_count > MVM_M7A_P0_MAX_KEYFRAMES || !request->opacity_keyframes) {
        set_error(error, error_size, "M7a-P0 render引数が不正です");
        return 1;
    }
    for (int index = 0; index < request->opacity_keyframe_count; ++index) {
        const MvmM7aP0OpacityKeyframe* key = &request->opacity_keyframes[index];
        if (key->local_frame < 0 || key->local_frame >= request->duration || key->opacity < 0.0 ||
            key->opacity > 1.0 ||
            (index > 0 && key->local_frame <= request->opacity_keyframes[index - 1].local_frame)) {
            set_error(error, error_size, "M7a-P0 opacity keyframeが不正です");
            return 1;
        }
    }

    profile = mlt_profile_init(NULL);
    if (!profile) {
        set_error(error, error_size, "profileを作れません");
        goto cleanup;
    }
    profile->width = request->width;
    profile->height = request->height;
    profile->frame_rate_num = request->fps_num;
    profile->frame_rate_den = request->fps_den;
    profile->sample_aspect_num = 1;
    profile->sample_aspect_den = 1;
    profile->display_aspect_num = request->width;
    profile->display_aspect_den = request->height;
    profile->progressive = 1;
    profile->colorspace = 709;

    {
        mlt_repository repository = mlt_factory_repository();
        if (!service_exists(mlt_repository_filters(repository), "crop") ||
            !service_exists(mlt_repository_filters(repository), "affine") ||
            !service_exists(mlt_repository_consumers(repository), "avformat")) {
            set_error(error, error_size, "必須service crop/affine/avformatがありません");
            goto cleanup;
        }
    }

    parent = mlt_factory_producer(profile, NULL, request->source_path);
    if (!parent || mlt_producer_get_playtime(parent) < request->source_in + request->duration) {
        set_error(error, error_size, "fixture producerの尺がcut範囲より短いです");
        goto cleanup;
    }
    cut = mlt_producer_cut(parent, request->source_in, request->source_in + request->duration - 1);
    if (!cut || mlt_producer_get_playtime(cut) != request->duration) {
        set_error(error, error_size, "明示的なclip-local cut producerを作れません");
        goto cleanup;
    }

    mlt_producer crop_target = request->crop_on_parent ? parent : cut;
    if (request->attach_affine_first) {
        if (attach_affine(profile, cut, request, &result->keyframes_verified, error, error_size) ||
            attach_crop(profile, crop_target, request, error, error_size))
            goto cleanup;
    } else {
        if (attach_crop(profile, crop_target, request, error, error_size) ||
            attach_affine(profile, cut, request, &result->keyframes_verified, error, error_size))
            goto cleanup;
    }
    result->crop_attached = 1;
    result->affine_attached = 1;

    playlist = mlt_playlist_new(profile);
    if (!playlist || mlt_playlist_append(playlist, cut) != 0) {
        set_error(error, error_size, "effect付きcutをplaylistへappendできません");
        goto cleanup;
    }
    mlt_producer output = MLT_PLAYLIST_PRODUCER(playlist);
    mlt_producer_set_in_and_out(output, 0, request->duration - 1);
    mlt_producer_seek(output, 0);

    consumer = mlt_factory_consumer(profile, "avformat", request->output_path);
    if (!consumer) {
        set_error(error, error_size, "avformat consumerを作れません");
        goto cleanup;
    }
    mlt_properties consumer_properties = MLT_CONSUMER_PROPERTIES(consumer);
    mlt_properties_set(consumer_properties, "target", request->output_path);
    mlt_properties_set(consumer_properties, "f", "mp4");
    mlt_properties_set(consumer_properties, "vcodec", "libx264");
    mlt_properties_set(consumer_properties, "preset", "ultrafast");
    mlt_properties_set(consumer_properties, "crf", "12");
    mlt_properties_set(consumer_properties, "pix_fmt", "yuv420p");
    mlt_properties_set_int(consumer_properties, "an", 1);
    mlt_properties_set_int(consumer_properties, "real_time", -1);
    mlt_properties_set_int(consumer_properties, "terminate_on_pause", 1);
    if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) != 0 ||
        mlt_consumer_start(consumer) != 0) {
        set_error(error, error_size, "M7a-P0 consumerを開始できません");
        goto cleanup;
    }
    {
        int waited = 0;
        const int timeout = request->timeout_ms > 0 ? request->timeout_ms : 120000;
        while (!mlt_consumer_is_stopped(consumer)) {
            Sleep(20);
            waited += 20;
            if (waited >= timeout) {
                set_error(error, error_size, "M7a-P0 consumerがtimeoutしました");
                goto cleanup;
            }
        }
    }
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    consumer = NULL;

    {
        MvmMltProbeResult probe;
        memset(&probe, 0, sizeof(probe));
        if (mvm_mlt_probe_file(request->output_path, &probe) != 0 || !probe.ok ||
            !probe.has_video || probe.frame_count < request->duration) {
            set_error(error, error_size, "M7a-P0出力のframe数または映像streamが不正です");
            goto cleanup;
        }
        result->frame_count = probe.frame_count;
    }
    failed = 0;

cleanup:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
    if (playlist)
        mlt_playlist_close(playlist);
    if (cut)
        mlt_producer_close(cut);
    if (parent)
        mlt_producer_close(parent);
    if (profile)
        mlt_profile_close(profile);
    return failed;
}
