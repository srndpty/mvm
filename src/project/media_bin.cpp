#include "project/media_bin.h"

#include <algorithm>
#include <set>
#include <utility>

namespace mvm::project {
namespace {

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
    return left.lexically_normal() == right.lexically_normal();
}

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
    return {true, {}};
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
    for (std::size_t index = 0; index < project.mediaItems.size(); ++index) {
        const auto& item = project.mediaItems[index];
        if (item.id.empty() || !ids.insert(item.id).second)
            return failure("素材の id が空または重複しています: " + item.id);
        if (item.name.empty() || item.mediaPath.empty())
            return failure("素材の名前または media_path が空です");
        if (!item.folderId.empty() && !findMediaFolder(project, item.folderId))
            return failure("素材 \"" + item.name + "\" のフォルダがありません");
        for (std::size_t other = 0; other < index; ++other) {
            if (samePath(project.mediaItems[other].mediaPath, item.mediaPath))
                return failure("同じファイルの素材が重複しています: " + item.name);
        }
        const auto values = validateItemValues(item);
        if (!values.success)
            return values;
    }
    return {true, {}};
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

MediaBinEditResult removeMediaBinEntries(Project& project,
                                         const std::vector<std::string>& entryIds) {
    if (entryIds.empty())
        return failure("削除する素材がありません");
    // 指定 folder の子孫を広げて、削除対象の folder id 集合を確定させる。
    std::set<std::string> removedFolders;
    std::set<std::string> removedItems;
    for (const auto& id : entryIds) {
        if (findMediaFolder(project, id))
            removedFolders.insert(id);
        else if (findMediaItem(project, id))
            removedItems.insert(id);
        else
            return failure("削除する素材がありません");
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& folder : project.mediaFolders) {
            if (removedFolders.contains(folder.parentId) && removedFolders.insert(folder.id).second)
                grew = true;
        }
    }
    for (const auto& item : project.mediaItems) {
        if (removedFolders.contains(item.folderId))
            removedItems.insert(item.id);
    }
    for (const auto& item : project.mediaItems) {
        if (removedItems.contains(item.id) && isMediaItemInUse(project, item))
            return failure("タイムラインで使用中の素材は削除できません: " + item.name);
    }

    Project candidate = project;
    std::erase_if(candidate.mediaFolders,
                  [&](const MediaFolder& folder) { return removedFolders.contains(folder.id); });
    std::erase_if(candidate.mediaItems,
                  [&](const MediaItem& item) { return removedItems.contains(item.id); });
    return commitCandidate(project, std::move(candidate));
}

const MediaItem* findMediaItem(const Project& project, const std::string& itemId) {
    for (const auto& item : project.mediaItems) {
        if (item.id == itemId)
            return &item;
    }
    return nullptr;
}

const MediaItem* findMediaItemByPath(const Project& project, const std::filesystem::path& path) {
    for (const auto& item : project.mediaItems) {
        if (samePath(item.mediaPath, path))
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

bool isMediaItemInUse(const Project& project, const MediaItem& item) {
    return std::any_of(
        project.timelineClips.begin(), project.timelineClips.end(),
        [&](const TimelineClip& clip) { return samePath(clip.mediaPath, item.mediaPath); });
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
