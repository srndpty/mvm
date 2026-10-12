/*
 * mvm M4 - clip 列を 1 本の MP4 へ逐次書き出す
 *
 * 位置づけ:
 *   M4 が要求するのは「clip を順に並べて 1 本の MP4 にする」ことだけである。
 *   トラック合成・トリム・トランジションは対象外なので、
 *   mvm_mlt_compose (tractor + transition) は使わず、
 *   1 本の mlt_playlist に producer を全長のまま append するだけにする。
 *   in/out のフレーム算術が消えるので失敗要因も消える。
 *
 *   consumer の駆動手順 (in/out の明示 / polling / timeout を成功にしない /
 *   0 バイト検出 / rename しない契約) は
 *   mvm_mlt_compose_render_audio と同一である。
 *
 * MLT の型はこのヘッダに一切出さない。
 */

#ifndef MVM_MLT_EXPORT_H
#define MVM_MLT_EXPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    long long local_frame;
    double opacity;
} MvmExportOpacityKeyframe;

typedef struct {
    long long local_frame;
    double gain;
} MvmExportGainKeyframe;

typedef struct {
    long long local_frame;
    double crop_left, crop_top, crop_right, crop_bottom;
    double rect_x, rect_y, rect_width, rect_height;
    double rotation_degrees, shear_degrees;
} MvmExportMotionFrame;

typedef struct {
    const char* path; /* UTF-8。実在する動画ファイル */
    long long source_fps_num;
    long long source_fps_den;
    long long source_frame_count;
    long long source_in_frame;  /* inclusive、素材固有 frame domain */
    long long source_out_frame; /* exclusive、素材固有 frame domain */
    /* producer (profile fps) の cut [in, out)。呼び出し側が Project の対応
     * (core/source_frame_mapping.h) から決める。ここで換算し直さない。 */
    long long producer_in_frame;
    long long producer_out_frame;
    /* cut の後ろを最終 frame で埋める数。tractor 経路だけが受け付ける。 */
    long long tail_padding_frames;
    /* 再生速度 (約分済みの正の有理数)。1/1 以外は timewarp producer で開き、
     * producer の位置は「速度込みの実効 fps」で数える (producer_in/out もその位置)。
     * preserve_pitch が非0なら timewarp の pitch を保つ。 */
    long long speed_num;
    long long speed_den;
    int preserve_pitch;
    int is_frame_hold;
    /* 保持する producer 位置。保持元 clip の速度 (hold_speed_num/den) の timewarp で数える。
     * 0/0 (未設定) は等速として扱わず拒否する。 */
    long long hold_position;
    long long hold_speed_num;
    long long hold_speed_den;
    double mixer_pan;   /* -1 (左) 〜 +1 (右)。ステレオのバランス */
    int is_audio;       /* 非0なら映像を隠して独立audio trackとしてmixする */
    int is_still_image; /* 非0なら明示した qimage producer で透過 PNG を開く (文字・画像) */
    int effects_enabled;
    const MvmExportMotionFrame* motion_frames;
    int motion_frame_count;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;
    double rect_x;
    double rect_y;
    double rect_width;
    double rect_height;
    double rotation_degrees;
    /* 縦横の拡大率が違う回転を剛体回転にする補正 (affine の fix_shear_x)。0 なら補正なし。 */
    double shear_degrees;
    const MvmExportOpacityKeyframe* opacity_keyframes;
    int opacity_keyframe_count;
    const MvmExportGainKeyframe* gain_keyframes;
    int gain_keyframe_count;
    int video_track; /* 映像 layer。0 が最下層 (V1)。トランジションの incoming は上の layer に積む
                        (上限なし) */
    long long timeline_start_frame;
    long long timeline_duration_frames;
    /* 非0なら素材の余白を不透明な黒で埋めてから上の layer として重ねる (クロスディゾルブの
     * incoming)。余白の所でも下の layer が 1 - 不透明度に減る。video_track > 0 のときだけ使う。 */
    int opaque_backdrop;
    /* 非0なら path は透過 PNG の連番 ("...%05d.png"、00000 から) で、producer の位置 n に
     * n 番の PNG を出す (qimage、1 枚 1 frame)。is_still_image と同時に指定する。
     * 数式の Write に使う (docs/math-clips.md P1-0 で frame 単位の対応を確認した)。 */
    int is_image_sequence;
    /* 試作 (既定は 0 = 使わない)。source_width が正なら、path の画像は出力と同じ大きさの画像の
     * [source_x, source_x + source_width) x [source_y, source_y + source_height) だけを切り出した
     * ものとして扱い、配置の矩形をその分だけずらして同じ位置へ置く。等倍・回転なし・crop なしの
     * 上位 track の静止画だけが使える (それ以外は拒否する)。rect_* は切り出す前の全体の矩形の
     * まま渡す。 */
    int source_x;
    int source_y;
    int source_width;
    int source_height;
    /* 試作 (P3-3A、診断専用。既定は NULL = 使わない)。NULL でなければ、この clip は「静止の数式で、
     * 画像はこの準備済みの画像 (上位の層の型。この層は中身を見ない) と同じ」と申告されている。
     * MvmExportObserver::diagnostic_math_group が非 0 で、配置が等倍・回転なし・crop なしのとき
     * だけ、affine transition の代わりに数式 group の transition でまとめて合成する。それ以外は
     * 無視して従来の経路のまま書き出す。呼び出し側は書き出しの関数から戻るまで保持する。 */
    const void* math_group_raster;
} MvmExportClip;

