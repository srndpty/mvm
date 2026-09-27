#ifndef MVM_PROJECT_MEDIA_BIN_H
#define MVM_PROJECT_MEDIA_BIN_H

#include "project/project.h"

#include <filesystem>
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
const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path);
const MediaFolder* findMediaFolder(const Project& project, const std::string& folderId);
// timeline clip がこの素材を参照しているか (mediaPath で照合する)。
bool isMediaItemInUse(const Project& project, const MediaItem& item);

const char* mediaKindName(MediaKind kind);
bool parseMediaKindName(const std::string& text, MediaKind& kind);

} // namespace mvm::project

#endif // MVM_PROJECT_MEDIA_BIN_H
