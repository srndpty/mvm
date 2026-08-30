#include "mvm_mlt_export.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_probe.h"
#include "mvm_mlt_runtime.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <framework/mlt.h>

#define MVM_EXPORT_MAX_CLIPS 64

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

int mvm_mlt_export_sequence(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                            const char* out_path, MvmExportResult* out, char* err,
                            size_t err_size) {
    mlt_profile profile = NULL;
    mlt_playlist playlist = NULL;
    mlt_producer producers[MVM_EXPORT_MAX_CLIPS];
    int producer_count = 0;
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

        /* 全長をそのまま追加する。M4 はトリムしない。 */
        if (mlt_playlist_append(playlist, p) != 0) {
            set_err(err, err_size, "clip %d を playlist へ追加できません: %s", i, clips[i].path);
            goto fail;
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
    for (int i = 0; i < producer_count; i++)
        mlt_producer_close(producers[i]);
    if (playlist)
        mlt_playlist_close(playlist);
    if (profile)
        mlt_profile_close(profile);
    return 1;
}
