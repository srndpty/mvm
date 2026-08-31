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
    const char* path; /* UTF-8。実在する動画ファイル */
    long long source_fps_num;
    long long source_fps_den;
    long long source_in_frame;  /* inclusive、素材固有 frame domain */
    long long source_out_frame; /* exclusive、素材固有 frame domain */
} MvmExportClip;

typedef struct {
    /* 呼び出し側が必ず明示する。既定値を推測しない。 */
    int width;
    int height;
    int fps_num;
    int fps_den;
    int timeout_ms;
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
} MvmExportResult;

/* 素材固有の frame 境界を MLT producer profile の frame 境界へ floor で変換する。
 * producer が実際に公開する位置 domain に合わせる操作であり、ceil を使う Project
 * timeline 変換とは別の意味論を持つため、Project helper は流用しない。 */
int mvm_source_boundary_to_producer_boundary(long long source_frame, long long source_fps_num,
                                             long long source_fps_den, int producer_fps_num,
                                             int producer_fps_den, long long* out_frame);

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
 * 戻り値: 0 = 成功
 */
int mvm_mlt_export_sequence(const MvmExportClip* clips, int clip_count, const MvmExportSpec* spec,
                            const char* out_path, MvmExportResult* out, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_EXPORT_H */
