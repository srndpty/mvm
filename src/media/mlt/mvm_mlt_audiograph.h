/*
 * mvm Phase 0 / S5 - 音声グラフの最小切り分け
 *
 * S5 で「MP4/AAC の音声が MLT を通すと壊れる」「volume filter でクラッシュする」
 * という 2 つの症状が出た。完全な 5 トラック構成のまま原因を推測しても
 * 切り分けられないので、最小構成から 1 段ずつ足して境界を特定する。
 *
 * 1 ケース 1 プロセスで実行する。アクセス違反が起きるケースがあるため、
 * 親プロセスを巻き込まないよう子プロセスの終了コードとして観測する。
 *
 * ここは調査用コードであり、製品経路ではない。
 */

#ifndef MVM_MLT_AUDIOGRAPH_H
#define MVM_MLT_AUDIOGRAPH_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 切り分け用素材 (_diag 配下)。UTF-8。 */
    char a1_av[1024];         /* 映像 + AAC A1 */
    char a1_audio_only[1024]; /* AAC A1 のみ */
    char v1_video_only[1024]; /* 映像のみ (音声なし) */
    char a2_wav[1024];        /* PCM A2 */
    /* 既存の 5 トラック構成で使う素材 */
    char v1_h264[1024];
    char v2_hevc[1024];
    char wav_48k[1024];
} MvmAudioGraphPaths;

/*
 * ケースを 1 つ実行して WAV を書き出す。
 *
 * case_name:
 *   "A" A1 MP4 producer -> consumer (playlist/tractor/transition なし)
 *   "B" A1 MP4 -> playlist -> tractor -> consumer
 *   "C" B + video_index=-1
 *   "D" V1=映像のみ / A1=音声のみ (別 resource, 別 producer)
 *   "E" V1 と A1 が同じ a1_av.mp4 (別 producer, stream 分離)
 *   "F" A1 + A2 + mix(sum=1)
 *   "G" F + A2 volume gain -6dB
 *   "H" 既存の 5 トラック完全構成に相当する音声グラフ
 *   "V0".."V4" volume filter の最小切り分け
 *
 * log には使用した producer / filter / transition の property を書き出す。
 *
 * 戻り値: 0 = 成功
 */
int mvm_mlt_audiograph_run(const char* case_name, const MvmAudioGraphPaths* paths,
                           const char* out_wav, int timeout_ms, FILE* log, char* err,
                           size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_AUDIOGRAPH_H */
