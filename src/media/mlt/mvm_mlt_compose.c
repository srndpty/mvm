#include "mvm_mlt_compose.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_runtime.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <framework/mlt.h>

#define MAX_PRODUCERS (MVM_BENCH_MAX_TRACKS * MVM_BENCH_MAX_CLIPS)

struct MvmComposeHandle {
    mlt_profile profile;
    mlt_tractor tractor;

    /* playlist と producer はこちらで所有し、close でまとめて解放する。
     * MLT の参照カウントに任せると、どれが生きているか追えなくなる。 */
    mlt_playlist playlists[MVM_BENCH_MAX_TRACKS];
    int playlist_count;

    mlt_producer producers[MAX_PRODUCERS];
    int producer_count;

    mlt_transition transitions[MVM_BENCH_MAX_TRACKS * 2];
    int transition_count;

    mlt_filter filters[MVM_BENCH_MAX_TRACKS * 2];
    int filter_count;

    MvmSeekMode seek_mode;
    long long length;
};

static void set_err(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void add_note(MvmComposeInfo* info, const char* subject, const char* fmt, ...) {
    if (!info || info->note_count >= MVM_COMPOSE_MAX_NOTES)
        return;
    MvmComposeNote* n = &info->notes[info->note_count++];
    snprintf(n->subject, sizeof(n->subject), "%s", subject ? subject : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->detail, sizeof(n->detail), fmt, ap);
    va_end(ap);
}

/* 指定した service が repository に登録されているか。
 * 「無ければ黙って別の方法へ」ではなく、無ければ失敗させるために使う。 */
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

void mvm_mlt_compose_set_seek_mode(MvmComposeHandle* h, MvmSeekMode mode) {
    if (h)
        h->seek_mode = mode;
}

long long mvm_mlt_compose_length(const MvmComposeHandle* h) {
    return h ? h->length : 0;
}

/* ------------------------------------------------------------------------- */
/* 構築                                                                       */
/* ------------------------------------------------------------------------- */

MvmComposeHandle* mvm_mlt_compose_open(const MvmBenchTimeline* tl, MvmComposeInfo* info, char* err,
                                       size_t err_size) {
    if (!mvm_mlt_runtime_is_ready()) {
        set_err(err, err_size, "MLT が初期化されていません");
        return NULL;
    }
    if (!tl) {
        set_err(err, err_size, "timeline が NULL です");
        return NULL;
    }
    if (tl->track_count <= 0 || tl->track_count > MVM_BENCH_MAX_TRACKS) {
        set_err(err, err_size, "track 数が不正です: %d", tl->track_count);
        return NULL;
    }

    MvmComposeHandle* h = (MvmComposeHandle*)calloc(1, sizeof(MvmComposeHandle));
    if (!h) {
        set_err(err, err_size, "メモリを確保できません");
        return NULL;
    }

    /* --- profile ---------------------------------------------------------
     * 実値を必ず検証する。mlt_profile_init は解決失敗時も NULL を返さず
     * 既定値へ黙ってフォールバックする (S1 所見 2)。 */
    h->profile = mlt_profile_init(tl->profile_name);
    if (!h->profile) {
        set_err(err, err_size, "profile を作れません: %s", tl->profile_name);
        goto fail;
    }
    if (h->profile->width != tl->width || h->profile->height != tl->height ||
        h->profile->frame_rate_num != tl->fps_num || h->profile->frame_rate_den != tl->fps_den) {
        set_err(err, err_size,
                "profile '%s' の実値が要求と一致しません: 実 %dx%d @ %d/%d / 要求 %dx%d @ %d/%d。"
                "profile 定義が解決できず既定値へフォールバックしている可能性があります",
                tl->profile_name, h->profile->width, h->profile->height, h->profile->frame_rate_num,
                h->profile->frame_rate_den, tl->width, tl->height, tl->fps_num, tl->fps_den);
        goto fail;
    }
    if (tl->sar_num && tl->sar_den &&
        (h->profile->sample_aspect_num != tl->sar_num ||
         h->profile->sample_aspect_den != tl->sar_den)) {
        set_err(err, err_size, "profile の SAR が要求と一致しません: 実 %d/%d / 要求 %d/%d",
                h->profile->sample_aspect_num, h->profile->sample_aspect_den, tl->sar_num,
                tl->sar_den);
        goto fail;
    }
    if (tl->progressive && !h->profile->progressive) {
        set_err(err, err_size, "profile が progressive ではありません");
        goto fail;
    }

    if (info) {
        info->profile_width = h->profile->width;
        info->profile_height = h->profile->height;
        info->profile_fps_num = h->profile->frame_rate_num;
        info->profile_fps_den = h->profile->frame_rate_den;
        info->profile_sar_num = h->profile->sample_aspect_num;
        info->profile_sar_den = h->profile->sample_aspect_den;
        info->profile_progressive = h->profile->progressive;
    }

    /* --- 必須 service の存在確認 ------------------------------------------
     * 無ければここで失敗させる。構築を進めてから
     * 「なぜか合成されない」となるのが最悪。 */
    mlt_repository repo = mlt_factory_repository();
    mlt_properties producers = mlt_repository_producers(repo);
    mlt_properties filters = mlt_repository_filters(repo);
    mlt_properties transitions = mlt_repository_transitions(repo);

    if (!service_exists(producers, "avformat")) {
        set_err(err, err_size, "必須 producer 'avformat' がありません");
        goto fail;
    }
    if (!service_exists(transitions, "qtblend")) {
        set_err(err, err_size, "必須 transition 'qtblend' がありません");
        goto fail;
    }
    if (!service_exists(transitions, "mix")) {
        set_err(err, err_size, "必須 transition 'mix' がありません");
        goto fail;
    }
    if (!service_exists(filters, "volume")) {
        set_err(err, err_size, "必須 filter 'volume' がありません");
        goto fail;
    }

    int has_text_track = 0;
    for (int i = 0; i < tl->track_count; i++) {
        if (tl->tracks[i].kind == MVM_BENCH_TRACK_TEXT)
            has_text_track = 1;
    }
    if (has_text_track) {
        if (!tl->text_service[0]) {
            set_err(err, err_size, "text_service が指定されていません");
            goto fail;
        }
        if (!service_exists(filters, tl->text_service)) {
            set_err(err, err_size,
                    "text service '%s' が filter として登録されていません。"
                    "別の service へフォールバックはしません",
                    tl->text_service);
            goto fail;
        }
        /* フォントは無言で別フォントへ落とさない。実ファイルの存在を確認する。 */
        if (!tl->font_file[0]) {
            set_err(err, err_size, "font_file が指定されていません");
            goto fail;
        }
        if (!file_exists_utf8(tl->font_file)) {
            set_err(err, err_size,
                    "フォントファイルが存在しません: %s。"
                    "別のフォントへ fallback せず失敗させます",
                    tl->font_file);
            goto fail;
        }
        if (info)
            add_note(info, "font", "%s", tl->font_file);
    }

    /* --- tractor ---------------------------------------------------------- */
    h->tractor = mlt_tractor_new();
    if (!h->tractor) {
        set_err(err, err_size, "tractor を作れません");
        goto fail;
    }
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(h->tractor), "_profile", h->profile, 0, NULL,
                            NULL);

    /* --- トラック --------------------------------------------------------- */
    /* z_order の小さい順に MLT の track index を割り当てる。
     * MLT では index が大きいほど上に合成される。 */
    int order[MVM_BENCH_MAX_TRACKS];
    for (int i = 0; i < tl->track_count; i++)
        order[i] = i;
    for (int i = 0; i < tl->track_count; i++) {
        for (int j = i + 1; j < tl->track_count; j++) {
            if (tl->tracks[order[j]].z_order < tl->tracks[order[i]].z_order) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    for (int ti = 0; ti < tl->track_count; ti++) {
        const MvmBenchTrack* track = &tl->tracks[order[ti]];

        mlt_playlist pl = mlt_playlist_new(h->profile);
        if (!pl) {
            set_err(err, err_size, "playlist を作れません (track %d)", ti);
            goto fail;
        }
        h->playlists[h->playlist_count++] = pl;

        if (track->clip_count <= 0 || track->clip_count > MVM_BENCH_MAX_CLIPS) {
            set_err(err, err_size, "track %d の clip 数が不正です: %d", ti, track->clip_count);
            goto fail;
        }

        long long cursor = 0; /* playlist 上の現在位置 */

        for (int ci = 0; ci < track->clip_count; ci++) {
            const MvmBenchClip* clip = &track->clips[ci];

            if (clip->timeline_in > clip->timeline_out) {
                set_err(err, err_size, "track %d clip %d: timeline_in > timeline_out (%lld > %lld)",
                        ti, ci, clip->timeline_in, clip->timeline_out);
                goto fail;
            }
            long long tl_len = clip->timeline_out - clip->timeline_in + 1;
            if (tl_len <= 0) {
                set_err(err, err_size, "track %d clip %d: 長さが 0 です", ti, ci);
                goto fail;
            }
            if (clip->timeline_in < cursor) {
                set_err(err, err_size,
                        "track %d clip %d: クリップが重なっているか順序が逆です "
                        "(timeline_in=%lld < cursor=%lld)",
                        ti, ci, clip->timeline_in, cursor);
                goto fail;
            }

            /* 先頭の空白 */
            if (clip->timeline_in > cursor) {
                /* mlt_playlist_blank は out を取り、out+1 フレームを作る (実測) */
                mlt_playlist_blank(pl, (mlt_position)(clip->timeline_in - cursor - 1));
                cursor = clip->timeline_in;
            }

            if (track->kind == MVM_BENCH_TRACK_TEXT) {
                /* 文字は color producer に text filter を載せる。
                 * 透明な背景を敷き、その上に文字を描く。 */
                mlt_producer cp = mlt_factory_producer(h->profile, "color", "#00000000");
                if (!cp) {
                    set_err(err, err_size, "track %d clip %d: color producer を作れません", ti, ci);
                    goto fail;
                }
                h->producers[h->producer_count++] = cp;
                mlt_producer_set_in_and_out(cp, 0, (mlt_position)(tl_len - 1));

                mlt_filter tf = mlt_factory_filter(h->profile, tl->text_service, NULL);
                if (!tf) {
                    set_err(err, err_size, "track %d clip %d: text filter '%s' を作れません", ti,
                            ci, tl->text_service);
                    goto fail;
                }
                mlt_properties tp = MLT_FILTER_PROPERTIES(tf);
                /* 実測した property 名を使う (推測しない):
                 * argument / geometry / family / size / fgcolour / bgcolour /
                 * halign / valign / pad / opacity */
                mlt_properties_set(tp, "argument", clip->text);
                mlt_properties_set(tp, "family", clip->font_family);
                mlt_properties_set_int(tp, "size", clip->font_size);
                mlt_properties_set(tp, "fgcolour", clip->fg_colour);
                mlt_properties_set(tp, "bgcolour", clip->bg_colour);
                mlt_properties_set(tp, "halign", clip->halign);
                mlt_properties_set(tp, "valign", clip->valign);
                mlt_properties_set_int(tp, "pad", 8);
                mlt_properties_set_int(tp, "weight", 400);

                char geom[128];
                snprintf(geom, sizeof(geom), "%d/%d:%dx%d", clip->text_x, clip->text_y,
                         clip->text_w, clip->text_h);
                mlt_properties_set(tp, "geometry", geom);

                mlt_producer_attach(cp, tf);
                h->filters[h->filter_count++] = tf;

                if (mlt_playlist_append_io(pl, cp, 0, (mlt_position)(tl_len - 1)) != 0) {
                    set_err(err, err_size, "track %d clip %d: playlist へ追加できません", ti, ci);
                    goto fail;
                }
                if (info) {
                    add_note(info, "text", "service=%s family=%s size=%d geometry=%s",
                             tl->text_service, clip->font_family, clip->font_size, geom);
                }
            } else {
                if (!clip->source[0]) {
                    set_err(err, err_size, "track %d clip %d: source が空です", ti, ci);
                    goto fail;
                }
                if (!file_exists_utf8(clip->source)) {
                    set_err(err, err_size, "track %d clip %d: source がありません: %s", ti, ci,
                            clip->source);
                    goto fail;
                }

                /* [重要] clip ごとに producer を新規に開く。
                 * 同じ producer オブジェクトを複数トラックで共有すると、
                 * 各トラックが同時刻に別位置を要求したときに
                 * 内部の読み取り位置を奪い合う。
                 * playlist への追加自体は共有でも通るので、
                 * 問題は「構築時」ではなく「再生時」に出る。 */
                mlt_producer p = mlt_factory_producer(h->profile, "avformat", clip->source);
                if (!p) {
                    set_err(err, err_size, "track %d clip %d: producer を開けません: %s", ti, ci,
                            clip->source);
                    goto fail;
                }
                h->producers[h->producer_count++] = p;

                long long src_len = (long long)mlt_producer_get_length(p);
                if (clip->source_in < 0 || clip->source_out < clip->source_in) {
                    set_err(err, err_size,
                            "track %d clip %d: source_in/out が不正です (%lld..%lld)", ti, ci,
                            clip->source_in, clip->source_out);
                    goto fail;
                }
                if (clip->source_out >= src_len) {
                    set_err(err, err_size,
                            "track %d clip %d: source_out が素材範囲外です "
                            "(%lld >= length %lld)",
                            ti, ci, clip->source_out, src_len);
                    goto fail;
                }
                long long src_span = clip->source_out - clip->source_in + 1;
                if (src_span != tl_len) {
                    set_err(err, err_size,
                            "track %d clip %d: 素材の長さ %lld とタイムライン長 %lld が一致しません"
                            " (速度変更は Phase 0 の範囲外)",
                            ti, ci, src_span, tl_len);
                    goto fail;
                }

                /* [実測所見] 拡縮配置は qtblend *transition* の rect で行う。
                 *
                 * qtblend の *filter* 版に rect を設定して producer へ attach すると、
                 * そのトラックが合成結果からまるごと消える (エラーは出ない)。
                 * filter と transition を併用してはいけない。 */

                if (mlt_playlist_append_io(pl, p, (mlt_position)clip->source_in,
                                           (mlt_position)clip->source_out) != 0) {
                    set_err(err, err_size, "track %d clip %d: playlist へ追加できません", ti, ci);
                    goto fail;
                }

                /* 音量。mix transition には gain 相当の property が無いことを
                 * 実測で確認済みなので、volume filter を使う。 */
                if (clip->gain_db != 0.0) {
                    mlt_filter vf = mlt_factory_filter(h->profile, "volume", NULL);
                    if (!vf) {
                        set_err(err, err_size, "track %d clip %d: volume filter を作れません", ti,
                                ci);
                        goto fail;
                    }
                    char level[64];
                    snprintf(level, sizeof(level), "%.4f", clip->gain_db);
                    mlt_properties_set(MLT_FILTER_PROPERTIES(vf), "level", level);
                    mlt_producer_attach(p, vf);
                    h->filters[h->filter_count++] = vf;
                    if (info)
                        add_note(info, "volume", "track %d clip %d level=%s dB", ti, ci, level);
                }

                if (info) {
                    add_note(info, "producer", "track %d clip %d avformat in=%lld out=%lld", ti, ci,
                             clip->source_in, clip->source_out);
                }
            }
            cursor = clip->timeline_out + 1;
        }

        mlt_tractor_set_track(h->tractor, MLT_PLAYLIST_PRODUCER(pl), ti);

        /* [実測所見] hide は「トラック (playlist)」に設定する。
         * 内側の clip producer に設定しても効かない。
         *
         * 効かないだけなら気づけるが、実際には音声トラックを 1 本足しただけで
         * 上位トラックの映像合成がまるごと無効化されるという形で表面化した。
         * V2 も文字も消え、出力は V1 だけになる。エラーは一切出ない。
         *
         * 値: 1=映像を隠す, 2=音声を隠す, 3=両方 */
        int hide = 0;
        if (!track->video_enabled)
            hide |= 1;
        if (!track->audio_enabled)
            hide |= 2;
        if (hide)
            mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(pl), "hide", hide);

        if (info) {
            add_note(info, "track", "index=%d kind=%d z_order=%d hide=%d clips=%d", ti,
                     (int)track->kind, track->z_order, hide, track->clip_count);
        }
    }

    /* --- transition ------------------------------------------------------- */
    /* 映像: track 0 を背景に、上のトラックを qtblend で重ねる。
     * 音声: mix で合成する。always_active と sum を立てないと
     *       トラック全域で効かない。 */
    for (int ti = 1; ti < tl->track_count; ti++) {
        const MvmBenchTrack* track = &tl->tracks[order[ti]];

        if (track->kind == MVM_BENCH_TRACK_AUDIO) {
            mlt_transition mx = mlt_factory_transition(h->profile, "mix", NULL);
            if (!mx) {
                set_err(err, err_size, "mix transition を作れません (track %d)", ti);
                goto fail;
            }
            mlt_properties mp = MLT_TRANSITION_PROPERTIES(mx);
            mlt_properties_set_int(mp, "always_active", 1);
            mlt_properties_set_int(mp, "sum", 1);
            mlt_properties_set(mp, "start", "0.5");
            mlt_properties_set(mp, "end", "0.5");
            mlt_transition_set_tracks(mx, 0, ti);
            if (mlt_field_plant_transition(mlt_tractor_field(h->tractor), mx, 0, ti) != 0) {
                set_err(err, err_size, "mix transition を配置できません (track %d)", ti);
                goto fail;
            }
            h->transitions[h->transition_count++] = mx;
            if (info)
                add_note(info, "transition", "mix a_track=0 b_track=%d always_active=1 sum=1", ti);
        } else {
            /* [実測所見] 映像の重ね合わせは qtblend transition + rect で行う。
             *
             * ここまでに試した 3 通りと結果:
             *   1. qtblend transition, rect なし  -> b_track がまったく合成されない
             *      (エラーは出ず、下のトラックだけが出力される)
             *   2. qtblend filter (rect) + transition -> そのトラックが消える
             *   3. composite transition + geometry -> アクセス違反でクラッシュ
             *
             * したがって現状は 1 の rect あり版だけが動く。ただし rect の
             * 幅・高さは拡縮に効かず、等倍の切り出しとして働く
             * (docs/research/composition-notes.md に記録)。
             * 縮小 PiP は未解決であり、M3 は未達である。 */
            mlt_transition qb = mlt_factory_transition(h->profile, "qtblend", NULL);
            if (!qb) {
                set_err(err, err_size, "qtblend transition を作れません (track %d)", ti);
                goto fail;
            }
            mlt_properties qp = MLT_TRANSITION_PROPERTIES(qb);
            mlt_properties_set_int(qp, "always_active", 1);

            const MvmBenchClip* c0 = &track->clips[0];
            char rect[128];
            if (c0->rect_w > 0 && c0->rect_h > 0) {
                snprintf(rect, sizeof(rect), "%g %g %g %g %g", c0->rect_x, c0->rect_y, c0->rect_w,
                         c0->rect_h, c0->opacity);
            } else {
                snprintf(rect, sizeof(rect), "0 0 %d %d %g", h->profile->width, h->profile->height,
                         c0->opacity);
            }
            mlt_properties_set(qp, "rect", rect);
            mlt_properties_set_int(qp, "compositing", 0);
            mlt_properties_set_int(qp, "distort", 0);

            mlt_transition_set_tracks(qb, 0, ti);
            if (mlt_field_plant_transition(mlt_tractor_field(h->tractor), qb, 0, ti) != 0) {
                set_err(err, err_size, "qtblend transition を配置できません (track %d)", ti);
                goto fail;
            }
            h->transitions[h->transition_count++] = qb;
            if (info)
                add_note(info, "transition", "qtblend a_track=0 b_track=%d rect=%s", ti, rect);
        }
    }

    h->length = (long long)mlt_producer_get_length(MLT_TRACTOR_PRODUCER(h->tractor));
    if (h->length <= 0) {
        set_err(err, err_size, "tractor の長さが 0 です");
        goto fail;
    }

    if (info) {
        info->length = h->length;
        info->track_count = tl->track_count;
    }
    return h;

fail:
    mvm_mlt_compose_close(h);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* フレーム取得                                                               */
/* ------------------------------------------------------------------------- */

int mvm_mlt_compose_frame(MvmComposeHandle* h, long long frame, MvmMltImage* img, char* err,
                          size_t err_size) {
    if (!h || !img) {
        set_err(err, err_size, "引数が不正です");
        return 1;
    }
    memset(img, 0, sizeof(*img));

    if (frame < 0) {
        set_err(err, err_size, "負のフレーム番号です: %lld", frame);
        return 1;
    }
    if (frame >= h->length) {
        set_err(err, err_size, "フレーム番号が範囲外です: %lld (length=%lld)", frame, h->length);
        return 1;
    }

    mlt_producer tp = MLT_TRACTOR_PRODUCER(h->tractor);
    mlt_producer_seek(tp, (mlt_position)frame);

    mlt_frame f = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(tp), &f, 0) != 0 || !f) {
        set_err(err, err_size, "フレームを取得できません: %lld", frame);
        return 1;
    }

    mlt_image_format fmt = mlt_image_rgba;
    int w = 0, hgt = 0;
    uint8_t* image = NULL;
    if (mlt_frame_get_image(f, &image, &fmt, &w, &hgt, 0) != 0 || !image || w <= 0 || hgt <= 0) {
        set_err(err, err_size, "画像を取得できません: %lld", frame);
        mlt_frame_close(f);
        return 1;
    }

    size_t bytes = (size_t)w * (size_t)hgt * 4;
    img->rgba = (unsigned char*)malloc(bytes);
    if (!img->rgba) {
        set_err(err, err_size, "メモリを確保できません");
        mlt_frame_close(f);
        return 1;
    }
    memcpy(img->rgba, image, bytes);
    img->width = w;
    img->height = hgt;

    mlt_frame_close(f);
    return 0;
}

