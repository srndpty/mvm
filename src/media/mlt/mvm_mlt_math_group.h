#ifndef MVM_MLT_MATH_GROUP_H
#define MVM_MLT_MATH_GROUP_H

/* 試作 (P3-3A、診断専用): 静止の数式 layer を 1 つの MLT transition でまとめて合成する。
 * 製品の書き出しでは使わない (MvmExportObserver::diagnostic_math_group が 0 なら graph
 * は従来どおり)。 このヘッダは mvm_mlt の中だけで使う。MLT の型を上位へ出さない。
 *
 * MLT 7.36.1 について確かめた前提 (docs/math-transition-p33a.md):
 *   - transition の画像の callback は、render worker から frame ごとに並行に呼ばれる。
 *     この transition の状態は作った後に書き換えない (counter だけを atomic に足す)。
 *   - 下の frame の buffer が、その frame だけのものだという保証は framework に無い
 *     (get_image の writable は producer への依頼で、framework は強制しない)。
 *     そのため渡された buffer へは書かない。複製へ合成し、成功したときだけ frame の画像を
 *     複製へ差し替える。失敗したら複製を捨て、渡された buffer はそのまま返す。
 *   - frame は transition の pointer を参照数なしで積む。frame が transition より長く生きても
 *     状態を参照できるよう、frame ごとに transition の参照を 1 つ持たせる。 */

#include "mvm_mlt_export.h"

#include <stddef.h>

#include <framework/mlt.h>

/* members は合成の順 (下から)。どれも mvm_mlt_math_group_clip_eligible を満たすこと。
 * 位置と不透明度は clip から複製して持つ (clip の配列は書き出しの呼び出しの間だけ有効)。
 * 画像 (math_group_raster) は複製しない。呼び出し側が、書き出しの関数から戻るまで保持する。
 * 失敗したら NULL を返して err に理由を書く。 */
mlt_transition mvm_mlt_math_group_new(const MvmExportClip* const* members, int count, int width,
                                      int height, MvmExportObserver* observer, char* err,
                                      size_t err_size);

/* clip をまとめて合成してよいか。だめなら 0 を返し、reason に事前に決めた理由の名前を書く
 * (reason は NULL でもよい)。画像が渡されていない clip は「対象として申告されていない」で、
 * reason は NULL のままになる。 */
int mvm_mlt_math_group_clip_eligible(const MvmExportClip* clip, int width, int height,
                                     const char** reason);

#endif
