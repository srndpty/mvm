#include "mvm_mlt_export.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_probe.h"
#include "mvm_mlt_runtime.h"

#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <framework/mlt.h>

#define MVM_EXPORT_MAX_CLIPS 64

static uint64_t gcd_u64(uint64_t left, uint64_t right) {
    while (right != 0) {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int checked_mul_u64(uint64_t left, uint64_t right, uint64_t* out) {
    if (left != 0 && right > UINT64_MAX / left)
        return 0;
    *out = left * right;
    return 1;
}

int mvm_source_boundary_to_producer_boundary(long long source_frame, long long source_fps_num,
                                             long long source_fps_den, int producer_fps_num,
                                             int producer_fps_den, long long* out_frame) {
    if (!out_frame || source_frame < 0 || source_fps_num <= 0 || source_fps_den <= 0 ||
        producer_fps_num <= 0 || producer_fps_den <= 0)
        return 1;
    uint64_t factors[3] = {(uint64_t)source_frame, (uint64_t)producer_fps_num,
                           (uint64_t)source_fps_den};
    uint64_t divisors[2] = {(uint64_t)producer_fps_den, (uint64_t)source_fps_num};
    for (int d = 0; d < 2; ++d) {
        for (int f = 0; f < 3; ++f) {
            uint64_t common = gcd_u64(factors[f], divisors[d]);
            factors[f] /= common;
            divisors[d] /= common;
        }
    }
    uint64_t numerator = 1;
    uint64_t denominator = 1;
    for (int i = 0; i < 3; ++i) {
        if (!checked_mul_u64(numerator, factors[i], &numerator))
            return 1;
    }
    for (int i = 0; i < 2; ++i) {
        if (!checked_mul_u64(denominator, divisors[i], &denominator) || denominator == 0)
            return 1;
    }
    uint64_t converted = numerator / denominator;
    if (converted > INT64_MAX)
        return 1;
    *out_frame = (long long)converted;
    return 0;
}

static void set_err(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
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

static int attach_export_crop(mlt_profile profile, mlt_producer cut, const MvmExportClip* clip,
                              char* err, size_t err_size) {
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
        set_err(err, err_size, "必須filter 'crop'を2 instance作れません");
        return 1;
    }
    mlt_properties props = MLT_FILTER_PROPERTIES(parameters);
    mlt_properties_set_int(props, "active", 0);
    mlt_properties_set_int(props, "use_profile", 1);
    mlt_properties_set_int(props, "left", clip->crop_left);
    mlt_properties_set_int(props, "top", clip->crop_top);
    mlt_properties_set_int(props, "right", clip->crop_right);
    mlt_properties_set_int(props, "bottom", clip->crop_bottom);
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(active), "active", 1);
    if (mlt_producer_attach(cut, parameters) != 0 || mlt_producer_attach(cut, active) != 0) {
        mlt_filter_close(parameters);
        mlt_filter_close(active);
        set_err(err, err_size, "crop filter pairをclipへattachできません");
        return 1;
    }
    mlt_filter_close(parameters);
    mlt_filter_close(active);
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
    mlt_properties_set_double(props, "transition.fix_rotate_z", clip->rotation_degrees);
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
            set_err(err, err_size, "V2 affine opacity keyframeを設定できません");
            return 1;
        }
    }
    mlt_transition_set_tracks(transition, 0, 1);
    if (mlt_field_plant_transition(mlt_tractor_field(tractor), transition, 0, 1) != 0) {
        mlt_transition_close(transition);
        set_err(err, err_size, "V2 affine transitionをV1/V2間へ配置できません");
        return 1;
    }
    mlt_transition_close(transition);
    return 0;
}

