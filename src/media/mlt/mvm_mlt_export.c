#include "mvm_mlt_export.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_math_group.h"
#include "mvm_mlt_probe.h"
#include "mvm_mlt_runtime.h"

#include <windows.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <framework/mlt.h>

static void set_err(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

long long mvm_mlt_export_observer_now_ns(void) {
    /* 周波数は起動中に変わらない。複数の thread が同時に初めて呼んでも、同じ値を atomic に
     * 書くだけにする (半分だけ書かれた値を別の thread が読まない)。 */
    static volatile LONG64 cached_frequency;
    LONG64 frequency = InterlockedCompareExchange64(&cached_frequency, 0, 0);
    LARGE_INTEGER now;
    if (!frequency) {
        LARGE_INTEGER queried;
        QueryPerformanceFrequency(&queried);
        frequency = queried.QuadPart;
        InterlockedExchange64(&cached_frequency, frequency);
    }
    QueryPerformanceCounter(&now);
    return (now.QuadPart / frequency) * 1000000000LL +
           (now.QuadPart % frequency) * 1000000000LL / frequency;
}

void mvm_mlt_export_observe(MvmExportObserver* observer, int point, long long frame) {
    if (!observer || point < 0 || point >= MVM_EXPORT_OBSERVE_POINT_COUNT)
        return;
    MvmExportObservePointState* state = &observer->points[point];
    const long long now = mvm_mlt_export_observer_now_ns();
    const long thread = (long)GetCurrentThreadId();
    const long long ordinal = InterlockedIncrement64(&state->count);
    if (ordinal == 1) {
        InterlockedExchange64(&state->first_ns, now);
        InterlockedExchange64(&state->first_frame, frame);
        InterlockedExchange(&state->first_thread, thread);
    }
    InterlockedExchange64(&state->last_ns, now);
    InterlockedExchange64(&state->last_frame, frame);
    InterlockedExchange(&state->last_thread, thread);
    /* 抜き取りは最初の 4 回と 2 の冪の回だけ。回数が増えても記録は対数でしか増えない。 */
    if (ordinal <= 4 || (ordinal & (ordinal - 1)) == 0) {
        const long slot = InterlockedIncrement(&observer->sample_count) - 1;
        if (slot >= 0 && slot < MVM_EXPORT_OBSERVE_SAMPLE_CAPACITY) {
            MvmExportObserveSample* sample = &observer->samples[slot];
            sample->point = point;
            sample->stage = (int)observer->stage;
            sample->cancel_requested = (int)observer->cancel_requested;
            sample->thread = thread;
            sample->ordinal = ordinal;
            sample->frame = frame;
            sample->ns = now;
            /* 中身を書き終えてから公開する。sample_count は「予約した数」なので、読む側は
             * ready の立っていない枠を読まない。 */
            InterlockedExchange(&sample->ready, 1);
        }
    }
}

static void observe_stage(const MvmExportSpec* spec, int stage) {
    if (!spec || !spec->observer)
        return;
    InterlockedExchange64(&spec->observer->stage_ns[stage], mvm_mlt_export_observer_now_ns());
    InterlockedExchange(&spec->observer->stage, stage);
}

typedef struct {
    const MvmExportSpec* spec;
    volatile LONG rejected;
    volatile LONG observed;
    /* 観測時だけ差し替える、最終 producer の元の get_frame。 */
    int (*original_get_frame)(mlt_service, mlt_frame_ptr, int);
} ExportRgbaAudit;

/* 最終 producer に付ける。Graph の staging や preview の画素で合成を追認しない。 */
static int export_rgba_image(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width,
                             int* height, int writable) {
    ExportRgbaAudit* audit = mlt_frame_pop_service(frame);
    MvmExportObserver* observer = audit->spec->observer;
    const long long position = (long long)mlt_frame_get_position(frame);
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_IMAGE_REQUEST, position);
    if (!audit->spec->rgba_callback) {
        /* 観測だけのとき。consumer が要求した形式を変えない。 */
        const long long began = observer ? mvm_mlt_export_observer_now_ns() : 0;
        const int passed = mlt_frame_get_image(frame, image, format, width, height, writable);
        if (observer)
            InterlockedExchangeAdd64(&observer->image_ns_total,
                                     mvm_mlt_export_observer_now_ns() - began);
        mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_IMAGE_RETURN, position);
        return passed;
    }
    const mlt_image_format requested = *format;
    *format = mlt_image_rgba;
    const long long began = observer ? mvm_mlt_export_observer_now_ns() : 0;
    int status = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (observer)
        InterlockedExchangeAdd64(&observer->image_ns_total,
                                 mvm_mlt_export_observer_now_ns() - began);
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_IMAGE_RETURN, position);
    if (status || !*image || *format != mlt_image_rgba || *width != audit->spec->width ||
        *height != audit->spec->height) {
        InterlockedExchange(&audit->rejected, 1);
        return status ? status : 1;
    }
    InterlockedIncrement(&audit->observed);
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_VALIDATOR_ENTRY, position);
    const int invalid =
        audit->spec->rgba_callback(position, *image, *width, *height, audit->spec->rgba_opaque);
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_VALIDATOR_RETURN, position);
    if (invalid) {
        InterlockedExchange(&audit->rejected, 1);
        return 1;
    }
    if (requested != mlt_image_none && requested != mlt_image_rgba && frame->convert_image)
        return frame->convert_image(frame, image, format, requested);
    return 0;
}

static mlt_frame export_rgba_process(mlt_filter filter, mlt_frame frame) {
    mlt_frame_push_service(frame, filter->child);
    mlt_frame_push_get_image(frame, export_rgba_image);
    return frame;
}

#define MVM_EXPORT_AUDIT_PROPERTY "_mvm_export_audit"

/* 観測時だけ、最終 producer の get_frame の前後を数える。処理は元の関数へそのまま渡す。 */
static int observed_get_frame(mlt_service service, mlt_frame_ptr frame, int index) {
    ExportRgbaAudit* audit =
        mlt_properties_get_data(MLT_SERVICE_PROPERTIES(service), MVM_EXPORT_AUDIT_PROPERTY, NULL);
    MvmExportObserver* observer = audit->spec->observer;
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_FRAME_REQUEST,
                           (long long)mlt_producer_position((mlt_producer)service));
    const long long began = mvm_mlt_export_observer_now_ns();
    const int status = audit->original_get_frame(service, frame, index);
    InterlockedExchangeAdd64(&observer->frame_ns_total, mvm_mlt_export_observer_now_ns() - began);
    mvm_mlt_export_observe(observer, MVM_EXPORT_OBSERVE_FRAME_RETURN,
                           frame && *frame ? (long long)mlt_frame_get_position(*frame) : -1);
    return status;
}

static int attach_rgba_audit(mlt_producer output, ExportRgbaAudit* audit) {
    if (!audit->spec->rgba_callback && !audit->spec->observer)
        return 0;
    mlt_filter filter = mlt_filter_new();
    if (!filter)
        return 1;
    filter->child = audit;
    filter->process = export_rgba_process;
    const int status = mlt_service_attach(MLT_PRODUCER_SERVICE(output), filter);
    mlt_filter_close(filter);
    if (status != 0 || !audit->spec->observer)
        return status;
    mlt_service service = MLT_PRODUCER_SERVICE(output);
    if (mlt_properties_set_data(MLT_SERVICE_PROPERTIES(service), MVM_EXPORT_AUDIT_PROPERTY, audit,
                                0, NULL, NULL) != 0)
        return 1;
    audit->original_get_frame = service->get_frame;
    service->get_frame = observed_get_frame;
    return 0;
}

/* audit は呼び出し側の stack にある。producer を閉じる前に、差し替えた get_frame を戻す。 */
static void detach_frame_observer(mlt_producer output, ExportRgbaAudit* audit) {
    if (!output || !audit->original_get_frame)
        return;
    MLT_PRODUCER_SERVICE(output)->get_frame = audit->original_get_frame;
    audit->original_get_frame = NULL;
}

static void on_consumer_frame_show(mlt_properties owner, void* self, mlt_event_data data) {
    (void)owner;
    mlt_frame frame = mlt_event_data_to_frame(data);
    mvm_mlt_export_observe((MvmExportObserver*)self, MVM_EXPORT_OBSERVE_ENCODER_SUBMIT,
                           frame ? (long long)mlt_frame_get_position(frame) : -1);
}

static void observe_consumer(mlt_consumer consumer, const MvmExportSpec* spec) {
    if (spec->observer)
        mlt_events_listen(MLT_CONSUMER_PROPERTIES(consumer), spec->observer, "consumer-frame-show",
                          (mlt_listener)on_consumer_frame_show);
}

static void observe_poll(const MvmExportSpec* spec, mlt_consumer consumer) {
    if (!spec->observer)
        return;
    InterlockedExchange64(&spec->observer->consumer_position,
                          (long long)mlt_consumer_position(consumer));
    InterlockedExchange(&spec->observer->consumer_stopped, mlt_consumer_is_stopped(consumer));
    InterlockedIncrement64(&spec->observer->poll_count);
}

static int export_cancel_requested(const MvmExportSpec* spec, mlt_consumer consumer,
                                   long long total) {
    if (!spec->progress_callback)
        return 0;
    long long completed = consumer ? (long long)mlt_consumer_position(consumer) + 1 : 0;
    if (completed < 0)
        completed = 0;
    if (completed > total)
        completed = total;
    const int cancel = spec->progress_callback(completed, total, spec->progress_opaque) != 0;
    if (cancel && spec->observer)
        InterlockedExchange(&spec->observer->cancel_requested, 1);
    return cancel;
}

static int export_prepare_cancel_requested(const MvmExportSpec* spec, int* status, long long total,
                                           char* err, size_t err_size) {
    if (!export_cancel_requested(spec, NULL, total))
        return 0;
    *status = MVM_EXPORT_CANCELLED;
    set_err(err, err_size, "書き出し準備をキャンセルしました");
    return 1;
}

/* 音声出力の authority は timeline 上の独立 audio clip だけである。video producer が
 * 内蔵する音声は、linked audio clip を削除した timeline で復活させてはならないため、
 * audio track を持たない書き出しは常に an=1 にする。 */
static void configure_mp4_consumer(mlt_consumer consumer, const char* out_path,
                                   const MvmExportSpec* spec, int with_audio) {
    mlt_properties cp = MLT_CONSUMER_PROPERTIES(consumer);
    mlt_properties_set(cp, "target", out_path);
    mlt_properties_set(cp, "f", spec->lossless_audio ? "matroska" : "mp4");
    mlt_properties_set(cp, "vcodec", "libx264");
    mlt_properties_set(cp, "preset", "medium");
    mlt_properties_set_int(cp, "crf", spec->video_crf);
    mlt_properties_set(cp, "pix_fmt", "yuv420p");
    if (!spec->lossless_audio)
        mlt_properties_set(cp, "movflags", "+faststart");
    if (with_audio) {
        mlt_properties_set(cp, "acodec", spec->lossless_audio ? "pcm_f32le" : "aac");
        if (spec->lossless_audio) {
            mlt_properties_set(cp, "sample_fmt", "flt");
            mlt_properties_set(cp, "mlt_audio_format", "f32le");
        }
        mlt_properties_set(cp, "ab", "192k");
        mlt_properties_set_int(cp, "ar", 48000);
    } else {
        mlt_properties_set_int(cp, "an", 1);
    }
    mlt_properties_set_int(cp, "real_time", -spec->render_threads);
    mlt_properties_set_int(cp, "threads", spec->encoder_threads);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);
    /* 診断時だけ、muxer に packet ごとの時刻の debug 記録を出させる (FF_FDEBUG_TS)。 */
    if (spec->observer && spec->observer->mux_trace)
        mlt_properties_set(cp, "fdebug", "ts");
}

/* 指定した service が repository に登録されているか。
 * 無ければ別の方法へ落とさず失敗させるために使う。 */
static int service_exists(mlt_properties list, const char* name) {
    if (!list || !name)
        return 0;
    int count = mlt_properties_count(list);
    for (int i = 0; i < count; i++) {
        const char* got = mlt_properties_get_name(list, i);
        if (got && strcmp(got, name) == 0)
            return 1;
    }
    return 0;
}

