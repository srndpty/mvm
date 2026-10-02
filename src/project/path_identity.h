#ifndef MVM_PROJECT_PATH_IDENTITY_H
#define MVM_PROJECT_PATH_IDENTITY_H

#include <filesystem>
#include <string>

namespace mvm::project {

// path の同一性の規則を 1 箇所に置く。
//
// canonicalPathKey: I/O をしない表記上の key。absolute + lexically_normal +
//   区切り文字を揃えたもの。ファイルが無くても決まり、
//   同じ入力には常に同じ値を返す。absolute にできなければ空文字列。
//   JSON の検証のように、結果を disk の状態に依存させたくない場所で使う。
//   大文字小文字は畳まない。NTFS は directory ごとに case sensitivity を有効にでき、
//   そこでは A.mp4 と a.mp4 が別のファイルになる。畳むと別物を同じと誤判定する
//   (media_source_identity.h の cache 用 identity と同じ規則)。
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
//   両方 FileId               -> file ID で決める (表記より優先する)
//   表記が同じ                 -> Same
//   Missing 同士 / FileId と Missing -> Different (同じ実体ではありえない)
//   どちらかが Unavailable     -> Unknown
enum class PathSameness { Same, Different, Unknown };

PathSameness comparePathIdentity(const FileIdentityKey& left, const FileIdentityKey& right);
PathSameness comparePathIdentity(const std::filesystem::path& left,
                                 const std::filesystem::path& right);

// 以前の版 (同じ schema 14) は clip と素材の path を大文字小文字を畳んで照合していた。
// 大文字小文字だけが違う clip の path を、読み込むときに素材の表記へ揃えてよいか。
// 揃えると clip が再生するファイルが素材のものになるので、同じ実体と言えるときだけ揃える。
//   両方 FileId で同じ file ID -> 揃える (case-insensitive な directory の普通の場合)
//   両方 Missing              -> 揃える (どちらも何も指していないので、再生する実体は変わらない)
//   file ID が違う / 片方だけある / Unavailable -> 揃えない (読み込みは照合で拒否する)
// 呼び出し側は、大文字小文字を畳んだ表記が同じことを先に確かめる。
bool mayAdoptLegacyCaseSpelling(const FileIdentityKey& clip, const FileIdentityKey& item);

} // namespace mvm::project

#endif // MVM_PROJECT_PATH_IDENTITY_H
