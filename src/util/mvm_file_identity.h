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

/* identity 取得の結果。「存在しない」と「存在するかもしれないが取れない」を分ける。
 * 後者を前者と同じに扱うと、同じ実体を別物と判定する (fail-open) 経路になる。 */
typedef enum MvmFileIdentityStatus {
    MVM_FILE_IDENTITY_OK = 0,
    /* ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND。path は何も指していない。 */
    MVM_FILE_IDENTITY_MISSING = 1,
    /* access denied・ネットワーク・ディレクトリ・特殊 FS など。実体は分からない。 */
    MVM_FILE_IDENTITY_UNAVAILABLE = 2
} MvmFileIdentityStatus;

/* 通常ファイルの identity を取る。OK 以外では out を 0 で埋める。 */
MvmFileIdentityStatus mvm_file_identity_probe(const wchar_t* path, MvmFileIdentity* out);

/* mvm_file_identity_probe が OK なら 0、それ以外は非 0。 */
int mvm_file_identity_query(const wchar_t* path, MvmFileIdentity* out);

/* 先頭と末尾の各 MVM_FILE_FINGERPRINT_EDGE_BYTES と size から 64bit の値を作る標本検査。
 * size と更新時刻が一致したまま中身だけ差し替えられたことを、file 全体を読まずに
 * 見つけるためのもの。**中央部だけの変更は検出しない** (取りこぼしうる、という契約)。
 * 差し替えを必ず検出したい用途では mvm_file_content_hash を使う。成功で 0。 */
#define MVM_FILE_FINGERPRINT_EDGE_BYTES (64 * 1024)
int mvm_file_content_fingerprint(const wchar_t* path, unsigned long long* out);

/* file 全体と size から 64bit の値を作る (FNV-1a)。暗号学的 hash ではないが、
 * どの位置の変更も値に反映される。file 全体を読むので、大きな素材を頻繁に検査する
 * 用途には向かない。成功で 0。 */
int mvm_file_content_hash(const wchar_t* path, unsigned long long* out);

#ifdef __cplusplus
}
#endif

#endif