/* clip の producer を開く。等速なら素材のパス、それ以外は "timewarp:<speed>:<path>" を
 * loader へ渡す。service 名 "timewarp" を直接指定すると loader が付ける音声の正規化
 * (resample) が付かない。timewarp は audio の sample rate を変えて速度を表すため、正規化が
 * 無いと tractor の mix で伸縮されずに元の速さで鳴る (§16.8 で実測)。 */
/* loader へ渡す resource。等速なら素材のパス、それ以外は "timewarp:<speed>:<path>"。
 * 呼び出し側が free する。速度が不正なら NULL。 */
static char* clip_resource(const char* path, long long speed_num, long long speed_den) {
    if (speed_num <= 0 || speed_den <= 0)
        return NULL;
    size_t size = strlen(path) + 64;
    char* resource = (char*)malloc(size);
    if (!resource)
        return NULL;
    if (speed_num == speed_den)
        snprintf(resource, size, "%s", path);
    else
        snprintf(resource, size, "timewarp:%.17g:%s", (double)speed_num / (double)speed_den, path);
    return resource;
}

static mlt_producer open_clip_producer(mlt_profile profile, const MvmExportClip* clip) {
    if (clip->is_frame_hold) {
        /* hold は resource を loader で開く。保持元 clip と同じ速度の timewarp にすると、
         * hold_position は元 clip と同じ実効 fps の位置になる (高 fps 素材の slow motion で、
         * 素材 fps のままでは timeline へ出せない frame も保持できる)。 */
        char* resource = clip_resource(clip->path, clip->hold_speed_num, clip->hold_speed_den);
        if (!resource)
            return NULL;
        mlt_producer producer = mlt_factory_producer(profile, "hold", resource);
        free(resource);
        if (!producer)
            return NULL;
        mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
        mlt_properties_set_int64(properties, "frame", clip->hold_position);
        mlt_properties_set_int64(properties, "length", clip->timeline_duration_frames);
        if (mlt_producer_set_in_and_out(producer, 0,
                                        (mlt_position)(clip->timeline_duration_frames - 1)) != 0) {
            mlt_producer_close(producer);
            return NULL;
        }
        if (mlt_properties_get_int64(properties, "frame") != clip->hold_position ||
            mlt_properties_get_int64(properties, "length") != clip->timeline_duration_frames ||
            mlt_producer_get_playtime(producer) != clip->timeline_duration_frames) {
            mlt_producer_close(producer);
            return NULL;
        }
        return producer;
    }
    if (clip->is_still_image && clip->is_image_sequence) {
        mlt_producer producer = mlt_factory_producer(profile, "qimage", clip->path);
        if (!producer)
            return NULL;
        /* 既定は 1 枚を 25 frame 出す。1 枚 1 frame を明示して読み戻す (位置 n が n 番の PNG)。 */
        mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
        mlt_properties_set_int(properties, "ttl", 1);
        if (mlt_properties_get_int(properties, "ttl") != 1 ||
            mlt_producer_get_playtime(producer) < clip->producer_out_frame) {
            mlt_producer_close(producer);
            return NULL;
        }
        return producer;
    }
    if (clip->is_image_sequence)
        return NULL;
    if (clip->is_still_image)
        return mlt_factory_producer(profile, "qimage", clip->path);
    if (clip->speed_num == 1 && clip->speed_den == 1)
        return mlt_factory_producer(profile, NULL, clip->path);
    char* resource = clip_resource(clip->path, clip->speed_num, clip->speed_den);
    if (!resource)
        return NULL;
    mlt_producer producer = mlt_factory_producer(profile, NULL, resource);
    free(resource);
    if (!producer)
        return NULL;
    /* 既定値は 0 だが、音程の扱いは仕様なので明示して読み戻す。 */
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set_int(properties, "warp_pitch", clip->preserve_pitch ? 1 : 0);
    const double speed = mlt_properties_get_double(properties, "warp_speed");
    const double wanted = (double)clip->speed_num / (double)clip->speed_den;
    if (mlt_properties_get_int(properties, "warp_pitch") != (clip->preserve_pitch ? 1 : 0) ||
        fabs(speed - wanted) > 1e-9 * wanted) {
        mlt_producer_close(producer);
        return NULL;
    }
    return producer;
}

static int clips_need_timewarp(const MvmExportClip* clips, int clip_count) {
    for (int i = 0; i < clip_count; ++i) {
        if (clips[i].speed_num != 1 || clips[i].speed_den != 1)
            return 1;
        if (clips[i].is_frame_hold && clips[i].hold_speed_num != clips[i].hold_speed_den)
            return 1;
    }
    return 0;
}

/* probe由来の素材末尾はtimebase変換後にproducer実尺より1 frameだけ長くなることがある。
 * 任意の範囲超過は隠さず、素材末尾を選んだ場合の+1だけを実測playtimeへ合わせる。 */
static int clamp_terminal_rounding(const MvmExportClip* clip, long long playtime,
                                   long long* producer_out) {
    if (*producer_out <= playtime)
        return 1;
    if (clip->source_out_frame == clip->source_frame_count && *producer_out == playtime + 1) {
        *producer_out = playtime;
        return 1;
    }
    return 0;
}

