#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "media_import.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <QFileInfo>

namespace mvm::app {
using detail::newClipId;
using detail::ProbedMedia;
using detail::videoFacts;

namespace {
// audio 素材には映像 fps が無い。Project timeline の fps を素材の frame domain と
// して採用し、尺だけを duration から求める。frame 算術を 1 種類に保つための選択で
// あり、素材側に fps があると主張しているわけではない。
ProbedMedia audioFacts(const MediaImportResult& probed, std::int64_t timelineFpsNum,
                       std::int64_t timelineFpsDen) {
    ProbedMedia result;
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    const bool hasAudio = probed.item.kind == project::MediaKind::Audio ||
                          (probed.item.kind == project::MediaKind::Video && probed.hasAudio);
    if (!hasAudio) {
        result.error = QStringLiteral("音声トラックがありません");
        return result;
    }
    const auto frames = audioSourceFrameCount(probed.durationSec, timelineFpsNum, timelineFpsDen);
    if (!frames.success) {
        result.error = QString::fromStdString(frames.error);
        return result;
    }
    result.fpsNum = timelineFpsNum;
    result.fpsDen = timelineFpsDen;
    result.frameCount = frames.frameCount;
    result.success = true;
    return result;
}

// 素材 1 つを timeline へ置くときの clip。音声を持つ動画は linkedAudio にリンク相手を持つ。
// 置き場所 (track / 開始位置) は呼び出し側が決める。
struct MediaClips {
    bool success = false;
    QString error;
    project::TimelineClip primary;
    std::optional<project::TimelineClip> linkedAudio;
};

project::TimelineClip mediaClip(project::TimelineClipKind kind, const project::MediaItem& item,
                                std::int64_t fpsNum, std::int64_t fpsDen, std::int64_t frameCount) {
    project::TimelineClip clip;
    clip.kind = kind;
    // clip は素材と同じファイルを指す (validateMediaReferences の不変条件)。
    clip.mediaPath = item.mediaPath;
    clip.mediaItemId = item.id;
    clip.name = QString::fromStdWString(item.mediaPath.filename().wstring()).toStdString();
    clip.id = newClipId();
    clip.sourceFpsNum = fpsNum;
    clip.sourceFpsDen = fpsDen;
    clip.sourceFrameCount = frameCount;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = frameCount;
    return clip;
}

// プロジェクトパネルの素材 itemId から、timeline へ置く clip を作る。kind は作る clip の種別
// (動画素材の音声だけを置くなら Audio)。使う時点でファイルを調べ直し、素材の値 (種別・fps・
// 尺・解像度) を実物に合わせて candidate 上で更新してから clip を作る。登録後に消えた・
// 別の種類へ差し替わったファイルは置かず、同じ種類の別の中身 (縦横が違う画像など) なら
// 素材の値が新しくなる。prebuilt は同じファイルを判定済みの結果 (あれば調べ直さない。
// 画像の decode は重い)。
MediaClips prepareMediaClips(project::Project& candidate, const std::string& itemId,
                             project::MediaKind kind, const MediaImportResult* prebuilt = nullptr) {
    MediaClips result;
    const auto* found = project::findMediaItem(candidate, itemId);
    if (!found) {
        result.error = QStringLiteral("素材がありません");
        return result;
    }
    const auto probed = prebuilt ? *prebuilt : probeMediaFile(found->mediaPath);
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    // 種別が clip の用途に合うかを先に見る (合わない差し替えで素材の値を書き換えない)。
    ProbedMedia facts;
    switch (kind) {
    case project::MediaKind::Video:
        facts = videoFacts(probed);
        break;
    case project::MediaKind::Audio:
        facts = audioFacts(probed, candidate.timelineFpsNum, candidate.timelineFpsDen);
        break;
    case project::MediaKind::Image:
        facts.success = probed.item.kind == project::MediaKind::Image;
        if (!facts.success)
            facts.error = QStringLiteral("画像ではありません");
        break;
    }
    if (!facts.success) {
        result.error = facts.error;
        return result;
    }
    const auto refreshed = project::refreshMediaItem(candidate, itemId, probed.item);
    if (!refreshed.success) {
        result.error = QStringLiteral("素材の情報を更新できません: ") +
                       QString::fromStdString(refreshed.error);
        return result;
    }
    const auto& item = *project::findMediaItem(candidate, itemId);
    switch (kind) {
    case project::MediaKind::Video:
        result.primary = mediaClip(project::TimelineClipKind::Video, item, facts.fpsNum,
                                   facts.fpsDen, facts.frameCount);
        if (facts.hasAudio) {
            result.linkedAudio = mediaClip(project::TimelineClipKind::Audio, item, facts.fpsNum,
                                           facts.fpsDen, facts.frameCount);
            result.primary.linkGroupId = result.linkedAudio->linkGroupId = newClipId();
        }
        break;
    case project::MediaKind::Audio:
        result.primary = mediaClip(project::TimelineClipKind::Audio, item, facts.fpsNum,
                                   facts.fpsDen, facts.frameCount);
        break;
    case project::MediaKind::Image:
        result.primary = mediaClip(
            project::TimelineClipKind::Image, item, candidate.timelineFpsNum,
            candidate.timelineFpsDen,
            project::defaultStillClipFrames(candidate.timelineFpsNum, candidate.timelineFpsDen));
        break;
    }
    result.success = true;
    return result;
}

} // namespace

bool MvmController::addVideoClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return false;
    }
    const QString localFile = fileUrl.toLocalFile();
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile()) {
        setStatus(QStringLiteral("存在する動画ファイルを選択してください"));
        return false;
    }

    const std::filesystem::path mediaPath(localFile.toStdWString());
    project::Project candidate = project_;
    // 素材を先にプロジェクトパネルへ登録し、clip はその素材から作る。
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips = prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Video);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    // 先頭素材だけ V1、以降は playhead 上の新しい Vn へ置く。
    const bool hasVideoClip = std::any_of(
        candidate.timelineClips.begin(), candidate.timelineClips.end(),
        [](const auto& entry) { return entry.kind != project::TimelineClipKind::Audio; });
    int videoTrackIndex = 0;
    if (hasVideoClip) {
        const auto addedTrack = project::addTrack(candidate, project::TrackKind::Video);
        if (!addedTrack.success) {
            setStatus(QString::fromStdString(addedTrack.error));
            return false;
        }
        videoTrackIndex = addedTrack.selectedIndex;
    }
    project::TimelineEditResult placed;
    if (clips.linkedAudio) {
        if (candidate.audioTracks.empty()) {
            const auto addedTrack = project::addTrack(candidate, project::TrackKind::Audio);
            if (!addedTrack.success) {
                setStatus(QString::fromStdString(addedTrack.error));
                return false;
            }
        }
        placed = project::placeLinkedAvPairAt(
            candidate, clips.primary, project::TrackRef{project::TrackKind::Video, videoTrackIndex},
            *clips.linkedAudio, project::TrackRef{project::TrackKind::Audio, 0}, playheadFrame_);
        // A1 が使用中なら既存 clip を壊さず、新しい audio track に同じ位置で置く。
        if (!placed.success) {
            const auto addedTrack = project::addTrack(candidate, project::TrackKind::Audio);
            if (addedTrack.success)
                placed = project::placeLinkedAvPairAt(
                    candidate, std::move(clips.primary),
                    project::TrackRef{project::TrackKind::Video, videoTrackIndex},
                    std::move(*clips.linkedAudio),
                    project::TrackRef{project::TrackKind::Audio, addedTrack.selectedIndex},
                    playheadFrame_);
        }
        if (!placed.success) {
            setStatus(QStringLiteral("リンク音声をA1へ配置できません: ") +
                      QString::fromStdString(placed.error));
            return false;
        }
    } else {
        placed = project::placeTimelineClipAt(
            candidate, std::move(clips.primary),
            project::TrackRef{project::TrackKind::Video, videoTrackIndex}, playheadFrame_);
        if (!placed.success) {
            setStatus(QString::fromStdString(placed.error));
            return false;
        }
    }

    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    const int index = placed.selectedIndex;
    Q_EMIT stateChanged();
    return selectClip(index);
}

