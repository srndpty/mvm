#ifndef MVM_MLT_V1_BACKGROUND_H
#define MVM_MLT_V1_BACKGROUND_H

/* V1 の隙間を埋める黒の背景 (MLT の color producer) を、render worker から安全に使える形で作る。
 * このヘッダは mvm_mlt の中だけで使う。
 *
 * MLT 7.36.1 の color producer (src/modules/core/producer_colour.c) は、作った画像を producer に
 * cache する。cache を作り直すとき、「作った」という記録 (色・幅・高さ・形式) を書いて lock を
 * 外してから、画素を埋める。同時に来た別の worker は、cache があると判断して、埋まる前の
 * buffer を複製して返す (P3-5A で ThreadSanitizer が報告し、AddressSanitizer の build では下地が
 * 未初期化になった。docs/mlt-color-race-p35a2.md)。作り直しは、古い buffer を、それを複製して
 * いる途中の worker がいても解放する。
 *
 * cache が作り直される条件は「色・幅・高さ・形式のどれかが、前の要求と違う」だけである。
 * そこで、ここでは次のようにする。
 *   1. producer に形式 (rgba) を固定する。要求側が別の形式を求めても、producer は rgba で作る
 *      (rgba64 の要求だけは例外で、MLT が rgba64 で作る。書き出しは rgba64 を要求しない)。
 *   2. consumer を始める前に、同じ producer へ 1 thread で画像を 1 回要求し、cache を作らせる
 *      (prewarm)。cache の全画素が黒 (0, 0, 0, 255) であることを確かめる。
 *   3. この producer が作る frame の画像の要求ごとに、
 *      (a) MLT へ渡す前: cache の key を変える要求 (形式の固定が外れている、rgba64、出力と違う
 *          大きさ、色の文字列が違う) は、producer へ渡さずに断る。cache は作り直されない。
 *      (b) 渡した後: cache が prewarm のときのままであること (buffer・幅・高さ・形式) と、
 *          返った画像が出力と同じ大きさの rgba であることを確かめる。
 *      どちらも、違っていれば記録する。呼び出し側は書き出しを失敗にする (黙って続けない)。
 *      (b) は MLT が要求を処理した後にしか働かないので、同期の壁ではない (見つけた時点で、別の
 *      worker に埋まる前の画素を渡しているかもしれない)。壁になるのは (a) で、(b) は (a) の
 *      見落とし (MLT の動きの読み違い) を見つけるための 2 段目である。
 * 3 があるので、「作り直しは起きないはず」という前提が崩れたときに、気づかずに誤った画素を
 * 出すことはない。この producer への画像の要求は、どれも 3 の検査を通る。cache を作り直す要求は、
 * それ自身が検査で見つかる: 形式か大きさが違う要求は、返る画像の形が違う。色の文字列だけが
 * 違う要求は、古い buffer を持ったまま新しい buffer を確保するので、その要求の検査では buffer が
 * 必ず違う。 */

#include <stddef.h>

#include <framework/mlt.h>

typedef struct MvmV1Background MvmV1Background;

/* 対照用の故障 (製品では 0)。 */
enum {
    MVM_V1_BACKGROUND_FAULT_NONE = 0,
    MVM_V1_BACKGROUND_FAULT_NO_FORMAT = 1,     /* 形式を固定しない */
    MVM_V1_BACKGROUND_FAULT_REBUILD = 2,       /* 渡す前の検査が見ない所で cache を作り直させる */
    MVM_V1_BACKGROUND_FAULT_PREWARM_FAILS = 3, /* prewarm の検査を失敗させる */
    MVM_V1_BACKGROUND_FAULT_NO_PREWARM = 4     /* prewarm も検査もしない (保護なし。検出器の対照) */
};

/* 黒 (#000000、alpha 255) の背景を作る。length は producer の尺 (frame)。失敗したら NULL を返し、
 * err に理由を書く (consumer を始める前に呼ぶこと)。 */
MvmV1Background* mvm_mlt_v1_background_new(mlt_profile profile, long long length, int width,
                                           int height, int fault, char* err, size_t err_size);

/* playlist へ置く producer。所有は移さない。 */
mlt_producer mvm_mlt_v1_background_producer(MvmV1Background* background);

/* cache の作り直し、または出力と違う形の画像を観測していれば非 0 を返し、err に最初の 1 件を書く。
 * 書き出しの待ち loop と、consumer を閉じた後に呼ぶ。 */
int mvm_mlt_v1_background_failed(MvmV1Background* background, char* err, size_t err_size);

/* 検査した画像の要求の数、違反の数、MLT へ渡す前に断った要求の数、要求された形式
 * (mlt_image_format の値を bit の位置にした和。例: yuv422 と rgba なら (1 << 3) | (1 << 2))。 */
void mvm_mlt_v1_background_stats(MvmV1Background* background, long long* frames,
                                 long long* violations, long long* rejected,
                                 int* requested_formats);

/* producer を閉じる。consumer と tractor を閉じた後に呼ぶ (frame が残っていないこと)。 */
void mvm_mlt_v1_background_close(MvmV1Background* background);

#endif