static int file_exists_utf8(const char* path) {
    if (!path || !*path)
        return 0;
    wchar_t* w = mvm_utf8_to_wide(path);
    if (!w)
        return 0;
    DWORD attr = GetFileAttributesW(w);
    mvm_str_free(w);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* 連番の path ("...%05d.png") の index 番の file 名。"%" が "%05d" の 1 か所以外にもあれば
 * (qimage が別の書式として読むので) 0 を返す。100000 番以降は 6 桁以上になる (qimage と
 * QString::arg の幅指定も同じ名前を作る)。 */
static int sequence_frame_path(const char* pattern, long long index, char* out, size_t size) {
    const char* at = strstr(pattern, "%05d");
    if (!at || strchr(pattern, '%') != at || strchr(at + 1, '%') || index < 0)
        return 0;
    const int written =
        snprintf(out, size, "%.*s%05lld%s", (int)(at - pattern), pattern, index, at + 4);
    return written > 0 && (size_t)written < size;
}

/* clip の素材が実在するか。連番なら cut の最初と最後の PNG を確かめる。 */
static int clip_media_exists(const MvmExportClip* clip) {
    if (!clip->is_image_sequence)
        return file_exists_utf8(clip->path);
    if (!clip->path || !clip->is_still_image)
        return 0;
    char first[4096];
    char last[4096];
    return sequence_frame_path(clip->path, clip->producer_in_frame, first, sizeof first) &&
           file_exists_utf8(first) &&
           sequence_frame_path(clip->path, clip->producer_out_frame - 1, last, sizeof last) &&
           file_exists_utf8(last);
}

static int file_size_utf8(const char* path, unsigned long long* size) {
    wchar_t* w = mvm_utf8_to_wide(path);
    if (!w)
        return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
    mvm_str_free(w);
    if (!ok)
        return 0;
    *size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return 1;
}

/* --- animation の keyframe を値の変わる所だけにする ------------------------------------
 * 呼び出し側は clip の全 frame の値を渡す。そのまま全 frame を keyframe にすると、MLT 7.36.1 の
 * affine は毎 frame、animation 全体を文字列へ直す (transition_affine.c が "rect" を
 * mlt_properties_get で読むたびに mlt_animation_serialize が走る。keyframe 数の 2 乗の手間で、
 * transition の lock の中なので render worker も直列になる)。16 layer・1920 frame の clip で
 * 1 frame の合成に 2.4 秒かかった (docs/math-semantic-layout.md)。
 * 前後と同じ値の keyframe は置かない。MLT の線形補間は y1 + (y2 - y1) * t なので、両端が同じ値の
 * 区間はどの frame でも同じ値を返し、keyframe の位置ではその値をそのまま返す。値が変わる区間は
 * 全 frame を残すので、どの frame で評価しても間引く前と同じ値になる。 */
static int same_rect(mlt_rect a, mlt_rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && a.o == b.o;
}

static mlt_rect crop_rect_at(const MvmExportClip* clip, int index, double width, double height) {
    const MvmExportMotionFrame* frame = &clip->motion_frames[index];
    mlt_rect rect = {frame->crop_left, frame->crop_top,
                     width - frame->crop_left - frame->crop_right,
                     height - frame->crop_top - frame->crop_bottom, 1};
    return rect;
}

/* 配置の矩形と不透明度。モーションがあれば、その frame の矩形を使う。 */
static mlt_rect placement_rect_at(const MvmExportClip* clip, int index) {
    const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[index];
    mlt_rect rect = {clip->rect_x, clip->rect_y, clip->rect_width, clip->rect_height, key->opacity};
    if (clip->motion_frame_count > 0) {
        const MvmExportMotionFrame* frame = &clip->motion_frames[key->local_frame];
        rect.x = frame->rect_x;
        rect.y = frame->rect_y;
        rect.w = frame->rect_width;
        rect.h = frame->rect_height;
    }
    /* 切り出した素材 (試作)。等倍だけを受け付けるので、矩形は切り出した範囲そのものになる。 */
    if (clip->source_width > 0) {
        rect.x += clip->source_x;
        rect.y += clip->source_y;
        rect.w = clip->source_width;
        rect.h = clip->source_height;
    }
    return rect;
}

/* 切り出した素材を置けるのは、矩形が出力全体と同じ大きさ (等倍) で、回転・shear・crop が無い
 * 上位 track の静止画だけ。拡縮があると MLT は素材を矩形の大きさへ縮拡してから置くので、
 * 切り出した画像では結果が変わる。 */
static int source_region_valid(const MvmExportClip* clip, const MvmExportSpec* spec) {
    if (clip->source_width == 0 && clip->source_height == 0 && clip->source_x == 0 &&
        clip->source_y == 0)
        return 1;
    if (clip->source_width <= 0 || clip->source_height <= 0 || clip->source_x < 0 ||
        clip->source_y < 0 || clip->source_x > spec->width - clip->source_width ||
        clip->source_y > spec->height - clip->source_height || clip->is_audio ||
        clip->video_track <= 0 || !clip->is_still_image || clip->is_image_sequence ||
        clip->opaque_backdrop || clip->rect_width != spec->width ||
        clip->rect_height != spec->height || clip->rotation_degrees != 0.0 ||
        clip->shear_degrees != 0.0 || clip->crop_left != 0 || clip->crop_top != 0 ||
        clip->crop_right != 0 || clip->crop_bottom != 0)
        return 0;
    for (int index = 0; index < clip->motion_frame_count; ++index) {
        const MvmExportMotionFrame* frame = &clip->motion_frames[index];
        if (frame->rect_width != spec->width || frame->rect_height != spec->height ||
            frame->rotation_degrees != 0.0 || frame->shear_degrees != 0.0 ||
            frame->crop_left != 0.0 || frame->crop_top != 0.0 || frame->crop_right != 0.0 ||
            frame->crop_bottom != 0.0)
            return 0;
    }
    return 1;
}

/* 診断用: 落とせる keyframe の一部をわざと残す (値は変わらない)。製品では常に 0。 */
static int diagnostic_stride(const MvmExportSpec* spec) {
    return spec && spec->observer && spec->observer->diagnostic_keyframe_stride > 0
               ? spec->observer->diagnostic_keyframe_stride
               : 0;
}

static int crop_key_redundant(const MvmExportClip* clip, int index, double width, double height,
                              int stride) {
    if (index <= 0 || index >= clip->motion_frame_count - 1 || (stride > 0 && index % stride == 0))
        return 0;
    const mlt_rect current = crop_rect_at(clip, index, width, height);
    return same_rect(crop_rect_at(clip, index - 1, width, height), current) &&
           same_rect(current, crop_rect_at(clip, index + 1, width, height));
}

static int placement_key_redundant(const MvmExportClip* clip, int index, int stride) {
    if (index <= 0 || index >= clip->opacity_keyframe_count - 1 ||
        (stride > 0 && index % stride == 0))
        return 0;
    const mlt_rect current = placement_rect_at(clip, index);
    return same_rect(placement_rect_at(clip, index - 1), current) &&
           same_rect(current, placement_rect_at(clip, index + 1));
}

/* 回転と shear。モーションのある clip だけが呼ぶ (opacity key の local_frame は index と同じ)。 */
static int rotation_key_redundant(const MvmExportClip* clip, int index, int stride) {
    if (index <= 0 || index >= clip->opacity_keyframe_count - 1 ||
        (stride > 0 && index % stride == 0))
        return 0;
    const MvmExportMotionFrame* frames = clip->motion_frames;
    const long long at = clip->opacity_keyframes[index].local_frame;
    const long long before = clip->opacity_keyframes[index - 1].local_frame;
    const long long after = clip->opacity_keyframes[index + 1].local_frame;
    return frames[before].rotation_degrees == frames[at].rotation_degrees &&
           frames[at].rotation_degrees == frames[after].rotation_degrees &&
           frames[before].shear_degrees == frames[at].shear_degrees &&
           frames[at].shear_degrees == frames[after].shear_degrees;
}

static void observe_animation(const MvmExportSpec* spec, long long samples, long long keyframes) {
    if (!spec || !spec->observer)
        return;
    InterlockedExchangeAdd64(&spec->observer->animation_samples, samples);
    InterlockedExchangeAdd64(&spec->observer->animation_keyframes, keyframes);
}

/* crop は qtcrop で「範囲の外を透明で塗る」形で掛け、frame の寸法を変えない。
 * 以前の crop filter は frame の寸法そのものを変えていた。その後の affine は正方形でない
 * 座標系で回転するため、crop と回転を組み合わせると平行四辺形 (shear) になっていた
 * (docs/premiere-like-editing.md §18.6)。寸法を保てば affine の座標系は出力と同じになる。
 * 配置の矩形 (rect_*) は呼び出し側が crop 前の全体の位置として渡す。 */
static int attach_export_crop(mlt_profile profile, mlt_producer cut, const MvmExportClip* clip,
                              long long filter_in, char* err, size_t err_size,
                              const MvmExportSpec* spec, int* cancel_status) {
    if (clip->crop_left == 0 && clip->crop_top == 0 && clip->crop_right == 0 &&
        clip->crop_bottom == 0 && clip->motion_frame_count == 0)
        return 0;
    if (!profile || profile->width <= 0 || profile->height <= 0) {
        set_err(err, err_size, "crop の基準になる profile の寸法が不正です");
        return 1;
    }
    mlt_filter filter = mlt_factory_filter(profile, "qtcrop", NULL);
    if (!filter) {
        set_err(err, err_size, "必須filter 'qtcrop'を作れません");
        return 1;
    }
    /* frame に対する割合で指定する。crop の px は profile (= 出力) の寸法を基準にしている。 */
    const double width = (double)profile->width;
    const double height = (double)profile->height;
    char rect[160];
    snprintf(rect, sizeof(rect), "%.6f%%/%.6f%%:%.6f%%x%.6f%%", clip->crop_left * 100.0 / width,
             clip->crop_top * 100.0 / height,
             (width - clip->crop_left - clip->crop_right) * 100.0 / width,
             (height - clip->crop_top - clip->crop_bottom) * 100.0 / height);
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set(props, "rect", rect);
    long long crop_keyframes = 0;
    for (int index = 0; index < clip->motion_frame_count; ++index) {
        if (index % 256 == 0 &&
            export_prepare_cancel_requested(spec, cancel_status, clip->timeline_duration_frames,
                                            err, err_size)) {
            mlt_filter_close(filter);
            return 1;
        }
        if (crop_key_redundant(clip, index, width, height, diagnostic_stride(spec)))
            continue;
        ++crop_keyframes;
        const MvmExportMotionFrame* frame = &clip->motion_frames[index];
        const mlt_rect animated = crop_rect_at(clip, index, width, height);
        if (mlt_properties_anim_set_rect(props, "rect", animated, (mlt_position)frame->local_frame,
                                         (mlt_position)clip->timeline_duration_frames,
                                         mlt_keyframe_linear) != 0) {
            mlt_filter_close(filter);
            set_err(err, err_size, "クロップのモーションを設定できません");
            return 1;
        }
    }
    observe_animation(spec, clip->motion_frame_count, crop_keyframes);
    mlt_properties_set(props, "color", "#00000000");
    mlt_filter_set_in_and_out(filter, (mlt_position)filter_in,
                              (mlt_position)(filter_in + clip->timeline_duration_frames - 1));
    mlt_properties_set_int(props, "circle", 0);
    mlt_properties_set_double(props, "radius", 0.0);
    if (mlt_producer_attach(cut, filter) != 0) {
        mlt_filter_close(filter);
        set_err(err, err_size, "qtcrop filterをclipへattachできません");
        return 1;
    }
    mlt_filter_close(filter);
    return 0;
}

static int attach_export_affine(mlt_profile profile, mlt_producer cut, const MvmExportClip* clip,
                                long long producer_in, long long duration, char* err,
                                size_t err_size, const MvmExportSpec* spec, int* cancel_status) {
    mlt_filter filter = mlt_factory_filter(profile, "affine", NULL);
    if (!filter) {
        set_err(err, err_size, "必須filter 'affine'を作れません");
        return 1;
    }
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set(props, "background", "colour:#000000");
    mlt_properties_set_int(props, "transition.fill", 1);
    mlt_properties_set_int(props, "transition.distort", 1);
    mlt_properties_set_int(props, "transition.b_alpha", 0);
    mlt_properties_set_int(props, "transition.repeat_off", 1);
    mlt_properties_set_int(props, "transition.mirror_off", 1);
    mlt_properties_set(props, "transition.halign", "center");
    mlt_properties_set(props, "transition.valign", "middle");
    /* 画面内の 2D 回転は keyed=0 の fix_rotate_x が担う (docs/m7b-p0-findings.md の実画素結果)。
     * fix_rotate_z は MLT 7.36.1 では実画素に現れず、V1 の回転が書き出されていなかった。 */
    mlt_properties_set_int(props, "transition.keyed", 0);
    mlt_properties_set_double(props, "transition.fix_rotate_x", clip->rotation_degrees);
    mlt_properties_set_double(props, "transition.fix_shear_x", clip->shear_degrees);
    mlt_filter_set_in_and_out(filter, (mlt_position)producer_in,
                              (mlt_position)(producer_in + duration - 1));
    long long keyframes = 0;
    for (int i = 0; i < clip->opacity_keyframe_count; ++i) {
        if (i % 256 == 0 &&
            export_prepare_cancel_requested(spec, cancel_status, clip->timeline_duration_frames,
                                            err, err_size)) {
            mlt_filter_close(filter);
            return 1;
        }
        const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[i];
        if (clip->motion_frame_count > 0 &&
            !rotation_key_redundant(clip, i, diagnostic_stride(spec))) {
            const MvmExportMotionFrame* frame = &clip->motion_frames[key->local_frame];
            keyframes += 2;
            if (mlt_properties_anim_set_double(
                    props, "transition.fix_rotate_x", frame->rotation_degrees,
                    (mlt_position)key->local_frame, (mlt_position)clip->timeline_duration_frames,
                    mlt_keyframe_linear) != 0 ||
                mlt_properties_anim_set_double(props, "transition.fix_shear_x",
                                               frame->shear_degrees, (mlt_position)key->local_frame,
                                               (mlt_position)clip->timeline_duration_frames,
                                               mlt_keyframe_linear) != 0) {
                mlt_filter_close(filter);
                set_err(err, err_size, "回転モーションを設定できません");
                return 1;
            }
        }
        if (placement_key_redundant(clip, i, diagnostic_stride(spec)))
            continue;
        ++keyframes;
        if (mlt_properties_anim_set_rect(props, "transition.rect", placement_rect_at(clip, i),
                                         (mlt_position)key->local_frame, (mlt_position)duration,
                                         mlt_keyframe_linear) != 0) {
            mlt_filter_close(filter);
            set_err(err, err_size, "affine opacity animationを設定できません");
            return 1;
        }
    }
    observe_animation(
        spec, (long long)clip->opacity_keyframe_count * (clip->motion_frame_count > 0 ? 3 : 1),
        keyframes);
    if (mlt_producer_attach(cut, filter) != 0) {
        mlt_filter_close(filter);
        set_err(err, err_size, "affine filterをclipへattachできません");
        return 1;
    }
    mlt_filter_close(filter);
    return 0;
}

/* クロスディゾルブの incoming を、素材の余白を不透明な黒で埋めた出力全体の 1 枚にする。
 * 黒背景の affine filter で素材を縦横比を保った矩形へ置く (上の layer へ重ねる affine transition
 * が出力全体と不透明度を掛ける)。余白が透明のままだと、incoming を不透明度 p で重ねても余白の
 * 所で outgoing が 100% 見え続け、区間の終わりで突然消える。
 * 矩形は素材の寸法からここで求めて distort=1 で置く。distort=0 で MLT に縦横比を任せると、
 * 上の transition が frame に設定する distort に左右されて引き伸ばされる・縮むことがあった。 */
static int attach_export_backdrop(mlt_profile profile, mlt_producer cut, const MvmExportClip* clip,
                                  long long producer_in, long long duration, char* err,
                                  size_t err_size) {
    /* 書き出しの producer は loader で開くので meta.media.* を持たない。素材を probe する。 */
    MvmMltProbeResult media;
    memset(&media, 0, sizeof(media));
    if (mvm_mlt_probe_file(clip->path, &media) != 0 || media.width <= 0 || media.height <= 0 ||
        profile->width <= 0 || profile->height <= 0) {
        set_err(err, err_size, "トランジションの incoming の素材寸法が分かりません: %s",
                media.error);
        return 1;
    }
    const int media_width = media.width;
    const int media_height = media.height;
    int sar_num = media.sar_num;
    int sar_den = media.sar_den;
    if (sar_num <= 0 || sar_den <= 0)
        sar_num = sar_den = 1;
    /* 表示上の縦横比 (画素の縦横比を含む) を出力の枠へ収める。 */
    const double display_width = (double)media_width * sar_num / sar_den;
    const double scale = fmin((double)profile->width / display_width,
                              (double)profile->height / (double)media_height);
    const double rect_width = display_width * scale;
    const double rect_height = (double)media_height * scale;
    mlt_filter filter = mlt_factory_filter(profile, "affine", NULL);
    if (!filter) {
        set_err(err, err_size, "必須filter 'affine'を作れません");
        return 1;
    }
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    mlt_properties_set(props, "background", "colour:#000000");
    mlt_properties_set_int(props, "transition.fill", 1);
    mlt_properties_set_int(props, "transition.distort", 1);
    mlt_properties_set_int(props, "transition.b_alpha", 0);
    mlt_properties_set_int(props, "transition.repeat_off", 1);
    mlt_properties_set_int(props, "transition.mirror_off", 1);
    mlt_properties_set(props, "transition.halign", "center");
    mlt_properties_set(props, "transition.valign", "middle");
    mlt_properties_set_int(props, "transition.keyed", 0);
    mlt_rect rect = {((double)profile->width - rect_width) * 0.5,
                     ((double)profile->height - rect_height) * 0.5, rect_width, rect_height, 1.0};
    mlt_properties_set_rect(props, "transition.rect", rect);
    mlt_filter_set_in_and_out(filter, (mlt_position)producer_in,
                              (mlt_position)(producer_in + duration - 1));
    if (mlt_producer_attach(cut, filter) != 0) {
        mlt_filter_close(filter);
        set_err(err, err_size, "backdrop affine filterをclipへattachできません");
        return 1;
    }
    mlt_filter_close(filter);
    return 0;
}

/* clip の keyframe [first, last] を 1 つの affine transition として植える。通常は clip 全体
 * (first = 0、last = 末尾)。試作の分割 (下の plant_export_overlay_affine) は区間ごとに呼ぶ。
 * transition の in は first の frame になり、keyframe の位置は first からの相対になる。 */
static int plant_export_overlay_segment(mlt_profile profile, mlt_tractor tractor,
                                        const MvmExportClip* clip, int first, int last, char* err,
                                        size_t err_size, const MvmExportSpec* spec,
                                        int* cancel_status) {
    const long long origin = clip->opacity_keyframes[first].local_frame;
    const long long span = clip->opacity_keyframes[last].local_frame - origin + 1;
    /* 分割していないときの animation の長さは従来どおり clip の尺にする。 */
    const long long length = first == 0 && last == clip->opacity_keyframe_count - 1
                                 ? clip->timeline_duration_frames
                                 : span;
    /* 診断用の故障注入 (比較が効いていることの対照。製品では常に 0)。 */
    const int fault = spec->observer ? spec->observer->diagnostic_split_fault : 0;
    mlt_transition transition = mlt_factory_transition(profile, "affine", NULL);
    if (!transition) {
        set_err(err, err_size, "必須transition 'affine'を作れません");
        return 1;
    }
    mlt_properties props = MLT_TRANSITION_PROPERTIES(transition);
    mlt_properties_set_int(props, "fill", 1);
    mlt_properties_set_int(props, "distort", 1);
    mlt_properties_set_int(props, "b_alpha", 0);
    mlt_properties_set_int(props, "repeat_off", 1);
    mlt_properties_set_int(props, "mirror_off", 1);
    mlt_properties_set_int(props, "keyed", 0);
    mlt_properties_set(props, "halign", "center");
    mlt_properties_set(props, "valign", "middle");
    /* M7b-P0実画素結果がauthority。名前からfix_rotate_zへ置換しない。 */
    mlt_properties_set_double(props, "fix_rotate_x", clip->rotation_degrees);
    mlt_properties_set_double(props, "fix_shear_x", clip->shear_degrees);
    if (first == 0 && last == clip->opacity_keyframe_count - 1) {
        /* 製品の経路 (分割なし)。区間は timeline の絶対位置で、clip の尺そのもの。 */
        mlt_transition_set_in_and_out(
            transition, (mlt_position)clip->timeline_start_frame,
            (mlt_position)(clip->timeline_start_frame + clip->timeline_duration_frames - 1));
    } else {
        mlt_transition_set_in_and_out(
            transition, (mlt_position)(clip->timeline_start_frame + origin),
            (mlt_position)(clip->timeline_start_frame + origin + span - 1));
    }
    long long keyframes = 0;
    for (int index = first; index <= last; ++index) {
        if (index % 256 == 0 &&
            export_prepare_cancel_requested(spec, cancel_status, clip->timeline_duration_frames,
                                            err, err_size)) {
            mlt_transition_close(transition);
            return 1;
        }
        const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[index];
        /* 区間の両端は必ず置く (隣の区間の keyframe には頼らない)。 */
        const int edge = index == first || index == last;
        long long position = key->local_frame - origin;
        if (fault == 1 && position + 1 < span)
            ++position; /* 故障: keyframe の位置を 1 frame ずらす */
        if (fault == 2 && !edge && (index - first) % 2 == 1)
            continue; /* 故障: 値の変わる区間の keyframe を 1 つおきに落とす */
        if (clip->motion_frame_count > 0 &&
            (edge || !rotation_key_redundant(clip, index, diagnostic_stride(spec)))) {
            const MvmExportMotionFrame* frame = &clip->motion_frames[key->local_frame];
            keyframes += 2;
            if (mlt_properties_anim_set_double(props, "fix_rotate_x", frame->rotation_degrees,
                                               (mlt_position)position, (mlt_position)length,
                                               mlt_keyframe_linear) != 0 ||
                mlt_properties_anim_set_double(props, "fix_shear_x", frame->shear_degrees,
                                               (mlt_position)position, (mlt_position)length,
                                               mlt_keyframe_linear) != 0) {
                mlt_transition_close(transition);
                set_err(err, err_size, "回転モーションを設定できません");
                return 1;
            }
        }
        if (!edge && placement_key_redundant(clip, index, diagnostic_stride(spec)))
            continue;
        ++keyframes;
        mlt_rect rect = placement_rect_at(clip, index);
        if (fault == 4 && index + 1 < clip->opacity_keyframe_count)
            rect = placement_rect_at(clip, index + 1); /* 故障: 1 つ後の frame の値を置く */
        if (fault == 3)
            rect.o *= 0.5; /* 故障: 不透明度を半分にする */
        if (mlt_properties_anim_set_rect(props, "rect", rect, (mlt_position)position,
                                         (mlt_position)length, mlt_keyframe_linear) != 0) {
            mlt_transition_close(transition);
            set_err(err, err_size, "上位track affine opacity keyframeを設定できません");
            return 1;
        }
    }
    observe_animation(spec, (long long)(last - first + 1) * (clip->motion_frame_count > 0 ? 3 : 1),
                      keyframes);
    if (spec->observer)
        InterlockedIncrement64(&spec->observer->overlay_transitions);
    mlt_transition_set_tracks(transition, 0, clip->video_track);
    if (mlt_field_plant_transition(mlt_tractor_field(tractor), transition, 0, clip->video_track) !=
        0) {
        mlt_transition_close(transition);
        set_err(err, err_size, "上位track affine transitionをV1との間へ配置できません");
        return 1;
    }
    mlt_transition_close(transition);
    return 0;
}

/* 上位 track の clip の配置と不透明度を affine transition として植える。
 *
 * 製品は clip ごとに 1 つ。試作 (observer の diagnostic_split_transitions、既定 0)
 * は、値の変わらない 区間と変わる区間に分けて植える。MLT の affine は毎 frame、その transition の
 * animation 全体を 文字列へ直すので、1 つの transition が持つ keyframe
 * を「いま通っている区間」の分だけにする (docs/math-export-p2a.md)。どの frame にもその frame の
 * keyframe があるか、値の変わらない区間の 中にあるので、評価される値は分けない場合と同じになる。
 *   - 同じ値が kStaticRun frame 以上続く所を「変わらない区間」にする (keyframe は両端の 2 個)。
 *   - 残りを「変わる区間」として、kAnimatedRun frame までの長さで区切る。 */
static int plant_export_overlay_affine(mlt_profile profile, mlt_tractor tractor,
                                       const MvmExportClip* clip, char* err, size_t err_size,
                                       const MvmExportSpec* spec, int* cancel_status) {
    const int count = clip->opacity_keyframe_count;
    int split = spec->observer && spec->observer->diagnostic_split_transitions;
    const int kStaticRun = split && spec->observer->diagnostic_split_static_run > 1
                               ? spec->observer->diagnostic_split_static_run
                               : 8;
    const int kAnimatedRun = split && spec->observer->diagnostic_split_animated_run >= kStaticRun
                                 ? spec->observer->diagnostic_split_animated_run
                                 : 64;
    const int kStaticChunk = split ? spec->observer->diagnostic_split_static_chunk : 0;
    /* keyframe が全 frame に並んでいる clip だけを分ける (歯抜けなら従来どおり 1 つ)。 */
    for (int index = 0; split && index < count; ++index)
        split = clip->opacity_keyframes[index].local_frame == index;
    if (!split || count < 2)
        return plant_export_overlay_segment(profile, tractor, clip, 0, count - 1, err, err_size,
                                            spec, cancel_status);
    int first = 0;
    while (first < count) {
        /* first から同じ値が続く範囲。 */
        int same = first;
        while (
            same + 1 < count &&
            same_rect(placement_rect_at(clip, same), placement_rect_at(clip, same + 1)) &&
            (clip->motion_frame_count == 0 || (clip->motion_frames[same].rotation_degrees ==
                                                   clip->motion_frames[same + 1].rotation_degrees &&
                                               clip->motion_frames[same].shear_degrees ==
                                                   clip->motion_frames[same + 1].shear_degrees)))
            ++same;
        int last = same;
        if (same - first + 1 >= kStaticRun && kStaticChunk >= kStaticRun &&
            same - first + 1 > kStaticChunk)
            last = first + kStaticChunk - 1; /* 診断用: 変わらない区間をさらに刻む */
        if (same - first + 1 < kStaticRun) {
            /* 変わる区間: 長い「変わらない区間」の手前まで、上限の長さで切る。 */
            last = first;
            int run = 1;
            while (last + 1 < count && last - first + 1 < kAnimatedRun) {
                const int next = last + 1;
                run = same_rect(placement_rect_at(clip, last), placement_rect_at(clip, next)) &&
                              (clip->motion_frame_count == 0 ||
                               (clip->motion_frames[last].rotation_degrees ==
                                    clip->motion_frames[next].rotation_degrees &&
                                clip->motion_frames[last].shear_degrees ==
                                    clip->motion_frames[next].shear_degrees))
                          ? run + 1
                          : 1;
                if (run >= kStaticRun) {
                    /* 直前の kStaticRun - 1 frame は次の「変わらない区間」へ回す。 */
                    last = next - kStaticRun;
                    break;
                }
                last = next;
            }
            if (last < first)
                last = first;
        }
        if (plant_export_overlay_segment(profile, tractor, clip, first, last, err, err_size, spec,
                                         cancel_status) != 0)
            return 1;
        first = last + 1;
    }
    return 0;
}

/* tractor経路でclip単位のcut（本体・末尾補完）へ付けるfilter。
 * filter_inはclip-local frame 0に対応するparent producer位置。 */
static int attach_tractor_clip_filters(mlt_profile profile, mlt_producer cut,
                                       const MvmExportClip* clip, int track, long long filter_in,
                                       char* err, size_t err_size, const MvmExportSpec* spec,
                                       int* cancel_status) {
    if (clip->is_audio) {
        if (clip->mixer_pan != 0.0) {
            // panner は初期値が無いと split の評価へ入らない。初期化後のパン値は split で指定する。
            mlt_filter balance = mlt_factory_filter(profile, "panner", "0.5");
            if (!balance) {
                set_err(err, err_size, "パンフィルターを作成できません");
                return 1;
            }
            mlt_properties props = MLT_FILTER_PROPERTIES(balance);
            mlt_properties_set_int(props, "channel", -1);
            mlt_properties_set_double(props, "split", (clip->mixer_pan + 1.0) * 0.5);
            mlt_filter_set_in_and_out(
                balance, (mlt_position)filter_in,
                (mlt_position)(filter_in + clip->timeline_duration_frames - 1));
            const int attached = mlt_producer_attach(cut, balance);
            mlt_filter_close(balance);
            if (attached != 0) {
                set_err(err, err_size, "パンフィルターを接続できません");
                return 1;
            }
        }
        const double first_gain = clip->gain_keyframes[0].gain;
        int constant = 1;
        for (int index = 1; index < clip->gain_keyframe_count; ++index)
            constant = constant && clip->gain_keyframes[index].gain == first_gain;
        if (constant && first_gain == 1.0)
            return 0;
        mlt_filter filter = mlt_factory_filter(profile, "volume", NULL);
        if (!filter) {
            set_err(err, err_size, "必須filter 'volume'を作れません");
            return 1;
        }
        mlt_filter_set_in_and_out(filter, (mlt_position)filter_in,
                                  (mlt_position)(filter_in + clip->timeline_duration_frames - 1));
        mlt_properties properties = MLT_FILTER_PROPERTIES(filter);
        /* 正規化済みの補正を MLT 既定の +20 dB 上限で黙って切り詰めない。 */
        mlt_properties_set_double(properties, "max_gain", 0.0);
        if (mlt_properties_get_double(properties, "max_gain") != 0.0) {
            mlt_filter_close(filter);
            set_err(err, err_size, "音量フィルターのゲイン上限を解除できません");
            return 1;
        }
        if (constant) {
            mlt_properties_set_double(properties, "gain", first_gain);
            if (fabs(mlt_properties_get_double(properties, "gain") - first_gain) > 1e-9) {
                mlt_filter_close(filter);
                set_err(err, err_size, "audio gainの読み戻しが違います");
                return 1;
            }
        } else {
            for (int index = 0; index < clip->gain_keyframe_count; ++index) {
                const double gain = clip->gain_keyframes[index].gain;
                const double level_db = gain == 0.0 ? -120.0 : 20.0 * log10(gain);
                if (mlt_properties_anim_set_double(
                        properties, "level", level_db, (mlt_position)index,
                        (mlt_position)clip->timeline_duration_frames, mlt_keyframe_discrete) != 0) {
                    mlt_filter_close(filter);
                    set_err(err, err_size, "audio level animationを設定できません");
                    return 1;
                }
                const double observed = mlt_properties_anim_get_double(
                    properties, "level", index, (int)clip->timeline_duration_frames);
                if (!isfinite(observed) || fabs(observed - level_db) > 1e-6) {
                    mlt_filter_close(filter);
                    set_err(err, err_size, "audio level animationの読み戻しが違います");
                    return 1;
                }
            }
        }
        if (mlt_producer_attach(cut, filter) != 0) {
            mlt_filter_close(filter);
            set_err(err, err_size, "audio level filterをattachできません");
            return 1;
        }
        mlt_filter_close(filter);
        return 0;
    }
    if (track == 0 && clip->effects_enabled)
        return attach_export_crop(profile, cut, clip, filter_in, err, err_size, spec,
                                  cancel_status) != 0 ||
               attach_export_affine(profile, cut, clip, filter_in, clip->timeline_duration_frames,
                                    err, err_size, spec, cancel_status) != 0;
    /* 上位映像trackへopaque-black affine filterをattachしない。cropだけをcutへ置く。
     * クロスディゾルブの incoming だけは余白を黒で埋める (attach_export_backdrop)。 */
    if (track > 0)
        return attach_export_crop(profile, cut, clip, filter_in, err, err_size, spec,
                                  cancel_status) != 0 ||
               (clip->opaque_backdrop &&
                attach_export_backdrop(profile, cut, clip, filter_in,
                                       clip->timeline_duration_frames, err, err_size) != 0);
    return 0;
}

static int validate_motion_frames(const MvmExportClip* clips, int count, const MvmExportSpec* spec,
                                  char* err, size_t err_size) {
    for (int index = 0; index < count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (clip->motion_frame_count < 0 ||
            (clip->motion_frame_count > 0 &&
             (!clip->motion_frames || clip->is_audio ||
              clip->motion_frame_count != clip->timeline_duration_frames ||
              clip->opacity_keyframe_count != clip->motion_frame_count ||
              !clip->opacity_keyframes))) {
            set_err(err, err_size, "モーション配列の件数または参照が不正です");
            return 1;
        }
        for (int i = 0; i < clip->motion_frame_count; ++i) {
            const MvmExportMotionFrame* frame = &clip->motion_frames[i];
            if (frame->local_frame != i || clip->opacity_keyframes[i].local_frame != i ||
                !isfinite(frame->crop_left) || !isfinite(frame->crop_top) ||
                !isfinite(frame->crop_right) || !isfinite(frame->crop_bottom) ||
                frame->crop_left < 0 || frame->crop_top < 0 || frame->crop_right < 0 ||
                frame->crop_bottom < 0 || frame->crop_left + frame->crop_right >= spec->width ||
                frame->crop_top + frame->crop_bottom >= spec->height || !isfinite(frame->rect_x) ||
                !isfinite(frame->rect_y) || !isfinite(frame->rect_width) ||
                !isfinite(frame->rect_height) || frame->rect_width <= 0 ||
                frame->rect_height <= 0 || !isfinite(frame->rotation_degrees) ||
                !isfinite(frame->shear_degrees)) {
                set_err(err, err_size, "モーションのフレーム値が不正です");
                return 1;
            }
        }
    }
    return 0;
}

int mvm_mlt_export_sequence(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                            const char* out_path, MvmExportResult* out, char* err,
                            size_t err_size) {
    mlt_profile profile = NULL;
    mlt_playlist playlist = NULL;
    mlt_producer* producers = NULL;
    mlt_producer* cuts = NULL;
    int producer_count = 0;
    int cut_count = 0;
    mlt_consumer consumer = NULL;
    mlt_producer pp = NULL;
    long long total = 0;
    int cancelled = 0;
    ExportRgbaAudit audit = {spec, 0, 0};

    if (!mvm_mlt_runtime_is_ready()) {
        set_err(err, err_size, "MLT が初期化されていません");
        return 1;
    }
    if (!clips || !spec || !out_path || !*out_path) {
        set_err(err, err_size, "引数が不正です");
        return 1;
    }
    if (clip_count <= 0) {
        set_err(err, err_size, "書き出す clip がありません");
        return 1;
    }
    if (spec->width <= 0 || spec->height <= 0 || spec->fps_num <= 0 || spec->fps_den <= 0 ||
        spec->video_crf < 0 || spec->video_crf > 51 || spec->render_threads <= 0 ||
        spec->render_threads > 16 || spec->encoder_threads < 0 || spec->encoder_threads > 16 ||
        spec->lossless_audio < 0 || spec->lossless_audio > 1) {
        set_err(err, err_size, "出力 profile の指定が不正です: %dx%d @ %d/%d", spec->width,
                spec->height, spec->fps_num, spec->fps_den);
        return 1;
    }
    if (validate_motion_frames(clips, clip_count, spec, err, err_size) != 0)
        return 1;
    if (spec->timeout_ms <= 0) {
        set_err(err, err_size, "timeout_ms が 0 以下です: %d", spec->timeout_ms);
        return 1;
    }
    for (int i = 0; i < clip_count; i++) {
        if (!clips[i].path || !clips[i].path[0]) {
            set_err(err, err_size, "clip %d のパスが空です", i);
            return 1;
        }
        if (!clip_media_exists(&clips[i])) {
            set_err(err, err_size, "clip %d のファイルがありません: %s", i, clips[i].path);
            return 1;
        }
        if (clips[i].source_fps_num <= 0 || clips[i].source_fps_den <= 0 ||
            clips[i].source_frame_count <= 0 || clips[i].source_in_frame < 0 ||
            clips[i].source_out_frame <= clips[i].source_in_frame ||
            clips[i].source_out_frame > clips[i].source_frame_count ||
            clips[i].producer_in_frame < 0 ||
            clips[i].producer_out_frame <= clips[i].producer_in_frame || clips[i].speed_num <= 0 ||
            clips[i].speed_den <= 0) {
            set_err(err, err_size, "clip %d の source range または FPS が不正です", i);
            return 1;
        }
        if (clips[i].tail_padding_frames != 0) {
            set_err(err, err_size, "clip %d は末尾の補完が要ります。sequential 経路は扱えません",
                    i);
            return 1;
        }
        if (clips[i].is_audio) {
            set_err(err, err_size, "clip %d は audio clip です。sequential 経路は映像専用です", i);
            return 1;
        }
        if (clips[i].source_width != 0 || clips[i].source_height != 0 || clips[i].source_x != 0 ||
            clips[i].source_y != 0) {
            set_err(err, err_size, "clip %d は切り出した素材です。sequential 経路は扱えません", i);
            return 1;
        }
        if (clips[i].effects_enabled &&
            (clips[i].opacity_keyframe_count <= 0 || !clips[i].opacity_keyframes ||
             clips[i].rect_width <= 0.0 || clips[i].rect_height <= 0.0)) {
            set_err(err, err_size, "clip %d のeffect mappingが不正です", i);
            return 1;
        }
    }

    observe_stage(spec, MVM_EXPORT_STAGE_GRAPH_BUILD);
    /* clip 数に上限を設けない。producer と cut は clip ごとに高々 1 本ずつ。 */
    producers = (mlt_producer*)calloc((size_t)clip_count, sizeof(*producers));
    cuts = (mlt_producer*)calloc((size_t)clip_count, sizeof(*cuts));
    if (!producers || !cuts) {
        set_err(err, err_size, "producer 配列を確保できません");
        goto fail;
    }

    /* --- profile ----------------------------------------------------------
     * mlt_profile_init は解決に失敗しても NULL を返さず既定値へ落ちる。
     * 名前付き profile に頼らず値を直接設定し、設定後に読み直して照合する。 */
    profile = mlt_profile_init(NULL);
    if (!profile) {
        set_err(err, err_size, "profile を作れません");
        goto fail;
    }
    profile->width = spec->width;
    profile->height = spec->height;
    profile->frame_rate_num = spec->fps_num;
    profile->frame_rate_den = spec->fps_den;
    profile->sample_aspect_num = 1;
    profile->sample_aspect_den = 1;
    profile->display_aspect_num = spec->width;
    profile->display_aspect_den = spec->height;
    profile->progressive = 1;
    profile->colorspace = 601;

    if (profile->width != spec->width || profile->height != spec->height ||
        profile->frame_rate_num != spec->fps_num || profile->frame_rate_den != spec->fps_den ||
        profile->sample_aspect_num != 1 || profile->sample_aspect_den != 1 ||
        !profile->progressive) {
        set_err(err, err_size, "profile の実値が要求と一致しません: 実 %dx%d @ %d/%d",
                profile->width, profile->height, profile->frame_rate_num, profile->frame_rate_den);
        goto fail;
    }

    /* --- 必須 service の存在確認 ------------------------------------------ */
    {
        mlt_repository repo = mlt_factory_repository();
        if (!service_exists(mlt_repository_producers(repo), "avformat")) {
            set_err(err, err_size, "必須 producer 'avformat' がありません");
            goto fail;
        }
        if (!service_exists(mlt_repository_consumers(repo), "avformat")) {
            set_err(err, err_size, "必須 consumer 'avformat' がありません");
            goto fail;
        }
        if (clips_need_timewarp(clips, clip_count) &&
            !service_exists(mlt_repository_producers(repo), "timewarp")) {
            set_err(err, err_size, "速度を変えたclipに必要なproducer 'timewarp'がありません");
            goto fail;
        }
        for (int i = 0; i < clip_count; ++i) {
            if (clips[i].effects_enabled &&
                (!service_exists(mlt_repository_filters(repo), "qtcrop") ||
                 !service_exists(mlt_repository_filters(repo), "affine"))) {
                set_err(err, err_size, "effectに必要なfilter qtcrop/affineがありません");
                goto fail;
            }
        }
    }

    /* --- playlist ---------------------------------------------------------- */
    playlist = mlt_playlist_new(profile);
    if (!playlist) {
        set_err(err, err_size, "playlist を作れません");
        goto fail;
    }

    for (int i = 0; i < clip_count; i++) {
        /* service には NULL (= loader) を渡す。"avformat" を明示すると
         * loader が付ける正規化 filter が外れる (mvm_mlt_compose.c と同じ理由)。 */
        mlt_producer p = open_clip_producer(profile, &clips[i]);
        if (!p) {
            set_err(err, err_size, "clip %d の producer を開けません: %s", i, clips[i].path);
            goto fail;
        }
        producers[producer_count++] = p;

        if (mlt_producer_get_playtime(p) <= 0) {
            set_err(err, err_size, "clip %d の長さが 0 です: %s", i, clips[i].path);
            goto fail;
        }

        const long long producer_in = clips[i].producer_in_frame;
        long long producer_out_exclusive = clips[i].producer_out_frame;
        const long long playtime = (long long)mlt_producer_get_playtime(p);
        if (!clamp_terminal_rounding(&clips[i], playtime, &producer_out_exclusive)) {
            set_err(err, err_size,
                    "clip %d の trim range が素材尺を超えています: out=%lld length=%lld", i,
                    producer_out_exclusive, playtime);
            goto fail;
        }
        if (!clips[i].effects_enabled) {
            if (mlt_playlist_append_io(playlist, p, (mlt_position)producer_in,
                                       (mlt_position)(producer_out_exclusive - 1)) != 0) {
                set_err(err, err_size, "clip %d を playlist へ追加できません: %s", i,
                        clips[i].path);
                goto fail;
            }
        } else {
            const long long duration = producer_out_exclusive - producer_in;
            mlt_producer cut = mlt_producer_cut(p, (mlt_position)producer_in,
                                                (mlt_position)(producer_out_exclusive - 1));
            if (!cut || mlt_producer_get_playtime(cut) != duration) {
                if (cut)
                    mlt_producer_close(cut);
                set_err(err, err_size, "clip %d のeffect用cutを作れません", i);
                goto fail;
            }
            cuts[cut_count++] = cut;
            if (attach_export_crop(profile, cut, &clips[i], producer_in, err, err_size, spec,
                                   &cancelled) != 0 ||
                attach_export_affine(profile, cut, &clips[i], producer_in, duration, err, err_size,
                                     spec, &cancelled) != 0) {
                goto fail;
            }
            if (mlt_playlist_append(playlist, cut) != 0) {
                set_err(err, err_size, "effect付きclipをplaylistへ追加できません");
                goto fail;
            }
        }
    }

    pp = MLT_PLAYLIST_PRODUCER(playlist);
    total = (long long)mlt_producer_get_playtime(pp);
    if (total <= 0) {
        set_err(err, err_size, "playlist の長さが 0 です");
        goto fail;
    }

    /* 出力範囲を明示する。設定しないと consumer がどこまで書くのかが曖昧になる。 */
    mlt_producer_set_in_and_out(pp, 0, (mlt_position)(total - 1));
    mlt_producer_seek(pp, 0);
    if (attach_rgba_audit(pp, &audit)) {
        set_err(err, err_size, "encoder 直前の RGBA 検査を接続できません");
        goto fail;
    }

    /* --- consumer ---------------------------------------------------------- */
    observe_stage(spec, MVM_EXPORT_STAGE_CONSUMER_START);
    consumer = mlt_factory_consumer(profile, "avformat", out_path);
    if (!consumer) {
        set_err(err, err_size, "avformat consumer を作れません: %s", out_path);
        goto fail;
    }
    /* sequential fast path は video clip だけを扱う。音声が必要な timeline は tractor へ送る。 */
    configure_mp4_consumer(consumer, out_path, spec, 0);
    observe_consumer(consumer, spec);

    if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(pp)) != 0) {
        set_err(err, err_size, "consumer を playlist へ接続できません");
        goto fail;
    }
    if (mlt_consumer_start(consumer) != 0) {
        set_err(err, err_size, "consumer を開始できません");
        goto fail;
    }

    /* 完了待ち。timeout を成功扱いにしない。 */
    observe_stage(spec, MVM_EXPORT_STAGE_RENDER);
    {
        int waited = 0;
        const int step = 20;
        while (!mlt_consumer_is_stopped(consumer)) {
            observe_poll(spec, consumer);
            if (export_cancel_requested(spec, consumer, total)) {
                cancelled = 1;
                mlt_consumer_stop(consumer);
                set_err(err, err_size, "書き出しをキャンセルしました");
                goto fail;
            }
            Sleep(step);
            waited += step;
            if (waited >= spec->timeout_ms) {
                mlt_consumer_stop(consumer);
                set_err(err, err_size, "consumer が %d ms 以内に終了しませんでした (timeout)",
                        spec->timeout_ms);
                goto fail;
            }
        }
        if (spec->progress_callback &&
            spec->progress_callback(total, total, spec->progress_opaque) != 0) {
            cancelled = 1;
            mlt_consumer_stop(consumer);
            set_err(err, err_size, "書き出しをキャンセルしました");
            goto fail;
        }
    }
    observe_stage(spec, MVM_EXPORT_STAGE_CONSUMER_CLOSE);
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    consumer = NULL;

    /* --- 出力の検証 -------------------------------------------------------- */
    observe_stage(spec, MVM_EXPORT_STAGE_OUTPUT_VERIFY);
    if (spec->rgba_callback && (audit.rejected || audit.observed < total)) {
        set_err(err, err_size, "encoder 直前の RGBA 検査が失敗または不足しました");
        goto fail;
    }
    {
        unsigned long long size = 0;
        if (!file_size_utf8(out_path, &size)) {
            set_err(err, err_size, "出力ファイルが生成されていません: %s", out_path);
            goto fail;
        }
        if (size == 0) {
            set_err(err, err_size, "出力ファイルが 0 バイトです: %s", out_path);
            goto fail;
        }
    }
    {
        MvmMltProbeResult probe;
        if (mvm_mlt_probe_file(out_path, &probe) != 0 || !probe.ok) {
            set_err(err, err_size, "出力ファイルを probe できません: %s (%s)", out_path,
                    probe.error);
            goto fail;
        }
        if (!probe.has_video) {
            set_err(err, err_size, "出力ファイルに映像がありません: %s", out_path);
            goto fail;
        }
        if (probe.frame_count <= 0) {
            set_err(err, err_size, "出力ファイルの frame 数が 0 です: %s", out_path);
            goto fail;
        }
        if (probe.has_audio) {
            set_err(err, err_size, "audio clip の無い出力に音声が含まれています: %s", out_path);
            goto fail;
        }
        if (out) {
            out->frame_count = probe.frame_count;
            out->duration_sec = probe.duration_sec;
            out->width = probe.width;
            out->height = probe.height;
            out->fps_num = probe.fps_num;
            out->fps_den = probe.fps_den;
        }
    }

    detach_frame_observer(pp, &audit);
    for (int i = 0; i < cut_count; i++)
        mlt_producer_close(cuts[i]);
    for (int i = 0; i < producer_count; i++)
        mlt_producer_close(producers[i]);
    free(cuts);
    free(producers);
    mlt_playlist_close(playlist);
    mlt_profile_close(profile);
    observe_stage(spec, MVM_EXPORT_STAGE_DONE);
    return 0;

