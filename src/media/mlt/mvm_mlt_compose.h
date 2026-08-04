/*
 * mvm Phase 0 / S5 + S6 - タイムライン合成とフレーム取得
 *
 * MvmBenchTimeline を MLT の tractor へ変換し、フレームと音声を取り出す。
 * MLT の型はこのヘッダに一切出さない。
 *
 * S6 (seek / scrub) でも同じハンドルを使う。合成済みタイムラインに対して
 * seek するのが要求であり、単一 producer に対する seek では不十分なため。
 */

#ifndef MVM_MLT_COMPOSE_H
#define MVM_MLT_COMPOSE_H

#include "../../core/mvm_bench_timeline.h"
#include "mvm_mlt_probe.h" /* MvmMltImage */

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_COMPOSE_MAX_NOTES 64

typedef struct {
    char subject[128];
    char detail[512];
} MvmComposeNote;

typedef struct {
    /* 実際に解決された profile の値。要求値と一致しているかは呼び出し側が見る。 */
    int profile_width;
    int profile_height;
    int profile_fps_num;
    int profile_fps_den;
    int profile_sar_num;
    int profile_sar_den;
    int profile_progressive;

    long long length; /* tractor の長さ (フレーム) */
    int track_count;  /* MLT 上のトラック数 */

    /* 実際に使用した service。推測ではなく構築時に記録する。 */
    int note_count;
    MvmComposeNote notes[MVM_COMPOSE_MAX_NOTES];
} MvmComposeInfo;

typedef struct MvmComposeHandle MvmComposeHandle;

/*
 * タイムラインを構築する。
 *
 * fail-closed で作る。以下はすべて失敗として扱い、黙って続行しない。
 *   - 必須 service が存在しない
 *   - producer を開けない
 *   - フォントファイルが存在しない
 *   - profile の実値が要求と異なる
 *   - clip の範囲が不正 (in > out、0 長、素材範囲外)
 *   - track / transition の index が不正
 *
 * 戻り値: 成功時はハンドル、失敗時は NULL (err にメッセージ)
 */
MvmComposeHandle* mvm_mlt_compose_open(const MvmBenchTimeline* timeline, MvmComposeInfo* info,
                                       char* err, size_t err_size);

/* 指定フレームを RGBA で取り出す。img は mvm_mlt_image_free で解放する。 */
int mvm_mlt_compose_frame(MvmComposeHandle* h, long long frame, MvmMltImage* img, char* err,
                          size_t err_size);

typedef struct {
    int sample_rate;
    int channels;
    int samples;
    float* data; /* interleaved。mvm_mlt_audio_free で解放する */
} MvmComposeAudio;

/*
 * 指定フレームの音声を取り出す。
 *
 * 映像の検証とは別の関数にしている。1 つの consumer 実装へ
 * 詰め込むと、どちらの失敗なのか切り分けられなくなるため。
 */
int mvm_mlt_compose_audio(MvmComposeHandle* h, long long frame, MvmComposeAudio* out, char* err,
                          size_t err_size);

void mvm_mlt_audio_free(MvmComposeAudio* a);

/* seek 後のキャッシュ破棄方法の比較 (S6 / 実験用) */
typedef enum {
    MVM_SEEK_PLAIN = 0,     /* seek のみ */
    MVM_SEEK_WITH_PURGE = 1 /* seek + 各 producer の seek をやり直す */
} MvmSeekMode;

void mvm_mlt_compose_set_seek_mode(MvmComposeHandle* h, MvmSeekMode mode);

long long mvm_mlt_compose_length(const MvmComposeHandle* h);

void mvm_mlt_compose_close(MvmComposeHandle* h);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_COMPOSE_H */
