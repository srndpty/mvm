#include "mvm_mlt_math_group.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 生きている transition の状態の数と、実行中の画像 callback の数。書き出しが終わった時点で
 * どちらも 0 でなければならない (mvm_mlt_export.c が確かめる)。 */
static volatile LONG64 g_live_states = 0;
static volatile LONG64 g_active_callbacks = 0;

long long mvm_mlt_math_group_live(void) {
    return (long long)InterlockedCompareExchange64(&g_live_states, 0, 0);
}

long long mvm_mlt_math_group_active(void) {
    return (long long)InterlockedCompareExchange64(&g_active_callbacks, 0, 0);
}

typedef struct {
    const void* raster;
    long long start_frame; /* timeline の絶対 frame */
    long long frame_count;
    double* x; /* frame_count 個。clip の先頭からの frame ごとの値 */
    double* y;
    double* opacity;
} MathGroupMember;

typedef struct {
    int width, height;
    int count;
    MathGroupMember* members;
    long long first_frame; /* 最初の member の先頭 (故障注入の基準) */
    MvmExportObserver* observer;
    MvmExportMathGroupCompose compose;
    void* compose_opaque;
    int fault;
} MathGroupState;

static void group_error(char* err, size_t err_size, const char* message) {
    if (err && err_size > 0)
        snprintf(err, err_size, "%s", message);
}

int mvm_mlt_math_group_clip_eligible(const MvmExportClip* clip, int width, int height,
                                     const char** reason) {
    if (reason)
        *reason = NULL;
    if (!clip || !clip->math_group_raster)
        return 0;
    const char* why = NULL;
    if (clip->is_audio || clip->video_track <= 0)
        why = "not-overlay-video";
    else if (clip->opaque_backdrop)
        why = "opaque-backdrop";
    else if (clip->is_image_sequence)
        why = "image-sequence";
    else if (clip->source_width != 0 || clip->source_height != 0)
        why = "source-region";
    else if (clip->timeline_duration_frames <= 0 ||
             clip->opacity_keyframe_count != clip->timeline_duration_frames ||
             !clip->opacity_keyframes ||
             (clip->motion_frame_count != 0 &&
              clip->motion_frame_count != clip->opacity_keyframe_count))
        why = "no-per-frame-values";
    else if (clip->crop_left != 0 || clip->crop_top != 0 || clip->crop_right != 0 ||
             clip->crop_bottom != 0)
        why = "crop";
    else if (clip->motion_frame_count == 0 &&
             (clip->rect_width != width || clip->rect_height != height))
        why = "scale";
    else if (clip->motion_frame_count == 0 &&
             (clip->rotation_degrees != 0.0 || clip->shear_degrees != 0.0))
        why = "rotation-or-shear";
    for (int index = 0; !why && index < clip->opacity_keyframe_count; ++index) {
        const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[index];
        if (key->local_frame != index || !(key->opacity >= 0.0 && key->opacity <= 1.0)) {
            why = "no-per-frame-values";
            break;
        }
        if (clip->motion_frame_count == 0)
            continue;
        const MvmExportMotionFrame* frame = &clip->motion_frames[index];
        if (frame->local_frame != index)
            why = "no-per-frame-values";
        else if (frame->rect_width != width || frame->rect_height != height)
            why = "scale";
        else if (frame->rotation_degrees != 0.0 || frame->shear_degrees != 0.0)
            why = "rotation-or-shear";
        else if (frame->crop_left != 0.0 || frame->crop_top != 0.0 || frame->crop_right != 0.0 ||
                 frame->crop_bottom != 0.0)
            why = "crop";
        else if (!isfinite(frame->rect_x) || !isfinite(frame->rect_y))
            why = "non-finite-position";
    }
    if (!why && clip->motion_frame_count == 0 &&
        (!isfinite(clip->rect_x) || !isfinite(clip->rect_y)))
        why = "non-finite-position";
    if (reason)
        *reason = why;
    return why == NULL;
}

static void state_free(MathGroupState* state) {
    if (!state)
        return;
    for (int index = 0; state->members && index < state->count; ++index) {
        free(state->members[index].x);
        free(state->members[index].y);
        free(state->members[index].opacity);
    }
    free(state->members);
    free(state);
    InterlockedDecrement64(&g_live_states);
}

static void math_group_close(mlt_transition transition) {
    state_free((MathGroupState*)transition->child);
    transition->child = NULL;
    /* MLT の規約: close を外してからもう一度呼ぶと、service 本体が解放される。 */
    transition->close = NULL;
    mlt_transition_close(transition);
}

/* 最初の失敗だけを記録する。export の待ち loop がこれを見て書き出しを失敗にする。 */
static void record_failure(MathGroupState* state, long long frame, const char* message) {
    MvmExportObserver* observer = state->observer;
    if (!observer)
        return;
    if (InterlockedIncrement64(&observer->math_group_errors) == 1) {
        observer->math_group_error_frame = frame;
        snprintf(observer->math_group_error, sizeof(observer->math_group_error), "%s", message);
        InterlockedExchange(&observer->math_group_error_ready, 1);
    }
}