fail:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
    detach_frame_observer(pp, &audit);
    observe_stage(spec, MVM_EXPORT_STAGE_FAILED);
    for (int i = 0; i < cut_count; i++)
        mlt_producer_close(cuts[i]);
    for (int i = 0; i < producer_count; i++)
        mlt_producer_close(producers[i]);
    free(cuts);
    free(producers);
    if (playlist)
        mlt_playlist_close(playlist);
    if (profile)
        mlt_profile_close(profile);
    return cancelled ? MVM_EXPORT_CANCELLED : MVM_EXPORT_FAILED;
}

/* playlist に frames 分の空きを足す。V1 (track 0) は黒の背景で埋め、それ以外は blank にする。
 * V1 を blank にすると、上位 track の affine transition が掛からない (上の定義を参照)。 */
/* --- 数式 group の transition (試作、P3-3A。observer の diagnostic_math_group が 0 なら何もしない)
 * ---
 *
 * 合成の順は clips の並び (上位 track の clip ごとに transition を植える順) で決まる。
 * group は 1 つだけ作り、最初の member の位置へ植える。member にするのは、次の両方を満たす clip:
 *   (1) 配置が対象 (mvm_mlt_math_group_clip_eligible)。
 *   (2) 並びで自分より前にある「従来の経路の上位 track の clip」のどれとも、時間が重ならない。
 * (2) の理由は 2 つある。
 *   - 順序: member は group の位置 (最初の member の位置) でまとめて合成される。自分より前にある
 *     従来の clip と同じ frame に見えていると、その clip より先に合成されて順序が変わる。
 *     時間が重ならなければ、どの frame でも合成の順は従来と同じになる。
 *   - 下地: 従来の affine が重ねた画素は alpha が 254 になりうる。合成器は不透明な下地だけを
 *     対象にする。(2) を満たす member の下にあるのは V1 と、同じ group の前の member だけになる。
 * どちらかを満たさない clip は従来の経路へ残し、数と最初の理由を observer へ書く。
 * groups[index] は member なら 1、従来の経路なら 0。member があれば 1 を返す (失敗は -1)。 */
