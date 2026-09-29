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

// folder / item の id 一意性、親子関係 (循環なし)、種別ごとの値と、
// timeline clip の素材参照 (validateMediaReferences) を検査する。
// Project JSON の読み書き (= commitProjectEdit) の双方がこれを通す。
MediaBinEditResult validateMediaBin(const Project& project);
// 動画・音声・画像の clip がプロジェクトパネルの素材を mediaItemId で指し、その素材と
// 種別・ファイルが一致すること。文字・Manim の clip は素材を指さないこと。
// プロジェクトパネルを素材の唯一の出どころにする不変条件。
MediaBinEditResult validateMediaReferences(const Project& project);

MediaBinEditResult addMediaFolder(Project& project, MediaFolder folder);
// 同じ mediaPath の item が既にあれば失敗する。
MediaBinEditResult addMediaItem(Project& project, MediaItem item);
// 素材の技術的な値 (種別・fps・尺・解像度・sample rate) を、調べ直した probed の値へ
// 置き換える。id・名前・フォルダ・ファイルは変えない。外部でファイルが差し替わっていても、
// パネルの値と timeline の計算 (枠の寸法など) を実物に合わせる。
MediaBinEditResult refreshMediaItem(Project& project, const std::string& itemId,
                                    const MediaItem& probed);
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

// 消える clip は mediaItemId で決まる (ファイルの実体は調べない)。
MediaBinRemovalPlan planMediaBinRemoval(const Project& project,
                                        const std::vector<std::string>& entryIds);
// planMediaBinRemoval の内容を 1 つの candidate として適用する。
MediaBinEditResult removeMediaBinEntries(Project& project,
                                         const std::vector<std::string>& entryIds);

const MediaItem* findMediaItem(const Project& project, const std::string& itemId);
// 同じ実体のファイルを指す素材を探す (path_identity.h の comparePathIdentity が Same のもの)。
// 同一性が Unknown の素材は一致とみなさない。素材の重複登録の防止に使う (best effort)。
// clip と素材の対応は mediaItemId が担い、これには依存しない。
const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path);
const MediaFolder* findMediaFolder(const Project& project, const std::string& folderId);

// timeline のどこかの clip が使っている素材の id (パネルの「使用中」表示用)。
std::set<std::string> mediaItemsInUse(const Project& project);

const char* mediaKindName(MediaKind kind);
bool parseMediaKindName(const std::string& text, MediaKind& kind);

} // namespace mvm::project

#endif // MVM_PROJECT_MEDIA_BIN_H