static int math_group_get_image(mlt_frame a_frame, uint8_t** image, mlt_image_format* format,
                                int* width, int* height, int writable) {
    mlt_transition transition = (mlt_transition)mlt_frame_pop_service(a_frame);
    MathGroupState* state = (MathGroupState*)transition->child;
    MvmExportObserver* observer = state->observer;
    const long long position = (long long)mlt_frame_get_position(a_frame);
    const long long begin_ns = mvm_mlt_export_observer_now_ns();
    InterlockedIncrement64(&g_active_callbacks);

    /* この frame で見えている member を、合成の順 (下から) に集める。 */
    MvmExportMathGroupLayer* layers =
        (MvmExportMathGroupLayer*)malloc(sizeof(MvmExportMathGroupLayer) * (size_t)state->count);
    int active = 0;
    const int fault = state->fault;
    for (int index = 0; layers && index < state->count; ++index) {
        const MathGroupMember* member = &state->members[index];
        long long local = position - member->start_frame;
        if (local < 0 || local >= member->frame_count)
            continue;
        /* 以下は比較の対照用の故障 (製品・通常の診断では fault は 0)。 */
        if (fault == 2 && local + 1 < member->frame_count)
            ++local; /* 1 つ後の frame の値を使う (対応の誤り) */
        layers[active].raster = member->raster;
        if (fault == 5) /* 別の member の画像を使う (古い・取り違えた画像) */
            layers[active].raster = state->members[(index + 1) % state->count].raster;
        layers[active].rect_x = member->x[local];
        layers[active].rect_y = member->y[local];
        layers[active].opacity = member->opacity[local] * (fault == 3 ? 0.5 : 1.0);
        ++active;
    }
    if (fault == 6 && active > 0)
        --active; /* 一番上の layer を落とす */
    if (fault == 1 && position == state->first_frame + 1)
        active = 0; /* この frame の合成を飛ばす */

    int error = 0;
    long long upstream_ns = 0, copy_ns = 0, compose_ns = 0;
    if (!layers) {
        record_failure(state, position, "数式 group: layer の一覧を確保できません");
        error = mlt_frame_get_image(a_frame, image, format, width, height, writable);
    } else if (active == 0) {
        /* 見えている member が無い frame は何もしない (形式も要求し直さない)。 */
        const long long t0 = mvm_mlt_export_observer_now_ns();
        error = mlt_frame_get_image(a_frame, image, format, width, height, writable);
        upstream_ns = mvm_mlt_export_observer_now_ns() - t0;
    } else {
        *format = mlt_image_rgba;
        const long long t0 = mvm_mlt_export_observer_now_ns();
        error = mlt_frame_get_image(a_frame, image, format, width, height, 1);
        const long long t1 = mvm_mlt_export_observer_now_ns();
        upstream_ns = t1 - t0;
        if (error || !image || !*image) {
            record_failure(state, position, "数式 group: 下の frame の画像を取得できません");
        } else if (*format != mlt_image_rgba || *width != state->width ||
                   *height != state->height) {
            record_failure(state, position,
                           "数式 group: 下の frame が出力と同じ大きさの RGBA ではありません");
        } else {
            const size_t stride = (size_t)state->width * 4U;
            const size_t size = stride * (size_t)state->height;
            /* 渡された buffer へは書かない。複製へ合成し、成功したときだけ差し替える。 */
            uint8_t* copy = (uint8_t*)mlt_pool_alloc((int)size);
            if (!copy) {
                record_failure(state, position, "数式 group: 合成先を確保できません");
            } else {
                memcpy(copy, *image, size);
                if (fault == 4)
                    copy[3] = 254; /* 下地に不透明でない画素を作る (合成器が拒否する) */
                const long long t2 = mvm_mlt_export_observer_now_ns();
                copy_ns = t2 - t1;
                char message[200] = {0};
                const int rejected =
                    state->compose(state->compose_opaque, copy, state->width, state->height, stride,
                                   layers, active, message, sizeof(message));
                compose_ns = mvm_mlt_export_observer_now_ns() - t2;
                if (rejected) {
                    char text[256];
                    snprintf(text, sizeof(text), "数式 group: 合成器が拒否しました: %s", message);
                    mlt_pool_release(copy);
                    record_failure(state, position, text);
                } else {
                    /* frame の画像を複製へ差し替える (前の buffer は持ち主の destructor
                     * が解放する)。 */
                    mlt_frame_set_image(a_frame, copy, (int)size, mlt_pool_release);
                    *image = copy;
                    if (observer) {
                        InterlockedIncrement64(&observer->math_group_frames);
                        InterlockedExchangeAdd64(&observer->math_group_layer_frames, active);
                    }
                }
            }
        }
    }
    free(layers);
    if (observer) {
        InterlockedIncrement64(&observer->math_group_callbacks);
        InterlockedExchangeAdd64(&observer->math_group_upstream_ns, upstream_ns);
        InterlockedExchangeAdd64(&observer->math_group_copy_ns, copy_ns);
        InterlockedExchangeAdd64(&observer->math_group_compose_ns, compose_ns);
        InterlockedExchangeAdd64(&observer->math_group_callback_ns,
                                 mvm_mlt_export_observer_now_ns() - begin_ns - upstream_ns);
    }
    InterlockedDecrement64(&g_active_callbacks);
    return error;
}