bool MvmController::addAudioClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在する音声ファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    project::Project candidate = project_;
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips = prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Audio);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    // 動画・画像と同じく再生ヘッドの位置に置く。空いた audio track が無ければ足す。
    const auto placed =
        project::placeAudioClipAt(candidate, std::move(clips.primary), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    const int index = placed.selectedIndex;
    Q_EMIT stateChanged();
    return selectClip(index);
}

const project::MediaItem*
MvmController::registerMediaItem(project::Project& candidate,
                                 const std::filesystem::path& mediaPath, QString& error,
                                 const MediaImportResult* prebuilt) const {
    if (const auto* existing = project::findMediaItemByPath(candidate, mediaPath))
        return existing;
    auto probed = prebuilt ? *prebuilt : probeMediaFile(mediaPath);
    if (!probed.success) {
        error = QStringLiteral("素材をプロジェクトへ登録できません: ") +
                QString::fromStdString(probed.error);
        return nullptr;
    }
    const std::string id = newClipId();
    probed.item.id = id;
    probed.item.name =
        QFileInfo(QString::fromStdWString(mediaPath.wstring())).fileName().toStdString();
    const auto added = project::addMediaItem(candidate, std::move(probed.item));
    if (!added.success) {
        error = QStringLiteral("素材をプロジェクトへ登録できません: ") +
                QString::fromStdString(added.error);
        return nullptr;
    }
    return project::findMediaItem(candidate, id);
}

