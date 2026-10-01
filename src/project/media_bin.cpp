#include "project/media_bin.h"

#include "project/path_identity.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mvm::project {
namespace {

MediaBinEditResult failure(std::string error) {
    MediaBinEditResult result;
    result.error = std::move(error);
    return result;
}

// candidate を検証し、通ったときだけ project を置き換える。
MediaBinEditResult commitCandidate(Project& project, Project candidate) {
    auto result = validateMediaBin(candidate);
    if (result.success)
        project = std::move(candidate);
    return result;
}

MediaBinEditResult validateItemValues(const MediaItem& item) {
    const auto label = "素材 \"" + item.name + "\" ";
    switch (item.kind) {
    case MediaKind::Video:
        if (!isCanonicalFrameRate(item.fpsNum, item.fpsDen) || item.frameCount <= 0 ||
            item.width <= 0 || item.height <= 0)
            return failure(label + "の動画情報 (fps / 尺 / 解像度) が不正です");
        if (item.sampleRate != 0 || item.durationSamples != 0)
            return failure(label + "は動画なのに音声専用の値を持っています");
        break;
    case MediaKind::Audio:
        if (item.sampleRate <= 0 || item.durationSamples <= 0)
            return failure(label + "の音声情報 (sample rate / 尺) が不正です");
        if (item.fpsNum != 0 || item.fpsDen != 1 || item.frameCount != 0 || item.width != 0 ||
            item.height != 0)
            return failure(label + "は音声なのに映像の値を持っています");
        break;
    case MediaKind::Image:
        if (item.width <= 0 || item.height <= 0)
            return failure(label + "の解像度が不正です");
        if (item.fpsNum != 0 || item.fpsDen != 1 || item.frameCount != 0 || item.sampleRate != 0 ||
            item.durationSamples != 0)
            return failure(label + "は静止画なのに尺を持っています");
        break;
    }
    return {true, {}, {}};
}

} // namespace

MediaBinEditResult validateMediaBin(const Project& project) {
    std::set<std::string> ids;
    for (const auto& folder : project.mediaFolders) {
        if (folder.id.empty() || !ids.insert(folder.id).second)
            return failure("素材フォルダの id が空または重複しています: " + folder.id);
        if (folder.name.empty())
            return failure("素材フォルダの名前が空です");
    }
    for (const auto& folder : project.mediaFolders) {
        if (!folder.parentId.empty() && !findMediaFolder(project, folder.parentId))
            return failure("素材フォルダ \"" + folder.name + "\" の親フォルダがありません");
    }
    // 全 folder の親が実在することを確かめてから辿る。
    for (const auto& folder : project.mediaFolders) {
        // 親をたどって root へ着くこと。folder 数を超えて続くなら循環している。
        std::string cursor = folder.parentId;
        for (std::size_t depth = 0; !cursor.empty(); ++depth) {
            if (cursor == folder.id || depth > project.mediaFolders.size())
                return failure("素材フォルダ \"" + folder.name + "\" の親子関係が循環しています");
            cursor = findMediaFolder(project, cursor)->parentId;
        }
    }
    // 重複は表記上の key で判定する。検証の結果をファイルの有無や実体に依存させると、
    // 保存済みの Project が disk 側の変化だけで開けなくなる。実体での判定は
    // 読み込み時の findMediaItemByPath が担う。
    std::set<std::wstring> pathKeys;
    // 素材ごとに folder 列を走査しない (素材数 x folder 数にしない)。
    std::unordered_set<std::string_view> folderIds;
    folderIds.reserve(project.mediaFolders.size());
    for (const auto& folder : project.mediaFolders)
        folderIds.insert(folder.id);
    for (const auto& item : project.mediaItems) {
        if (item.id.empty() || !ids.insert(item.id).second)
            return failure("素材の id が空または重複しています: " + item.id);
        if (item.name.empty() || item.mediaPath.empty())
            return failure("素材の名前または media_path が空です");
        if (!item.folderId.empty() && !folderIds.contains(item.folderId))
            return failure("素材 \"" + item.name + "\" のフォルダがありません");
        const auto key = canonicalPathKey(item.mediaPath);
        if (key.empty())
            return failure("素材 \"" + item.name + "\" の media_path を解決できません");
        if (!pathKeys.insert(key).second)
            return failure("同じファイルの素材が重複しています: " + item.name);
        const auto values = validateItemValues(item);
        if (!values.success)
            return values;
    }
    return validateMediaReferences(project);
}

