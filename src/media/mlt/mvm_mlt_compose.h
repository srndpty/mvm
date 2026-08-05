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

/*
 * タイムラインの音声を WAV へ書き出す (M3 の正式な検証経路)。
 *
 * mlt_frame_get_audio の生バッファ解釈が未解決なので、M3 の判定には
 * MLT の avformat consumer による実ファイル出力を使う。
 * FFmpeg CLI で別途 mix するのではなく、必ず MLT の tractor +
 * mix transition が合成した結果を書き出す。
 *
 * out_path には一時パスを渡すこと。呼び出し側が ffprobe で検証してから
 * 正規名へ rename する。この関数は rename しない。
 *
 * 空ファイル・途中出力・timeout はすべて失敗として返す。
 *
 * 戻り値: 0 = 成功
 */
int mvm_mlt_compose_render_audio(MvmComposeHandle* h, const char* out_path, int timeout_ms,
                                 char* err, size_t err_size);

/* seek 後のキャッシュ破棄方法の比較 (S6 / 実験用) */
typedef enum {
    MVM_SEEK_PLAIN = 0,     /* seek のみ */
    MVM_SEEK_WITH_PURGE = 1 /* seek + 各 producer の seek をやり直す */
} MvmSeekMode;

void mvm_mlt_compose_set_seek_mode(MvmComposeHandle* h, MvmSeekMode mode);

/* --------------------------------------------------------------------------
 * preview (S7 / M7)
 *
 * compose_frame を連続で呼ぶことは preview ではない。あれは「毎回 seek して
 * 1 枚取る」経路であり、consumer の read-ahead も worker thread も通らない。
 * preview は mlt_consumer に tractor を接続し、consumer 側が連続して
 * frame を要求する経路で測る。
 *
 * MLT 7.36.1 のソースで確認した事実 (src/framework/mlt_consumer.c):
 *
 *   real_time > 0 : 非同期。フレームドロップあり。
 *   real_time < 0 : 非同期。フレームドロップなし。
 *   real_time = 0 : 同期。**mlt_frame_get_image を呼ばない。**
 *                   frame を取得して "rendered" を 1 にするだけである。
 *   render thread 数 = abs(real_time)  (consumer_work_start の n = abs(real_time))
 *   drop_count       = consumer の int property。"rendered" が立っていない
 *                      frame を数える。
 *
 *   null consumer (src/modules/core/consumer_null.c) も get_image を呼ばない。
 *   したがって real_time=0 と null consumer の組み合わせでは
 *   **何も描画されない。** fps を測っても意味が無いので、
 *   この組み合わせは呼び出し側で拒否すること。
 *
 * 計測は wall 時間で打ち切る。素材の尺で打ち切ると、遅い構成ほど
 * 長時間走ることになり、構成間で比較できなくなる。
 * -------------------------------------------------------------------------- */

typedef struct {
    long long position; /* mlt_frame_get_position */
    int rendered;       /* frame の "rendered" property */
    double t_ms;        /* consumer start からの経過ミリ秒 */
} MvmPreviewSample;

typedef struct {
    const char* consumer_service; /* "null" など。NULL 不可 */
    int real_time;                /* consumer の real_time property。0 は拒否する */
    int measure_ms;               /* wall 時間での計測上限。0 以下は拒否する */
    int warmup_ms;                /* 計測前に別 start/stop で回す時間。0 で省略 */
    int timeout_ms;               /* 停止待ちの上限 */
    long long max_samples;        /* サンプル配列の上限 */
} MvmPreviewRequest;

typedef struct {
    MvmPreviewSample* samples; /* mvm_mlt_preview_free で解放する */
    long long sample_count;
    long long sample_overflow; /* max_samples を超えて捨てた数 */

    int effective_real_time; /* consumer から読み戻した real_time */
    long long drop_count;    /* consumer の drop_count property */
    double start_latency_ms; /* start() から最初の frame-show まで */
    double wall_sec;         /* start() から stop 完了まで */
    int producer_ended;      /* terminate_on_pause で自然終了したか */
    int stopped_by_timeout;  /* timeout で打ち切ったか (失敗扱い) */

    char actual_properties[2048]; /* 実際に設定された consumer property のダンプ */
} MvmPreviewResult;

/*
 * consumer 経路で preview を計測する。
 * 戻り値: 0 = 成功。失敗時は err に理由を書く。
 */
int mvm_mlt_compose_preview(MvmComposeHandle* h, const MvmPreviewRequest* req,
                            MvmPreviewResult* out, char* err, size_t err_size);

void mvm_mlt_preview_free(MvmPreviewResult* r);

long long mvm_mlt_compose_length(const MvmComposeHandle* h);

void mvm_mlt_compose_close(MvmComposeHandle* h);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_COMPOSE_H */
