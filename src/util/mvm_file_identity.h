/*
 * 実ファイルの identity と、軽量な内容 fingerprint。
 *
 * path の文字列は同じ実体を指すとは限らない (hard link / junction / case-sensitive
 * directory)。実体の比較には volume serial と file ID を使う。
 * 更新時刻は FILETIME (100ns) のまま返し、ms へ丸めない。
 */

#ifndef MVM_FILE_IDENTITY_H
#define MVM_FILE_IDENTITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MvmFileIdentity {
    unsigned long long volume_serial;
    unsigned char file_id[16];
    long long size;
    /* FILETIME と同じ 100ns 単位。 */
    long long last_write_time;
} MvmFileIdentity;

/* 通常ファイルの identity を取る。成功で 0。存在しない・ディレクトリ・
 * 開けない場合は非 0 を返し、out は 0 で埋める。 */
int mvm_file_identity_query(const wchar_t* path, MvmFileIdentity* out);

/* 先頭と末尾の各 MVM_FILE_FINGERPRINT_EDGE_BYTES と size から 64bit の値を作る。
 * 暗号学的 hash ではなく、size と更新時刻が一致したまま中身だけ差し替えられた
 * ことを検出するためのもの。中央部だけの変更は検出しない。成功で 0。 */
#define MVM_FILE_FINGERPRINT_EDGE_BYTES (64 * 1024)
int mvm_file_content_fingerprint(const wchar_t* path, unsigned long long* out);

#ifdef __cplusplus
}
#endif

#endif
