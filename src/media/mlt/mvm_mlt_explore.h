/*
 * mvm Phase 0 / S3 - MLT の API と property を実測で確認する調査用コード
 *
 * property 名を推測で採用しないための道具。
 * 結果は docs/research/mlt-notes.md に記録する。
 */

#ifndef MVM_MLT_EXPLORE_H
#define MVM_MLT_EXPLORE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 主要な service の property と、playlist / tractor の position・length の
 * 振る舞いを出力する。戻り値: 0 = 成功 */
int mvm_mlt_explore(const char* video_path, const char* audio_path, FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* MVM_MLT_EXPLORE_H */