/* 数式 group の 1 layer (合成の順に並ぶ。先頭が一番下)。 */
typedef struct {
    const void* raster; /* MvmExportClip::math_group_raster */
    double rect_x;
    double rect_y;
    double opacity;
} MvmExportMathGroupLayer;

/* rgba (width x height、行の間隔 stride) へ layers を下から順に合成する。0 で成功。
 * 失敗 (入力が対象外) のときは rgba へ何も書かず、非 0 を返して err に理由を書く。
 * render worker から並行に呼ばれる。 */
typedef int (*MvmExportMathGroupCompose)(void* opaque, unsigned char* rgba, int width, int height,
                                         size_t stride, const MvmExportMathGroupLayer* layers,
                                         int count, int minimum_destination_alpha, char* err,
                                         size_t err_size);
/* minimum_destination_alpha: 合成を始める前の rgba の alpha の下限。-1 なら合成器の契約の値
 * (実験用の指定が無いとき)。 */

/* 合成の直前の下地 (group の下にある全 layer を重ねた RGBA) を見せる。書いてはならない。
 * frame は timeline の絶対 frame、group は group の番号 (1 始まり)、layers はその frame で見えて
 * いる member。render worker から並行に呼ばれる。 */
typedef void (*MvmExportMathGroupProbe)(void* opaque, long long frame, int group,
                                        const unsigned char* rgba, int width, int height,
                                        size_t stride, const MvmExportMathGroupLayer* layers,
                                        int count);

/* --- 書き出しの観測 (診断用) -------------------------------------------------
 * 「どの段階まで進んだか」を、出力 file の大きさではなく各境界の通過回数で見るために使う。
 * 呼び出し側が 0 で初期化して MvmExportSpec::observer へ渡す。NULL なら何も記録せず、MLT の
 * graph にも手を入れない (製品の書き出しは NULL のまま)。
 * 記録は counter と、上限のある抜き取り (各境界の最初の 4 回と 2 の冪の回) だけ。frame ごとに
 * I/O や lock をしない。render worker・consumer thread から同時に書くので、読む側は途中の値を
 * 読むことがある (診断の表示用。合否の判定には書き出しが終わってからの値を使う)。 */
