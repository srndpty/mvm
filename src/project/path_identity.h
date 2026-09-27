#ifndef MVM_PROJECT_PATH_IDENTITY_H
#define MVM_PROJECT_PATH_IDENTITY_H

#include <filesystem>
#include <string>

namespace mvm::project {

// path の同一性の規則を 1 箇所に置く。
//
// canonicalPathKey: I/O をしない表記上の key。absolute + lexically_normal +
//   区切り文字と大文字小文字を揃えたもの。ファイルが無くても決まり、
//   同じ入力には常に同じ値を返す。absolute にできなければ空文字列。
//
// mediaFileKey: 実体の key。存在する通常ファイルは volume serial + file ID
//   (大文字小文字・junction・8.3 名・hard link を吸収する)、存在しなければ
//   canonicalPathKey へ落とす。同じ path は同時に「存在する / しない」の両方には
//   ならないので、2 種類の key が同じ実体を別物と判定することは無い。
std::wstring canonicalPathKey(const std::filesystem::path& path);
std::wstring mediaFileKey(const std::filesystem::path& path);

} // namespace mvm::project

#endif // MVM_PROJECT_PATH_IDENTITY_H