static int assign_math_groups(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                              int* groups, char* err, size_t err_size) {
    MvmExportObserver* observer = spec->observer;
    if (!observer || !observer->diagnostic_math_group)
        return 0;
    if (!observer->diagnostic_math_group_compose) {
        set_err(err, err_size, "数式 group の合成の callback がありません");
        return -1;
    }
    int members = 0;
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (clip->is_audio || clip->video_track <= 0)
            continue; /* transition を植えない clip。順序にも下地の alpha にも関わらない */
        const char* reason = NULL;
        int eligible = mvm_mlt_math_group_clip_eligible(clip, spec->width, spec->height, &reason);
        const int geometry = eligible;
        for (int before = 0; eligible && before < index; ++before) {
            const MvmExportClip* other = &clips[before];
            if (groups[before] != 0 || other->is_audio || other->video_track <= 0)
                continue;
            if (other->timeline_start_frame <
                    clip->timeline_start_frame + clip->timeline_duration_frames &&
                other->timeline_start_frame + other->timeline_duration_frames >
                    clip->timeline_start_frame) {
                eligible = 0;
                reason = "above-retained-layer";
            }
        }
        if (eligible) {
            groups[index] = 1;
            ++members;
            continue;
        }
        if (clip->math_group_raster) {
            /* 申告されたが対象外。黙って合成器へ回さず、従来の経路へ残して数える。 */
            InterlockedIncrement64(geometry ? &observer->math_group_retained_above
                                            : &observer->math_group_retained_geometry);
            if (InterlockedIncrement64(&observer->math_group_retained_clips) == 1)
                snprintf(observer->math_group_retained_reason,
                         sizeof(observer->math_group_retained_reason), "%s",
                         reason ? reason : "not-overlay-video");
        }
    }
    return members > 0 ? 1 : 0;
}

