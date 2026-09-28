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
     * 音程は速度に連動させる (warp_pitch=0)。 */
    long long speed_num;
    long long speed_den;
    int is_audio; /* 非0なら映像を隠して独立audio trackとしてmixする */
    int is_text;  /* 非0なら明示した qimage producer で透過 PNG を開く */
    int effects_enabled;
    int crop_left;
    int crop_top;
    int crop_right;
    int crop_bottom;
    double rect_x;
    double rect_y;
    double rect_width;
    double rect_height;
    double rotation_degrees;
    const MvmExportOpacityKeyframe* opacity_keyframes;
    int opacity_keyframe_count;
    const MvmExportGainKeyframe* gain_keyframes;
    int gain_keyframe_count;
    int video_track; /* 0=V1, 1=V2, 2=V3 */
    long long timeline_start_frame;
    long long timeline_duration_frames;
} MvmExportClip;

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

/* M7b製品経路。2本の固定playlistとV2 clipごとのaffine transitionを使う。 */
int mvm_mlt_export_two_track(const MvmExportClip* clips, int clip_count, long long total_duration,
                             const MvmExportSpec* spec, const char* out_path, MvmExportResult* out,
                             char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_EXPORT_H */