MediaBinEditResult validateMediaReferences(const Project& project) {
    // clip ごとに素材列を走査しない (clip 数 x 素材数にしない)。素材の表記上の key も
    // 素材ごとに 1 回だけ作る。id が重複していれば先頭を使う (findMediaItem と同じ)。
    struct IndexedItem {
        const MediaItem* item = nullptr;
        std::wstring pathKey;
        bool keyed = false;
    };

    std::unordered_map<std::string_view, IndexedItem> items;
    items.reserve(project.mediaItems.size());
    for (const auto& item : project.mediaItems)
        items.try_emplace(item.id, IndexedItem{&item, {}, false});
    for (const auto& clip : project.timelineClips) {
        if (!clipUsesMediaItem(clip.kind)) {
            if (!clip.mediaItemId.empty())
                return failure("文字・Manim の clip は素材を参照できません: " + clip.name);
            continue;
        }
        const auto indexed = items.find(clip.mediaItemId);
        if (indexed == items.end())
            return failure("clip \"" + clip.name + "\" の素材がプロジェクトパネルにありません");
        const auto* item = indexed->second.item;
        // 映像のある素材からは音声 clip も作れる (リンクした音声)。
        const bool kindMatches =
            (clip.kind == TimelineClipKind::Video && item->kind == MediaKind::Video) ||
            (clip.kind == TimelineClipKind::Audio &&
             (item->kind == MediaKind::Audio || item->kind == MediaKind::Video)) ||
            (clip.kind == TimelineClipKind::Image && item->kind == MediaKind::Image);
        if (!kindMatches)
            return failure("clip \"" + clip.name + "\" の種別が素材の種別と一致しません");
        // clip は素材と同じファイルを指す。表記で比べ、ファイルの有無には依存させない。
        if (!indexed->second.keyed) {
            indexed->second.pathKey = canonicalPathKey(item->mediaPath);
            indexed->second.keyed = true;
        }
        if (canonicalPathKey(clip.mediaPath) != indexed->second.pathKey)
            return failure("clip \"" + clip.name + "\" のファイルが素材と一致しません");
    }
    return {true, {}, {}};
}

MediaBinEditResult addMediaFolder(Project& project, MediaFolder folder) {
    Project candidate = project;
    candidate.mediaFolders.push_back(std::move(folder));
    return commitCandidate(project, std::move(candidate));
}

MediaBinEditResult addMediaItem(Project& project, MediaItem item) {
    if (findMediaItemByPath(project, item.mediaPath))
        return failure("既に読み込み済みの素材です: " + item.name);
    Project candidate = project;
    candidate.mediaItems.push_back(std::move(item));
    return commitCandidate(project, std::move(candidate));
}

MediaBinEditResult refreshMediaItem(Project& project, const std::string& itemId,
                                    const MediaItem& probed) {
    Project candidate = project;
    for (auto& item : candidate.mediaItems) {
        if (item.id != itemId)
            continue;
        const bool timingChanged =
            item.kind != probed.kind || item.fpsNum != probed.fpsNum ||
            item.fpsDen != probed.fpsDen || item.frameCount != probed.frameCount ||
            item.sampleRate != probed.sampleRate || item.durationSamples != probed.durationSamples;
        if (timingChanged && mediaItemsInUse(project).contains(itemId))
            return failure("タイムラインで使用中の素材 \"" + item.name +
                           "\" のファイルが別の長さ・fps・種類のものに差し替わっています");
        item.kind = probed.kind;
        item.fpsNum = probed.fpsNum;
        item.fpsDen = probed.fpsDen;
        item.frameCount = probed.frameCount;
        item.width = probed.width;
        item.height = probed.height;
        item.sampleRate = probed.sampleRate;
        item.durationSamples = probed.durationSamples;
        return commitCandidate(project, std::move(candidate));
    }
    return failure("調べ直す素材がありません");
}

MediaBinEditResult renameMediaBinEntry(Project& project, const std::string& entryId,
                                       std::string name) {
    if (name.empty())
        return failure("名前を空にはできません");
    Project candidate = project;
    for (auto& folder : candidate.mediaFolders) {
        if (folder.id == entryId) {
            folder.name = std::move(name);
            return commitCandidate(project, std::move(candidate));
        }
    }
    for (auto& item : candidate.mediaItems) {
        if (item.id == entryId) {
            item.name = std::move(name);
            return commitCandidate(project, std::move(candidate));
        }
    }
    return failure("名前を変更する素材がありません");
}

MediaBinEditResult moveMediaBinEntries(Project& project, const std::vector<std::string>& entryIds,
                                       const std::string& targetFolderId) {
    if (entryIds.empty())
        return failure("移動する素材がありません");
    if (!targetFolderId.empty() && !findMediaFolder(project, targetFolderId))
        return failure("移動先のフォルダがありません");
    Project candidate = project;
    for (const auto& id : entryIds) {
        bool found = false;
        for (auto& folder : candidate.mediaFolders) {
            if (folder.id != id)
                continue;
            // 自分自身や子孫の下へ入れると循環する。validate でも弾けるが、理由を明示する。
            for (std::string cursor = targetFolderId; !cursor.empty();
                 cursor = findMediaFolder(project, cursor)->parentId) {
                if (cursor == id)
                    return failure("フォルダをそれ自身の中へは移動できません");
            }
            folder.parentId = targetFolderId;
            found = true;
        }
        for (auto& item : candidate.mediaItems) {
            if (item.id != id)
                continue;
            item.folderId = targetFolderId;
            found = true;
        }
        if (!found)
            return failure("移動する素材がありません");
    }
    return commitCandidate(project, std::move(candidate));
}

