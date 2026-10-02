#include "mvm_mlt_export.h"

#include "../../util/mvm_win_utf8.h"
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

static int export_cancel_requested(const MvmExportSpec* spec, mlt_consumer consumer,
                                   long long total) {
    if (!spec->progress_callback)
        return 0;
    long long completed = consumer ? (long long)mlt_consumer_position(consumer) + 1 : 0;
    if (completed < 0)
        completed = 0;
    if (completed > total)
        completed = total;
    return spec->progress_callback(completed, total, spec->progress_opaque) != 0;
}

/* 音声出力の authority は timeline 上の独立 audio clip だけである。video producer が
 * 内蔵する音声は、linked audio clip を削除した timeline で復活させてはならないため、
 * audio track を持たない書き出しは常に an=1 にする。 */
static void configure_mp4_consumer(mlt_consumer consumer, const char* out_path,
                                   const MvmExportSpec* spec, int with_audio) {
    mlt_properties cp = MLT_CONSUMER_PROPERTIES(consumer);
    mlt_properties_set(cp, "target", out_path);
    mlt_properties_set(cp, "f", "mp4");
    mlt_properties_set(cp, "vcodec", "libx264");
    mlt_properties_set(cp, "preset", "medium");
    mlt_properties_set_int(cp, "crf", spec->video_crf);
    mlt_properties_set(cp, "pix_fmt", "yuv420p");
    mlt_properties_set(cp, "movflags", "+faststart");
    if (with_audio) {
        mlt_properties_set(cp, "acodec", "aac");
        mlt_properties_set(cp, "ab", "192k");
        mlt_properties_set_int(cp, "ar", 48000);
    } else {
        mlt_properties_set_int(cp, "an", 1);
    }
    mlt_properties_set_int(cp, "real_time", -spec->render_threads);
    mlt_properties_set_int(cp, "threads", spec->encoder_threads);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);
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

/* crop は qtcrop で「範囲の外を透明で塗る」形で掛け、frame の寸法を変えない。
 * 以前の crop filter は frame の寸法そのものを変えていた。その後の affine は正方形でない
 * 座標系で回転するため、crop と回転を組み合わせると平行四辺形 (shear) になっていた
 * (docs/premiere-like-editing.md §18.6)。寸法を保てば affine の座標系は出力と同じになる。
 * 配置の矩形 (rect_*) は呼び出し側が crop 前の全体の位置として渡す。 */