bool MvmController::applyMediaBinEdit(
    const std::function<project::MediaBinEditResult(project::Project&)>& edit,
    const QString& successStatus) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    const auto edited = edit(candidate);
    if (!edited.success) {
        setStatus(QString::fromStdString(edited.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    setStatus(successStatus);
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::importMediaFiles(const QList<QUrl>& fileUrls, const QString& folderId) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    int imported = 0;
    int alreadyImported = 0;
    QStringList failures;
    for (const QUrl& url : fileUrls) {
        if (!url.isLocalFile()) {
            failures << url.toString() + QStringLiteral(": ローカルファイルではありません");
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() || !info.isFile()) {
            failures << info.fileName() + QStringLiteral(": ファイルがありません");
            continue;
        }
        const std::filesystem::path mediaPath(info.absoluteFilePath().toStdWString());
        if (project::findMediaItemByPath(candidate, mediaPath)) {
            ++alreadyImported;
            continue;
        }
        auto probed = probeMediaFile(mediaPath);
        if (!probed.success) {
            failures << info.fileName() + QStringLiteral(": ") +
                            QString::fromStdString(probed.error);
            continue;
        }
        probed.item.id = newClipId();
        probed.item.name = info.fileName().toStdString();
        probed.item.folderId = folderId.toStdString();
        const auto added = project::addMediaItem(candidate, std::move(probed.item));
        if (!added.success) {
            failures << info.fileName() + QStringLiteral(": ") +
                            QString::fromStdString(added.error);
            continue;
        }
        ++imported;
    }
    if (imported > 0 &&
        !commitProjectEdit(std::move(candidate), QStringLiteral("素材を読み込めません: ")))
        return false;

    QStringList parts;
    if (imported > 0)
        parts << QStringLiteral("%1 件の素材を読み込みました").arg(imported);
    if (alreadyImported > 0)
        parts << QStringLiteral("読み込み済みの %1 件は省略しました").arg(alreadyImported);
    if (!failures.isEmpty())
        parts << QStringLiteral("%1 件は読み込めません (%2)")
                     .arg(failures.size())
                     .arg(failures.join(QStringLiteral(" / ")));
    setStatus(parts.join(QStringLiteral("。")));
    if (imported > 0 && !folderId.isEmpty())
        mediaBinModel_->setExpanded(folderId, true);
    Q_EMIT stateChanged();
    // 一部が読めなくても、読めた素材は commit 済みである。false を「変更なし」の意味に保つ。
    return imported > 0;
}

QString MvmController::createMediaFolder(const QString& parentFolderId) {
    // 兄弟に限らず Project 全体で未使用の番号を振る。移動しても名前が衝突しない。
    QString name;
    for (int number = 1;; ++number) {
        name = QStringLiteral("フォルダ %1").arg(number, 2, 10, QLatin1Char('0'));
        const std::string candidateName = name.toStdString();
        if (std::none_of(project_.mediaFolders.begin(), project_.mediaFolders.end(),
                         [&](const auto& folder) { return folder.name == candidateName; }))
            break;
    }
    project::MediaFolder folder{newClipId(), name.toStdString(), parentFolderId.toStdString()};
    const QString id = QString::fromStdString(folder.id);
    const bool created = applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::addMediaFolder(candidate, std::move(folder));
        },
        QStringLiteral("フォルダを作成しました: ") + name);
    if (!created)
        return {};
    if (!parentFolderId.isEmpty())
        mediaBinModel_->setExpanded(parentFolderId, true);
    return id;
}

bool MvmController::renameMediaBinEntry(const QString& entryId, const QString& name) {
    const QString trimmed = name.trimmed();
    return applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::renameMediaBinEntry(candidate, entryId.toStdString(),
                                                trimmed.toStdString());
        },
        QStringLiteral("名前を変更しました: ") + trimmed);
}

