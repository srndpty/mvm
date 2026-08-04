/*
 * mvm Phase 0 / S3 - MLT の API と property を実測で確認する調査用コード
 *
 * 目的:
 *   S5 (5 トラック合成) と S6 (seek / scrub) で使う API・property を、
 *   上流ドキュメントの記述ではなく実際の挙動で確かめる。
 *
 *   S1 と S4 の経験から、MLT については「そう書いてある」ことと
 *   「そう動く」ことを区別しないと危険であることが分かっている。
 *   property 名を推測で採用しない。
 *
 * これは調査用であり、製品コードでも IMediaEngine でもない。
 * 得られた結果は docs/research/mlt-notes.md に記録する。
 */

#include "mvm_mlt_explore.h"

#include "mvm_mlt_runtime.h"

#include <stdlib.h>
#include <string.h>

#include <framework/mlt.h>

static void dump_properties(FILE* out, const char* label, mlt_properties props) {
    if (!props) {
        fprintf(out, "  %s: (NULL)\n", label);
        return;
    }
    int n = mlt_properties_count(props);
    fprintf(out, "\n  --- %s (%d) ---\n", label, n);
    for (int i = 0; i < n; i++) {
        const char* name = mlt_properties_get_name(props, i);
        const char* value = mlt_properties_get_value(props, i);
        fprintf(out, "    %-40s = %s\n", name ? name : "(null)", value ? value : "(null)");
    }
}