typedef enum {
    MVM_EXPORT_OBSERVE_FRAME_REQUEST = 0, /* 最終 producer への frame 要求 (get_frame の入口) */
    MVM_EXPORT_OBSERVE_FRAME_RETURN,      /* 同、戻り (この時点では画素は未合成) */
    MVM_EXPORT_OBSERVE_IMAGE_REQUEST,     /* 合成後の画像の要求 (render worker) */
    MVM_EXPORT_OBSERVE_IMAGE_RETURN,      /* 同、戻り (全 layer の合成が終わった) */
    MVM_EXPORT_OBSERVE_VALIDATOR_ENTRY,   /* rgba_callback の入口 */
    MVM_EXPORT_OBSERVE_VALIDATOR_RETURN,
    MVM_EXPORT_OBSERVE_ENCODER_SUBMIT, /* consumer が encoder へ渡す直前 (consumer-frame-show) */
    /* 次の 2 つは MLT の外 (FFmpeg) の境界。この層は記録せず、呼び出し側が
     * mvm_mlt_export_observe で記録する。0 のままなら「未観測」であって「0 件」ではない。 */
    MVM_EXPORT_OBSERVE_ENCODED_PACKET,
    MVM_EXPORT_OBSERVE_MUXED_PACKET,
    MVM_EXPORT_OBSERVE_POINT_COUNT
} MvmExportObservePoint;

typedef enum {
    MVM_EXPORT_STAGE_IDLE = 0,
    MVM_EXPORT_STAGE_GRAPH_BUILD,    /* producer・filter・transition の構築 */
    MVM_EXPORT_STAGE_CONSUMER_START, /* encoder と muxer の初期化 */
    MVM_EXPORT_STAGE_RENDER,         /* consumer の完了待ち */
    MVM_EXPORT_STAGE_CONSUMER_CLOSE, /* encoder の flush と trailer */
    MVM_EXPORT_STAGE_OUTPUT_VERIFY,  /* 出力 file の probe */
    MVM_EXPORT_STAGE_DONE,
    MVM_EXPORT_STAGE_FAILED,
    MVM_EXPORT_STAGE_COUNT
} MvmExportStage;

typedef struct {
    volatile long long count; /* 累計 */
    volatile long long first_ns;
    volatile long long first_frame;
    volatile long first_thread;
    volatile long long last_ns;
    volatile long long last_frame;
    volatile long last_thread;
} MvmExportObservePointState;

typedef struct {
    int point;
    int stage;
    int cancel_requested;
    long thread;
    long long ordinal; /* その境界で何回目か (1 始まり) */
    long long frame;
    long long ns;
    volatile long ready; /* 非0になってから上の値を読む (書いている途中の枠を読まない) */
} MvmExportObserveSample;

#define MVM_EXPORT_OBSERVE_SAMPLE_CAPACITY 256

