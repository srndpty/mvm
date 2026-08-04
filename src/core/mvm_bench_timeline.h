/*
 * mvm Phase 0 / S5 - 検証用タイムライン記述
 *
 * 位置づけ:
 *   S5 (合成) と S6 (seek / scrub) の検証だけに使う小さな純データ構造。
 *   **本番の Project Model ではない。**
 *
 *   MLT の型も property 名もここには出さない。MLT への変換は
 *   src/media/mlt/mvm_mlt_compose.c だけが行う。
 *   この分離が保てるかどうかが、そのまま M15 (抽象化可能性) の予行演習になる。
 *
 * 意図的に持たないもの:
 *   JSON schema、保存形式、undo、キーフレーム、エフェクトチェーン。
 *   Phase 0 の判定に不要なものは作らない。
 *
 * 固定長配列を使っているのは、Phase 0 の検証シナリオが小さく、
 * 所有権の管理で事故を起こす方が高くつくため。
 */

#ifndef MVM_BENCH_TIMELINE_H
#define MVM_BENCH_TIMELINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_BENCH_MAX_TRACKS 8
#define MVM_BENCH_MAX_CLIPS 8
#define MVM_BENCH_PATH_MAX 1024
#define MVM_BENCH_TEXT_MAX 2048

typedef enum {
    MVM_BENCH_TRACK_VIDEO = 0,
    MVM_BENCH_TRACK_AUDIO = 1,
    MVM_BENCH_TRACK_TEXT = 2
} MvmBenchTrackKind;

typedef struct {
    /* 素材のパス (UTF-8)。TEXT トラックでは空。 */
    char source[MVM_BENCH_PATH_MAX];

    /* タイムライン上の位置。inclusive。 */
    long long timeline_in;
    long long timeline_out;

    /* 素材内の位置。inclusive。TEXT では無視。 */
    long long source_in;
    long long source_out;

    /* 映像の配置。qtblend の rect に渡す "x y w h opacity" 形式を
     * そのまま持たせず、数値で持つ。文字列化は変換側の責務。
     * 全画面なら w=h=0 を指定する。 */
    double rect_x;
    double rect_y;
    double rect_w;
    double rect_h;
    double opacity; /* 0.0 - 1.0 */

    /* 音声 */
    double gain_db; /* 0 = 変更なし */

    /* 文字 */
    char text[MVM_BENCH_TEXT_MAX];
    char font_family[128];
    int font_size;
    char fg_colour[32]; /* "0xRRGGBBAA" */
    char bg_colour[32];
    char halign[16]; /* left / center / right */
    char valign[16]; /* top / middle / bottom */
    int text_x;
    int text_y;
    int text_w;
    int text_h;
} MvmBenchClip;

typedef struct {
    MvmBenchTrackKind kind;

    /* シナリオ内でトラックを指す名前 ("V1" / "V2" / "A1" / "A2" / "T1")。
     * A/B 差分検証で「V2 だけ無効」を指定するために使う。 */
    char name[64];

    /* 合成順。小さいほど下 (背景側)。MLT の track index とは独立に持つ。 */
    int z_order;

    int video_enabled;
    int audio_enabled;

    /* 1 なら、このトラックをグラフから完全に除外する。
     * A/B 差分検証で使う。hide で隠すのではなく構築自体から外すので、
     * 「本当にそのトラックが無いときの絵」が得られる。 */
    int disabled;

    MvmBenchClip clips[MVM_BENCH_MAX_CLIPS];
    int clip_count;
} MvmBenchTrack;

typedef struct {
    char profile_name[64];
    int width;
    int height;
    int fps_num;
    int fps_den;
    int sar_num;
    int sar_den;
    int progressive;

    /* 文字描画に使う service: "qtext" または "dynamictext"。
     * どちらを第一候補にするかは S5 の比較で決める。 */
    char text_service[32];

    /* 映像の重ね合わせに使う transition。既定 "affine"。
     * 拡縮配置がどの service で成立するかを実測で比較するために可変にする。 */
    char video_transition[32];

    /* 音声 mix transition の設定。実測で決めるため可変にする。
     *   "sum"    : sum=1 のみ (start/end を設定しない)。これが正式。
     *   "b_half" : sum=1 + start=0.5 end=0.5。B だけが 0.5 倍される。
     *              0.5A + 0.5B ではない (実測で確認)。実験用に残す。
     * property を読み戻せたことではなく、出力 WAV の測定結果で決める。 */
    char audio_mix_mode[16];

    /* フォントファイルの存在確認に使う。無言で別フォントへ
     * fallback させないため、実ファイルのパスを持つ。 */
    char font_file[MVM_BENCH_PATH_MAX];

    MvmBenchTrack tracks[MVM_BENCH_MAX_TRACKS];
    int track_count;
} MvmBenchTimeline;

#ifdef __cplusplus
}
#endif

#endif /* MVM_BENCH_TIMELINE_H */
