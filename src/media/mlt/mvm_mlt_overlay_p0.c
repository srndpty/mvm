#include "mvm_mlt_overlay_p0.h"

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

static int valid_clip(const MvmM7bP0OverlayClip* clip, int total_duration) {
    if (!clip || clip->timeline_start < 0 || clip->source_in < 0 || clip->duration <= 0 ||
        clip->timeline_start + clip->duration > total_duration || clip->rect_width <= 0.0 ||
        clip->rect_height <= 0.0 || clip->transition_in < 0 ||
        clip->transition_out < clip->transition_in || clip->opacity_keyframe_count <= 0 ||
        clip->opacity_keyframe_count > MVM_M7B_P0_MAX_KEYFRAMES || !clip->opacity_keyframes)
        return 0;
    if (clip->crop_left < 0 || clip->crop_top < 0 || clip->crop_right < 0 || clip->crop_bottom < 0)
        return 0;
    for (int index = 0; index < clip->opacity_keyframe_count; ++index) {
        const MvmM7bP0OpacityKeyframe* key = &clip->opacity_keyframes[index];
        if (key->position < 0 || key->opacity < 0.0 || key->opacity > 1.0 ||
            (index > 0 && key->position <= clip->opacity_keyframes[index - 1].position))
            return 0;
    }
    return 1;
}

static int attach_crop_pair(mlt_profile profile, mlt_producer cut, const MvmM7bP0OverlayClip* clip,
                            char* error, size_t error_size) {
    if (clip->crop_left == 0 && clip->crop_top == 0 && clip->crop_right == 0 &&
        clip->crop_bottom == 0)
        return 0;
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
    mlt_properties_set_int(properties, "left", clip->crop_left);
    mlt_properties_set_int(properties, "top", clip->crop_top);
    mlt_properties_set_int(properties, "right", clip->crop_right);
    mlt_properties_set_int(properties, "bottom", clip->crop_bottom);
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(active), "active", 1);
    if (mlt_producer_attach(cut, parameters) != 0 || mlt_producer_attach(cut, active) != 0) {
        mlt_filter_close(parameters);
        mlt_filter_close(active);
        set_error(error, error_size, "crop filter pairをV2 cutへattachできません");
        return 1;
    }
    mlt_filter_close(parameters);
    mlt_filter_close(active);
    return 0;
}

static int plant_affine(mlt_profile profile, mlt_tractor tractor, const MvmM7bP0OverlayClip* clip,
                        int* verified, char* error, size_t error_size) {
    mlt_transition transition = mlt_factory_transition(profile, "affine", NULL);
    if (!transition) {
        set_error(error, error_size, "必須transition 'affine'を作れません");
        return 1;
    }
    mlt_properties properties = MLT_TRANSITION_PROPERTIES(transition);
    mlt_properties_set_int(properties, "fill", 1);
    mlt_properties_set_int(properties, "distort", 1);
    mlt_properties_set_int(properties, "b_alpha", 0);
    mlt_properties_set_int(properties, "repeat_off", 1);
    mlt_properties_set_int(properties, "mirror_off", 1);
    mlt_properties_set_int(properties, "keyed", 0);
    mlt_properties_set(properties, "halign", "center");
    mlt_properties_set(properties, "valign", "middle");
    /* MLT affine 7.36.1では画面内の2D回転をfix_rotate_xが担う。名前ではなく
     * P0の実画素結果をauthorityにする。 */
    mlt_properties_set_double(properties, "fix_rotate_x", clip->rotation_degrees);
    mlt_transition_set_in_and_out(transition, clip->transition_in, clip->transition_out);

    for (int index = 0; index < clip->opacity_keyframe_count; ++index) {
        const MvmM7bP0OpacityKeyframe* key = &clip->opacity_keyframes[index];
        mlt_rect rectangle = {clip->rect_x, clip->rect_y, clip->rect_width, clip->rect_height,
                              key->opacity};
        if (mlt_properties_anim_set_rect(properties, "rect", rectangle, key->position,
                                         clip->duration, mlt_keyframe_linear) != 0) {
            mlt_transition_close(transition);
            set_error(error, error_size, "affine rect keyframe %dを設定できません", index);
            return 1;
        }
    }
    for (int index = 0; index < clip->opacity_keyframe_count; ++index) {
        const MvmM7bP0OpacityKeyframe* key = &clip->opacity_keyframes[index];
        const mlt_rect actual =
            mlt_properties_anim_get_rect(properties, "rect", key->position, clip->duration);
        if (fabs(actual.x - clip->rect_x) > 0.001 || fabs(actual.y - clip->rect_y) > 0.001 ||
            fabs(actual.w - clip->rect_width) > 0.001 ||
            fabs(actual.h - clip->rect_height) > 0.001 || fabs(actual.o - key->opacity) > 0.001) {
            mlt_transition_close(transition);
            set_error(error, error_size, "affine rect keyframe %dの読み戻しが違います", index);
            return 1;
        }
        ++*verified;
    }

    mlt_transition_set_tracks(transition, 0, 1);
    if (mlt_field_plant_transition(mlt_tractor_field(tractor), transition, 0, 1) != 0) {
        mlt_transition_close(transition);
        set_error(error, error_size, "affine transitionをV1/V2間へ配置できません");
        return 1;
    }
    mlt_transition_close(transition);
    return 0;
}