typedef struct MvmExportObserver {
    MvmExportObservePointState points[MVM_EXPORT_OBSERVE_POINT_COUNT];
    volatile long stage;
    volatile long long stage_ns[MVM_EXPORT_STAGE_COUNT]; /* 各段階へ入った時刻 (0 は未到達) */
    volatile long cancel_requested;
    volatile long long consumer_position; /* 完了待ちの間に読んだ consumer の位置 */
    volatile long long poll_count;
    volatile long consumer_stopped;
    /* animation へ渡された値の数 (clip の frame 数 × 属性数) と、実際に置いた keyframe の数。
     * 値の変わらない区間は両端だけを置く (mvm_mlt_export.c)。 */
    volatile long long animation_samples;
    volatile long long animation_keyframes;
    /* 合成後の画像の要求から戻りまでの時間の合計 (全 render worker の和、ns)。検査の callback は
     * 含まない。1 frame の合成コストを、検査や encoder の時間と分けて比べるために使う。 */
    volatile long long image_ns_total;
    /* 最終 producer の get_frame の時間の合計 (ns)。tractor が全 track と全 transition を通って
     * frame を組み立てる時間で、画素の合成は含まない。 */
    volatile long long frame_ns_total;
    volatile long sample_count; /* 採ろうとした数。CAPACITY を超えた分は捨てている */
    MvmExportObserveSample samples[MVM_EXPORT_OBSERVE_SAMPLE_CAPACITY];
    /* 入力: 非0なら muxer に packet ごとの debug 記録 (fdebug=ts) を出させる。記録を数えるのは
     * 呼び出し側 (av_log の callback)。出力の内容は変えない。 */
    int mux_trace;
    /* 入力 (診断用。0 が製品の動作): 値の変わらない区間で落とす keyframe のうち、index がこの値の
     * 倍数のものを残す。1 なら全 frame を keyframe にする (間引く前と同じ)。残すのは前後と同じ値の
     * keyframe だけなので、どの frame で評価しても値は変わらない。keyframe の数だけを変えて、
     * 合成の時間への影響を測るために使う。 */
    int diagnostic_keyframe_stride;
    /* 入力 (診断用。count が 0 なら全体): graph は Project 全体のまま、
     * [diagnostic_first_frame, diagnostic_first_frame + diagnostic_frame_count) だけを書き出す。
     * tractor 経路だけが受け付ける。出力は Project の書き出しとしては不完全なので、検証用に限る。
     */
    long long diagnostic_first_frame;
    long long diagnostic_frame_count;
    /* 入力 (試作。0 が製品の動作): 上位 track の clip の affine transition を、値の変わらない区間と
     * 変わる区間に分けて植える。評価される値は変えない (mvm_mlt_export.c)。 */
    int diagnostic_split_transitions;
    /* 入力 (試作の分け方。0 は既定値 8 / 64 / 無制限)。
     *   static_run   同じ値がこの frame 数以上続く所を「変わらない区間」にする
     *   animated_run 「変わる区間」の長さの上限
     *   static_chunk 「変わらない区間」の長さの上限。出力と keyframe の複雑さを変えずに transition
     * の 総数だけを増やして、transition 1 つの固定費を測るために使う */
    int diagnostic_split_static_run;
    int diagnostic_split_animated_run;
    int diagnostic_split_static_chunk;
    /* 入力 (比較の対照用。0 以外は意図的に壊す): 1 = keyframe の位置を 1 frame ずらす
     * (区間の先頭の keyframe
     * が無くなり、書き出しそのものが失敗する。画素の比較の対照にはならない)、 2 = keyframe を 1
     * つおきに落とす、3 = 不透明度を半分にする、 4 = 各 frame に 1 つ後の frame の値を置く
     * (animation としては正しく、対応だけが誤り)。 */
    int diagnostic_split_fault;
    /* 植えた上位 track の affine transition の数。 */
    volatile long long overlay_transitions;

    /* --- 数式 group の transition (試作、P3-3A。入力がすべて 0 / NULL なら製品の動作) ---
     * 入力: 非 0 なら、math_group_raster を持つ対象の clip のうち、下に従来の経路の layer が
     * 重ならないものを 1 つの transition でまとめて合成する (それ以外は従来の経路へ残す)。compose
     * が NULL なら書き出しを拒否する。tractor 経路だけ。 */
    int diagnostic_math_group;
    MvmExportMathGroupCompose diagnostic_math_group_compose;
    void* diagnostic_math_group_opaque;
    /* 入力 (比較の対照用。0 以外は意図的に壊す): 1 = group の 2 frame 目の合成を飛ばす、
     * 2 = 1 つ後の frame の値を使う、3 = 不透明度を半分にする、4 = 下地に不透明でない画素を作る
     * (合成器が拒否し、書き出しが失敗しなければならない)、5 = 別の member の画像を使う、
     * 6 = 一番上の layer を落とす。
     * (4 は P3-3B から alpha 0 の画素を作る。合成器の下限が 1 になったため。) */
    int diagnostic_math_group_fault;
    /* 入力 (P3-3B)。
     *   diagnostic_math_group  1 = まとめて合成する / 2 = 測るだけ (member は従来の経路のまま、
     *     group の位置に何も書かない transition を植えて probe を呼ぶ。出力は製品と同じになる)。
     *   rule  0 = 下に従来の layer が時間的に重ならない clip だけを 1 つの group にする (P3-3A)。
     *         1 = 合成の順だけを守る。従来の layer と時間が重なる所で group を切り、複数の group に
     *             する。この場合、group の下地は従来の layer や前の group が作った画素を含む。
     *   alpha_override / alpha_floor  非 0 なら、合成器の契約の下限の代わりに alpha_floor を使う
     *     (下限の根拠を調べる実験用)。
     *   probe  合成の直前の下地を見せる callback (NULL なら呼ばない)。
     *   assignment  clip ごとの結果を書く配列 (clip の数以上の長さ。NULL なら書かない):
     *     正 = group の番号、0 = 申告なし、-1 = 配置が対象外、-2 = 下に従来の layer が重なる (rule
     * 0)。 raster_budget_bytes  準備済みの画像の byte 数の上限 (0 は上限なし)。超えたら上位の層が
     *     書き出しを失敗にする。 */
    int diagnostic_math_group_rule;
    int diagnostic_math_group_alpha_override;
    int diagnostic_math_group_alpha_floor;
    MvmExportMathGroupProbe diagnostic_math_group_probe;
    void* diagnostic_math_group_probe_opaque;
    int* diagnostic_math_group_assignment;
    int diagnostic_math_group_assignment_capacity;
    long long diagnostic_math_group_raster_budget_bytes;
    /* 出力: 書き出しの関数へ入った時点の process の working set (byte)。画像の準備の後の値。 */
    long long math_group_working_set_at_export;
    /* 出力。最初の 2 つは画像を準備する上位の層が書く。 */
    long long math_group_raster_bytes;            /* 準備済みの画像が持つ画素の byte 数の合計 */
    long long math_group_prepare_ns;              /* 画像の準備 (前処理と範囲の走査) の時間の合計 */
    volatile long long math_group_transitions;    /* 植えた group の transition の数 */
    volatile long long math_group_clips;          /* まとめた clip の数 */
    volatile long long math_group_retained_clips; /* 申告されたが従来の経路へ残した clip の数 */
    /* 残した clip の内訳: 配置が対象外 (拡縮・回転・crop など) / 配置は対象だが、下に従来の経路の
     * layer が重なる。 */
    volatile long long math_group_retained_geometry;
    volatile long long math_group_retained_above;
    char math_group_retained_reason[64];        /* 最初に残した clip の理由 */
    volatile long long math_group_callbacks;    /* 画像の callback の回数 */
    volatile long long math_group_frames;       /* 合成を行って画像を差し替えた回数 */
    volatile long long math_group_layer_frames; /* 合成した layer の延べ数 */
    volatile long long math_group_upstream_ns;  /* 下の frame の画像を得る時間 (group の外) */
    volatile long long math_group_copy_ns;      /* 合成先の確保と複製 */
    volatile long long math_group_compose_ns;   /* compose の呼び出し (下地の検査を含む) */
    volatile long long math_group_callback_ns;  /* callback 全体から upstream を引いた時間 */
    volatile long long math_group_errors;       /* 0 でなければ書き出しは失敗する */
    volatile long math_group_error_ready;
    long long math_group_error_frame;
    char math_group_error[256]; /* 最初の失敗 (error_ready が非 0 になってから読む) */
} MvmExportObserver;