int mvm_mlt_explore(const char* video_path, const char* audio_path, FILE* out) {
    if (!mvm_mlt_runtime_is_ready()) {
        fprintf(out, "MLT が初期化されていません\n");
        return 1;
    }

    fprintf(out, "=== S3 調査: MLT %s ===\n", mlt_version_get_string());

    /* profile を明示的に作る。S5 の要求は 1920x1080 / 60fps / SAR 1:1 / progressive。 */
    mlt_profile profile = mlt_profile_init("atsc_1080p_60");
    if (!profile) {
        fprintf(out, "profile を作れません\n");
        return 1;
    }
    fprintf(out, "\n[profile] %dx%d @ %d/%d SAR %d/%d DAR %d/%d progressive=%d colorspace=%d\n",
            profile->width, profile->height, profile->frame_rate_num, profile->frame_rate_den,
            profile->sample_aspect_num, profile->sample_aspect_den, profile->display_aspect_num,
            profile->display_aspect_den, profile->progressive, profile->colorspace);

    /* ------------------------------------------------------------------ */
    /* producer の position / length                                       */
    /* ------------------------------------------------------------------ */
    mlt_producer vp = mlt_factory_producer(profile, "avformat", video_path);
    if (!vp) {
        fprintf(out, "映像 producer を開けません: %s\n", video_path);
        mlt_profile_close(profile);
        return 1;
    }

    fprintf(out, "\n[producer] length=%d in=%d out=%d position=%d\n",
            (int)mlt_producer_get_length(vp), (int)mlt_producer_get_in(vp),
            (int)mlt_producer_get_out(vp), (int)mlt_producer_position(vp));
    fprintf(out, "  playtime=%d fps=%f\n", (int)mlt_producer_get_playtime(vp),
            mlt_producer_get_fps(vp));

    dump_properties(out, "producer properties", MLT_PRODUCER_PROPERTIES(vp));

    /* in/out を絞ると length と playtime がどうなるか。
     * S5 で「素材の一部を切り出す」ために必要な知識。 */
    mlt_producer_set_in_and_out(vp, 30, 149);
    fprintf(out, "\n[producer in/out を 30..149 に設定]\n");
    fprintf(out, "  length=%d in=%d out=%d playtime=%d\n", (int)mlt_producer_get_length(vp),
            (int)mlt_producer_get_in(vp), (int)mlt_producer_get_out(vp),
            (int)mlt_producer_get_playtime(vp));
    mlt_producer_set_in_and_out(vp, 0, -1);

    /* ------------------------------------------------------------------ */
    /* playlist と blank                                                   */
    /* ------------------------------------------------------------------ */
    mlt_playlist pl = mlt_playlist_new(profile);
    fprintf(out, "\n[playlist]\n");
    fprintf(out, "  空の状態: count=%d length=%d\n", mlt_playlist_count(pl),
            (int)mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(pl)));

    /* blank を先頭に置いてからクリップを足す。
     * S5 で「タイムライン上の開始位置をずらす」のに使う。 */
    mlt_playlist_blank(pl, 59); /* 60 フレーム分の空白 (blank の引数は out) */
    mlt_playlist_append_io(pl, vp, 0, 119);
    fprintf(out, "  blank(59) + append(0..119): count=%d length=%d\n", mlt_playlist_count(pl),
            (int)mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(pl)));

    for (int i = 0; i < mlt_playlist_count(pl); i++) {
        mlt_playlist_clip_info info;
        if (mlt_playlist_get_clip_info(pl, &info, i) == 0) {
            fprintf(out, "    clip[%d] start=%d length=%d in=%d out=%d blank=%s\n", i,
                    (int)info.start, (int)info.length, (int)info.frame_in, (int)info.frame_out,
                    mlt_playlist_is_blank(pl, i) ? "yes" : "no");
        }
    }

    /* ------------------------------------------------------------------ */
    /* 同じ producer を 2 つの playlist で共有できるか                      */
    /* ------------------------------------------------------------------ */
    mlt_playlist pl2 = mlt_playlist_new(profile);
    mlt_playlist_append_io(pl2, vp, 0, 59);
    fprintf(out, "\n[producer の共有] 同一 producer を 2 つの playlist に append\n");
    fprintf(out, "  pl length=%d / pl2 length=%d\n",
            (int)mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(pl)),
            (int)mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(pl2)));

    /* ------------------------------------------------------------------ */
    /* tractor と multitrack                                               */
    /* ------------------------------------------------------------------ */
    mlt_tractor tractor = mlt_tractor_new();
    mlt_multitrack mt = mlt_tractor_multitrack(tractor);
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(tractor), "_profile", profile, 0, NULL, NULL);

    mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(pl), 0);
    mlt_tractor_set_track(tractor, MLT_PLAYLIST_PRODUCER(pl2), 1);

    fprintf(out, "\n[tractor]\n");
    fprintf(out, "  multitrack count=%d length=%d\n", mlt_multitrack_count(mt),
            (int)mlt_producer_get_length(MLT_MULTITRACK_PRODUCER(mt)));
    fprintf(out, "  tractor length=%d\n",
            (int)mlt_producer_get_length(MLT_TRACTOR_PRODUCER(tractor)));

    /* hide プロパティ。1=video を隠す, 2=audio を隠す, 3=両方。
     * S5 で「映像トラックの音声を無効化する」のに使うので実値を確認する。 */
    mlt_producer t0 = mlt_tractor_get_track(tractor, 0);
    if (t0) {
        mlt_properties tp = MLT_PRODUCER_PROPERTIES(t0);
        fprintf(out, "  track0 hide (設定前) = %d\n", mlt_properties_get_int(tp, "hide"));
        mlt_properties_set_int(tp, "hide", 2);
        fprintf(out, "  track0 hide (設定後) = %d\n", mlt_properties_get_int(tp, "hide"));
        mlt_properties_set_int(tp, "hide", 0);
    }

    /* ------------------------------------------------------------------ */
    /* transition の a_track / b_track                                     */
    /* ------------------------------------------------------------------ */
    fprintf(out, "\n[transition]\n");
    mlt_transition qtblend = mlt_factory_transition(profile, "qtblend", NULL);
    if (qtblend) {
        mlt_transition_set_tracks(qtblend, 0, 1);
        fprintf(out, "  qtblend: a_track=%d b_track=%d\n", mlt_transition_get_a_track(qtblend),
                mlt_transition_get_b_track(qtblend));
        dump_properties(out, "qtblend properties", MLT_TRANSITION_PROPERTIES(qtblend));
        mlt_transition_close(qtblend);
    } else {
        fprintf(out, "  qtblend を作れません\n");
    }

    mlt_transition mix = mlt_factory_transition(profile, "mix", NULL);
    if (mix) {
        mlt_transition_set_tracks(mix, 0, 1);
        fprintf(out, "  mix: a_track=%d b_track=%d\n", mlt_transition_get_a_track(mix),
                mlt_transition_get_b_track(mix));
        dump_properties(out, "mix properties", MLT_TRANSITION_PROPERTIES(mix));
        mlt_transition_close(mix);
    } else {
        fprintf(out, "  mix を作れません\n");
    }

    /* ------------------------------------------------------------------ */
    /* 文字 producer / filter                                              */
    /* ------------------------------------------------------------------ */
    fprintf(out, "\n[text services]\n");

    mlt_filter dyn = mlt_factory_filter(profile, "dynamictext", NULL);
    if (dyn) {
        dump_properties(out, "dynamictext properties", MLT_FILTER_PROPERTIES(dyn));
        mlt_filter_close(dyn);
    } else {
        fprintf(out, "  dynamictext を作れません\n");
    }

    mlt_filter qtext = mlt_factory_filter(profile, "qtext", NULL);
    if (qtext) {
        dump_properties(out, "qtext properties", MLT_FILTER_PROPERTIES(qtext));
        mlt_filter_close(qtext);
    } else {
        fprintf(out, "  qtext を作れません\n");
    }

    /* qtext は producer 版もある。文字だけのトラックを作るならこちらが自然。 */
    mlt_producer qtextp = mlt_factory_producer(profile, "qtext", NULL);
    if (qtextp) {
        fprintf(out, "\n  qtext producer: length=%d\n", (int)mlt_producer_get_length(qtextp));
        dump_properties(out, "qtext producer properties", MLT_PRODUCER_PROPERTIES(qtextp));
        mlt_producer_close(qtextp);
    } else {
        fprintf(out, "\n  qtext producer を作れません\n");
    }

    /* 音量 */
    mlt_filter vol = mlt_factory_filter(profile, "volume", NULL);
    if (vol) {
        dump_properties(out, "volume properties", MLT_FILTER_PROPERTIES(vol));
        mlt_filter_close(vol);
    }

    /* affine (V7 で使う。S5 では縮小配置に使えるか確認) */
    mlt_filter affine = mlt_factory_filter(profile, "affine", NULL);
    if (affine) {
        dump_properties(out, "affine properties", MLT_FILTER_PROPERTIES(affine));
        mlt_filter_close(affine);
    }

    /* qtblend の filter 版。transition 版の rect は「切り出し」として働き
     * 拡縮しないことが実測で分かったため、拡縮配置に使えるか確認する。 */
    mlt_filter qbf = mlt_factory_filter(profile, "qtblend", NULL);
    if (qbf) {
        dump_properties(out, "qtblend filter properties", MLT_FILTER_PROPERTIES(qbf));
        mlt_filter_close(qbf);
    } else {
        fprintf(out, "\n  qtblend filter を作れません\n");
    }

    /* 音声 producer */
    if (audio_path && *audio_path) {
        mlt_producer ap = mlt_factory_producer(profile, "avformat", audio_path);
        if (ap) {
            fprintf(out, "\n[音声 producer] length=%d\n", (int)mlt_producer_get_length(ap));
            mlt_producer_close(ap);
        }
    }

    mlt_tractor_close(tractor);
    mlt_playlist_close(pl2);
    mlt_playlist_close(pl);
    mlt_producer_close(vp);
    mlt_profile_close(profile);
    return 0;
}