static int attach_export_crop(mlt_profile profile, mlt_producer cut, const MvmExportClip* clip,
                              char* err, size_t err_size) {
    if (clip->crop_left == 0 && clip->crop_top == 0 && clip->crop_right == 0 &&
        clip->crop_bottom == 0)
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
    mlt_properties_set(props, "color", "#00000000");
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
                                size_t err_size) {
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
    for (int i = 0; i < clip->opacity_keyframe_count; ++i) {
        const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[i];
        mlt_rect rect = {clip->rect_x, clip->rect_y, clip->rect_width, clip->rect_height,
                         key->opacity};
        if (mlt_properties_anim_set_rect(props, "transition.rect", rect,
                                         (mlt_position)key->local_frame, (mlt_position)duration,
                                         mlt_keyframe_linear) != 0) {
            mlt_filter_close(filter);
            set_err(err, err_size, "affine opacity animationを設定できません");
            return 1;
        }
    }
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

static int plant_export_overlay_affine(mlt_profile profile, mlt_tractor tractor,
                                       const MvmExportClip* clip, char* err, size_t err_size) {
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
    mlt_transition_set_in_and_out(
        transition, (mlt_position)clip->timeline_start_frame,
        (mlt_position)(clip->timeline_start_frame + clip->timeline_duration_frames - 1));
    for (int index = 0; index < clip->opacity_keyframe_count; ++index) {
        const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[index];
        mlt_rect rect = {clip->rect_x, clip->rect_y, clip->rect_width, clip->rect_height,
                         key->opacity};
        if (mlt_properties_anim_set_rect(props, "rect", rect, (mlt_position)key->local_frame,
                                         (mlt_position)clip->timeline_duration_frames,
                                         mlt_keyframe_linear) != 0) {
            mlt_transition_close(transition);
            set_err(err, err_size, "上位track affine opacity keyframeを設定できません");
            return 1;
        }
    }
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

/* tractor経路でclip単位のcut（本体・末尾補完）へ付けるfilter。
 * filter_inはclip-local frame 0に対応するparent producer位置。 */
static int attach_tractor_clip_filters(mlt_profile profile, mlt_producer cut,
                                       const MvmExportClip* clip, int track, long long filter_in,
                                       char* err, size_t err_size) {
    if (clip->is_audio) {
        if (clip->mixer_pan != 0.0) {
            mlt_filter balance = mlt_factory_filter(profile, "panner", NULL);
            if (!balance) {
                set_err(err, err_size, "パンフィルターを作成できません");
                return 1;
            }
            mlt_properties props = MLT_FILTER_PROPERTIES(balance);
            mlt_properties_set_int(props, "channel", -1);
            mlt_properties_set_double(props, "start", (clip->mixer_pan + 1.0) * 0.5);
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
        return attach_export_crop(profile, cut, clip, err, err_size) != 0 ||
               attach_export_affine(profile, cut, clip, filter_in, clip->timeline_duration_frames,
                                    err, err_size) != 0;
    /* 上位映像trackへopaque-black affine filterをattachしない。cropだけをcutへ置く。
     * クロスディゾルブの incoming だけは余白を黒で埋める (attach_export_backdrop)。 */
    if (track > 0)
        return attach_export_crop(profile, cut, clip, err, err_size) != 0 ||
               (clip->opaque_backdrop &&
                attach_export_backdrop(profile, cut, clip, filter_in,
                                       clip->timeline_duration_frames, err, err_size) != 0);
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
        spec->render_threads > 16 || spec->encoder_threads < 0 || spec->encoder_threads > 16) {
        set_err(err, err_size, "出力 profile の指定が不正です: %dx%d @ %d/%d", spec->width,
                spec->height, spec->fps_num, spec->fps_den);
        return 1;
    }
    if (spec->timeout_ms <= 0) {
        set_err(err, err_size, "timeout_ms が 0 以下です: %d", spec->timeout_ms);
        return 1;
    }
    for (int i = 0; i < clip_count; i++) {
        if (!clips[i].path || !clips[i].path[0]) {
            set_err(err, err_size, "clip %d のパスが空です", i);
            return 1;
        }
        if (!file_exists_utf8(clips[i].path)) {
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
        if (clips[i].effects_enabled &&
            (clips[i].opacity_keyframe_count <= 0 || !clips[i].opacity_keyframes ||
             clips[i].rect_width <= 0.0 || clips[i].rect_height <= 0.0)) {
            set_err(err, err_size, "clip %d のeffect mappingが不正です", i);
            return 1;
        }
    }

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
            if (attach_export_crop(profile, cut, &clips[i], err, err_size) != 0 ||
                attach_export_affine(profile, cut, &clips[i], producer_in, duration, err,
                                     err_size) != 0) {
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

    /* --- consumer ---------------------------------------------------------- */
    consumer = mlt_factory_consumer(profile, "avformat", out_path);
    if (!consumer) {
        set_err(err, err_size, "avformat consumer を作れません: %s", out_path);
        goto fail;
    }
    /* sequential fast path は video clip だけを扱う。音声が必要な timeline は tractor へ送る。 */
    configure_mp4_consumer(consumer, out_path, spec, 0);

    if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(pp)) != 0) {
        set_err(err, err_size, "consumer を playlist へ接続できません");
        goto fail;
    }
    if (mlt_consumer_start(consumer) != 0) {
        set_err(err, err_size, "consumer を開始できません");
        goto fail;
    }

    /* 完了待ち。timeout を成功扱いにしない。 */
    {
        int waited = 0;
        const int step = 20;
        while (!mlt_consumer_is_stopped(consumer)) {
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
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    consumer = NULL;

    /* --- 出力の検証 -------------------------------------------------------- */
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

    for (int i = 0; i < cut_count; i++)
        mlt_producer_close(cuts[i]);
    for (int i = 0; i < producer_count; i++)
        mlt_producer_close(producers[i]);
    free(cuts);
    free(producers);
    mlt_playlist_close(playlist);
    mlt_profile_close(profile);
    return 0;

fail:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
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

    if (out)
        memset(out, 0, sizeof(*out));
    if (!mvm_mlt_runtime_is_ready() || !clips || !spec || !out_path || !*out_path ||
        clip_count <= 0 || total_duration <= 0 || spec->width <= 0 || spec->height <= 0 ||
        spec->fps_num <= 0 || spec->fps_den <= 0 || spec->video_crf < 0 || spec->video_crf > 51 ||
        spec->timeout_ms <= 0 || spec->render_threads <= 0 || spec->render_threads > 16 ||
        spec->encoder_threads < 0 || spec->encoder_threads > 16) {
        set_err(err, err_size, "M7b tractor export引数が不正です");
        return 1;
    }
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
    track_capacity = video_playlist_count + audio_clip_count;
    playlists = (mlt_playlist*)calloc((size_t)track_capacity, sizeof(*playlists));
    audio_tracks = (int*)calloc((size_t)track_capacity, sizeof(*audio_tracks));
    cursors = (long long*)calloc((size_t)track_capacity, sizeof(*cursors));
    producers = (mlt_producer*)calloc((size_t)clip_count, sizeof(*producers));
    if (!playlists || !audio_tracks || !cursors || !producers) {
        set_err(err, err_size, "track 配列を確保できません");
        goto cleanup;
    }
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (!clip->path || !clip->path[0] || !file_exists_utf8(clip->path) ||
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
                    key->gain > 2.0 * 5.623414) {
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
    cuts = (mlt_producer*)calloc((size_t)cut_capacity, sizeof(*cuts));
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
        if (attach_tractor_clip_filters(profile, cut, clip, track, producer_in, err, err_size) != 0)
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
                                            err, err_size) != 0)
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
        if (plant_export_overlay_affine(profile, tractor, &clips[index], err, err_size) != 0)
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
        consumer = mlt_factory_consumer(profile, "avformat", out_path);
        if (!consumer) {
            set_err(err, err_size, "avformat consumerを作れません");
            goto cleanup;
        }
        configure_mp4_consumer(consumer, out_path, spec, playlist_count > video_playlist_count);
        if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) != 0 ||
            mlt_consumer_start(consumer) != 0) {
            set_err(err, err_size, "tractor consumerを開始できません");
            goto cleanup;
        }
    }
    {
        int waited = 0;
        while (!mlt_consumer_is_stopped(consumer)) {
            if (export_cancel_requested(spec, consumer, total_duration)) {
                failed = MVM_EXPORT_CANCELLED;
                mlt_consumer_stop(consumer);
                set_err(err, err_size, "書き出しをキャンセルしました");
                goto cleanup;
            }
            Sleep(20);
            waited += 20;
            if (waited >= spec->timeout_ms) {
                set_err(err, err_size, "tractor consumerがtimeoutしました");
                goto cleanup;
            }
        }
        if (spec->progress_callback &&
            spec->progress_callback(total_duration, total_duration, spec->progress_opaque) != 0) {
            failed = MVM_EXPORT_CANCELLED;
            mlt_consumer_stop(consumer);
            set_err(err, err_size, "書き出しをキャンセルしました");
            goto cleanup;
        }
    }
    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);
    consumer = NULL;
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
    if (profile)
        mlt_profile_close(profile);
    return failed;
}