int mvm_mlt_export_sequence(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                            const char* out_path, MvmExportResult* out, char* err,
                            size_t err_size) {
    mlt_profile profile = NULL;
    mlt_playlist playlist = NULL;
    mlt_producer producers[MVM_EXPORT_MAX_CLIPS];
    mlt_producer cuts[MVM_EXPORT_MAX_CLIPS];
    int producer_count = 0;
    int cut_count = 0;
    mlt_consumer consumer = NULL;
    mlt_producer pp = NULL;
    long long total = 0;

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
    if (clip_count > MVM_EXPORT_MAX_CLIPS) {
        set_err(err, err_size, "clip 数が上限を超えています: %d (上限 %d)", clip_count,
                MVM_EXPORT_MAX_CLIPS);
        return 1;
    }
    if (spec->width <= 0 || spec->height <= 0 || spec->fps_num <= 0 || spec->fps_den <= 0) {
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
            clips[i].source_in_frame < 0 || clips[i].source_out_frame <= clips[i].source_in_frame) {
            set_err(err, err_size, "clip %d の source range または FPS が不正です", i);
            return 1;
        }
        if (clips[i].effects_enabled &&
            (clips[i].opacity_keyframe_count <= 0 ||
             clips[i].opacity_keyframe_count > MVM_EXPORT_MAX_OPACITY_KEYFRAMES ||
             clips[i].rect_width <= 0.0 || clips[i].rect_height <= 0.0)) {
            set_err(err, err_size, "clip %d のeffect mappingが不正です", i);
            return 1;
        }
    }

    /* --- profile ----------------------------------------------------------
     * mlt_profile_init は解決に失敗しても NULL を返さず既定値へ落ちる。
     * 名前付き profile に頼らず値を直接設定し、設定後に読み直して照合する。 */
    profile = mlt_profile_init(NULL);
    if (!profile) {
        set_err(err, err_size, "profile を作れません");
        return 1;
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
        for (int i = 0; i < clip_count; ++i) {
            if (clips[i].effects_enabled &&
                (!service_exists(mlt_repository_filters(repo), "crop") ||
                 !service_exists(mlt_repository_filters(repo), "affine"))) {
                set_err(err, err_size, "effectに必要なfilter crop/affineがありません");
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
        mlt_producer p = mlt_factory_producer(profile, NULL, clips[i].path);
        if (!p) {
            set_err(err, err_size, "clip %d の producer を開けません: %s", i, clips[i].path);
            goto fail;
        }
        producers[producer_count++] = p;

        if (mlt_producer_get_playtime(p) <= 0) {
            set_err(err, err_size, "clip %d の長さが 0 です: %s", i, clips[i].path);
            goto fail;
        }

        long long producer_in = 0;
        long long producer_out_exclusive = 0;
        if (mvm_source_boundary_to_producer_boundary(
                clips[i].source_in_frame, clips[i].source_fps_num, clips[i].source_fps_den,
                spec->fps_num, spec->fps_den, &producer_in) != 0 ||
            mvm_source_boundary_to_producer_boundary(
                clips[i].source_out_frame, clips[i].source_fps_num, clips[i].source_fps_den,
                spec->fps_num, spec->fps_den, &producer_out_exclusive) != 0 ||
            producer_out_exclusive <= producer_in) {
            set_err(err, err_size, "clip %d の producer frame range を変換できません", i);
            goto fail;
        }
        const long long playtime = (long long)mlt_producer_get_playtime(p);
        if (producer_out_exclusive > playtime) {
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
    {
        mlt_properties cp = MLT_CONSUMER_PROPERTIES(consumer);
        /* 推測に頼らず全て明示する。M4 は固定 profile で音声なし。 */
        mlt_properties_set(cp, "target", out_path);
        mlt_properties_set(cp, "f", "mp4");
        mlt_properties_set(cp, "vcodec", "libx264");
        mlt_properties_set(cp, "preset", "medium");
        mlt_properties_set(cp, "crf", "23");
        mlt_properties_set(cp, "pix_fmt", "yuv420p");
        mlt_properties_set(cp, "movflags", "+faststart");
        mlt_properties_set_int(cp, "an", 1);
        mlt_properties_set_int(cp, "real_time", -1);
        mlt_properties_set_int(cp, "terminate_on_pause", 1);
    }

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
            Sleep(step);
            waited += step;
            if (waited >= spec->timeout_ms) {
                mlt_consumer_stop(consumer);
                set_err(err, err_size, "consumer が %d ms 以内に終了しませんでした (timeout)",
                        spec->timeout_ms);
                goto fail;
            }
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
    if (playlist)
        mlt_playlist_close(playlist);
    if (profile)
        mlt_profile_close(profile);
    return 1;
}

int mvm_mlt_export_two_track(const MvmExportClip* clips, int clip_count, long long total_duration,
                             const MvmExportSpec* spec, const char* out_path, MvmExportResult* out,
                             char* err, size_t err_size) {
    mlt_profile profile = NULL;
    mlt_tractor tractor = NULL;
    mlt_playlist playlists[2] = {NULL, NULL};
    mlt_producer producers[MVM_EXPORT_MAX_CLIPS] = {NULL};
    mlt_producer cuts[MVM_EXPORT_MAX_CLIPS] = {NULL};
    int producer_count = 0;
    int cut_count = 0;
    mlt_consumer consumer = NULL;
    long long cursors[2] = {0, 0};
    int failed = 1;

    if (out)
        memset(out, 0, sizeof(*out));
    if (!mvm_mlt_runtime_is_ready() || !clips || !spec || !out_path || !*out_path ||
        clip_count <= 0 || clip_count > MVM_EXPORT_MAX_CLIPS || total_duration <= 0 ||
        spec->width <= 0 || spec->height <= 0 || spec->fps_num <= 0 || spec->fps_den <= 0 ||
        spec->timeout_ms <= 0) {
        set_err(err, err_size, "M7b tractor export引数が不正です");
        return 1;
    }
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        if (!clip->path || !clip->path[0] || !file_exists_utf8(clip->path) ||
            (clip->video_track != 0 && clip->video_track != 1) || clip->timeline_start_frame < 0 ||
            clip->timeline_duration_frames <= 0 ||
            clip->timeline_start_frame > total_duration - clip->timeline_duration_frames ||
            clip->source_fps_num <= 0 || clip->source_fps_den <= 0 || clip->source_in_frame < 0 ||
            clip->source_out_frame <= clip->source_in_frame || clip->crop_left < 0 ||
            clip->crop_top < 0 || clip->crop_right < 0 || clip->crop_bottom < 0) {
            set_err(err, err_size, "tractor clip %dのmappingが不正です", index);
            return 1;
        }
        if (clip->timeline_start_frame < cursors[clip->video_track]) {
            set_err(err, err_size, "tractor clip %dが同一trackで重複または未sortです", index);
            return 1;
        }
        cursors[clip->video_track] = clip->timeline_start_frame + clip->timeline_duration_frames;
        if ((clip->video_track == 1 || clip->effects_enabled) &&
            (clip->opacity_keyframe_count <= 0 ||
             clip->opacity_keyframe_count > MVM_EXPORT_MAX_OPACITY_KEYFRAMES ||
             clip->rect_width <= 0.0 || clip->rect_height <= 0.0)) {
            set_err(err, err_size, "clip %dのeffect/transition mappingが不正です", index);
            return 1;
        }
        for (int key_index = 0; key_index < clip->opacity_keyframe_count; ++key_index) {
            const MvmExportOpacityKeyframe* key = &clip->opacity_keyframes[key_index];
            if (key->local_frame < 0 || key->local_frame >= clip->timeline_duration_frames ||
                key->opacity < 0.0 || key->opacity > 1.0 ||
                (key_index > 0 &&
                 key->local_frame <= clip->opacity_keyframes[key_index - 1].local_frame)) {
                set_err(err, err_size, "clip %dのopacity key domainが不正です", index);
                return 1;
            }
        }
        if (clip->video_track == 1 &&
            (clip->opacity_keyframes[0].local_frame != 0 ||
             clip->opacity_keyframes[clip->opacity_keyframe_count - 1].local_frame !=
                 clip->timeline_duration_frames - 1)) {
            set_err(err, err_size, "V2 clip %dのtransition-local端keyがありません", index);
            return 1;
        }
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
            !service_exists(mlt_repository_filters(repo), "crop") ||
            !service_exists(mlt_repository_filters(repo), "affine") ||
            !service_exists(mlt_repository_transitions(repo), "affine")) {
            set_err(err, err_size, "tractor exportに必要なcrop/affine/avformatがありません");
            goto cleanup;
        }
    }

    tractor = mlt_tractor_new();
    playlists[0] = mlt_playlist_new(profile);
    playlists[1] = mlt_playlist_new(profile);
    if (!tractor || !playlists[0] || !playlists[1]) {
        set_err(err, err_size, "tractorまたはV1/V2 playlistを作れません");
        goto cleanup;
    }
    cursors[0] = 0;
    cursors[1] = 0;
    for (int index = 0; index < clip_count; ++index) {
        const MvmExportClip* clip = &clips[index];
        const int track = clip->video_track;
        if (clip->timeline_start_frame > cursors[track]) {
            if (mlt_playlist_blank(playlists[track], (mlt_position)(clip->timeline_start_frame -
                                                                    cursors[track] - 1)) != 0) {
                set_err(err, err_size, "track %dへblankを追加できません", track);
                goto cleanup;
            }
            if (out)
                ++out->playlist_blank_count;
        }

        mlt_producer parent = mlt_factory_producer(profile, NULL, clip->path);
        if (!parent || mlt_producer_get_playtime(parent) <= 0) {
            if (parent)
                mlt_producer_close(parent);
            set_err(err, err_size, "clip %dのproducerを開けません", index);
            goto cleanup;
        }
        producers[producer_count++] = parent;
        long long producer_in = 0;
        long long producer_out = 0;
        if (mvm_source_boundary_to_producer_boundary(clip->source_in_frame, clip->source_fps_num,
                                                     clip->source_fps_den, spec->fps_num,
                                                     spec->fps_den, &producer_in) != 0 ||
            mvm_source_boundary_to_producer_boundary(clip->source_out_frame, clip->source_fps_num,
                                                     clip->source_fps_den, spec->fps_num,
                                                     spec->fps_den, &producer_out) != 0 ||
            producer_out - producer_in != clip->timeline_duration_frames ||
            producer_out > (long long)mlt_producer_get_playtime(parent)) {
            set_err(err, err_size, "clip %dのcut尺がtimeline配置尺と一致しません", index);
            goto cleanup;
        }
        mlt_producer cut =
            mlt_producer_cut(parent, (mlt_position)producer_in, (mlt_position)(producer_out - 1));
        if (!cut || mlt_producer_get_playtime(cut) != clip->timeline_duration_frames) {
            if (cut)
                mlt_producer_close(cut);
            set_err(err, err_size, "clip %dの明示cutを作れません", index);
            goto cleanup;
        }
        cuts[cut_count++] = cut;
        if (track == 0 && clip->effects_enabled) {
            if (attach_export_crop(profile, cut, clip, err, err_size) != 0 ||
                attach_export_affine(profile, cut, clip, producer_in,
                                     clip->timeline_duration_frames, err, err_size) != 0)
                goto cleanup;
        } else if (track == 1) {
            /* V2へopaque-black affine filterをattachしてはならない。cropだけをcutへ置く。 */
            if (attach_export_crop(profile, cut, clip, err, err_size) != 0)
                goto cleanup;
        }
        if (mlt_playlist_append(playlists[track], cut) != 0) {
            set_err(err, err_size, "clip %dをtrack %d playlistへ追加できません", index, track);
            goto cleanup;
        }
        cursors[track] = clip->timeline_start_frame + clip->timeline_duration_frames;
    }
    for (int track = 0; track < 2; ++track) {
        if (cursors[track] < total_duration) {
            if (mlt_playlist_blank(playlists[track],
                                   (mlt_position)(total_duration - cursors[track] - 1)) != 0) {
                set_err(err, err_size, "track %dへ末尾blankを追加できません", track);
                goto cleanup;
            }
            if (out)
                ++out->playlist_blank_count;
        }
        mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(playlists[track]), "hide", 2);
        if (mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(playlists[track]), track) != 0) {
            set_err(err, err_size, "track %dをtractorへ設定できません", track);
            goto cleanup;
        }
    }
    for (int index = 0; index < clip_count; ++index) {
        if (clips[index].video_track != 1)
            continue;
        if (plant_export_overlay_affine(profile, tractor, &clips[index], err, err_size) != 0)
            goto cleanup;
        if (out)
            ++out->transition_count;
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
        mlt_properties cp = MLT_CONSUMER_PROPERTIES(consumer);
        mlt_properties_set(cp, "target", out_path);
        mlt_properties_set(cp, "f", "mp4");
        mlt_properties_set(cp, "vcodec", "libx264");
        mlt_properties_set(cp, "preset", "medium");
        mlt_properties_set(cp, "crf", "23");
        mlt_properties_set(cp, "pix_fmt", "yuv420p");
        mlt_properties_set(cp, "movflags", "+faststart");
        mlt_properties_set_int(cp, "an", 1);
        mlt_properties_set_int(cp, "real_time", -1);
        mlt_properties_set_int(cp, "terminate_on_pause", 1);
        if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(output)) != 0 ||
            mlt_consumer_start(consumer) != 0) {
            set_err(err, err_size, "tractor consumerを開始できません");
            goto cleanup;
        }
    }
    {
        int waited = 0;
        while (!mlt_consumer_is_stopped(consumer)) {
            Sleep(20);
            waited += 20;
            if (waited >= spec->timeout_ms) {
                set_err(err, err_size, "tractor consumerがtimeoutしました");
                goto cleanup;
            }
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
            probe.frame_count <= 0) {
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
    failed = 0;

cleanup:
    if (consumer) {
        mlt_consumer_stop(consumer);
        mlt_consumer_close(consumer);
    }
    if (tractor)
        mlt_tractor_close(tractor);
    for (int track = 1; track >= 0; --track)
        if (playlists[track])
            mlt_playlist_close(playlists[track]);
    for (int index = 0; index < cut_count; ++index)
        if (cuts[index])
            mlt_producer_close(cuts[index]);
    for (int index = 0; index < producer_count; ++index)
        if (producers[index])
            mlt_producer_close(producers[index]);
    if (profile)
        mlt_profile_close(profile);
    return failed;
}