/* group の member (groups が 1 の clip、並びの順) を 1 つの transition として、V1 と group 用の
 * 空の track の間へ植える。transition は上の track の frame を隠すので、他の clip が載らない
 * 専用の track を使う。 */
static int plant_export_math_group(mlt_tractor tractor, const MvmExportClip* clips, int clip_count,
                                   const int* groups, int group_track, const MvmExportSpec* spec,
                                   char* err, size_t err_size) {
    int count = 0;
    for (int index = 0; index < clip_count; ++index)
        count += groups[index] != 0;
    const MvmExportClip** members =
        (const MvmExportClip**)calloc((size_t)(count > 0 ? count : 1), sizeof(*members));
    if (!members || count <= 0) {
        free(members);
        set_err(err, err_size, "数式 group の member 配列を確保できません");
        return 1;
    }
    for (int index = 0, at = 0; index < clip_count; ++index)
        if (groups[index] != 0)
            members[at++] = &clips[index];
    char reason[160] = {0};
    mlt_transition transition = mvm_mlt_math_group_new(members, count, spec->width, spec->height,
                                                       spec->observer, reason, sizeof(reason));
    free(members);
    if (!transition) {
        set_err(err, err_size, "%s", reason);
        return 1;
    }
    mlt_transition_set_tracks(transition, 0, group_track);
    if (mlt_field_plant_transition(mlt_tractor_field(tractor), transition, 0, group_track) != 0) {
        mlt_transition_close(transition);
        set_err(err, err_size, "数式 group の transition を V1 との間へ配置できません");
        return 1;
    }
    mlt_transition_close(transition);
    MvmExportObserver* observer = spec->observer; /* group は observer がある時だけ作られる */
    if (observer) {
        InterlockedIncrement64(&observer->math_group_transitions);
        InterlockedExchangeAdd64(&observer->math_group_clips, count);
    }
    return 0;
}

/* group の transition が失敗を記録していれば、書き出しを失敗にする。 */
static int math_group_failed(const MvmExportSpec* spec, char* err, size_t err_size) {
    MvmExportObserver* observer = spec->observer;
    if (!observer || !observer->diagnostic_math_group ||
        InterlockedCompareExchange64(&observer->math_group_errors, 0, 0) == 0)
        return 0;
    if (InterlockedCompareExchange(&observer->math_group_error_ready, 0, 0))
        set_err(err, err_size, "%s (frame %lld)", observer->math_group_error,
                observer->math_group_error_frame);
    else
        set_err(err, err_size, "数式 group の合成に失敗しました");
    return 1;
}

static int append_gap(mlt_playlist playlist, int track, mlt_producer v1_background,
                      long long frames) {
    if (frames <= 0)
        return 1;
    if (track == 0)
        return mlt_playlist_append_io(playlist, v1_background, 0, (mlt_position)(frames - 1));
    return mlt_playlist_blank(playlist, (mlt_position)(frames - 1));
}

