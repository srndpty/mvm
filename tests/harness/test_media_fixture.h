#ifndef MVM_TESTS_HARNESS_TEST_MEDIA_FIXTURE_H
#define MVM_TESTS_HARNESS_TEST_MEDIA_FIXTURE_H

// 試験用 Project の動画・音声・画像 clip に、プロジェクトパネルの素材を付ける。
// clip は素材を mediaItemId で指す必要がある (validateMediaReferences) ので、timeline の
// 挙動だけを見る試験でも保存・読み込み・commit の前に素材を揃えておく。
// 同じファイルの clip (動画とリンクした音声) は 1 つの素材を共有する。素材の値は
// 検証を通る最小のもので、試験の期待値には使わない。

#include "project/media_bin.h"
#include "project/path_identity.h"
#include "project/project.h"

#include <map>
#include <numeric>
#include <string>

namespace mvm::test {

inline void attachFixtureMedia(project::Project& project) {
    using project::MediaItem;
    using project::MediaKind;
    using project::TimelineClipKind;
    std::map<std::wstring, std::string> itemByPath;
    for (const auto& item : project.mediaItems)
        itemByPath.emplace(project::canonicalPathKey(item.mediaPath), item.id);
    // 映像のある clip を先に見る。同じファイルの音声 clip はその素材を共有する。
    for (const bool audioPass : {false, true}) {
        for (auto& clip : project.timelineClips) {
            if (!project::clipUsesMediaItem(clip.kind) || !clip.mediaItemId.empty() ||
                (clip.kind == TimelineClipKind::Audio) != audioPass)
                continue;
            const auto key = project::canonicalPathKey(clip.mediaPath);
            if (const auto found = itemByPath.find(key); found != itemByPath.end()) {
                clip.mediaItemId = found->second;
                continue;
            }
            MediaItem item;
            item.id = "fixture-media-" + std::to_string(project.mediaItems.size());
            item.mediaPath = clip.mediaPath;
            item.name = clip.name.empty() ? item.id : clip.name;
            switch (clip.kind) {
            case TimelineClipKind::Video: {
                item.kind = MediaKind::Video;
                const auto divisor = std::gcd(clip.sourceFpsNum, clip.sourceFpsDen);
                item.fpsNum = divisor > 0 ? clip.sourceFpsNum / divisor : 60;
                item.fpsDen = divisor > 0 ? clip.sourceFpsDen / divisor : 1;
                item.frameCount = clip.sourceFrameCount > 0 ? clip.sourceFrameCount : 1;
                item.width = 1920;
                item.height = 1080;
                break;
            }
            case TimelineClipKind::Audio:
                item.kind = MediaKind::Audio;
                item.sampleRate = 48000;
                item.durationSamples = 48000;
                break;
            default:
                item.kind = MediaKind::Image;
                item.width = 1920;
                item.height = 1080;
                break;
            }
            itemByPath.emplace(key, item.id);
            clip.mediaItemId = item.id;
            project.mediaItems.push_back(std::move(item));
        }
    }
}

} // namespace mvm::test

#endif // MVM_TESTS_HARNESS_TEST_MEDIA_FIXTURE_H
