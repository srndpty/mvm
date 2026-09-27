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
//   JSON の検証のように、結果を disk の状態に依存させたくない場所で使う。
std::wstring canonicalPathKey(const std::filesystem::path& path);

// 実体の identity。取れなかった理由を区別する。
//   FileId      : 存在する通常ファイル。volume serial + file ID を持つ
//                 (大文字小文字・junction・8.3 名・hard link を吸収する)
//   Missing     : path は何も指していない (FILE/PATH_NOT_FOUND)
//   Unavailable : 存在するかもしれないが identity を取れない
//                 (access denied・ネットワーク・ディレクトリ・特殊 FS など)
enum class FileIdentityStatus { FileId, Missing, Unavailable };

struct FileIdentityKey {
    FileIdentityStatus status = FileIdentityStatus::Unavailable;
    std::wstring pathKey; // canonicalPathKey
    std::wstring fileKey; // FileId のときだけ
};

FileIdentityKey fileIdentityKey(const std::filesystem::path& path);

// 同じ実体か。Unknown は「同じとも違うとも言えない」であり、呼び出し側が
// 安全な側 (削除を拒否する、外部変更の検査を行う等) へ倒す。
//   表記が同じ                 -> Same
//   両方 FileId               -> file ID で決める
//   Missing 同士 / FileId と Missing -> Different (同じ実体ではありえない)
//   どちらかが Unavailable     -> Unknown
enum class PathSameness { Same, Different, Unknown };

PathSameness comparePathIdentity(const FileIdentityKey& left, const FileIdentityKey& right);
PathSameness comparePathIdentity(const std::filesystem::path& left,
                                 const std::filesystem::path& right);

} // namespace mvm::project

#endif // MVM_PROJECT_PATH_IDENTITY_H