static mlt_frame math_group_process(mlt_transition transition, mlt_frame a_frame,
                                    mlt_frame b_frame) {
    (void)b_frame; /* 上の track の frame は使わない (member は playlist に置いていない) */
    /* frame が transition より長く生きても状態を参照できるよう、frame に参照を 1 つ持たせる。 */
    char name[64];
    snprintf(name, sizeof(name), "mvm.math_group.%p", (void*)transition);
    mlt_properties_inc_ref(MLT_TRANSITION_PROPERTIES(transition));
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(a_frame), name, transition, 0,
                            (mlt_destructor)mlt_transition_close, NULL);
    mlt_frame_push_service(a_frame, transition);
    mlt_frame_push_get_image(a_frame, math_group_get_image);
    return a_frame;
}

mlt_transition mvm_mlt_math_group_new(const MvmExportClip* const* members, int count, int width,
                                      int height, MvmExportObserver* observer, char* err,
                                      size_t err_size) {
    if (!members || count <= 0 || width < 2 || height < 2 || !observer ||
        !observer->diagnostic_math_group_compose) {
        group_error(err, err_size, "数式 group の入力が不正です");
        return NULL;
    }
    MathGroupState* state = (MathGroupState*)calloc(1, sizeof(MathGroupState));
    if (!state) {
        group_error(err, err_size, "数式 group の状態を確保できません");
        return NULL;
    }
    InterlockedIncrement64(&g_live_states);
    state->width = width;
    state->height = height;
    state->observer = observer;
    state->compose = observer->diagnostic_math_group_compose;
    state->compose_opaque = observer->diagnostic_math_group_opaque;
    state->fault = observer->diagnostic_math_group_fault;
    state->members = (MathGroupMember*)calloc((size_t)count, sizeof(MathGroupMember));
    state->count = state->members ? count : 0;
    long long first = 0, last = 0;
    for (int index = 0; index < state->count; ++index) {
        const MvmExportClip* clip = members[index];
        if (!mvm_mlt_math_group_clip_eligible(clip, width, height, NULL)) {
            state_free(state);
            group_error(err, err_size, "数式 group に対象外の clip が含まれています");
            return NULL;
        }
        MathGroupMember* member = &state->members[index];
        const size_t frames = (size_t)clip->timeline_duration_frames;
        member->raster = clip->math_group_raster;
        member->start_frame = clip->timeline_start_frame;
        member->frame_count = clip->timeline_duration_frames;
        member->x = (double*)malloc(sizeof(double) * frames);
        member->y = (double*)malloc(sizeof(double) * frames);
        member->opacity = (double*)malloc(sizeof(double) * frames);
        if (!member->x || !member->y || !member->opacity) {
            state_free(state);
            group_error(err, err_size, "数式 group の frame ごとの値を確保できません");
            return NULL;
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            const int moving = clip->motion_frame_count > 0;
            member->x[frame] = moving ? clip->motion_frames[frame].rect_x : clip->rect_x;
            member->y[frame] = moving ? clip->motion_frames[frame].rect_y : clip->rect_y;
            member->opacity[frame] = clip->opacity_keyframes[frame].opacity;
        }
        const long long end = member->start_frame + member->frame_count - 1;
        if (index == 0 || member->start_frame < first)
            first = member->start_frame;
        if (index == 0 || end > last)
            last = end;
    }
    if (state->count != count) {
        state_free(state);
        group_error(err, err_size, "数式 group の member を確保できません");
        return NULL;
    }
    state->first_frame = state->members[0].start_frame;
    mlt_transition transition = mlt_transition_new();
    if (!transition) {
        state_free(state);
        group_error(err, err_size, "数式 group の transition を作れません");
        return NULL;
    }
    transition->child = state;
    transition->process = math_group_process;
    transition->close = math_group_close;
    mlt_properties properties = MLT_TRANSITION_PROPERTIES(transition);
    mlt_properties_set(properties, "mlt_service", "mvm_math_group");
    mlt_properties_set_int(properties, "_transition_type", 1); /* 映像 */
    /* 上の track は blank (member を playlist に置かない)。blank でも呼ばれるようにする。 */
    mlt_properties_set_int(properties, "accepts_blanks", 1);
    mlt_transition_set_in_and_out(transition, (mlt_position)first, (mlt_position)last);
    return transition;
}