int mvm_mlt_export_two_track(const MvmExportClip* clips, int clip_count, long long total_duration,
                             const MvmExportSpec* spec, const char* out_path, MvmExportResult* out,
                             char* err, size_t err_size) {
    mlt_profile profile = NULL;
    mlt_tractor tractor = NULL;
    /* track 数と clip 数に上限を設けない。配列は下で実数から確保する。 */
    mlt_playlist* playlists = NULL;
    int* audio_tracks = NULL;
    int video_playlist_count = 2;
    int audio_clip_count = 0;
    int track_capacity = 0;
    int playlist_count = 0;
    mlt_producer* producers = NULL;
    mlt_producer* cuts = NULL;
    int producer_count = 0;
    int cut_count = 0;
    long long cut_capacity = 0;
    mlt_consumer consumer = NULL;
    long long* cursors = NULL;
    mlt_producer v1_background = NULL;
    int failed = MVM_EXPORT_FAILED;
    ExportRgbaAudit audit = {spec, 0, 0};
    long long render_frames = total_duration;
    int* group_members = NULL; /* 試作 (P3-3A)。NULL なら全 clip が従来の経路 */
    int group_track = -1;      /* group の transition が上の track として使う空の track */
    int group_planted = 0;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!mvm_mlt_runtime_is_ready() || (clip_count > 0 && !clips) || !spec || !out_path ||
        !*out_path || clip_count < 0 || total_duration <= 0 || spec->width <= 0 ||
        spec->height <= 0 || spec->fps_num <= 0 || spec->fps_den <= 0 || spec->video_crf < 0 ||
        spec->video_crf > 51 || spec->timeout_ms <= 0 || spec->render_threads <= 0 ||
        spec->render_threads > 16 || spec->encoder_threads < 0 || spec->encoder_threads > 16 ||
        spec->lossless_audio < 0 || spec->lossless_audio > 1) {
        set_err(err, err_size, "M7b tractor export引数が不正です");
        return 1;
    }
    if (validate_motion_frames(clips, clip_count, spec, err, err_size) != 0)
        return 1;
    /* 映像 playlist は V1/V2 を常に作り、それより上は使われている最上位 track まで作る。
     * audio は clip ごとに 1 本の playlist を持つ。 */
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (clip->is_audio) {
            ++audio_clip_count;
        } else if (clip->video_track < 0 || clip->video_track >= INT32_MAX - clip_count) {
            set_err(err, err_size, "tractor clip %dのmappingが不正です", index);
            return 1;
        } else if (clip->video_track + 1 > video_playlist_count) {
            video_playlist_count = clip->video_track + 1;
        }
    }
    if (spec->observer && spec->observer->diagnostic_math_group) {
        /* 試作 (P3-3A): member を決め、member があれば映像 track の一番上に空の track を 1 本足す
         * (audio の track はその上に続く)。 */
        group_members = (int*)calloc((size_t)(clip_count > 0 ? clip_count : 1), sizeof(int));
        if (!group_members) {
            set_err(err, err_size, "数式 group の配列を確保できません");
            return 1;
        }
        const int assigned =
            assign_math_groups(clips, clip_count, spec, group_members, err, err_size);
        if (assigned < 0) {
            free(group_members);
            return 1;
        }
        if (assigned > 0)
            group_track = video_playlist_count++;
        else {
            free(group_members);
            group_members = NULL;
        }
    }
    track_capacity = video_playlist_count + audio_clip_count;
    playlists = (mlt_playlist*)calloc((size_t)track_capacity, sizeof(*playlists));
    audio_tracks = (int*)calloc((size_t)track_capacity, sizeof(*audio_tracks));
    cursors = (long long*)calloc((size_t)track_capacity, sizeof(*cursors));
    producers =
        (mlt_producer*)calloc((size_t)(clip_count > 0 ? clip_count : 1), sizeof(*producers));
    if (!playlists || !audio_tracks || !cursors || !producers) {
        set_err(err, err_size, "track 配列を確保できません");
        goto cleanup;
    }
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (!clip->path || !clip->path[0] || !clip_media_exists(clip) ||
            clip->timeline_start_frame < 0 || clip->timeline_duration_frames <= 0 ||
            clip->timeline_start_frame > total_duration - clip->timeline_duration_frames ||
            clip->source_fps_num <= 0 || clip->source_fps_den <= 0 ||
            clip->source_frame_count <= 0 || clip->source_in_frame < 0 ||
            clip->source_out_frame <= clip->source_in_frame ||
            clip->source_out_frame > clip->source_frame_count || clip->crop_left < 0 ||
            clip->crop_top < 0 || clip->crop_right < 0 || clip->crop_bottom < 0 ||
            clip->producer_in_frame < 0 || clip->producer_out_frame <= clip->producer_in_frame ||
            clip->tail_padding_frames < 0 || clip->speed_num <= 0 || clip->speed_den <= 0 ||
            /* 次の加算の signed overflow (未定義動作) を先に止める。 */
            clip->tail_padding_frames >
                LLONG_MAX - (clip->producer_out_frame - clip->producer_in_frame) ||
            clip->producer_out_frame - clip->producer_in_frame + clip->tail_padding_frames !=
                clip->timeline_duration_frames) {
            set_err(err, err_size, "tractor clip %dのmappingが不正です", index);
            goto cleanup;
        }
        /* 本体 cut 1 本、末尾の補完 frame ごとに 1 本、素材末尾の +1 丸めの補完 1 本。
         * clip 数に上限が無いので、合計は加算前に表現範囲と確保可能な要素数を検査する。 */
        if (clip->tail_padding_frames > LLONG_MAX - 2 - cut_capacity ||
            (unsigned long long)(cut_capacity + 2 + clip->tail_padding_frames) >
                SIZE_MAX / sizeof(*cuts)) {
            set_err(err, err_size, "tractor clip %dまでのcut数が表現範囲を超えています", index);
            goto cleanup;
        }
        cut_capacity += 2 + clip->tail_padding_frames;
        if (!clip->is_audio && clip->timeline_start_frame < cursors[clip->video_track]) {
            set_err(err, err_size, "tractor clip %dが同一trackで重複または未sortです", index);
            goto cleanup;
        }
        if (!source_region_valid(clip, spec)) {
            set_err(err, err_size, "clip %dの切り出した素材の配置が不正です", index);
            goto cleanup;
        }
        if (clip->is_audio) {
            if (!isfinite(clip->mixer_pan) || clip->mixer_pan < -1.0 || clip->mixer_pan > 1.0) {
                set_err(err, err_size, "音声パンが範囲外です");
                goto cleanup;
            }
            if (!clip->gain_keyframes ||
                clip->gain_keyframe_count != clip->timeline_duration_frames) {
                set_err(err, err_size, "audio clip %dのgain key数が尺と一致しません", index);
                goto cleanup;
            }
            for (int key_index = 0; key_index < clip->gain_keyframe_count; ++key_index) {
                const MvmExportGainKeyframe* key = &clip->gain_keyframes[key_index];
                if (key->local_frame != key_index || !isfinite(key->gain) || key->gain < 0.0 ||
                    key->gain > 2.0 * 5.623414 * 1000.0) {
                    set_err(err, err_size, "audio clip %dのgain keyが不正です", index);
                    goto cleanup;
                }
            }
        }
        if (!clip->is_audio)
            cursors[clip->video_track] =
                clip->timeline_start_frame + clip->timeline_duration_frames;
        if (!clip->is_audio && (clip->video_track > 0 || clip->effects_enabled) &&
            (clip->opacity_keyframe_count <= 0 || !clip->opacity_keyframes ||
             clip->rect_width <= 0.0 || clip->rect_height <= 0.0)) {
            set_err(err, err_size, "clip %dのeffect/transition mappingが不正です", index);
            goto cleanup;
        }
        for (int key_index = 0; key_index < clip->opacity_keyframe_count; ++key_index) {
            const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[key_index];
            if (key->local_frame < 0 || key->local_frame >= clip->timeline_duration_frames ||
                key->opacity < 0.0 || key->opacity > 1.0 ||
                (key_index > 0 &&
                 key->local_frame <= clip->opacity_keyframes[key_index - 1].local_frame)) {
                set_err(err, err_size, "clip %dのopacity key domainが不正です", index);
                goto cleanup;
            }
        }
        if (!clip->is_audio && clip->video_track > 0 &&
            (clip->opacity_keyframes[0].local_frame != 0 ||
             clip->opacity_keyframes[clip->opacity_keyframe_count - 1].local_frame !=
                 clip->timeline_duration_frames - 1)) {
            set_err(err, err_size, "上位track clip %dのtransition-local端keyがありません", index);
            goto cleanup;
        }
    }
    observe_stage(spec, MVM_EXPORT_STAGE_GRAPH_BUILD);
    cuts = (mlt_producer*)calloc((size_t)(cut_capacity > 0 ? cut_capacity : 1), sizeof(*cuts));
    if (!cuts) {
        set_err(err, err_size, "cut 配列を確保できません");
        goto cleanup;
    }

    profile = mlt_profile_init(NULL);
    if (!profile) {
        set_err(err, err_size, "profileを作れません");
        goto cleanup;
    }
    profile->width = spec->width;
    profile->height = spec->height;
    profile->frame_rate_num = spec->fps_num;
    profile->frame_rate_den = spec->fps_den;
    profile->sample_aspect_num = 1;
    profile->sample_aspect_den = 1;
    profile->display_aspect_num = spec->width;
    profile->display_aspect_den = spec->height;
    profile->progressive = 1;
    profile->colorspace = 601;
    if (profile->width != spec->width || profile->height != spec->height ||
        profile->frame_rate_num != spec->fps_num || profile->frame_rate_den != spec->fps_den) {
        set_err(err, err_size, "profileの実値が要求と一致しません");
        goto cleanup;
    }
    {
        mlt_repository repo = mlt_factory_repository();
        if (!service_exists(mlt_repository_producers(repo), "avformat") ||
            !service_exists(mlt_repository_consumers(repo), "avformat") ||
            !service_exists(mlt_repository_filters(repo), "qtcrop") ||
            !service_exists(mlt_repository_producers(repo), "color") ||
            !service_exists(mlt_repository_filters(repo), "affine") ||
            !service_exists(mlt_repository_transitions(repo), "affine") ||
            !service_exists(mlt_repository_transitions(repo), "mix") ||
            !service_exists(mlt_repository_producers(repo), "qimage") ||
            (clips_need_timewarp(clips, clip_count) &&
             !service_exists(mlt_repository_producers(repo), "timewarp"))) {
            set_err(err, err_size,
                    "tractor exportに必要なqtcrop/color/affine/mix/avformat/timewarpがありません");
            goto cleanup;
        }
    }

    /* V1 の gap は blank ではなく黒の color producer で埋める。blank は test card になり、
     * affine transition は下の frame が test card だと上の frame をそのまま返す (実測)。
     * そのため V1 が空の区間では、V2 以上の clip の位置・拡大・回転・crop・不透明度が
     * 書き出されていなかった (docs/premiere-like-editing.md §18.6)。 */
    v1_background = mlt_factory_producer(profile, "color", "#000000");
    if (!v1_background) {
        set_err(err, err_size, "V1 の背景 (color producer) を作れません");
        goto cleanup;
    }
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(v1_background), "length",
                                (mlt_position)total_duration + 1);
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(v1_background), "out",
                                (mlt_position)total_duration);

    tractor = mlt_tractor_new();
    for (int track = 0; track < video_playlist_count; ++track) {
        playlists[track] = mlt_playlist_new(profile);
        playlist_count = track + 1;
        if (!tractor || !playlists[track]) {
            set_err(err, err_size, "tractorまたは映像 playlistを作れません");
            goto cleanup;
        }
        cursors[track] = 0;
    }
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        int track = clip->video_track;
        if (clip->is_audio) {
            if (playlist_count >= track_capacity) {
                set_err(err, err_size, "audio playlist数がaudio clip数と一致しません");
                goto cleanup;
            }
            track = playlist_count++;
            playlists[track] = mlt_playlist_new(profile);
            audio_tracks[track] = 1;
            if (!playlists[track]) {
                set_err(err, err_size, "audio clip %d用playlistを作れません", index);
                goto cleanup;
            }
        }
        /* 試作 (P3-3A): 数式 group でまとめる clip は playlist に置かない。その区間は blank の
         * まま残り (下の gap の処理が埋める)、画像は group の transition が合成する。 */
        if (group_members && group_members[index])
            continue;
        if (clip->timeline_start_frame > cursors[track]) {
            if (append_gap(playlists[track], track, v1_background,
                           clip->timeline_start_frame - cursors[track]) != 0) {
                set_err(err, err_size, "track %dへblankを追加できません", track);
                goto cleanup;
            }
            if (out)
                ++out->playlist_blank_count;
        }

        mlt_producer parent = open_clip_producer(profile, clip);
        if (!parent || mlt_producer_get_playtime(parent) <= 0) {
            if (parent)
                mlt_producer_close(parent);
            set_err(err, err_size, "clip %dのproducerを開けません", index);
            goto cleanup;
        }
        producers[producer_count++] = parent;
        const long long producer_in = clip->producer_in_frame;
        long long producer_out = clip->producer_out_frame;
        if (!clamp_terminal_rounding(clip, (long long)mlt_producer_get_playtime(parent),
                                     &producer_out) ||
            producer_out - producer_in <= 0) {
            set_err(err, err_size, "clip %dのcut尺がtimeline配置尺と一致しません", index);
            goto cleanup;
        }
        const long long actual_duration = producer_out - producer_in;
        /* 補完は呼び出し側が決めた末尾の分と、素材末尾の +1 丸めで削った 1 frame だけ。 */
        const long long padding_frames = clip->timeline_duration_frames - actual_duration;
        if (padding_frames < clip->tail_padding_frames ||
            padding_frames > clip->tail_padding_frames + 1) {
            set_err(err, err_size, "clip %dのcut尺差が許容範囲外です: timeline=%lld producer=%lld",
                    index, clip->timeline_duration_frames, actual_duration);
            goto cleanup;
        }
        mlt_producer cut =
            mlt_producer_cut(parent, (mlt_position)producer_in, (mlt_position)(producer_out - 1));
        if (!cut || mlt_producer_get_playtime(cut) != actual_duration) {
            if (cut)
                mlt_producer_close(cut);
            set_err(err, err_size, "clip %dの明示cutを作れません", index);
            goto cleanup;
        }
        cuts[cut_count++] = cut;
        if (attach_tractor_clip_filters(profile, cut, clip, track, producer_in, err, err_size, spec,
                                        &failed) != 0)
            goto cleanup;
        if (mlt_playlist_append(playlists[track], cut) != 0) {
            set_err(err, err_size, "clip %dをtrack %d playlistへ追加できません", index, track);
            goto cleanup;
        }
        for (long long padding = 0; padding < padding_frames; ++padding) {
            mlt_producer tail = mlt_producer_cut(parent, (mlt_position)(producer_out - 1),
                                                 (mlt_position)(producer_out - 1));
            if (!tail || mlt_producer_get_playtime(tail) != 1) {
                if (tail)
                    mlt_producer_close(tail);
                set_err(err, err_size, "clip %dの末尾frameを補完できません", index);
                goto cleanup;
            }
            cuts[cut_count++] = tail;
            /* 補完frameも本体cutと同じcrop/effectを受けなければならない。tailは素材位置
             * producer_out-1を返すため、affineのclip-local位置が
             * actual_duration+paddingになるようfilter原点をずらす。 */
            if (attach_tractor_clip_filters(profile, tail, clip, track, producer_in - 1 - padding,
                                            err, err_size, spec, &failed) != 0)
                goto cleanup;
            if (mlt_playlist_append(playlists[track], tail) != 0) {
                set_err(err, err_size, "clip %dの末尾frameを補完できません", index);
                goto cleanup;
            }
        }
        cursors[track] = clip->timeline_start_frame + clip->timeline_duration_frames;
    }
    for (int track = 0; track < playlist_count; ++track) {
        if (cursors[track] < total_duration) {
            if (append_gap(playlists[track], track, v1_background,
                           total_duration - cursors[track]) != 0) {
                set_err(err, err_size, "track %dへ末尾blankを追加できません", track);
                goto cleanup;
            }
            if (out)
                ++out->playlist_blank_count;
        }
        mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(playlists[track]), "hide",
                               audio_tracks[track] ? 1 : 2);
        if (mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(playlists[track]), track) != 0) {
            set_err(err, err_size, "track %dをtractorへ設定できません", track);
            goto cleanup;
        }
    }
    for (int index = 0; index < clip_count; ++index) {
        if (clips[index].is_audio || clips[index].video_track == 0)
            continue;
        if (group_members && group_members[index]) {
            /* 試作 (P3-3A): 最初の member の位置へ、全 member を 1 つの transition として植える。
             * 残りの member はここでは何も植えない (合成の順が変わらない理由は
             * assign_math_groups)。 */
            if (!group_planted) {
                if (plant_export_math_group(tractor, clips, clip_count, group_members, group_track,
                                            spec, err, err_size) != 0)
                    goto cleanup;
                group_planted = 1;
                if (out)
                    ++out->transition_count;
            }
            continue;
        }
        if (plant_export_overlay_affine(profile, tractor, &clips[index], err, err_size, spec,
                                        &failed) != 0)
            goto cleanup;
        if (out)
            ++out->transition_count;
    }
    for (int track = video_playlist_count; track < playlist_count; ++track) {
        mlt_transition mix = mlt_factory_transition(profile, "mix", NULL);
        if (!mix) {
            set_err(err, err_size, "audio track %d用mix transitionを作れません", track);
            goto cleanup;
        }
        mlt_properties mix_properties = MLT_TRANSITION_PROPERTIES(mix);
        mlt_properties_set_int(mix_properties, "always_active", 1);
        mlt_properties_set_int(mix_properties, "sum", 1);
        mlt_transition_set_tracks(mix, 0, track);
        if (mlt_field_plant_transition(mlt_tractor_field(tractor), mix, 0, track) != 0) {
            mlt_transition_close(mix);
            set_err(err, err_size, "audio track %dをmixできません", track);
            goto cleanup;
        }
        mlt_transition_close(mix);
    }

    {
        mlt_producer output = MLT_TRACTOR_PRODUCER(tractor);
        if ((long long)mlt_producer_get_playtime(output) != total_duration) {
            set_err(err, err_size, "tractor尺がProject timeline endと一致しません");
            goto cleanup;
        }
        mlt_producer_set_in_and_out(output, 0, (mlt_position)(total_duration - 1));
        mlt_producer_seek(output, 0);
        if (spec->observer && spec->observer->diagnostic_frame_count != 0) {
            /* 診断用: graph はそのまま、途中の区間だけを書き出す。 */
            const long long first = spec->observer->diagnostic_first_frame;
            const long long count = spec->observer->diagnostic_frame_count;
            if (first < 0 || count <= 0 || first > total_duration - count) {
                set_err(err, err_size, "診断用の書き出し区間がProjectの尺に収まりません");
                goto cleanup;
            }
            mlt_producer_set_in_and_out(output, 0, (mlt_position)(first + count - 1));
            mlt_producer_seek(output, (mlt_position)first);
            render_frames = count;
        }
        if (attach_rgba_audit(output, &audit)) {
            set_err(err, err_size, "encoder 直前の RGBA 検査を接続できません");
            goto cleanup;
        }
        observe_stage(spec, MVM_EXPORT_STAGE_CONSUMER_START);
        consumer = mlt_factory_consumer(profile, "avformat", out_path);
        if (!consumer) {
            set_err(err, err_size, "avformat consumerを作れません");
            goto cleanup;
        }
        configure_mp4_consumer(consumer, out_path, spec, playlist_count > video_playlist_count);
        observe_consumer(consumer, spec);
        if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) != 0 ||
            mlt_consumer_start(consumer) != 0) {
            set_err(err, err_size, "tractor consumerを開始できません");
            goto cleanup;
        }
    }
    observe_stage(spec, MVM_EXPORT_STAGE_RENDER);
    {
        int waited = 0;
        while (!mlt_consumer_is_stopped(consumer)) {
            observe_poll(spec, consumer);
            if (export_cancel_requested(spec, consumer, total_duration)) {
                failed = MVM_EXPORT_CANCELLED;
                mlt_consumer_stop(consumer);
                set_err(err, err_size, "書き出しをキャンセルしました");
                goto cleanup;
            }
            if (math_group_failed(spec, err, err_size)) {
                mlt_consumer_stop(consumer);
                goto cleanup;
            }
            Sleep(20);
            waited += 20;
            if (waited >= spec->timeout_ms) {
                set_err(err, err_size, "tractor consumerがtimeoutしました");
                goto cleanup;
            }
        }
        /* 最後の frame での失敗を取りこぼさない。 */
        if (math_group_failed(spec, err, err_size))
            goto cleanup;
        if (spec->progress_callback &&
            spec->progress_callback(total_duration, total_duration, spec->progress_opaque) != 0) {
            failed = MVM_EXPORT_CANCELLED;
            mlt_consumer_stop(consumer);
            set_err(err, err_size, "書き出しをキャンセルしました");
            goto cleanup;
        }
    }
    observe_stage(spec, MVM_EXPORT_STAGE_CONSUMER_CLOSE);
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    consumer = NULL;
    observe_stage(spec, MVM_EXPORT_STAGE_OUTPUT_VERIFY);
    if (spec->rgba_callback && (audit.rejected || audit.observed < render_frames)) {
        set_err(err, err_size, "encoder 直前の RGBA 検査が失敗または不足しました");
        goto cleanup;
    }
    {
        unsigned long long size = 0;
        MvmMltProbeResult probe;
        if (!file_size_utf8(out_path, &size) || size == 0 ||
            mvm_mlt_probe_file(out_path, &probe) != 0 || !probe.ok || !probe.has_video ||
            probe.frame_count <= 0 ||
            (probe.has_audio != 0) != (playlist_count > video_playlist_count)) {
            set_err(err, err_size, "tractor出力を検証できません");
            goto cleanup;
        }
        if (out) {
            out->frame_count = probe.frame_count;
            out->duration_sec = probe.duration_sec;
            out->width = probe.width;
            out->height = probe.height;
            out->fps_num = probe.fps_num;
            out->fps_den = probe.fps_den;
            out->used_tractor = 1;
            out->opaque_black_affine_filter_count = 0;
        }
    }
    failed = MVM_EXPORT_OK;

