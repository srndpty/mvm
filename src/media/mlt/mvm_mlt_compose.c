#include "mvm_mlt_compose.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_runtime.h"

#include <math.h>
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

    mlt_filter filters[MVM_BENCH_MAX_TRACKS * MVM_BENCH_MAX_CLIPS];
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

    /* disabled のトラックはグラフから完全に外す。
     * hide で隠すのではなく構築自体から外すので、A/B 差分検証で
     * 「本当にそのトラックが無いときの絵」が得られる。 */
    int built[MVM_BENCH_MAX_TRACKS];
    int built_count = 0;
    for (int i = 0; i < tl->track_count; i++) {
        if (!tl->tracks[order[i]].disabled)
            built[built_count++] = order[i];
    }
    if (built_count <= 0) {
        set_err(err, err_size, "有効なトラックがありません");
        goto fail;
    }

    for (int ti = 0; ti < built_count; ti++) {
        const MvmBenchTrack* track = &tl->tracks[built[ti]];

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
                /* [重要] service には NULL (= loader) を渡す。"avformat" を
                 * 明示してはいけない。
                 *
                 * loader は avformat producer に音声の正規化 filter を付ける。
                 * これが無いと以下が両方起きる (最小構成で実測):
                 *   - AAC/MP4 の音声が壊れる (トーンが消え RMS が 1.6 倍になる)
                 *   - volume filter を付けるとアクセス違反 / heap 破壊で落ちる
                 * どちらも playlist / tractor / mix とは無関係で、
                 * producer -> consumer の最小構成から再現する。
                 * melt が正常なのは既定が loader だからである。 */
                mlt_producer p = mlt_factory_producer(h->profile, NULL, clip->source);
                if (!p) {
                    set_err(err, err_size, "track %d clip %d: producer を開けません: %s", ti, ci,
                            clip->source);
                    goto fail;
                }
                h->producers[h->producer_count++] = p;

                /* stream の無効化を index で明示する。playlist の hide は
                 * 二重防御として別途設定している。
                 *
                 * astream / vstream が設定されている場合はそちらが優先されるため、
                 * 推測で設定せず読み戻した値を記録する。 */
                {
                    mlt_properties pr = MLT_PRODUCER_PROPERTIES(p);
                    if (!track->audio_enabled)
                        mlt_properties_set_int(pr, "audio_index", -1);
                    if (!track->video_enabled)
                        mlt_properties_set_int(pr, "video_index", -1);

                    if (info) {
                        add_note(
                            info, "stream",
                            "track=%s audio_index=%s video_index=%s astream=%s vstream=%s",
                            track->name[0] ? track->name : "(no name)",
                            mlt_properties_get(pr, "audio_index")
                                ? mlt_properties_get(pr, "audio_index")
                                : "(未設定)",
                            mlt_properties_get(pr, "video_index")
                                ? mlt_properties_get(pr, "video_index")
                                : "(未設定)",
                            mlt_properties_get(pr, "astream") ? mlt_properties_get(pr, "astream")
                                                              : "(未設定)",
                            mlt_properties_get(pr, "vstream") ? mlt_properties_get(pr, "vstream")
                                                              : "(未設定)");
                    }
                }

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
                    /* 配列容量を超えたら黙って捨てず失敗させる。
                     * 最大 track 数 x 最大 clip 数を格納できる必要がある。 */
                    if (h->filter_count >= (int)(sizeof(h->filters) / sizeof(h->filters[0]))) {
                        set_err(err, err_size,
                                "filter 配列の容量を超えました (%d)。容量を増やしてください",
                                (int)(sizeof(h->filters) / sizeof(h->filters[0])));
                        goto fail;
                    }
                    mlt_filter vf = mlt_factory_filter(h->profile, "volume", NULL);
                    if (!vf) {
                        set_err(err, err_size, "track %d clip %d: volume filter を作れません", ti,
                                ci);
                        goto fail;
                    }
                    /* [実測所見] volume filter の in/out を明示しないとクラッシュする。
                     *
                     * filter は既定で in=out=0 であり、length が 1 になる。
                     * volume filter は "level" を animated property として
                     * mlt_properties_anim_get_double(props, "level", position, length)
                     * で読むため、position が length を超えた時点で破綻する。
                     * consumer で通しレンダリングすると必ず踏む。
                     *
                     * 症状はアクセス違反であり、gain=0 (filter を付けない) なら
                     * 起きないので「音量を変えた時だけ落ちる」という形で出る。 */
                    mlt_filter_set_in_and_out(vf, 0, (mlt_position)(tl_len - 1));

                    /* "level" は animated property であり、consumer で通し
                     * レンダリングするとクラッシュする (in/out を設定しても解消しない)。
                     * 非 animated の "gain" を dB 文字列で使う。 */
                    char level[64];
                    snprintf(level, sizeof(level), "%.4fdB", clip->gain_db);
                    mlt_properties_set(MLT_FILTER_PROPERTIES(vf), "gain", level);
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
            add_note(info, "track", "index=%d name=%s kind=%d z_order=%d hide=%d clips=%d", ti,
                     track->name[0] ? track->name : "(no name)", (int)track->kind, track->z_order,
                     hide, track->clip_count);
        }
    }

    /* transition の rect を設定する前に length を確定させる。
     * anim_set_rect は length を必要とする。 */
    h->length = (long long)mlt_producer_get_length(MLT_TRACTOR_PRODUCER(h->tractor));
    if (h->length <= 0) {
        set_err(err, err_size, "tractor の長さが 0 です");
        goto fail;
    }

    /* --- transition ------------------------------------------------------- */
    /* 映像: track 0 を背景に、上のトラックを qtblend で重ねる。
     * 音声: mix で合成する。always_active と sum を立てないと
     *       トラック全域で効かない。 */
    for (int ti = 1; ti < built_count; ti++) {
        const MvmBenchTrack* track = &tl->tracks[built[ti]];

        if (track->kind == MVM_BENCH_TRACK_AUDIO) {
            mlt_transition mx = mlt_factory_transition(h->profile, "mix", NULL);
            if (!mx) {
                set_err(err, err_size, "mix transition を作れません (track %d)", ti);
                goto fail;
            }
            mlt_properties mp = MLT_TRANSITION_PROPERTIES(mx);
            mlt_properties_set_int(mp, "always_active", 1);
            mlt_properties_set_int(mp, "sum", 1);

            /* start/end を設定するかどうかは実測で決める (docs 参照)。
             * property を設定できたことではなく、出力 WAV の振幅で判断する。 */
            const char* mix_mode = tl->audio_mix_mode[0] ? tl->audio_mix_mode : "sum";
            if (strcmp(mix_mode, "b_half") == 0) {
                mlt_properties_set(mp, "start", "0.5");
                mlt_properties_set(mp, "end", "0.5");
            } else if (strcmp(mix_mode, "sum") != 0) {
                set_err(err, err_size, "未知の audio_mix_mode: %s", mix_mode);
                mlt_transition_close(mx);
                goto fail;
            }
            mlt_transition_set_tracks(mx, 0, ti);
            if (mlt_field_plant_transition(mlt_tractor_field(h->tractor), mx, 0, ti) != 0) {
                set_err(err, err_size, "mix transition を配置できません (track %d)", ti);
                goto fail;
            }
            h->transitions[h->transition_count++] = mx;
            if (info)
                add_note(info, "transition",
                         "mix a_track=0 b_track=%d always_active=1 sum=1 mode=%s", ti, mix_mode);
        } else {
            /* 映像の重ね合わせは qtblend transition + rect。
             *
             * [重要] rect は文字列で組み立てず mlt_rect の typed API で設定する。
             *
             * 以前は "1260 700 640 360 1" という空白区切りの文字列を渡していた。
             * MLT rect の書式は "X/Y:WxH[:opacity]" であり、空白区切りは
             * 正式な書式ではない。それでも MLT はエラーを返さず、
             * 一部の値だけを拾って「等倍の切り出し」という別の絵を出していた。
             * 縮小されないだけで何かは重なるため、気づきにくい。
             *
             * 設定後は anim_get_rect で読み戻して照合する。
             * 「設定できたつもり」を成功と見なさない。 */
            /* [実測所見] 映像の重ね合わせは affine transition を使う。
             *
             * qtblend transition では拡縮配置ができなかった。typed API
             * (mlt_properties_anim_set_rect) で設定し、anim_get_rect で
             * 読み戻して値が一致することまで確認しても、実際の描画では
             * x のオフセットしか効かず、y・w・h と縮小が無視される。
             * エラーは出ないため「重なってはいる」状態で見過ごしやすい。
             *
             * affine transition は同じ typed rect でそのまま正しく動く。
             * PiP だけでなく、文字トラックの合成も affine で初めて成立した。 */
            const char* vt = tl->video_transition[0] ? tl->video_transition : "affine";
            if (!service_exists(transitions, vt)) {
                set_err(err, err_size, "映像合成 transition '%s' がありません", vt);
                goto fail;
            }
            mlt_transition qb = mlt_factory_transition(h->profile, vt, NULL);
            if (!qb) {
                set_err(err, err_size, "%s transition を作れません (track %d)", vt, ti);
                goto fail;
            }
            mlt_properties qp = MLT_TRANSITION_PROPERTIES(qb);
            /* [重要] always_active ではなく in/out を明示する。
             * always_active=1 のまま in=out=0 にしておくと transition の
             * length が 1 になり、animated property の評価位置が壊れる。 */
            mlt_transition_set_in_and_out(qb, 0, (mlt_position)(h->length - 1));

            const MvmBenchClip* c0 = &track->clips[0];
            mlt_rect want;
            if (c0->rect_w > 0 && c0->rect_h > 0) {
                want.x = c0->rect_x;
                want.y = c0->rect_y;
                want.w = c0->rect_w;
                want.h = c0->rect_h;
            } else {
                want.x = 0;
                want.y = 0;
                want.w = h->profile->width;
                want.h = h->profile->height;
            }
            want.o = c0->opacity;

            int tl_last = (int)(h->length > 0 ? h->length - 1 : 0);
            if (mlt_properties_anim_set_rect(qp, "rect", want, 0, tl_last, mlt_keyframe_discrete) !=
                0) {
                set_err(err, err_size, "rect を設定できません (track %d)", ti);
                goto fail;
            }

            /* 読み戻して照合する。frame 0 と代表フレームの両方を見る。 */
            const int probe_frames[] = {0, 1, 137, tl_last};
            for (size_t pi = 0; pi < sizeof(probe_frames) / sizeof(probe_frames[0]); pi++) {
                int pf = probe_frames[pi];
                if (pf > tl_last)
                    continue;
                mlt_rect got = mlt_properties_anim_get_rect(qp, "rect", pf, tl_last);
                const double eps = 0.001;
                if (fabs(got.x - want.x) > eps || fabs(got.y - want.y) > eps ||
                    fabs(got.w - want.w) > eps || fabs(got.h - want.h) > eps ||
                    fabs(got.o - want.o) > eps) {
                    set_err(err, err_size,
                            "track %d: rect の読み戻しが一致しません (frame %d)。"
                            "設定 %g/%g:%gx%g:%g -> 読戻 %g/%g:%gx%g:%g",
                            ti, pf, want.x, want.y, want.w, want.h, want.o, got.x, got.y, got.w,
                            got.h, got.o);
                    goto fail;
                }
            }

            /* distort=0 でアスペクト比を維持する。
             * 1920x1080 -> 640x360 は同じ 16:9 なので歪まない。 */
            mlt_properties_set_int(qp, "distort", 0);
            mlt_properties_set_int(qp, "compositing", 0);

            mlt_transition_set_tracks(qb, 0, ti);
            if (mlt_field_plant_transition(mlt_tractor_field(h->tractor), qb, 0, ti) != 0) {
                set_err(err, err_size, "%s transition を配置できません (track %d)", vt, ti);
                goto fail;
            }
            h->transitions[h->transition_count++] = qb;
            if (info) {
                add_note(info, "transition",
                         "%s a_track=0 b_track=%d rect=%g/%g:%gx%g:%g (typed, 読戻し照合済)", vt,
                         ti, want.x, want.y, want.w, want.h, want.o);
            }
        }
    }

    if (info) {
        info->length = h->length;
        info->track_count = h->playlist_count;
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

    /* [実測所見] mlt_audio_float は **planar** (チャンネルごとに連続) である。
     * interleaved が欲しいときは mlt_audio_f32le を要求する。
     * planar を interleaved と誤解して読むと、RMS が 1e+34 のような
     * 明らかな異常値になる。値が異常なので気づけたが、もし桁が近ければ
     * 「音は出ている」と誤認しかねない。
     *
     * さらに、要求した format が通るとは限らない。MLT は変換できない場合に
     * 別の format を返す。戻り値の afmt を必ず確認する。 */
    mlt_audio_format afmt = mlt_audio_f32le;
    int frequency = 48000;
    int channels = 2;
    /* [重要] samples は in/out である。0 を渡してはいけない。
     * 0 のまま呼ぶと MLT は 0 サンプル分しか用意しないのに戻り値では
     * 800 を返し、その差分を読んだ結果 heap 破壊と nan を引き起こす。
     * 正しいフレーム内サンプル数を入力として渡す。 */
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
    if (afmt != mlt_audio_f32le) {
        set_err(err, err_size,
                "音声 format が要求と異なります (要求 f32le=%d, 実際 %d)。"
                "誤った解釈を避けるため失敗させます",
                (int)mlt_audio_f32le, (int)afmt);
        mlt_frame_close(f);
        return 1;
    }

    /* [未解決 / 危険]
     * ここで buffer を samples * channels * sizeof(float) として読むと、
     * 値が nan や 1e+33 になり、さらに heap 破壊 (0xC0000374) が発生する。
     *
     * 試した範囲:
     *   - mlt_audio_float (planar) として読む      -> 値が 1e+34
     *   - mlt_audio_f32le (interleaved) を要求      -> format 検査は通るが値は異常
     *   - samples を 0 で渡す / 正しい値で渡す      -> どちらも異常
     *
     * つまり MLT が返すバッファの実サイズと、報告される samples/channels が
     * 一致していない。原因を特定できていない以上、読むこと自体が危険なので
     * 読まずに失敗させる。誤った RMS を「音が出ている」と誤認するより、
     * 検証できていないことを明示する方が安全である。
     *
     * docs/research/composition-notes.md に記録。次バッチの最優先項目。 */
    (void)buffer;
    set_err(err, err_size,
            "音声バッファの解釈が未解決です (frequency=%d channels=%d samples=%d format=%d)。"
            "誤った値を返すより失敗させます",
            frequency, channels, samples, (int)afmt);
    mlt_frame_close(f);
    return 1;

#if 0
    out->sample_rate = frequency;
    out->channels = channels;
    out->samples = samples;
    out->data = (float*)malloc((size_t)samples * (size_t)channels * sizeof(float));
    if (!out->data) {
        set_err(err, err_size, "メモリを確保できません");
        mlt_frame_close(f);
        return 1;
    }
    memcpy(out->data, buffer, (size_t)samples * (size_t)channels * sizeof(float));
#endif

    mlt_frame_close(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* WAV 書き出し (M3 の正式な音声検証経路)                                     */
/* ------------------------------------------------------------------------- */

int mvm_mlt_compose_render_audio(MvmComposeHandle* h, const char* out_path, int timeout_ms,
                                 char* err, size_t err_size) {
    if (!h || !out_path || !*out_path) {
        set_err(err, err_size, "引数が不正です");
        return 1;
    }

    mlt_producer tp = MLT_TRACTOR_PRODUCER(h->tractor);

    /* 出力範囲を明示する。in/out を設定しないと consumer が
     * どこまで書くのかが曖昧になる。 */
    mlt_producer_set_in_and_out(tp, 0, (mlt_position)(h->length - 1));
    mlt_producer_seek(tp, 0);

    mlt_consumer consumer = mlt_factory_consumer(h->profile, "avformat", out_path);
    if (!consumer) {
        set_err(err, err_size, "avformat consumer を作れません: %s", out_path);
        return 1;
    }

    mlt_properties cp = MLT_CONSUMER_PROPERTIES(consumer);
    /* 音声だけを PCM で出す。推測に頼らず全て明示する。 */
    mlt_properties_set(cp, "target", out_path);
    mlt_properties_set(cp, "f", "wav");
    mlt_properties_set(cp, "acodec", "pcm_s16le");
    mlt_properties_set_int(cp, "frequency", 48000);
    mlt_properties_set_int(cp, "channels", 2);
    mlt_properties_set_int(cp, "ar", 48000);
    mlt_properties_set_int(cp, "ac", 2);
    mlt_properties_set_int(cp, "vn", 1);
    mlt_properties_set_int(cp, "real_time", -1);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);

    if (mlt_consumer_connect(consumer, MLT_PRODUCER_SERVICE(tp)) != 0) {
        set_err(err, err_size, "consumer を tractor へ接続できません");
        mlt_consumer_close(consumer);
        return 1;
    }

    if (mlt_consumer_start(consumer) != 0) {
        set_err(err, err_size, "consumer を開始できません");
        mlt_consumer_close(consumer);
        return 1;
    }

    /* 完了待ち。timeout を成功扱いにしない。 */
    int waited = 0;
    const int step = 20;
    while (!mlt_consumer_is_stopped(consumer)) {
        Sleep(step);
        waited += step;
        if (waited >= timeout_ms) {
            mlt_consumer_stop(consumer);
            mlt_consumer_close(consumer);
            set_err(err, err_size, "consumer が %d ms 以内に終了しませんでした (timeout)",
                    timeout_ms);
            return 1;
        }
    }

    mlt_consumer_stop(consumer);
    mlt_consumer_close(consumer);

    /* 空ファイルを成功扱いしない。中身の妥当性は呼び出し側が ffprobe で見る。 */
    wchar_t* w = mvm_utf8_to_wide(out_path);
    if (!w) {
        set_err(err, err_size, "出力パスを変換できません");
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
    mvm_str_free(w);
    if (!ok) {
        set_err(err, err_size, "出力ファイルが生成されていません: %s", out_path);
        return 1;
    }
    unsigned long long size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size == 0) {
        set_err(err, err_size, "出力ファイルが 0 バイトです: %s", out_path);
        return 1;
    }

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