bool MvmController::moveMediaBinEntries(const QStringList& entryIds, const QString& folderId) {
    std::vector<std::string> ids;
    for (const auto& id : entryIds)
        ids.push_back(id.toStdString());
    const bool moved = applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::moveMediaBinEntries(candidate, ids, folderId.toStdString());
        },
        QStringLiteral("%1 件を移動しました").arg(entryIds.size()));
    if (moved && !folderId.isEmpty())
        mediaBinModel_->setExpanded(folderId, true);
    return moved;
}

namespace {

std::vector<std::string> toStdIds(const QStringList& ids) {
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(ids.size()));
    for (const auto& id : ids)
        result.push_back(id.toStdString());
    return result;
}

} // namespace

int MvmController::mediaBinRemovalClipCount(const QStringList& entryIds) {
    const auto plan = project::planMediaBinRemoval(project_, toStdIds(entryIds));
    if (!plan.success) {
        setStatus(QString::fromStdString(plan.error));
        return -1;
    }
    return static_cast<int>(plan.clipIds.size());
}

bool MvmController::removeMediaBinEntries(const QStringList& entryIds) {
    if (busy_)
        return false;
    const auto ids = toStdIds(entryIds);
    const auto plan = project::planMediaBinRemoval(project_, ids);
    if (!plan.success) {
        setStatus(QString::fromStdString(plan.error));
        return false;
    }
    // 再生中の source が削除する clip を掴んだまま進まないよう、先に止める。
    if (!plan.clipIds.empty() && !pauseTimeline())
        return false;
    std::size_t removedClips = 0;
    const bool removed = applyMediaBinEdit(
        [&](project::Project& candidate) {
            auto result = project::removeMediaBinEntries(candidate, ids);
            removedClips = result.removedClipIds.size();
            return result;
        },
        QStringLiteral("%1 件を削除しました").arg(entryIds.size()));
    if (!removed || removedClips == 0)
        return removed;
    const QString resetFailure = resetAfterClipRemoval();
    if (!resetFailure.isEmpty()) {
        setStatus(QStringLiteral("素材とclipは削除しましたが、") + resetFailure);
        return true;
    }
    setStatus(QStringLiteral("%1 件の素材と %2 個のclipを削除しました")
                  .arg(entryIds.size())
                  .arg(removedClips));
    return true;
}

bool MvmController::addMediaItemToTimeline(const QString& itemId) {
    const auto* item = project::findMediaItem(project_, itemId.toStdString());
    if (!item) {
        setStatus(QStringLiteral("素材がありません"));
        return false;
    }
    const QUrl url = QUrl::fromLocalFile(QString::fromStdWString(item->mediaPath.wstring()));
    switch (item->kind) {
    case project::MediaKind::Video:
        return addVideoClip(url);
    case project::MediaKind::Audio:
        return addAudioClip(url);
    case project::MediaKind::Image:
        return addImageClip(url);
    }
    setStatus(QStringLiteral("素材の種別が不正です"));
    return false;
}

