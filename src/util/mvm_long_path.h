/*
 * MAX_PATH (260 文字) を超える path の扱い (docs/math-clips.md の P2-8)。
 *
 * なぜ必要か:
 *   この開発機の Windows は LongPathsEnabled が無効で、MSYS2 UCRT64 の libstdc++ は
 *   260 文字を超える path で次のように壊れる (実測)。
 *     - 通常の path の recursive_directory_iterator は、その下の file を error 無しで 0 件にする
 *     - "\\?\" 付きでも、directory 自体の path が 260 文字を超えると、その中を error 無しで
 *       列挙しない
 *     - remove_all(path, error) は 260 文字を超える木で返らない (CPU を使い続ける)
 *   数式の外部 renderer (Manim) は作業 directory を current directory にして相対 path で
 *   書くので、作業 directory の下には 260 文字を超える file ができうる。
 *
 * "\\?\" の path は Windows が正規化しないので、"/" と "." / ".." は付ける前に字句的に解決する。
 * 外部 process の current directory など、"\\?\" を受け付けない引数には使わない。
 */

#ifndef MVM_LONG_PATH_H
#define MVM_LONG_PATH_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "\\?\" 付きの絶対 path (extended_path) の下の木を消す。260 文字を超えても扱い、各項目を
 * 1 回だけ試すので必ず返る。消せない項目があっても残りを試し、最初の Win32 error を返す。
 * 0 は成功 (path が無い場合を含む)。読み取り専用の file は属性を外して消す。
 * reparse point (junction・symlink) は辿らずにその項目だけを消す。 */
unsigned long mvm_remove_tree(const wchar_t* extended_path);

#ifdef __cplusplus
}

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace mvm::util {

// path を絶対 path にして "\\?\" (UNC は "\\?\UNC\") を付ける。既に "\\?\" または "\\.\" で
// 始まる path はそのまま返す。current directory を得られなければ元の path を返す (呼び出し側の
// 操作がそのまま失敗し、error を返す)。260 文字を超える入力も字句的に変換する。
//
// 受け付ける入力は、drive の絶対 path (C:\...)・UNC・既に extended-length / device の path・
// 普通の相対 path (current directory からの相対) だけである。Windows 固有の drive 相対
// (C:foo) と root 相対 (\foo) の意味は定義しない (試験もしていない)。呼び出し側は Project と
// cache の directory から作った絶対 path だけを渡す。
inline std::filesystem::path extendedLengthPath(const std::filesystem::path& path) {
    const std::wstring& text = path.native();
    if (text.empty() || text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\"))
        return path;
    // UNC (\\server\share\...)。libstdc++ はこれを絶対 path と見なさないので、字句で扱う。
    if (text.size() > 2 && (text[0] == L'\\' || text[0] == L'/') &&
        (text[1] == L'\\' || text[1] == L'/')) {
        auto rest = std::filesystem::path(text.substr(2)).lexically_normal();
        rest.make_preferred();
        return std::filesystem::path(L"\\\\?\\UNC\\" + rest.native());
    }
    std::filesystem::path full = path;
    if (!full.is_absolute()) {
        std::error_code error;
        const auto current = std::filesystem::current_path(error);
        if (error)
            return path;
        full = current / path;
    }
    full = full.lexically_normal();
    full.make_preferred();
    return std::filesystem::path(L"\\\\?\\" + full.native());
}

// directory の木 (または file) を消す。260 文字を超える file・directory を含んでも返る。
// 失敗すると error を設定して false (無い path は成功)。
inline bool removeTree(const std::filesystem::path& path, std::error_code& error) {
    error.clear();
    const auto extended = extendedLengthPath(path);
    const unsigned long code = mvm_remove_tree(extended.c_str());
    if (code != 0)
        error.assign(static_cast<int>(code), std::system_category());
    return code == 0;
}

} // namespace mvm::util
#endif

#endif /* MVM_LONG_PATH_H */