int mvm_mlt_compose_audio(MvmComposeHandle* h, long long frame, MvmComposeAudio* out, char* err,
                          size_t err_size) {
    if (!h || !out) {
        set_err(err, err_size, "引数が不正です");
        return 1;
    }
    memset(out, 0, sizeof(*out));

    if (frame < 0 || frame >= h->length) {
        set_err(err, err_size, "フレーム番号が範囲外です: %lld (length=%lld)", frame, h->length);
        return 1;
    }

    mlt_producer tp = MLT_TRACTOR_PRODUCER(h->tractor);
    mlt_producer_seek(tp, (mlt_position)frame);

    mlt_frame f = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(tp), &f, 0) != 0 || !f) {
        set_err(err, err_size, "フレームを取得できません: %lld", frame);
        return 1;
    }

    mlt_audio_format afmt = mlt_audio_float;
    int frequency = h->profile->frame_rate_num ? 48000 : 48000;
    int channels = 2;
    int samples = mlt_audio_calculate_frame_samples((float)h->profile->frame_rate_num /
                                                        (float)h->profile->frame_rate_den,
                                                    frequency, (int)frame);
    void* buffer = NULL;

    if (mlt_frame_get_audio(f, &buffer, &afmt, &frequency, &channels, &samples) != 0 || !buffer ||
        samples <= 0) {
        set_err(err, err_size, "音声を取得できません: %lld", frame);
        mlt_frame_close(f);
        return 1;
    }

    /* mlt_audio_float は planar (チャンネルごとに連続)。
     * 呼び出し側で扱いやすいよう interleaved へ直す。 */
    out->sample_rate = frequency;
    out->channels = channels;
    out->samples = samples;
    out->data = (float*)malloc((size_t)samples * (size_t)channels * sizeof(float));
    if (!out->data) {
        set_err(err, err_size, "メモリを確保できません");
        mlt_frame_close(f);
        return 1;
    }

    const float* src = (const float*)buffer;
    for (int c = 0; c < channels; c++) {
        const float* plane = src + (size_t)c * (size_t)samples;
        for (int s = 0; s < samples; s++)
            out->data[(size_t)s * (size_t)channels + (size_t)c] = plane[s];
    }

    mlt_frame_close(f);
    return 0;
}

void mvm_mlt_audio_free(MvmComposeAudio* a) {
    if (!a)
        return;
    free(a->data);
    a->data = NULL;
    a->samples = 0;
}

void mvm_mlt_compose_close(MvmComposeHandle* h) {
    if (!h)
        return;

    /* tractor は track として設定した playlist を参照している。
     * tractor を先に閉じてから個別に閉じる。 */
    if (h->tractor)
        mlt_tractor_close(h->tractor);

    for (int i = 0; i < h->playlist_count; i++)
        if (h->playlists[i])
            mlt_playlist_close(h->playlists[i]);
    for (int i = 0; i < h->producer_count; i++)
        if (h->producers[i])
            mlt_producer_close(h->producers[i]);

    if (h->profile)
        mlt_profile_close(h->profile);

    free(h);
}