cleanup:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
    if (tractor)
        detach_frame_observer(MLT_TRACTOR_PRODUCER(tractor), &audit);
    observe_stage(spec, failed == MVM_EXPORT_OK ? MVM_EXPORT_STAGE_DONE : MVM_EXPORT_STAGE_FAILED);
    if (tractor)
        mlt_tractor_close(tractor);
    for (int track = playlist_count - 1; track >= 0; --track)
        if (playlists[track])
            mlt_playlist_close(playlists[track]);
    for (int index = 0; index < cut_count; ++index)
        if (cuts[index])
            mlt_producer_close(cuts[index]);
    free(cuts);
    for (int index = 0; index < producer_count; ++index)
        if (producers[index])
            mlt_producer_close(producers[index]);
    free(producers);
    if (v1_background)
        mlt_producer_close(v1_background);
    free(cursors);
    free(audio_tracks);
    free(playlists);
    free(group_members);
    if (profile)
        mlt_profile_close(profile);
    /* 試作 (P3-3A): graph を閉じた後に、group の transition と実行中の callback が残っていない
     * こと。残っていれば、呼び出し側が持つ画像を解放後に参照しうるので、成功として返さない。 */
    if (spec && spec->observer && spec->observer->diagnostic_math_group &&
        (mvm_mlt_math_group_live() != 0 || mvm_mlt_math_group_active() != 0)) {
        if (failed == MVM_EXPORT_OK) {
            set_err(err, err_size, "数式 group の transition が解放されていません");
            failed = MVM_EXPORT_FAILED;
        }
    }
    return failed;
}
