#ifndef MVM_PROJECT_MEDIA_BIN_H
#define MVM_PROJECT_MEDIA_BIN_H

#include "project/project.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mvm::project {

// プロジェクトパネル (素材とフォルダ) の編集。
// どの操作も candidate を検証してから置き換え、失敗時は Project を変更しない。

struct MediaBinEditResult {
    bool success = false;
    std::string error;
};

// folder / item の id 一意性、親子関係 (循環なし)、種別ごとの値を検査する。
// Project JSON の読み書き (= commitProjectEdit) の双方がこれを通す。
MediaBinEditResult validateMediaBin(const Project& project);

MediaBinEditResult addMediaFolder(Project& project, MediaFolder folder);
// 同じ mediaPath の item が既にあれば失敗する。
MediaBinEditResult addMediaItem(Project& project, MediaItem item);
MediaBinEditResult renameMediaBinEntry(Project& project, const std::string& entryId,
                                       std::string name);
// targetFolderId が空なら root へ移す。folder を自分自身や子孫へは移せない。
MediaBinEditResult moveMediaBinEntries(Project& project, const std::vector<std::string>& entryIds,
                                       const std::string& targetFolderId);
// folder は中身ごと削除する。timeline で使用中の素材が 1 つでも含まれれば全体を拒否する。
MediaBinEditResult removeMediaBinEntries(Project& project,
                                         const std::vector<std::string>& entryIds);

const MediaItem* findMediaItem(const Project& project, const std::string& itemId);
// 同じ実体のファイルを指す素材を探す (path_identity.h の comparePathIdentity が Same のもの)。
// 同一性が Unknown の素材は一致とみなさない。重複登録の防止は best effort であり、
// 安全判定 (使用中の素材を削除させない) は mediaItemUsage が fail-closed で担う。
const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path);
const MediaFolder* findMediaFolder(const Project& project, const std::string& folderId);

// timeline clip からの参照状況。
//   inUse   : どれかの clip と同じ実体 (Same)
//   unknown : Same は無いが、identity を取れず Unknown の clip がある。未使用と断定しない
struct MediaItemUsage {
    std::set<std::string> inUse;
    std::set<std::string> unknown;
};

MediaItemUsage mediaItemUsage(const Project& project);

const char* mediaKindName(MediaKind kind);
bool parseMediaKindName(const std::string& text, MediaKind& kind);

} // namespace mvm::project

#endif // MVM_PROJECT_MEDIA_BIN_H