/* 生きている数式 group の transition の数と、実行中の画像 callback の数。書き出しの関数から
 * 戻った後はどちらも 0 でなければならない (0 でなければ書き出しは失敗を返す)。 */
long long mvm_mlt_math_group_live(void);
long long mvm_mlt_math_group_active(void);

/* 単調増加の時刻 (QueryPerformanceCounter、ns)。 */
long long mvm_mlt_export_observer_now_ns(void);
/* observer が NULL なら何もしない。どの thread から呼んでもよい。 */
void mvm_mlt_export_observe(MvmExportObserver* observer, int point, long long frame);

typedef struct {
    /* 呼び出し側が必ず明示する。既定値を推測しない。 */
    int width;
    int height;
    int fps_num;
    int fps_den;
    /* libx264 CRF。0..51 の範囲を明示し、consumer側で既定値へ落とさない。 */
    int video_crf;
    int timeout_ms;
    /* 負の real_time として設定する非drop render worker数。 */
    int render_threads;
    /* 0はencoderによる自動選択。 */
    int encoder_threads;
    /* 非0を返すと書き出しを中止する。completed/totalはframe数。 */
    int (*progress_callback)(long long completed, long long total, void* opaque);
    void* progress_opaque;
    /* 非0なら Matroska / PCM で音声を非圧縮出力する。0なら MP4 / AAC。 */
    int lossless_audio;
    /* 通常 layer 合成後、encoder の色変換前の RGBA8 を検査する。非0は失敗で公開しない。
     * render worker から呼ぶため、呼び出し側は同期を行う。画素の所有権は渡さない。 */
    int (*rgba_callback)(long long frame, const unsigned char* rgba, int width, int height,
                         void* opaque);
    void* rgba_opaque;
    /* 診断用の観測。NULL なら記録しない。 */
    MvmExportObserver* observer;
} MvmExportSpec;