bool MvmController::placeMediaAtDropPoint(const std::vector<DropMedia>& media,
                                          const QString& trackKind, int trackIndex, qint64 frame) {
    if (busy_)
        return false;
    if (media.empty()) {
        setStatus(QStringLiteral("タイムラインへ置く素材がありません"));
        return false;
    }
    project::TrackRef target;
    if (trackKind == QStringLiteral("video"))
        target.kind = project::TrackKind::Video;
    else if (trackKind == QStringLiteral("audio"))
        target.kind = project::TrackKind::Audio;
    else {
        setStatus(QStringLiteral("ドロップ先の track 種別が不正です"));
        return false;
    }
    target.index = trackIndex;
    if (!pauseTimeline())
        return false;

    project::Project candidate = project_;
    std::int64_t start = std::max<qint64>(frame, 0);
    int firstIndex = -1;
    for (const auto& entry : media) {
        const QString name = QString::fromStdWString(entry.path.filename().wstring());
        QString registerError;
        const auto* item = entry.itemId.empty() ? registerMediaItem(candidate, entry.path,
                                                                    registerError, entry.probed)
                                                : project::findMediaItem(candidate, entry.itemId);
        if (!item) {
            setStatus(entry.itemId.empty() ? registerError : QStringLiteral("素材がありません"));
            return false;
        }
        auto clips = prepareMediaClips(candidate, std::string(item->id), entry.kind, entry.probed);
        if (!clips.success) {
            setStatus(name + QStringLiteral(": ") + clips.error);
            return false;
        }
        const auto placed = project::placeMediaAtDrop(candidate, std::move(clips.primary),
                                                      std::move(clips.linkedAudio), target, start);
        if (!placed.success) {
            setStatus(name + QStringLiteral(": ") + QString::fromStdString(placed.error));
            return false;
        }
        // track を足して置いた場合も、その track は同じ index にあるので続きも同じ行へ並ぶ。
        const auto duration = project::timelineClipDuration(
            candidate, candidate.timelineClips[static_cast<std::size_t>(placed.selectedIndex)]);
        if (!duration.success) {
            setStatus(QString::fromStdString(duration.error));
            return false;
        }
        if (firstIndex < 0)
            firstIndex = placed.selectedIndex;
        start += duration.frame;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    Q_EMIT stateChanged();
    return selectClip(firstIndex);
}

bool MvmController::addMediaItemsToTimelineAt(const QStringList& itemIds, const QString& trackKind,
                                              int trackIndex, qint64 frame) {
    std::vector<DropMedia> media;
    for (const auto& id : itemIds) {
        // フォルダは中身を展開せずに飛ばす (どの順で並べるかを決められないため)。
        const auto* item = project::findMediaItem(project_, id.toStdString());
        if (item)
            media.push_back({item->mediaPath, item->kind, nullptr, item->id});
    }
    return placeMediaAtDropPoint(media, trackKind, trackIndex, frame);
}

bool MvmController::addMediaFilesToTimelineAt(const QList<QUrl>& fileUrls, const QString& trackKind,
                                              int trackIndex, qint64 frame) {
    if (busy_)
        return false;
    // probed は media から指されるので、途中で再配置されないよう先に確保する。
    std::vector<MediaImportResult> probed;
    probed.reserve(static_cast<std::size_t>(fileUrls.size()));
    std::vector<DropMedia> media;
    for (const auto& url : fileUrls) {
        const std::filesystem::path mediaPath =
            localMediaFile(url, QStringLiteral("存在するファイルを選択してください"));
        if (mediaPath.empty())
            return false;
        probed.push_back(probeMediaFile(mediaPath));
        if (!probed.back().success) {
            setStatus(QFileInfo(url.toLocalFile()).fileName() + QStringLiteral(": ") +
                      QString::fromStdString(probed.back().error));
            return false;
        }
        media.push_back({mediaPath, probed.back().item.kind, &probed.back(), {}});
    }
    return placeMediaAtDropPoint(media, trackKind, trackIndex, frame);
}

std::filesystem::path MvmController::localMediaFile(const QUrl& fileUrl,
                                                    const QString& missingText) {
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return {};
    }
    const QFileInfo info(fileUrl.toLocalFile());
    if (!info.exists() || !info.isFile()) {
        setStatus(missingText);
        return {};
    }
    return std::filesystem::path(info.absoluteFilePath().toStdWString());
}

bool MvmController::addImageClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在する画像ファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    const auto probed = probeMediaFile(mediaPath);
    if (!probed.success) {
        setStatus(QString::fromStdString(probed.error));
        return false;
    }
    if (probed.item.kind != project::MediaKind::Image) {
        setStatus(QStringLiteral("画像ではありません"));
        return false;
    }
    return placeImageClip(mediaPath, QFileInfo(fileUrl.toLocalFile()).fileName(), probed);
}

bool MvmController::placeImageClip(const std::filesystem::path& mediaPath, const QString& fileName,
                                   const MediaImportResult& probed) {
    project::Project candidate = project_;
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError, &probed);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips =
        prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Image, &probed);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    clips.primary.name = fileName.toStdString();
    const auto placed =
        project::placeStillClipAt(candidate, std::move(clips.primary), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("画像 clip を追加できません: ")))
        return false;
    Q_EMIT stateChanged();
    return selectClip(placed.selectedIndex);
}

bool MvmController::addMediaFileToTimeline(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在するファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    const auto probed = probeMediaFile(mediaPath);
    if (!probed.success) {
        setStatus(QString::fromStdString(probed.error));
        return false;
    }
    switch (probed.item.kind) {
    case project::MediaKind::Video:
        return addVideoClip(fileUrl);
    case project::MediaKind::Audio:
        return addAudioClip(fileUrl);
    case project::MediaKind::Image:
        return placeImageClip(mediaPath, QFileInfo(fileUrl.toLocalFile()).fileName(), probed);
    }
    setStatus(QStringLiteral("素材の種別が不正です"));
    return false;
}

} // namespace mvm::app