MediaBinRemovalPlan planMediaBinRemoval(const Project& project,
                                        const std::vector<std::string>& entryIds) {
    MediaBinRemovalPlan plan;
    if (entryIds.empty()) {
        plan.error = "削除する素材がありません";
        return plan;
    }
    // 指定 folder の子孫を広げて、削除対象の folder id 集合を確定させる。
    for (const auto& id : entryIds) {
        if (findMediaFolder(project, id)) {
            plan.folderIds.insert(id);
        } else if (findMediaItem(project, id)) {
            plan.itemIds.insert(id);
        } else {
            plan.error = "削除する素材がありません";
            return plan;
        }
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& folder : project.mediaFolders) {
            if (plan.folderIds.contains(folder.parentId) && plan.folderIds.insert(folder.id).second)
                grew = true;
        }
    }
    for (const auto& item : project.mediaItems) {
        if (plan.folderIds.contains(item.folderId))
            plan.itemIds.insert(item.id);
    }

    // clip は mediaItemId で素材を指すので、ファイルの実体を調べずに決まる。
    std::set<std::string> linkGroups;
    std::set<std::string> clipIds;
    for (const auto& clip : project.timelineClips) {
        if (clip.mediaItemId.empty() || !plan.itemIds.contains(clip.mediaItemId))
            continue;
        clipIds.insert(clip.id);
        if (!clip.linkGroupId.empty())
            linkGroups.insert(clip.linkGroupId);
    }
    // リンク相手も消す。片側だけ残すと deleteTimelineClip と挙動が食い違う。
    for (const auto& clip : project.timelineClips) {
        if (clipIds.contains(clip.id) ||
            (!clip.linkGroupId.empty() && linkGroups.contains(clip.linkGroupId)))
            plan.clipIds.push_back(clip.id);
    }
    plan.success = true;
    return plan;
}

MediaBinEditResult removeMediaBinEntries(Project& project,
                                         const std::vector<std::string>& entryIds) {
    const auto plan = planMediaBinRemoval(project, entryIds);
    if (!plan.success)
        return failure(plan.error);

    Project candidate = project;
    std::erase_if(candidate.mediaFolders,
                  [&](const MediaFolder& folder) { return plan.folderIds.contains(folder.id); });
    std::erase_if(candidate.mediaItems,
                  [&](const MediaItem& item) { return plan.itemIds.contains(item.id); });
    const std::set<std::string> clipIds(plan.clipIds.begin(), plan.clipIds.end());
    std::erase_if(candidate.timelineClips,
                  [&](const TimelineClip& clip) { return clipIds.contains(clip.id); });
    const auto timeline = finalizeTimelineCandidate(candidate);
    if (!timeline.success)
        return failure(timeline.error);
    auto result = commitCandidate(project, std::move(candidate));
    if (result.success)
        result.removedClipIds = plan.clipIds;
    return result;
}

const MediaItem* findMediaItem(const Project& project, const std::string& itemId) {
    for (const auto& item : project.mediaItems) {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path) {
    const auto key = fileIdentityKey(path);
    for (const auto& item : project.mediaItems) {
        if (comparePathIdentity(fileIdentityKey(item.mediaPath), key) == PathSameness::Same)
            return &item;
    }
    return nullptr;
}

const MediaFolder* findMediaFolder(const Project& project, const std::string& folderId) {
    for (const auto& folder : project.mediaFolders) {
        if (folder.id == folderId)
            return &folder;
    }
    return nullptr;
}

std::set<std::string> mediaItemsInUse(const Project& project) {
    std::set<std::string> used;
    for (const auto& clip : project.timelineClips) {
        if (!clip.mediaItemId.empty())
            used.insert(clip.mediaItemId);
    }
    return used;
}

const char* mediaKindName(MediaKind kind) {
    switch (kind) {
    case MediaKind::Video:
        return "video";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Image:
        return "image";
    }
    return "";
}

bool parseMediaKindName(const std::string& text, MediaKind& kind) {
    if (text == "video")
        kind = MediaKind::Video;
    else if (text == "audio")
        kind = MediaKind::Audio;
    else if (text == "image")
        kind = MediaKind::Image;
    else
        return false;
    return true;
}

} // namespace mvm::project
