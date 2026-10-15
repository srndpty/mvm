/*
 * Explorer で指定ファイルを選択した状態のフォルダを開く。
 *
 * explorer.exe /select,<path> をコマンドラインで起動すると、空白や日本語を含む
 * path の quote 規則が explorer 独自で壊れやすい。shell API
 * (SHOpenFolderAndSelectItems) を直接呼んで quote を経由しない。
 *
 * SHParseDisplayName は区切りが / の path を E_INVALIDARG (0x80070057) で拒否する。
 * QML の FileDialog の URL から作った path (QUrl::toLocalFile) は / 区切りなので、
 * Shell へ渡す前に GetFullPathNameW で Windows の絶対 path (\ 区切り) にする (P3-7A)。
 */

#ifndef MVM_REVEAL_IN_EXPLORER_H
#define MVM_REVEAL_IN_EXPLORER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 成功で 0。失敗時は err (UTF-8) に理由を書いて非 0 を返す。path は実在するファイル
 * (区切りは / でも \ でもよい。相対 path は現在の directory から解決する)。 */
int mvm_reveal_in_explorer(const wchar_t* path, char* err, size_t err_size);

/* 試験用: Shell API の境界を差し替える。各項目は NULL なら実際の API を使う。
 * shell 自体を NULL にすると、全部を実際の API に戻す。process 全体に効く (試験の外で使わない)。 */
typedef struct MvmRevealShell {
    /* Shell へ渡す直前の path (正規化の後) を受け取る。 */
    void (*observe_path)(const wchar_t* path, void* opaque);
    /* SHParseDisplayName の代わり。成功なら *item に CoTaskMemFree で解放できる値 (NULL 可)
     * を入れる。 */
    long (*parse)(const wchar_t* path, void** item, void* opaque);
    /* SHOpenFolderAndSelectItems の代わり (Explorer を開かずに結果を返す)。 */
    long (*open)(const void* item, void* opaque);
    void* opaque;
} MvmRevealShell;

void mvm_reveal_set_shell_for_test(const MvmRevealShell* shell);

#ifdef __cplusplus
}
#endif

#endif