int mvm_m7b_p0_render(const MvmM7bP0RenderRequest* request, MvmM7bP0RenderResult* result,
                      char* error, size_t error_size) {
    mlt_profile profile = NULL;
    mlt_tractor tractor = NULL;
    mlt_playlist v1_playlist = NULL;
    mlt_playlist v2_playlist = NULL;
    mlt_producer parents[1 + MVM_M7B_P0_MAX_CLIPS] = {NULL};
    mlt_producer cuts[1 + MVM_M7B_P0_MAX_CLIPS] = {NULL};
    int parent_count = 0;
    int cut_count = 0;
    mlt_consumer consumer = NULL;
    int failed = 1;

    if (result)
        memset(result, 0, sizeof(*result));
    if (!mvm_mlt_runtime_is_ready() || !request || !result || !request->v1_source_path ||
        !request->output_path || request->width <= 0 || request->height <= 0 ||
        request->fps_num <= 0 || request->fps_den <= 0 || request->total_duration <= 0 ||
        request->v2_clip_count < 0 || request->v2_clip_count > MVM_M7B_P0_MAX_CLIPS ||
        (request->v2_clip_count > 0 && (!request->v2_source_path || !request->v2_clips))) {
        set_error(error, error_size, "M7b-P0 render引数が不正です");
        return 1;
    }
    int previous_end = 0;
    for (int index = 0; index < request->v2_clip_count; ++index) {
        const MvmM7bP0OverlayClip* clip = &request->v2_clips[index];
        if (!valid_clip(clip, request->total_duration) || clip->timeline_start < previous_end) {
            set_error(error, error_size, "M7b-P0 V2 clipが不正または重複しています");
            return 1;
        }
        previous_end = clip->timeline_start + clip->duration;
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
            !service_exists(mlt_repository_transitions(repository), "affine") ||
            !service_exists(mlt_repository_consumers(repository), "avformat")) {
            set_error(error, error_size, "必須service crop/affine/avformatがありません");
            goto cleanup;
        }
    }

    tractor = mlt_tractor_new();
    v1_playlist = mlt_playlist_new(profile);
    v2_playlist = mlt_playlist_new(profile);
    if (!tractor || !v1_playlist || !v2_playlist) {
        set_error(error, error_size, "tractorまたはV1/V2 playlistを作れません");
        goto cleanup;
    }
    result->playlist_count = 2;

    parents[parent_count] = mlt_factory_producer(profile, NULL, request->v1_source_path);
    if (!parents[parent_count] ||
        mlt_producer_get_playtime(parents[parent_count]) < request->total_duration) {
        set_error(error, error_size, "V1 fixture producerの尺が不足しています");
        goto cleanup;
    }
    cuts[cut_count] = mlt_producer_cut(parents[parent_count], 0, request->total_duration - 1);
    ++parent_count;
    if (!cuts[cut_count] || mlt_producer_get_playtime(cuts[cut_count]) != request->total_duration ||
        mlt_playlist_append(v1_playlist, cuts[cut_count]) != 0) {
        set_error(error, error_size, "V1 cutをplaylistへ追加できません");
        goto cleanup;
    }
    ++cut_count;

    int cursor = 0;
    for (int index = 0; index < request->v2_clip_count; ++index) {
        const MvmM7bP0OverlayClip* clip = &request->v2_clips[index];
        if (clip->timeline_start > cursor) {
            if (mlt_playlist_blank(v2_playlist, clip->timeline_start - cursor - 1) != 0) {
                set_error(error, error_size, "V2先頭またはclip間blankを追加できません");
                goto cleanup;
            }
            cursor = clip->timeline_start;
        }
        parents[parent_count] = mlt_factory_producer(profile, NULL, request->v2_source_path);
        if (!parents[parent_count] ||
            mlt_producer_get_playtime(parents[parent_count]) < clip->source_in + clip->duration) {
            set_error(error, error_size, "V2 fixture producerの尺が不足しています");
            goto cleanup;
        }
        cuts[cut_count] = mlt_producer_cut(parents[parent_count], clip->source_in,
                                           clip->source_in + clip->duration - 1);
        ++parent_count;
        if (!cuts[cut_count] || mlt_producer_get_playtime(cuts[cut_count]) != clip->duration) {
            set_error(error, error_size, "V2 clip-local cutを作れません");
            goto cleanup;
        }
        if (attach_crop_pair(profile, cuts[cut_count], clip, error, error_size) != 0)
            goto cleanup;
        if (clip->crop_left || clip->crop_top || clip->crop_right || clip->crop_bottom)
            ++result->crop_pair_count;
        if (mlt_playlist_append(v2_playlist, cuts[cut_count]) != 0) {
            set_error(error, error_size, "V2 cutをplaylistへ追加できません");
            goto cleanup;
        }
        ++cut_count;
        cursor += clip->duration;
    }
    if (cursor < request->total_duration &&
        mlt_playlist_blank(v2_playlist, request->total_duration - cursor - 1) != 0) {
        set_error(error, error_size, "V2末尾blankを追加できません");
        goto cleanup;
    }
    mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(v1_playlist), "hide", 2);
    mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(v2_playlist), "hide", 2);
    mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(v1_playlist), 0);
    mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(v2_playlist), 1);

    for (int index = 0; index < request->v2_clip_count; ++index) {
        if (plant_affine(profile, tractor, &request->v2_clips[index], &result->keyframes_verified,
                         error, error_size) != 0)
            goto cleanup;
        ++result->transition_count;
    }

    {
        mlt_producer output = MLT_TRACTOR_PRODUCER(tractor);
        if (mlt_producer_get_playtime(output) != request->total_duration) {
            set_error(error, error_size, "tractor尺が要求尺と一致しません");
            goto cleanup;
        }
        mlt_producer_set_in_and_out(output, 0, request->total_duration - 1);
        mlt_producer_seek(output, 0);
        consumer = mlt_factory_consumer(profile, "avformat", request->output_path);
        if (!consumer) {
            set_error(error, error_size, "avformat consumerを作れません");
            goto cleanup;
        }
        mlt_properties properties = MLT_CONSUMER_PROPERTIES(consumer);
        mlt_properties_set(properties, "target", request->output_path);
        mlt_properties_set(properties, "f", "mp4");
        mlt_properties_set(properties, "vcodec", "libx264");
        mlt_properties_set(properties, "preset", "ultrafast");
        mlt_properties_set(properties, "crf", "12");
        mlt_properties_set(properties, "pix_fmt", "yuv420p");
        mlt_properties_set_int(properties, "an", 1);
        mlt_properties_set_int(properties, "real_time", -1);
        mlt_properties_set_int(properties, "terminate_on_pause", 1);
        if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) != 0 ||
            mlt_consumer_start(consumer) != 0) {
            set_error(error, error_size, "M7b-P0 consumerを開始できません");
            goto cleanup;
        }
    }
    {
        int waited = 0;
        const int timeout = request->timeout_ms > 0 ? request->timeout_ms : 120000;
        while (!mlt_consumer_is_stopped(consumer)) {
            Sleep(20);
            waited += 20;
            if (waited >= timeout) {
                set_error(error, error_size, "M7b-P0 consumerがtimeoutしました");
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
            !probe.has_video || probe.frame_count < request->total_duration) {
            set_error(error, error_size, "M7b-P0出力のframe数または映像streamが不正です");
            goto cleanup;
        }
        result->frame_count = probe.frame_count;
    }
    result->opaque_black_affine_filter_count = 0;
    failed = 0;

cleanup:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
    if (tractor)
        mlt_tractor_close(tractor);
    if (v2_playlist)
        mlt_playlist_close(v2_playlist);
    if (v1_playlist)
        mlt_playlist_close(v1_playlist);
    for (int index = 0; index < cut_count; ++index)
        if (cuts[index])
            mlt_producer_close(cuts[index]);
    for (int index = 0; index < parent_count; ++index)
        if (parents[index])
            mlt_producer_close(parents[index]);
    if (profile)
        mlt_profile_close(profile);
    return failed;
}
