/*
 * Explorer で指定ファイルを選択した状態のフォルダを開く。
 *
 * explorer.exe /select,<path> をコマンドラインで起動すると、空白や日本語を含む
 * path の quote 規則が explorer 独自で壊れやすい。shell API
 * (SHOpenFolderAndSelectItems) を直接呼んで quote を経由しない。
 */

#ifndef MVM_REVEAL_IN_EXPLORER_H
#define MVM_REVEAL_IN_EXPLORER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 成功で 0。失敗時は err (UTF-8) に理由を書いて非 0 を返す。path は実在するファイル。 */
int mvm_reveal_in_explorer(const wchar_t* path, char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
