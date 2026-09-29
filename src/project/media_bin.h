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
    // removeMediaBinEntries が素材と一緒に削除した timeline clip の id。
    std::vector<std::string> removedClipIds;
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

// 削除で消えるもの。folder は子孫と中身ごと、素材はそれを参照する timeline clip
// (とそのリンク相手) ごと消す。プロジェクトパネルを素材の唯一の出どころにするため、
// パネルに無い素材を timeline に残さない。
struct MediaBinRemovalPlan {
    bool success = false;
    std::string error;
    std::set<std::string> folderIds;
    std::set<std::string> itemIds;
    std::vector<std::string> clipIds; // timeline の並び順
};

// 同じ実体か確認できない (Unknown) clip が 1 つでもあれば失敗する。
// 残すべき clip を消す / 消すべき clip を残す のどちらも起こさないため。
MediaBinRemovalPlan planMediaBinRemoval(const Project& project,
                                        const std::vector<std::string>& entryIds);
// planMediaBinRemoval の内容を 1 つの candidate として適用する。
MediaBinEditResult removeMediaBinEntries(Project& project,
                                         const std::vector<std::string>& entryIds);

const MediaItem* findMediaItem(const Project& project, const std::string& itemId);
// 同じ実体のファイルを指す素材を探す (path_identity.h の comparePathIdentity が Same のもの)。
// 同一性が Unknown の素材は一致とみなさない。重複登録の防止は best effort であり、
// 削除時にどの clip を巻き込むかの判定は planMediaBinRemoval が fail-closed で担う。
const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path);
const MediaFolder* findMediaFolder(const Project& project, const std::string& folderId);

// timeline clip からの参照状況 (パネルの「使用中」表示用)。
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