typedef struct {
    /* 出力ファイルを probe した実測値。要求値と同じとは限らないので、
     * 呼び出し側が照合できるよう実測値をそのまま返す。 */
    long long frame_count;
    double duration_sec;
    int width;
    int height;
    int fps_num;
    int fps_den;
    int used_tractor;
    int playlist_blank_count;
    int transition_count;
    int opaque_black_affine_filter_count;
} MvmExportResult;

typedef enum {
    MVM_EXPORT_OK = 0,
    MVM_EXPORT_FAILED = 1,
    MVM_EXPORT_CANCELLED = 2,
} MvmExportStatus;

/*
 * clips を順に連結して out_path へ H.264 / MP4 で書き出す。
 *
 * fail-closed で作る。以下はすべて失敗として扱い、黙って続行しない。
 *   - MLT が初期化されていない
 *   - clip が 0 本 / パスが空 / ファイルが存在しない
 *   - producer を開けない、または長さが 0
 *   - profile の実値が要求と異なる
 *   - consumer が timeout 以内に終了しない
 *   - 出力が 0 バイト、または probe で映像が確認できない
 *
 * out_path には一時パスを渡すこと。呼び出し側が検証してから正規名へ rename する。
 * この関数は rename しない (mvm_mlt_compose_render_audio と同じ契約)。
 *
 * M4 では音声を書き出さない (映像のみ)。
 *
 * 戻り値: MvmExportStatus
 */
int mvm_mlt_export_sequence(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                            const char* out_path, MvmExportResult* out, char* err, size_t err_size);

/* M7b製品経路。映像 track ごとの playlist (V1/V2 は常に作り、上は使う最上位 track まで) と、
 * V2 以上の clip ごとに V1 との間へ植える affine transition を使う。track 数・clip 数の上限は無い。
 * playlist は video_track をそのまま MLT の track 番号に使うので、V1 と V10000 にだけ clip が
 * あっても空の playlist を 10000 本作る (UI は track を連続で足すので通常は起きない)。
 * 詰めるなら transition の track 番号も一緒に写し替える必要がある。
 * (関数名は 2 track 時代のまま。) */
int mvm_mlt_export_two_track(const MvmExportClip* clips, int clip_count, long long total_duration,
                             const MvmExportSpec* spec, const char* out_path, MvmExportResult* out,
                             char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_EXPORT_H */
