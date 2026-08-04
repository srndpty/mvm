#include "mvm_mlt_audiograph.h"

#include "../../util/mvm_win_utf8.h"
#include "mvm_mlt_runtime.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <framework/mlt.h>

/* 追跡する資源。ケースごとに構成が違うので、生成したものを全部ここへ入れ、
 * 最後にちょうど 1 回ずつ解放する。 */
#define MAX_OBJ 16

typedef struct {
    mlt_profile profile;
    mlt_producer producers[MAX_OBJ];
    int producer_count;
    mlt_playlist playlists[MAX_OBJ];
    int playlist_count;
    mlt_filter filters[MAX_OBJ];
    int filter_count;
    mlt_transition transitions[MAX_OBJ];
    int transition_count;
    mlt_tractor tractor;
} Graph;

static void set_err(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* producer の stream 選択と、実際に読み戻せた値を記録する。
 *
 * astream / vstream が既に設定されている場合は audio_index / video_index
 * より優先されるため、推測で設定せず値を出力する。 */
static void log_producer(FILE* log, const char* track_name, mlt_producer p) {
    if (!log || !p)
        return;
    mlt_properties pr = MLT_PRODUCER_PROPERTIES(p);
    fprintf(log, "  producer track=%-4s resource=%s\n", track_name,
            mlt_properties_get(pr, "resource") ? mlt_properties_get(pr, "resource") : "(null)");
    fprintf(
        log, "    audio_index=%s video_index=%s astream=%s vstream=%s length=%d\n",
        mlt_properties_get(pr, "audio_index") ? mlt_properties_get(pr, "audio_index") : "(未設定)",
        mlt_properties_get(pr, "video_index") ? mlt_properties_get(pr, "video_index") : "(未設定)",
        mlt_properties_get(pr, "astream") ? mlt_properties_get(pr, "astream") : "(未設定)",
        mlt_properties_get(pr, "vstream") ? mlt_properties_get(pr, "vstream") : "(未設定)",
        (int)mlt_producer_get_length(p));
}

/* 環境変数 MVM_AG_SERVICE で producer service を切り替えられるようにする。
 * 既定は loader。"avformat" を明示すると音声が壊れることが実測で分かっている
 * (loader が付ける正規化 filter が無いため)。 */
static const char* producer_service(void) {
    const char* s = getenv("MVM_AG_SERVICE");
    if (s && *s && strcmp(s, "loader") != 0)
        return s;
    return NULL; /* NULL = loader */
}

static mlt_producer open_av(Graph* g, FILE* log, const char* name, const char* path,
                            int disable_audio, int disable_video, char* err, size_t err_size) {
    const char* svc = producer_service();
    mlt_producer p = mlt_factory_producer(g->profile, svc, path);
    if (!p) {
        set_err(err, err_size, "producer を開けません: %s", path);
        return NULL;
    }
    if (log)
        fprintf(log, "  service=%s\n", svc ? svc : "loader");
    mlt_properties pr = MLT_PRODUCER_PROPERTIES(p);

    /* stream の無効化は index で明示する。playlist の hide は二重防御として
     * 別途設定する (どちらが効いているか分けて観測するため)。 */
    if (disable_audio)
        mlt_properties_set_int(pr, "audio_index", -1);
    if (disable_video)
        mlt_properties_set_int(pr, "video_index", -1);

    log_producer(log, name, p);
    g->producers[g->producer_count++] = p;
    return p;
}

static mlt_playlist add_playlist(Graph* g, mlt_producer p, int hide, FILE* log,
                                 const char* track_name) {
    mlt_playlist pl = mlt_playlist_new(g->profile);
    if (!pl)
        return NULL;
    mlt_playlist_append(pl, p);
    if (hide)
        mlt_properties_set_int(MLT_PLAYLIST_PROPERTIES(pl), "hide", hide);
    if (log)
        fprintf(log, "  playlist track=%-4s hide=%d length=%d\n", track_name, hide,
                (int)mlt_producer_get_length(MLT_PLAYLIST_PRODUCER(pl)));
    g->playlists[g->playlist_count++] = pl;
    return pl;
}

static void graph_close(Graph* g) {
    /* 参照はちょうど 1 回ずつ解放する。
     * tractor -> playlist -> producer の順。filter と transition は
     * attach / plant した時点で所有権が移っているものがあるため、
     * ここでは close しない (二重解放を避ける)。 */
    if (g->tractor)
        mlt_tractor_close(g->tractor);
    for (int i = 0; i < g->playlist_count; i++)
        if (g->playlists[i])
            mlt_playlist_close(g->playlists[i]);
    for (int i = 0; i < g->producer_count; i++)
        if (g->producers[i])
            mlt_producer_close(g->producers[i]);
    if (g->profile)
        mlt_profile_close(g->profile);
    memset(g, 0, sizeof(*g));
}

/* consumer で WAV を書き出す */
static int render(mlt_profile profile, mlt_producer src, const char* out_wav, int timeout_ms,
                  FILE* log, char* err, size_t err_size) {
    mlt_producer_set_in_and_out(src, 0, mlt_producer_get_length(src) - 1);
    mlt_producer_seek(src, 0);

    mlt_consumer c = mlt_factory_consumer(profile, "avformat", out_wav);
    if (!c) {
        set_err(err, err_size, "consumer を作れません");
        return 1;
    }
    mlt_properties cp = MLT_CONSUMER_PROPERTIES(c);
    mlt_properties_set(cp, "target", out_wav);
    mlt_properties_set(cp, "f", "wav");
    mlt_properties_set(cp, "acodec", "pcm_s16le");
    mlt_properties_set_int(cp, "frequency", 48000);
    mlt_properties_set_int(cp, "channels", 2);
    mlt_properties_set_int(cp, "ar", 48000);
    mlt_properties_set_int(cp, "ac", 2);
    mlt_properties_set_int(cp, "vn", 1);
    mlt_properties_set_int(cp, "real_time", -1);
    mlt_properties_set_int(cp, "terminate_on_pause", 1);

    if (log)
        fprintf(log, "  consumer avformat f=wav acodec=pcm_s16le 48000/2 vn=1 real_time=-1 "
                     "terminate_on_pause=1\n");

    if (mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(src)) != 0) {
        set_err(err, err_size, "consumer を接続できません");
        mlt_consumer_close(c);
        return 1;
    }
    if (mlt_consumer_start(c) != 0) {
        set_err(err, err_size, "consumer を開始できません");
        mlt_consumer_close(c);
        return 1;
    }
    int waited = 0;
    while (!mlt_consumer_is_stopped(c)) {
        Sleep(20);
        waited += 20;
        if (waited >= timeout_ms) {
            mlt_consumer_stop(c);
            mlt_consumer_close(c);
            set_err(err, err_size, "timeout (%d ms)", timeout_ms);
            return 2; /* timeout は専用コード */
        }
    }
    mlt_consumer_stop(c);
    mlt_consumer_close(c);

    /* consumer が成功を返しても、実ファイルが無い / 0 バイトなら失敗である。
     * 書き込めない出力先を指定した場合、MLT はエラーを返さずに終わる。 */
    wchar_t* w = mvm_utf8_to_wide(out_wav);
    if (!w) {
        set_err(err, err_size, "出力パスを変換できません");
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
    mvm_str_free(w);
    if (!ok) {
        set_err(err, err_size, "出力ファイルが生成されていません: %s", out_wav);
        return 1;
    }
    unsigned long long sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (sz < 1024) {
        set_err(err, err_size, "出力ファイルが小さすぎます (%llu バイト): %s", sz, out_wav);
        return 1;
    }
    return 0;
}

static mlt_filter attach_volume(Graph* g, mlt_producer p, const char* prop, const char* value,
                                int set_in_out, int length, FILE* log) {
    mlt_filter vf = mlt_factory_filter(g->profile, "volume", NULL);
    if (!vf)
        return NULL;
    mlt_properties fp = MLT_FILTER_PROPERTIES(vf);
    mlt_properties_set(fp, prop, value);
    if (set_in_out)
        mlt_filter_set_in_and_out(vf, 0, length - 1);
    mlt_producer_attach(p, vf);
    if (log)
        fprintf(log, "  filter volume %s=%s in_out=%s (読み戻し %s=%s)\n", prop, value,
                set_in_out ? "設定" : "既定", prop,
                mlt_properties_get(fp, prop) ? mlt_properties_get(fp, prop) : "(null)");
    g->filters[g->filter_count++] = vf;
    return vf;
}

static mlt_transition plant_mix(Graph* g, int a_track, int b_track, FILE* log) {
    mlt_transition mx = mlt_factory_transition(g->profile, "mix", NULL);
    if (!mx)
        return NULL;
    mlt_properties mp = MLT_TRANSITION_PROPERTIES(mx);
    /* [重要] sum=1 は output = A + mix*B である。
     * start/end を 0.5 にしても 0.5A + 0.5B にはならず A + 0.5B になる。
     * したがって「両方をそのまま足す」には start/end を設定しない。 */
    mlt_properties_set_int(mp, "always_active", 1);
    mlt_properties_set_int(mp, "sum", 1);
    mlt_transition_set_tracks(mx, a_track, b_track);
    mlt_field_plant_transition(mlt_tractor_field(g->tractor), mx, a_track, b_track);
    if (log)
        fprintf(log,
                "  transition mix a_track=%d b_track=%d always_active=1 sum=1 "
                "(start/end は設定しない)\n",
                a_track, b_track);
    g->transitions[g->transition_count++] = mx;
    return mx;
}

int mvm_mlt_audiograph_run(const char* case_name, const MvmAudioGraphPaths* paths,
                           const char* out_wav, int timeout_ms, FILE* log, char* err,
                           size_t err_size) {
    if (!mvm_mlt_runtime_is_ready()) {
        set_err(err, err_size, "MLT が初期化されていません");
        return 1;
    }

    Graph g;
    memset(&g, 0, sizeof(g));
    g.profile = mlt_profile_init("atsc_1080p_60");
    if (!g.profile) {
        set_err(err, err_size, "profile を作れません");
        return 1;
    }

    if (log)
        fprintf(log, "=== case %s ===\n", case_name);

    int rc = 1;
    mlt_producer src = NULL;

#define FAILG(...)                                                                                 \
    do {                                                                                           \
        set_err(err, err_size, __VA_ARGS__);                                                       \
        graph_close(&g);                                                                           \
        return 1;                                                                                  \
    } while (0)

    /* --- A: producer -> consumer。playlist も tractor も transition も無し --- */
    if (strcmp(case_name, "A") == 0) {
        src = open_av(&g, log, "A1", paths->a1_av, 0, 0, err, err_size);
        if (!src)
            FAILG("A: producer を開けません");

    } else if (strcmp(case_name, "AL") == 0) {
        /* --- AL: A と同じだが producer service を loader にする ---
         * melt の既定は loader であり、avformat を直接使うのとは
         * 付随する正規化 filter の有無が違う。ここが効くかを見る。 */
        mlt_producer p = mlt_factory_producer(g.profile, NULL, paths->a1_av);
        if (!p)
            FAILG("AL: loader producer を開けません");
        log_producer(log, "A1", p);
        if (log)
            fprintf(log, "  (service=loader / mlt_factory_producer の service を NULL)\n");
        g.producers[g.producer_count++] = p;
        src = p;

    } else if (strcmp(case_name, "B") == 0) {
        /* --- B: producer -> playlist -> tractor -> consumer --- */
        mlt_producer p = open_av(&g, log, "A1", paths->a1_av, 0, 0, err, err_size);
        if (!p)
            FAILG("B: producer を開けません");
        mlt_playlist pl = add_playlist(&g, p, 0, log, "A1");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pl), 0);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (strcmp(case_name, "C") == 0) {
        /* --- C: B + video_index=-1 --- */
        mlt_producer p = open_av(&g, log, "A1", paths->a1_av, 0, 1, err, err_size);
        if (!p)
            FAILG("C: producer を開けません");
        mlt_playlist pl = add_playlist(&g, p, 1 /* hide video */, log, "A1");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pl), 0);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (strcmp(case_name, "D") == 0) {
        /* --- D: 別 resource。V1=映像のみ / A1=音声のみ --- */
        mlt_producer pv = open_av(&g, log, "V1", paths->v1_video_only, 1, 0, err, err_size);
        if (!pv)
            FAILG("D: 映像 producer を開けません");
        mlt_producer pa = open_av(&g, log, "A1", paths->a1_audio_only, 0, 1, err, err_size);
        if (!pa)
            FAILG("D: 音声 producer を開けません");
        mlt_playlist plv = add_playlist(&g, pv, 2 /* hide audio */, log, "V1");
        mlt_playlist pla = add_playlist(&g, pa, 1 /* hide video */, log, "A1");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(plv), 0);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pla), 1);
        plant_mix(&g, 0, 1, log);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (strcmp(case_name, "E") == 0) {
        /* --- E: 同一 resource を別 producer で開き stream を分ける --- */
        mlt_producer pv = open_av(&g, log, "V1", paths->a1_av, 1, 0, err, err_size);
        if (!pv)
            FAILG("E: 映像 producer を開けません");
        mlt_producer pa = open_av(&g, log, "A1", paths->a1_av, 0, 1, err, err_size);
        if (!pa)
            FAILG("E: 音声 producer を開けません");
        mlt_playlist plv = add_playlist(&g, pv, 2, log, "V1");
        mlt_playlist pla = add_playlist(&g, pa, 1, log, "A1");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(plv), 0);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pla), 1);
        plant_mix(&g, 0, 1, log);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (strcmp(case_name, "F") == 0 || strcmp(case_name, "G") == 0) {
        /* --- F: A1 + A2 を mix。G: F + A2 に volume -6dB --- */
        mlt_producer pa1 = open_av(&g, log, "A1", paths->a1_audio_only, 0, 1, err, err_size);
        if (!pa1)
            FAILG("%s: A1 producer を開けません", case_name);
        mlt_producer pa2 = open_av(&g, log, "A2", paths->a2_wav, 0, 1, err, err_size);
        if (!pa2)
            FAILG("%s: A2 producer を開けません", case_name);

        if (strcmp(case_name, "G") == 0) {
            /* 順序: producer 作成 -> stream 設定 -> filter 作成 -> attach -> append */
            attach_volume(&g, pa2, "gain", "-6dB", 0, (int)mlt_producer_get_length(pa2), log);
        }

        mlt_playlist pl1 = add_playlist(&g, pa1, 1, log, "A1");
        mlt_playlist pl2 = add_playlist(&g, pa2, 1, log, "A2");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pl1), 0);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(pl2), 1);
        plant_mix(&g, 0, 1, log);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (strcmp(case_name, "H") == 0) {
        /* --- H: 5 トラック相当 (V1/V2 は音声無効、A1/A2 を mix) --- */
        mlt_producer pv1 = open_av(&g, log, "V1", paths->v1_h264, 1, 0, err, err_size);
        mlt_producer pv2 = open_av(&g, log, "V2", paths->v2_hevc, 1, 0, err, err_size);
        mlt_producer pa1 = open_av(&g, log, "A1", paths->v1_h264, 0, 1, err, err_size);
        mlt_producer pa2 = open_av(&g, log, "A2", paths->wav_48k, 0, 1, err, err_size);
        if (!pv1 || !pv2 || !pa1 || !pa2)
            FAILG("H: producer を開けません");

        mlt_playlist p0 = add_playlist(&g, pv1, 2, log, "V1");
        mlt_playlist p1 = add_playlist(&g, pv2, 2, log, "V2");
        mlt_playlist p2 = add_playlist(&g, pa1, 1, log, "A1");
        mlt_playlist p3 = add_playlist(&g, pa2, 1, log, "A2");
        g.tractor = mlt_tractor_new();
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(g.tractor), "_profile", g.profile, 0, NULL,
                                NULL);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(p0), 0);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(p1), 1);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(p2), 2);
        mlt_tractor_set_track(g.tractor, MLT_PLAYLIST_PRODUCER(p3), 3);
        plant_mix(&g, 0, 2, log);
        plant_mix(&g, 0, 3, log);
        src = MLT_TRACTOR_PRODUCER(g.tractor);

    } else if (case_name[0] == 'V') {
        /* --- volume filter の最小切り分け。A2 WAV だけを使う --- */
        mlt_producer p = open_av(&g, log, "A2", paths->a2_wav, 0, 1, err, err_size);
        if (!p)
            FAILG("%s: producer を開けません", case_name);
        int len = (int)mlt_producer_get_length(p);

        if (strcmp(case_name, "V0") == 0) {
            /* filter 無し。基準 */
            src = p;
        } else if (strcmp(case_name, "V1") == 0) {
            /* attach してから playlist へ append */
            attach_volume(&g, p, "gain", "-6dB", 0, len, log);
            mlt_playlist pl = add_playlist(&g, p, 1, log, "A2");
            src = MLT_PLAYLIST_PRODUCER(pl);
        } else if (strcmp(case_name, "V2") == 0) {
            /* append してから attach */
            mlt_playlist pl = add_playlist(&g, p, 1, log, "A2");
            attach_volume(&g, p, "gain", "-6dB", 0, len, log);
            src = MLT_PLAYLIST_PRODUCER(pl);
        } else if (strcmp(case_name, "V3") == 0) {
            /* level + in/out を明示 */
            attach_volume(&g, p, "level", "-6", 1, len, log);
            mlt_playlist pl = add_playlist(&g, p, 1, log, "A2");
            src = MLT_PLAYLIST_PRODUCER(pl);
        } else if (strcmp(case_name, "V4") == 0) {
            /* tractor も playlist も無し。producer + volume -> consumer */
            attach_volume(&g, p, "gain", "-6dB", 0, len, log);
            src = p;
        } else {
            FAILG("未知のケース: %s", case_name);
        }
    } else {
        FAILG("未知のケース: %s", case_name);
    }

    if (!src)
        FAILG("%s: グラフを構築できませんでした", case_name);

    rc = render(g.profile, src, out_wav, timeout_ms, log, err, err_size);
    graph_close(&g);
    return rc;

#undef FAILG
}
