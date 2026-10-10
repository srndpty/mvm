#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "core/timecode.h"
#include "image_raster_cache.h"
#include "project/clip_effects.h"
#include "project/equation_sequence_edit.h"
#include "project/graph_edit.h"
#include "project/subtitles.h"
#include "project/timeline_edit.h"
#include "shuttle_audio_playback.h"
#include "timeline_clip_model.h"
#include "track_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace mvm::app {
using detail::fromPath;
using detail::indexOfClipId;
using detail::linearToDb;
using detail::newClipId;
using detail::previewErrorText;

namespace {
// QML から渡る「リンク相手にも適用するか」をモデルの LinkMode へ写す。
project::LinkMode linkModeFor(bool linked) {
    return linked ? project::LinkMode::Linked : project::LinkMode::Single;
}

std::optional<project::ClipSpeedDurationEdit>
speedDurationEdit(const QString& input, double speedPercent, const QString& durationText,
                  bool preservePitch, bool ripple, std::int64_t fpsNum, std::int64_t fpsDen) {
    project::ClipSpeedDurationEdit edit;
    edit.preservePitch = preservePitch;
    edit.ripple = ripple;
    if (input == QStringLiteral("speed")) {
        if (!std::isfinite(speedPercent) || speedPercent < 10.0 || speedPercent > 1000.0)
            return std::nullopt;
        const auto hundredths = static_cast<std::int64_t>(std::llround(speedPercent * 100.0));
        edit.input = project::ClipSpeedDurationEdit::Input::Speed;
        edit.speedNum = hundredths;
        edit.speedDen = 10000;
    } else if (input == QStringLiteral("duration")) {
        const auto frames = core::parseTimecode(durationText.toStdString(), fpsNum, fpsDen);
        if (!frames || *frames < 1)
            return std::nullopt;
        edit.input = project::ClipSpeedDurationEdit::Input::Duration;
        edit.durationFrames = *frames;
    } else {
        return std::nullopt;
    }
    return edit;
}

} // namespace

QVariantList MvmController::timelineMarkers() const {
    QVariantList frames;
    for (const auto frame : project_.timelineMarkers)
        frames.push_back(QVariant::fromValue<qlonglong>(frame));
    return frames;
}

std::vector<project::TimelineClip>
MvmController::selectedTimelineClipsInOrder(const std::string& anchorId) const {
    const bool anchorSelected =
        anchorId.empty() || std::find(selectedClipIds_.begin(), selectedClipIds_.end(), anchorId) !=
                                selectedClipIds_.end();
    std::vector<project::TimelineClip> clips;
    for (const auto& clip : project_.timelineClips) {
        if (clip.id == anchorId ||
            (anchorSelected && std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                         clip.id) != selectedClipIds_.end()))
            clips.push_back(clip);
    }
    return clips;
}

void MvmController::storeClipboard(std::vector<project::TimelineClip> clips) {
    // 別 Project へ貼ったときに Project パネルへ同じ素材を登録できるよう、bin の項目も持つ。
    clipboardMediaItems_.clear();
    for (const auto& clip : clips) {
        if (!project::clipUsesMediaItem(clip.kind))
            continue;
        const auto* item = project::findMediaItem(project_, clip.mediaItemId);
        if (item && !std::any_of(clipboardMediaItems_.begin(), clipboardMediaItems_.end(),
                                 [&](const project::MediaItem& stored) {
                                     return stored.mediaPath == item->mediaPath;
                                 }))
            clipboardMediaItems_.push_back(*item);
    }
    clipboardClips_ = std::move(clips);
    clipboardHoldsSubtitles_ = false;
    clipboardFpsNum_ = project_.timelineFpsNum;
    clipboardFpsDen_ = project_.timelineFpsDen;
}

bool MvmController::copySelectedClips() {
    if (!selectedSubtitleIds_.empty())
        return copySelectedSubtitles(false);
    if (busy_ || selectedClipIds_.empty()) {
        setStatus(QStringLiteral("コピーするclipがありません"));
        return false;
    }
    auto copied = selectedTimelineClipsInOrder({});
    if (copied.size() != selectedClipIds_.size()) {
        setStatus(QStringLiteral("選択clipがProjectにありません"));
        return false;
    }
    storeClipboard(std::move(copied));
    setStatus(QString::number(clipboardClips_.size()) + QStringLiteral("個のclipをコピーしました"));
    return true;
}

bool MvmController::cutSelectedClips() {
    if (!selectedSubtitleIds_.empty())
        return copySelectedSubtitles(true);
    if (busy_ || !pauseTimeline())
        return false;
    if (selectedClipIds_.empty()) {
        setStatus(QStringLiteral("カットするclipがありません"));
        return false;
    }
    auto copied = selectedTimelineClipsInOrder({});
    project::Project candidate = project_;
    if (copied.size() != selectedClipIds_.size()) {
        setStatus(QStringLiteral("カットするclipがProjectにありません"));
        return false;
    }
    candidate.timelineClips.erase(
        std::remove_if(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                       [&](const project::TimelineClip& clip) {
                           return std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                            clip.id) != selectedClipIds_.end();
                       }),
        candidate.timelineClips.end());
    // 削除と同じく、カットした clip にリンクした字幕も消す。
    project::eraseLinkedSubtitles(candidate, {selectedClipIds_.begin(), selectedClipIds_.end()});
    for (auto& clip : candidate.timelineClips) {
        if (clip.linkGroupId.empty())
            continue;
        const bool partnerRemains =
            std::any_of(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                        [&](const project::TimelineClip& other) {
                            return other.id != clip.id && other.linkGroupId == clip.linkGroupId;
                        });
        if (!partnerRemains)
            clip.linkGroupId.clear();
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("カットできません: ")))
        return false;
    // cut は bin を変えないので、commit 後の Project から素材を控えてよい。
    storeClipboard(std::move(copied));
    setTimelineSelection({});
    if (totalTimelineFrames_ == 0) {
        const bool reset = resetPreviewEngine();
        currentSource_.reset();
        currentClipIndex_ = -1;
        currentClipName_.clear();
        currentClipPath_.clear();
        Q_EMIT stateChanged();
        if (!reset) {
            setStatus(QStringLiteral("clipはカットしましたが、Previewを初期化できません"));
            return true;
        }
        setStatus(QStringLiteral("clipをカットしました"));
        return true;
    }
    return refreshPreviewAfterSavedEdit({}, QStringLiteral("clipをカットしました"));
}

bool MvmController::placeCopiedClips(const std::vector<project::TimelineClip>& clips,
                                     const std::vector<project::MediaItem>& mediaItems,
                                     std::int64_t sourceFpsNum, std::int64_t sourceFpsDen,
                                     std::int64_t destinationFrame, int videoTrackDelta,
                                     int audioTrackDelta, CopyPlacement placement) {
    if (busy_ || !pauseTimeline())
        return false;
    if (clips.empty()) {
        setStatus(QStringLiteral("配置するclipがありません"));
        return false;
    }
    if (destinationFrame < 0) {
        setStatus(QStringLiteral("配置先のframeが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto first =
        std::min_element(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            return a.timelineStartFrame < b.timelineStartFrame;
        });
    std::map<std::pair<project::TrackKind, int>, std::vector<project::TimelineClip>> lanes;
    std::map<std::string, int> linkCounts;
    for (const auto& clip : clips) {
        if (project::clipKindHasMediaPath(clip.kind) && !std::filesystem::exists(clip.mediaPath)) {
            setStatus(QStringLiteral("コピー元の素材が見つかりません: ") +
                      fromPath(clip.mediaPath));
            return false;
        }
        const auto offset = project::sourceBoundaryToTimelineBoundary(
            clip.timelineStartFrame - first->timelineStartFrame, sourceFpsNum, sourceFpsDen,
            candidate.timelineFpsNum, candidate.timelineFpsDen);
        if (!offset.success ||
            destinationFrame > std::numeric_limits<std::int64_t>::max() - offset.frame) {
            setStatus(QStringLiteral("貼り付け位置を変換できません"));
            return false;
        }
        auto placed = clip;
        placed.timelineStartFrame = destinationFrame + offset.frame;
        const int delta =
            clip.track.kind == project::TrackKind::Video ? videoTrackDelta : audioTrackDelta;
        // 個別に 0 へ丸めると clip 同士の上下関係が崩れる。範囲はドラッグ側で揃えておく。
        if (clip.track.index + delta < 0) {
            setStatus(QStringLiteral("配置先のtrackが範囲外です"));
            return false;
        }
        placed.track.index = clip.track.index + delta;
        if (sourceFpsNum != candidate.timelineFpsNum || sourceFpsDen != candidate.timelineFpsDen) {
            // key は clip 先頭からの timeline frame なので、fps が違えば同じ秒位置へ移す。
            const auto duration = project::timelineClipDuration(candidate, placed);
            bool retimed = duration.success;
            for (const auto& channel : project::effectChannels())
                retimed =
                    retimed && project::retimeClipKeys(placed.effects.*channel.keys, sourceFpsNum,
                                                       sourceFpsDen, candidate.timelineFpsNum,
                                                       candidate.timelineFpsDen, duration.frame);
            if (!retimed) {
                setStatus(QStringLiteral("キーフレームを貼り付け先のfpsへ変換できません"));
                return false;
            }
        }
        lanes[{clip.track.kind, clip.track.index}].push_back(std::move(placed));
        if (!clip.linkGroupId.empty())
            ++linkCounts[clip.linkGroupId];
    }
    // 素材の追加と同じく、timeline へ置いた素材は同じ編集で Project パネルにも載せる。
    // clip を置く前に登録する (登録の検証が、置いた clip の素材参照も見るため)。
    // コピー元の bin 項目があればそれを使い (folder は移し先に無いので root)、無ければ調べ直す。
    for (const auto& clip : clips) {
        if (!project::clipUsesMediaItem(clip.kind) ||
            project::findMediaItemByPath(candidate, clip.mediaPath))
            continue;
        const auto copied =
            std::find_if(mediaItems.begin(), mediaItems.end(), [&](const project::MediaItem& item) {
                return item.mediaPath == clip.mediaPath;
            });
        if (copied == mediaItems.end()) {
            QString registerError;
            if (!registerMediaItem(candidate, clip.mediaPath, registerError)) {
                setStatus(registerError);
                return false;
            }
            continue;
        }
        auto item = *copied;
        item.id = newClipId();
        item.folderId.clear();
        const auto added = project::addMediaItem(candidate, std::move(item));
        if (!added.success) {
            setStatus(QStringLiteral("素材をプロジェクトへ登録できません: ") +
                      QString::fromStdString(added.error));
            return false;
        }
    }
    std::map<std::string, std::string> newLinkIds;
    std::set<std::pair<project::TrackKind, int>> reservedTracks;
    std::vector<std::string> newIds;
    for (auto& [sourceLane, laneClips] : lanes) {
        const auto kind = sourceLane.first;
        auto& tracks = project::tracksOfKind(candidate, kind);
        int chosen = -1;
        std::vector<int> trackOrder;
        if (placement == CopyPlacement::ExactTrack) {
            // ドラッグで見せた track にそのまま置く。重なりは validateTimeline が拒否する。
            if (laneClips.front().track.index >= static_cast<int>(tracks.size())) {
                setStatus(QStringLiteral("配置先のtrackが範囲外です"));
                return false;
            }
        } else if (laneClips.front().track.index <= static_cast<int>(tracks.size()))
            trackOrder.push_back(laneClips.front().track.index);
        for (int index = 0;
             placement == CopyPlacement::FindFreeTrack && index <= static_cast<int>(tracks.size());
             ++index) {
            if (std::find(trackOrder.begin(), trackOrder.end(), index) == trackOrder.end())
                trackOrder.push_back(index);
        }
        if (placement == CopyPlacement::ExactTrack)
            chosen = laneClips.front().track.index;
        for (const int index : trackOrder) {
            if (index < static_cast<int>(tracks.size()) &&
                tracks[static_cast<std::size_t>(index)].muted)
                continue;
            if (reservedTracks.contains({kind, index}))
                continue;
            bool free = true;
            for (const auto& copy : laneClips) {
                const auto copyDuration = project::timelineClipDuration(candidate, copy);
                if (!copyDuration.success ||
                    copy.timelineStartFrame >
                        std::numeric_limits<std::int64_t>::max() - copyDuration.frame) {
                    setStatus(QStringLiteral("コピーしたclipの尺が不正です"));
                    return false;
                }
                const auto copyEnd = copy.timelineStartFrame + copyDuration.frame;
                for (const auto& existing : candidate.timelineClips) {
                    if (existing.track.kind != kind || existing.track.index != index)
                        continue;
                    const auto existingDuration =
                        project::timelineClipDuration(candidate, existing);
                    if (!existingDuration.success ||
                        (copy.timelineStartFrame <
                             existing.timelineStartFrame + existingDuration.frame &&
                         existing.timelineStartFrame < copyEnd)) {
                        free = false;
                        break;
                    }
                }
                if (!free)
                    break;
            }
            if (free) {
                chosen = index;
                break;
            }
        }
        if (chosen < 0) {
            setStatus(QStringLiteral("空きtrackを確保できません"));
            return false;
        }
        if (chosen == static_cast<int>(tracks.size()))
            tracks.push_back(project::Track{project::defaultTrackName(kind, chosen), false});
        reservedTracks.insert({kind, chosen});
        for (auto& copy : laneClips) {
            copy.track = {kind, chosen};
            copy.id = newClipId();
            if (copy.kind == project::TimelineClipKind::Graph) {
                std::string error;
                if (!project::remapGraphIds(copy.graph, [this] { return newClipId(); }, error)) {
                    setStatus(QString::fromStdString(error));
                    return false;
                }
            }
            if (copy.kind == project::TimelineClipKind::EquationSequence) {
                std::string error;
                if (!project::remapEquationSequenceIds(
                        copy.equationSequence, [this] { return newClipId(); }, error)) {
                    setStatus(QString::fromStdString(error));
                    return false;
                }
            }
            if (!copy.linkGroupId.empty()) {
                if (linkCounts[copy.linkGroupId] == 2) {
                    auto& newId = newLinkIds[copy.linkGroupId];
                    if (newId.empty())
                        newId = newClipId();
                    copy.linkGroupId = newId;
                } else {
                    copy.linkGroupId.clear();
                }
            }
            newIds.push_back(copy.id);
            candidate.timelineClips.push_back(std::move(copy));
        }
    }
    // 置いた clip の素材 id を貼り付け先の素材へ付け替える。別 Project からの貼り付けでは
    // コピー元の id は貼り付け先に無い (同じ Project なら元の素材のまま)。
    for (auto& clip : candidate.timelineClips) {
        if (!project::clipUsesMediaItem(clip.kind) ||
            std::find(newIds.begin(), newIds.end(), clip.id) == newIds.end())
            continue;
        const auto* item = project::findMediaItem(candidate, clip.mediaItemId);
        if (!item || item->mediaPath != clip.mediaPath)
            item = project::findMediaItemByPath(candidate, clip.mediaPath);
        if (!item) {
            setStatus(QStringLiteral("貼り付けた素材がプロジェクトパネルにありません: ") +
                      fromPath(clip.mediaPath));
            return false;
        }
        clip.mediaItemId = item->id;
        clip.mediaPath = item->mediaPath;
    }
    const auto valid = project::finalizeTimelineCandidate(candidate);
    if (!valid.success) {
        setStatus(QStringLiteral("clipを配置できません: ") + QString::fromStdString(valid.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("clipを配置できません: ")))
        return false;
    setTimelineSelection(newIds, false);
    return refreshPreviewAfterSavedEdit(
        newIds.front(), QString::number(newIds.size()) + QStringLiteral("個のclipを配置しました"));
}

bool MvmController::pasteClips() {
    if (clipboardHoldsSubtitles_)
        return pasteSubtitles(playheadFrame_);
    return placeCopiedClips(clipboardClips_, clipboardMediaItems_, clipboardFpsNum_,
                            clipboardFpsDen_, playheadFrame_, 0, 0, CopyPlacement::FindFreeTrack);
}

bool MvmController::duplicateSelectedClips() {
    if (!selectedSubtitleIds_.empty()) {
        // 複製はクリップボードを変えない (clip の複製と同じ)。
        const auto clipboard = subtitleClipboard_;
        const bool holdsSubtitles = clipboardHoldsSubtitles_;
        const bool duplicated = copySelectedSubtitles(false) && pasteSubtitles(playheadFrame_);
        subtitleClipboard_ = clipboard;
        clipboardHoldsSubtitles_ = holdsSubtitles;
        return duplicated;
    }
    return placeCopiedClips(selectedTimelineClipsInOrder({}), project_.mediaItems,
                            project_.timelineFpsNum, project_.timelineFpsDen, playheadFrame_, 0, 0,
                            CopyPlacement::FindFreeTrack);
}

QVariantMap MvmController::timelineDragBounds(const QString& clipId) const {
    // ドラッグで一緒に動く clip 群 (anchor が選択中なら選択全体) の端。QML はこれで
    // ドラッグ量を先に丸め、表示した位置のまま移動・複製を確定させる。
    const auto clips = selectedTimelineClipsInOrder(clipId.toStdString());
    QVariantMap bounds;
    if (clips.empty())
        return bounds;
    qint64 minStart = std::numeric_limits<qint64>::max();
    std::map<project::TrackKind, std::pair<int, int>> trackRange;
    QStringList clipIds;
    for (const auto& clip : clips) {
        clipIds.append(QString::fromStdString(clip.id));
        minStart = std::min<qint64>(minStart, clip.timelineStartFrame);
        const auto [range, inserted] =
            trackRange.try_emplace(clip.track.kind, clip.track.index, clip.track.index);
        range->second.first = std::min(range->second.first, clip.track.index);
        range->second.second = std::max(range->second.second, clip.track.index);
    }
    bounds.insert(QStringLiteral("minStartFrame"), minStart);
    // 一緒に動く clip の ID。QML は吸着の候補から外す (自分の端には吸着しない)。
    bounds.insert(QStringLiteral("clipIds"), clipIds);
    for (const auto& [kind, range] : trackRange) {
        const QString prefix =
            kind == project::TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio");
        bounds.insert(prefix + QStringLiteral("MinTrack"), range.first);
        bounds.insert(prefix + QStringLiteral("MaxTrack"), range.second);
    }
    return bounds;
}

bool MvmController::duplicateTimelineClipsAt(const QString& clipId, const QString& trackKind,
                                             int trackIndex, qint64 timelineStartFrame) {
    project::TrackRef destination;
    if (!resolveTrackRef(trackKind, trackIndex, destination))
        return false;
    const auto anchorId = clipId.toStdString();
    const int anchorIndex = indexOfClipId(project_.timelineClips, anchorId);
    if (anchorIndex < 0)
        return false;
    const auto& anchor = project_.timelineClips[static_cast<std::size_t>(anchorIndex)];
    if (destination.kind != anchor.track.kind) {
        setStatus(QStringLiteral("異なる種別のtrackへ複製できません"));
        return false;
    }
    const auto clips = selectedTimelineClipsInOrder(anchorId);
    const auto first =
        std::min_element(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            return a.timelineStartFrame < b.timelineStartFrame;
        });
    // 位置や track を後から寄せると、ドラッグ中に見せた位置と確定位置がずれる。QML が
    // timelineDragBounds で丸めた値を渡すので、範囲外や既存 clip との重なりは拒否する。
    const qint64 destinationFirst =
        timelineStartFrame - (anchor.timelineStartFrame - first->timelineStartFrame);
    return placeCopiedClips(
        clips, project_.mediaItems, project_.timelineFpsNum, project_.timelineFpsDen,
        destinationFirst,
        destination.kind == project::TrackKind::Video ? destination.index - anchor.track.index : 0,
        destination.kind == project::TrackKind::Audio ? destination.index - anchor.track.index : 0,
        CopyPlacement::ExactTrack);
}

bool MvmController::addTimelineMarker() {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto at = std::lower_bound(candidate.timelineMarkers.begin(),
                                     candidate.timelineMarkers.end(), playheadFrame_);
    if (at != candidate.timelineMarkers.end() && *at == playheadFrame_)
        return true;
    candidate.timelineMarkers.insert(at, playheadFrame_);
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("マーカーを追加できません: ")))
        return false;
    setStatus(QStringLiteral("マーカーを追加しました"));
    return true;
}

bool MvmController::deleteTimelineMarker(qint64 frame) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto at =
        std::lower_bound(candidate.timelineMarkers.begin(), candidate.timelineMarkers.end(), frame);
    if (at == candidate.timelineMarkers.end() || *at != frame)
        return false;
    candidate.timelineMarkers.erase(at);
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("マーカーを削除できません: ")))
        return false;
    setStatus(QStringLiteral("マーカーを削除しました"));
    return true;
}

bool MvmController::jumpToMarker(int direction) {
    if (busy_ || direction == 0 || project_.timelineMarkers.empty())
        return false;
    const auto& markers = project_.timelineMarkers;
    if (direction > 0) {
        const auto next = std::upper_bound(markers.begin(), markers.end(), playheadFrame_);
        return next != markers.end() && seekTimelineFrame(*next);
    }
    const auto previous = std::lower_bound(markers.begin(), markers.end(), playheadFrame_);
    return previous != markers.begin() && seekTimelineFrame(*std::prev(previous));
}

bool MvmController::markIn() {
    if (busy_ || !pauseTimeline())
        return false;
    if (project_.outFrame && playheadFrame_ >= *project_.outFrame) {
        setStatus(QStringLiteral("インはアウトより前に設定してください"));
        return false;
    }
    if (project_.inFrame == playheadFrame_)
        return true;
    project::Project candidate = project_;
    candidate.inFrame = playheadFrame_;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("インを設定できません: ")))
        return false;
    setStatus(QStringLiteral("インを設定しました"));
    return true;
}

bool MvmController::markOut() {
    if (busy_ || !pauseTimeline())
        return false;
    if (project_.inFrame && playheadFrame_ <= *project_.inFrame) {
        setStatus(QStringLiteral("アウトはインより後に設定してください"));
        return false;
    }
    if (project_.outFrame == playheadFrame_)
        return true;
    project::Project candidate = project_;
    candidate.outFrame = playheadFrame_;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("アウトを設定できません: ")))
        return false;
    setStatus(QStringLiteral("アウトを設定しました"));
    return true;
}

bool MvmController::jumpToIn() {
    return project_.inFrame && seekTimelineFrame(*project_.inFrame);
}

bool MvmController::jumpToOut() {
    return project_.outFrame && seekTimelineFrame(*project_.outFrame);
}

bool MvmController::clearInOut() {
    if (busy_ || !pauseTimeline())
        return false;
    if (!project_.inFrame && !project_.outFrame)
        return true;
    project::Project candidate = project_;
    candidate.inFrame.reset();
    candidate.outFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("イン・アウトを消去できません: ")))
        return false;
    setStatus(QStringLiteral("イン・アウトを消去しました"));
    return true;
}

bool MvmController::clearIn() {
    if (busy_ || !pauseTimeline() || !project_.inFrame)
        return false;
    project::Project candidate = project_;
    candidate.inFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("インを消去できません: ")))
        return false;
    setStatus(QStringLiteral("インを消去しました"));
    return true;
}

bool MvmController::clearOut() {
    if (busy_ || !pauseTimeline() || !project_.outFrame)
        return false;
    project::Project candidate = project_;
    candidate.outFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("アウトを消去できません: ")))
        return false;
    setStatus(QStringLiteral("アウトを消去しました"));
    return true;
}

bool MvmController::moveTimelineClip(const QString& clipId, const QString& trackKind,
                                     int trackIndex, qint64 timelineStartFrame, bool) {
    if (busy_)
        return false;
    // 候補の作成と検証は再生を止める前に行う (applyTimelineEdit と同じ)。受理されない操作・
    // 変更の無い操作だけで再生を止めない。
    project::TrackRef destination;
    if (!resolveTrackRef(trackKind, trackIndex, destination)) {
        setStatus(QStringLiteral("移動先trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const std::string anchorId = clipId.toStdString();
    std::vector<std::string> movedIds = selectedClipIds_;
    if (std::find(movedIds.begin(), movedIds.end(), anchorId) == movedIds.end()) {
        movedIds = {anchorId};
        setTimelineSelection(movedIds);
    }
    const auto moved = project::moveClips(candidate, movedIds, anchorId, destination,
                                          std::max<qint64>(0, timelineStartFrame),
                                          project::LinkMode::Single, newClipId);
    if (!moved.success) {
        setStatus(QString::fromStdString(moved.error));
        return false;
    }
    if (!pauseTimeline())
        return false;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    const QString status = movedIds.size() > 1 ? QString::number(movedIds.size()) +
                                                     QStringLiteral("個のclipを移動しました")
                                               : QStringLiteral("clipを移動しました");
    return refreshPreviewAfterSavedEdit(anchorId, status);
}

bool MvmController::resolveTrimEdge(const QString& edge, project::TrimEdge& trimEdge) {
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else {
        setStatus(QStringLiteral("trim edgeが不正です"));
        return false;
    }
    return true;
}

bool MvmController::applyTimelineEdit(
    const std::function<project::TimelineEditResult(project::Project&)>& edit,
    const std::string& selectedClipId, const QString& successStatus) {
    if (busy_)
        return false;
    // 候補の作成と検証は再生を止める前に行う。存在しない clip・限界を超えた trim・変更なし
    // など、受理されない操作だけで再生を止めない。止めるのは commit が確定してから。
    project::Project candidate = project_;
    const auto edited = edit(candidate);
    if (!edited.success) {
        setStatus(QString::fromStdString(edited.error));
        return false;
    }
    if (!pauseTimeline())
        return false;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    return refreshPreviewAfterSavedEdit(selectedClipId, successStatus);
}

bool MvmController::trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta,
                             bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::trimTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                             linkModeFor(linked));
        },
        id, QStringLiteral("clipをtrimしました"));
}

bool MvmController::rateStretchClip(const QString& clipId, const QString& edge,
                                    qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rateStretchTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                                    linkModeFor(linked));
        },
        id, QStringLiteral("clipの速度を変えました"));
}

QVariantMap MvmController::clipSpeedDurationState(const QString& clipId) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const int index = indexOfClipId(project_.timelineClips, id);
    if (index < 0)
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success)
        return {};
    return {{QStringLiteral("clipId"), QString::fromStdString(id)},
            {QStringLiteral("speedPercent"),
             100.0 * static_cast<double>(clip.speedNum) / static_cast<double>(clip.speedDen)},
            {QStringLiteral("durationText"),
             QString::fromStdString(core::formatTimecode(duration.frame, project_.timelineFpsNum,
                                                         project_.timelineFpsDen))},
            {QStringLiteral("preservePitch"), clip.preservePitch},
            {QStringLiteral("still"), project::hasSyntheticSourceDomain(clip)}};
}

QVariantMap MvmController::previewClipSpeedDuration(const QString& clipId, const QString& input,
                                                    double speedPercent,
                                                    const QString& durationText, bool preservePitch,
                                                    bool ripple) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const auto edit = speedDurationEdit(input, speedPercent, durationText, preservePitch, ripple,
                                        project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit)
        return {{QStringLiteral("error"), QStringLiteral("速度または尺が不正です")}};
    auto previewEdit = *edit;
    previewEdit.ripple = true;
    const auto preview =
        project::previewClipSpeedDuration(project_, id, previewEdit, project::LinkMode::Linked);
    if (!preview.success)
        return {{QStringLiteral("error"), QString::fromStdString(preview.error)}};
    return {{QStringLiteral("durationText"),
             QString::fromStdString(core::formatTimecode(
                 preview.durationFrames, project_.timelineFpsNum, project_.timelineFpsDen))},
            {QStringLiteral("speedPercent"), 100.0 * static_cast<double>(preview.speedNum) /
                                                 static_cast<double>(preview.speedDen)}};
}

bool MvmController::clipSpeedDurationNeedsOverwrite(const QString& clipId, const QString& input,
                                                    double speedPercent,
                                                    const QString& durationText) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const auto edit = speedDurationEdit(input, speedPercent, durationText, false, false,
                                        project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit)
        return false;
    return project::previewClipSpeedDuration(project_, id, *edit, project::LinkMode::Linked)
        .overlapsFollowing;
}

bool MvmController::applyClipSpeedDuration(const QString& clipId, const QString& input,
                                           double speedPercent, const QString& durationText,
                                           bool preservePitch, bool ripple, bool overwrite) {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    auto edit = speedDurationEdit(input, speedPercent, durationText, preservePitch, ripple,
                                  project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit) {
        setStatus(QStringLiteral("速度または尺が不正です"));
        return false;
    }
    edit->overwrite = overwrite;
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::setClipSpeedDuration(candidate, id, *edit, project::LinkMode::Linked);
        },
        id, QStringLiteral("clip の速度と尺を変更しました"));
}

bool MvmController::canInsertFrameHold(const QString& clipId) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    if (busy_ || indexOfClipId(project_.timelineClips, id) < 0)
        return false;
    // 判定を別に書かず、複製した Project で実際の挿入を試す。「押せるのに実行すると失敗する」
    // メニューにしない (保持できない frame 等も同じ規則で弾く)。ID も本番と同じ生成器にする。
    // 固定の ID だと、同じ ID の clip を持つ Project でだけ試行が重複で失敗していた。
    project::Project candidate = project_;
    return project::insertFrameHold(
               candidate, id, playheadFrame_,
               project::defaultFrameHoldFrames(candidate.timelineFpsNum, candidate.timelineFpsDen),
               newClipId)
        .success;
}

bool MvmController::insertFrameHoldAtPlayhead(const QString& clipId) {
    if (!canInsertFrameHold(clipId))
        return false;
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::insertFrameHold(
                candidate, id, playheadFrame_,
                project::defaultFrameHoldFrames(candidate.timelineFpsNum, candidate.timelineFpsDen),
                newClipId);
        },
        id, QStringLiteral("フレーム保持を挿入しました"));
}

QVariantMap MvmController::previewRateStretch(const QString& clipId, const QString& edge,
                                              qint64 projectFrameDelta, bool linked) const {
    QVariantMap result{{QStringLiteral("delta"), qint64{0}},
                       {QStringLiteral("clips"), QVariantMap{}}};
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else
        return result;
    const auto preview = project::previewRateStretch(project_, clipId.toStdString(), trimEdge,
                                                     projectFrameDelta, linkModeFor(linked));
    if (!preview.success)
        return result;
    QVariantMap clips;
    for (const auto& shown : preview.clips) {
        clips.insert(
            QString::fromStdString(shown.clipId),
            QVariantMap{{QStringLiteral("startDelta"), qint64{shown.startDelta}},
                        {QStringLiteral("endDelta"), qint64{shown.endDelta}},
                        {QStringLiteral("speed"), static_cast<double>(shown.speedNum) /
                                                      static_cast<double>(shown.speedDen)}});
    }
    result.insert(QStringLiteral("delta"), qint64{preview.appliedDelta});
    result.insert(QStringLiteral("clips"), clips);
    return result;
}

qint64 MvmController::clampEdgeDrag(const QString& clipId, const QString& edge, const QString& tool,
                                    qint64 projectFrameDelta, bool linked) const {
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else
        return 0;
    const project::EdgeEditKind kind =
        tool == QStringLiteral("ripple")    ? project::EdgeEditKind::Ripple
        : tool == QStringLiteral("rolling") ? project::EdgeEditKind::Roll
                                            : project::EdgeEditKind::Trim;
    const auto clamped = project::clampEdgeEdit(project_, clipId.toStdString(), trimEdge, kind,
                                                projectFrameDelta, linkModeFor(linked));
    return clamped.success ? clamped.frame : 0;
}

bool MvmController::rippleTrimClip(const QString& clipId, const QString& edge,
                                   qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rippleTrimTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                                   linkModeFor(linked));
        },
        id, QStringLiteral("clipをリップルトリムしました"));
}

bool MvmController::rollClipEdge(const QString& clipId, const QString& edge,
                                 qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rollTimelineEdit(candidate, id, trimEdge, projectFrameDelta,
                                             linkModeFor(linked));
        },
        id, QStringLiteral("編集点をローリングしました"));
}

bool MvmController::slipClip(const QString& clipId, qint64 projectFrameDelta, bool linked) {
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::slipTimelineClip(candidate, id, projectFrameDelta, linkModeFor(linked));
        },
        id, QStringLiteral("clipをスリップしました"));
}

bool MvmController::beginSlipPreview(const QString& clipId, bool linked) {
    if (busy_ || !pauseTimeline())
        return false;
    if (indexOfClipId(project_.timelineClips, clipId.toStdString()) < 0) {
        setStatus(QStringLiteral("スリップするclipがありません"));
        return false;
    }
    endSlipPreview();
    slipPreview_.emplace();
    slipPreview_->clipId = clipId.toStdString();
    slipPreview_->linkMode = linkModeFor(linked);
    return true;
}

qint64 MvmController::previewSlip(qint64 projectFrameDelta) {
    if (!slipPreview_)
        return 0;
    const int index = indexOfClipId(project_.timelineClips, slipPreview_->clipId);
    if (index < 0)
        return 0;
    const auto& original = project_.timelineClips[static_cast<std::size_t>(index)];
    // 素材の端での止め方は確定時と同じ slipTimelineClip に任せる。1 frame も
    // ずらせない (素材の端) 場合は失敗するので、元の in のまま表示する。
    project::Project candidate = project_;
    const bool slipped = project::slipTimelineClip(candidate, slipPreview_->clipId,
                                                   projectFrameDelta, slipPreview_->linkMode)
                             .success;
    const auto& clip =
        slipped ? candidate.timelineClips[static_cast<std::size_t>(index)] : original;
    const std::int64_t sourceDelta = clip.sourceInFrame - original.sourceInFrame;

    // audio clip は timeline の波形だけで示す。preview は video の frame を出す。
    if (clip.kind == project::TimelineClipKind::Audio)
        return sourceDelta;
    if (!project::sourceRateMatchesTimelineRate(project_, clip)) {
        setStatus(timelineFpsText() +
                  QStringLiteral(" ではない素材のclipはスリップ中のPreviewに未対応です"));
        return sourceDelta;
    }
    if (slipPreview_->sourceFrame != clip.sourceInFrame || !slipPreview_->source) {
        slipPreview_->mediaPath = clip.mediaPath;
        slipPreview_->sourceFpsNum = clip.sourceFpsNum;
        slipPreview_->sourceFpsDen = clip.sourceFpsDen;
        slipPreview_->sourceFrameCount = clip.sourceFrameCount;
        slipPreview_->sourceFrame = clip.sourceInFrame;
        slipPreview_->pending = true;
        if (!slipPreviewTimer_.isActive()) {
            applySlipPreview();
            if (slipPreview_ && slipPreview_->pending)
                slipPreviewTimer_.start();
        }
    }
    return sourceDelta;
}

void MvmController::applySlipPreview() {
    if (!slipPreview_ || !slipPreview_->pending) {
        slipPreviewTimer_.stop();
        return;
    }
    const auto state = previewEngine_->status().state;
    if (state == preview::PreviewEngineState::Error) {
        slipPreview_->pending = false;
        slipPreviewTimer_.stop();
        return;
    }
    // addSource / seek は ReadyPaused でしか受理されない。Seeking 中は次の tick へ回す。
    if (state != preview::PreviewEngineState::ReadyPaused)
        return;
    if (!slipPreview_->source) {
        // timeline frame N = 素材 frame N と写す source を 1 つだけ作る。drag 中は
        // seek だけで新しい in の frame を出せ、位置が変わるたびに decoder を開き直さない。
        preview::PreviewSourceDescriptor descriptor;
        descriptor.mediaPath = slipPreview_->mediaPath;
        descriptor.videoEnabled = true;
        descriptor.videoTimelineMappingEnabled = true;
        descriptor.videoSourceInFrame = 0;
        descriptor.videoSourceFrameCount = slipPreview_->sourceFrameCount;
        descriptor.videoTimelineStartFrame = 0;
        descriptor.expectedVideoSourceFrameRate = {
            static_cast<std::uint32_t>(slipPreview_->sourceFpsNum),
            static_cast<std::uint32_t>(slipPreview_->sourceFpsDen)};
        const auto added = previewEngine_->addSource(descriptor);
        if (!added) {
            setStatus(QStringLiteral("スリップのPreviewを準備できません: ") +
                      previewErrorText(added.error()));
            slipPreview_->pending = false;
            slipPreviewTimer_.stop();
            return;
        }
        slipPreview_->source = added.value();
    }
    // スリップ中は clip の素材だけを全面に出す (Premiere のスリップ中のモニタと同じ)。
    auto composition = std::make_shared<preview::CompositionSnapshot>();
    preview::PreviewCompositionLayer layer;
    layer.source = *slipPreview_->source;
    composition->layers.push_back(layer);
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted)
        return;
    submittedComposition_ = composition;
    preview::PreviewFrameRequest request;
    request.outputFrameNumber = slipPreview_->sourceFrame;
    request.sources.push_back({*slipPreview_->source, slipPreview_->sourceFrame});
    if (!previewEngine_->seekFrameRequest(request))
        return;
    slipPreview_->pending = false;
    statusText_ = QStringLiteral("スリップ: 新しいイン点は素材の ") +
                  QString::number(slipPreview_->sourceFrame) + QStringLiteral(" frame目です");
    Q_EMIT stateChanged();
}

void MvmController::endSlipPreview() {
    if (!slipPreview_)
        return;
    slipPreviewTimer_.stop();
    const std::optional<preview::PreviewSourceId> source = slipPreview_->source;
    slipPreview_.reset();
    if (!source)
        return;
    // 先に通常の composition へ戻し、slip 用 source を参照から外してから削除する。
    // まだ参照中で拒否されたら retirement queue に回す。
    seekTimelineFrame(playheadFrame_);
    if (!previewEngine_->removeSource(*source))
        retiredSources_.push_back(*source);
}

qint64 MvmController::clampSlideDrag(const QString& clipId, qint64 projectFrameDelta,
                                     bool linked) const {
    const auto clamped = project::clampSlideEdit(project_, clipId.toStdString(), projectFrameDelta,
                                                 linkModeFor(linked));
    return clamped.success ? clamped.frame : 0;
}

bool MvmController::slideClip(const QString& clipId, qint64 projectFrameDelta, bool linked) {
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::slideTimelineClip(candidate, id, projectFrameDelta,
                                              linkModeFor(linked));
        },
        id, QStringLiteral("clipをスライドしました"));
}

bool MvmController::splitClipAt(const QString& clipId, qint64 frame, bool allTracks, bool linked) {
    const std::string id = clipId.toStdString();
    const std::vector<std::string> clipIds =
        allTracks ? project::clipIdsSpanningFrame(project_, frame) : std::vector<std::string>{id};
    if (clipIds.empty()) {
        setStatus(QStringLiteral("分割位置にclipがありません"));
        return false;
    }
    const QString status =
        allTracks ? QString::number(clipIds.size()) + QStringLiteral("個のclipを分割しました")
                  : QStringLiteral("clipを分割しました");
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::splitTimelineClips(candidate, clipIds, frame, newClipId,
                                               linkModeFor(linked));
        },
        allTracks ? currentClipId() : id, status);
}

bool MvmController::splitSelectionAtPlayhead() {
    if (!selectedSubtitleIds_.empty())
        return splitSelectedSubtitlesAtPlayhead();
    const qint64 frame = playheadFrame_;
    std::vector<std::string> clipIds =
        project::clipIdsSpanningFrame(project_, frame, selectedClipIds_);
    if (clipIds.empty()) {
        const std::string current = currentClipId();
        if (!current.empty())
            clipIds = project::clipIdsSpanningFrame(project_, frame, {current});
    }
    if (clipIds.empty()) {
        setStatus(QStringLiteral("再生ヘッド位置に分割できる選択clipがありません"));
        return false;
    }
    const std::string selectedId = clipIds.front();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::splitTimelineClips(candidate, clipIds, frame, newClipId,
                                               project::LinkMode::Linked);
        },
        selectedId, QStringLiteral("再生ヘッド位置でclipを分割しました"));
}

bool MvmController::toggleSelectedClipsEnabled() {
    std::vector<std::string> clipIds = selectedClipIds_;
    if (clipIds.empty() && !currentClipId().empty())
        clipIds.push_back(currentClipId());
    return toggleClipsEnabled(clipIds);
}

bool MvmController::toggleTimelineClipEnabled(const QString& clipId) {
    return toggleClipsEnabled({clipId.toStdString()});
}

bool MvmController::toggleClipsEnabled(const std::vector<std::string>& clipIds) {
    if (clipIds.empty()) {
        setStatus(QStringLiteral("有効/無効を切り換えるclipが選択されていません"));
        return false;
    }
    const int first = indexOfClipId(project_.timelineClips, clipIds.front());
    std::string selectedId = currentClipId();
    if (selectedId.empty())
        selectedId = clipIds.front();
    // 結果の向きは project 側が決める。status は確定後の状態から作る。
    const bool succeeded = applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::toggleClipsEnabled(candidate, clipIds);
        },
        selectedId, QString());
    if (succeeded && first >= 0) {
        const bool enabled = project_.timelineClips[static_cast<std::size_t>(first)].enabled;
        setStatus(enabled ? QStringLiteral("clipを有効にしました")
                          : QStringLiteral("clipを無効にしました"));
    }
    return succeeded;
}

bool MvmController::applyDefaultTransition() {
    const std::int64_t defaultFrames =
        project::defaultTransitionFrames(project_.timelineFpsNum, project_.timelineFpsDen);
    // 選んだトランジションは、その編集点へ既定の長さで置き直す。
    std::string outgoing = selectedEditOutgoing_;
    std::string incoming = selectedEditIncoming_;
    for (const auto& transition : project_.timelineTransitions) {
        if (!selectedTransitionId_.empty() && transition.id == selectedTransitionId_) {
            outgoing = transition.outgoingClipId;
            incoming = transition.incomingClipId;
        }
    }
    if (!outgoing.empty()) {
        project::TransitionEditResult placed;
        const bool applied = applyTimelineEdit(
            [&](project::Project& candidate) {
                placed = project::applyDefaultEditTransition(candidate, outgoing, incoming,
                                                             defaultFrames,
                                                             project::LinkMode::Linked, newClipId);
                project::TimelineEditResult result;
                result.success = placed.success;
                result.error = placed.error;
                return result;
            },
            std::string{}, QString());
        if (!applied)
            return false;
        // 置いたトランジションを選ぶ (続けて Delete で消せる)。
        selectedEditOutgoing_.clear();
        selectedEditIncoming_.clear();
        selectedTransitionId_ = placed.transitionId;
        QString status = QString::number(placed.frames) +
                         QStringLiteral("フレームのトランジションを作成しました");
        if (placed.transitionCount > 1)
            status += QStringLiteral(" (リンク相手を含む") +
                      QString::number(placed.transitionCount) + QStringLiteral("個)");
        if (placed.frames < defaultFrames)
            status += QStringLiteral("。素材の余白が足りないため短くしました");
        setStatus(status);
        notifyTimelineTransitions();
        Q_EMIT stateChanged();
        return true;
    }
    std::vector<std::string> clipIds = selectedClipIds_;
    if (clipIds.empty() && !currentClipId().empty())
        clipIds.push_back(currentClipId());
    if (clipIds.empty()) {
        setStatus(QStringLiteral("トランジションを適用するclipが選択されていません"));
        return false;
    }
    std::string selectedId = currentClipId();
    if (selectedId.empty())
        selectedId = clipIds.front();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::applyDefaultClipFades(candidate, clipIds, defaultFrames);
        },
        selectedId, QStringLiteral("clipの先頭と末尾にフェードを付けました"));
}

bool MvmController::stepSelectedClipVolume(double stepDb) {
    std::vector<std::string> clipIds = selectedClipIds_;
    if (clipIds.empty() && !currentClipId().empty())
        clipIds.push_back(currentClipId());
    if (clipIds.empty()) {
        setStatus(QStringLiteral("音量を変えるclipが選択されていません"));
        return false;
    }
    std::string selectedId = currentClipId();
    if (selectedId.empty())
        selectedId = clipIds.front();
    const QString sign = stepDb > 0.0 ? QStringLiteral("+") : QString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::stepClipVolume(candidate, clipIds, stepDb);
        },
        selectedId,
        QStringLiteral("clipの音量を") + sign + QString::number(stepDb) +
            QStringLiteral("dB変えました"));
}

bool MvmController::selectClipsFromFrame(qint64 frame, const QString& direction,
                                         const QString& trackKind, int trackIndex) {
    project::SelectDirection selectDirection;
    if (direction == QStringLiteral("forward"))
        selectDirection = project::SelectDirection::Forward;
    else if (direction == QStringLiteral("backward"))
        selectDirection = project::SelectDirection::Backward;
    else {
        setStatus(QStringLiteral("選択方向が不正です"));
        return false;
    }
    // trackKind が空なら全 track を対象にする。
    std::optional<project::TrackRef> track;
    if (!trackKind.isEmpty()) {
        project::TrackRef resolved;
        if (!resolveTrackRef(trackKind, trackIndex, resolved)) {
            setStatus(QStringLiteral("trackが不正です"));
            return false;
        }
        track = resolved;
    }
    const auto ids =
        project::clipIdsFromFrame(project_, std::max<qint64>(0, frame), selectDirection, track);
    setTimelineSelection(ids);
    if (ids.empty()) {
        setCurrentClipSelection(-1);
        setStatus(QStringLiteral("選択できるclipがありません"));
        return true;
    }
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, ids.front()));
    setStatus(QString::number(selectedClipIds_.size()) + QStringLiteral("個のclipを選択しました"));
    return true;
}

void MvmController::notifyTimelineTransitions() {
    auto selected = computeSelectedTransition();
    if (selected != shownSelectedTransition_) {
        shownSelectedTransition_ = std::move(selected);
        Q_EMIT selectedTransitionChanged();
    }
    auto shown = timelineTransitions();
    if (shown == shownTransitions_)
        return;
    shownTransitions_ = std::move(shown);
    Q_EMIT timelineTransitionsChanged();
}

QVariantList MvmController::timelineTransitions() const {
    QVariantList list;
    for (const auto& transition : project_.timelineTransitions) {
        const int outgoing = indexOfClipId(project_.timelineClips, transition.outgoingClipId);
        if (outgoing < 0)
            continue;
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(outgoing)];
        const auto duration = project::timelineClipDuration(project_, clip);
        if (!duration.success)
            continue;
        const qint64 cut = clip.timelineStartFrame + duration.frame;
        list.append(
            QVariantMap{{QStringLiteral("transitionId"), QString::fromStdString(transition.id)},
                        {QStringLiteral("trackKind"),
                         QString::fromLatin1(project::trackKindName(clip.track.kind))},
                        {QStringLiteral("trackIndex"), clip.track.index},
                        {QStringLiteral("start"), cut - transition.framesBeforeCut},
                        {QStringLiteral("cut"), cut},
                        {QStringLiteral("end"), cut + transition.framesAfterCut},
                        {QStringLiteral("kind"),
                         QString::fromLatin1(project::transitionKindName(transition.kind))}});
    }
    return list;
}

QVariantMap MvmController::selectedEditPoint() const {
    const int outgoing = indexOfClipId(project_.timelineClips, selectedEditOutgoing_);
    if (outgoing < 0)
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(outgoing)];
    const int incoming = indexOfClipId(project_.timelineClips, selectedEditIncoming_);
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success || incoming < 0)
        return {};
    const auto& incomingClip = project_.timelineClips[static_cast<std::size_t>(incoming)];
    // 試せるかどうかだけを示す (どちらかが数式 clip)。数式 clip と他の clip の編集点も試せるように
    // し、置けない理由は model が返すものをそのまま出す。
    const bool mathCandidate = clip.kind == project::TimelineClipKind::Math ||
                               incomingClip.kind == project::TimelineClipKind::Math;
    const bool rejectedHere = mathTransformRejection_.revision == currentRevision_ &&
                              mathTransformRejection_.transitionId.empty() &&
                              mathTransformRejection_.outgoingId == selectedEditOutgoing_ &&
                              mathTransformRejection_.incomingId == selectedEditIncoming_;
    return {
        {QStringLiteral("trackKind"), QString::fromLatin1(project::trackKindName(clip.track.kind))},
        {QStringLiteral("trackIndex"), clip.track.index},
        {QStringLiteral("frame"), clip.timelineStartFrame + duration.frame},
        {QStringLiteral("outgoingName"), QString::fromStdString(clip.name)},
        {QStringLiteral("incomingName"), QString::fromStdString(incomingClip.name)},
        {QStringLiteral("mathTransformCandidate"), mathCandidate},
        {QStringLiteral("mathTransformRejection"),
         rejectedHere ? mathTransformRejection_.message : QString()}};
}

bool MvmController::canDeleteSelection() const {
    return !busy_ && (!selectedTransitionId_.empty() || !selectedSubtitleIds_.empty() ||
                      (selectedEditOutgoing_.empty() && currentClipIndex_ >= 0));
}

bool MvmController::selectEditPoint(const QString& clipId, const QString& edge) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    if (indexOfClipId(project_.timelineClips, id) < 0) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    const std::string neighbor = project::touchingClipId(project_, id, trimEdge);
    // 接している clip が無い端は編集点ではない。clip の選択として扱う。
    if (neighbor.empty())
        return selectTimelineClip(clipId, true);
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    selectedEditOutgoing_ = trimEdge == project::TrimEdge::Right ? id : neighbor;
    selectedEditIncoming_ = trimEdge == project::TrimEdge::Right ? neighbor : id;
    setStatus(QStringLiteral("編集点を選択しました"));
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::selectTransition(const QString& transitionId) {
    const std::string id = transitionId.toStdString();
    if (std::none_of(project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
                     [&](const auto& transition) { return transition.id == id; })) {
        setStatus(QStringLiteral("選択したトランジションがありません"));
        return false;
    }
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    selectedTransitionId_ = id;
    setStatus(QStringLiteral("トランジションを選択しました"));
    notifyTimelineTransitions();
    Q_EMIT stateChanged();
    return true;
}

QVariantMap MvmController::computeSelectedTransition() const {
    const auto found = std::find_if(
        project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
        [&](const auto& transition) { return transition.id == selectedTransitionId_; });
    if (selectedTransitionId_.empty() || found == project_.timelineTransitions.end())
        return {};
    const int outgoing = indexOfClipId(project_.timelineClips, found->outgoingClipId);
    const int incoming = indexOfClipId(project_.timelineClips, found->incomingClipId);
    if (outgoing < 0 || incoming < 0)
        return {};
    const auto& outgoingClip = project_.timelineClips[static_cast<std::size_t>(outgoing)];
    const auto& incomingClip = project_.timelineClips[static_cast<std::size_t>(incoming)];
    const auto outgoingDuration = project::timelineClipDuration(project_, outgoingClip);
    const auto incomingDuration = project::timelineClipDuration(project_, incomingClip);
    const auto limits =
        project::transitionSpanLimits(project_, found->id, project::LinkMode::Linked);
    if (!outgoingDuration.success || !incomingDuration.success || !limits.success)
        return {};
    const qint64 cut = outgoingClip.timelineStartFrame + outgoingDuration.frame;
    QVariantMap selected{
        {QStringLiteral("transitionId"), QString::fromStdString(found->id)},
        {QStringLiteral("trackKind"),
         QString::fromLatin1(project::trackKindName(outgoingClip.track.kind))},
        {QStringLiteral("cut"), cut},
        {QStringLiteral("framesBeforeCut"), static_cast<qint64>(found->framesBeforeCut)},
        {QStringLiteral("framesAfterCut"), static_cast<qint64>(found->framesAfterCut)},
        {QStringLiteral("durationText"), QString::fromStdString(core::formatTimecode(
                                             found->framesBeforeCut + found->framesAfterCut,
                                             project_.timelineFpsNum, project_.timelineFpsDen))},
        {QStringLiteral("maxBefore"), static_cast<qint64>(limits.maxBefore)},
        {QStringLiteral("maxAfter"), static_cast<qint64>(limits.maxAfter)},
        {QStringLiteral("outgoingClipId"), QString::fromStdString(outgoingClip.id)},
        {QStringLiteral("outgoingName"), QString::fromStdString(outgoingClip.name)},
        {QStringLiteral("outgoingStart"), static_cast<qint64>(outgoingClip.timelineStartFrame)},
        {QStringLiteral("outgoingEnd"), cut},
        {QStringLiteral("incomingClipId"), QString::fromStdString(incomingClip.id)},
        {QStringLiteral("incomingName"), QString::fromStdString(incomingClip.name)},
        {QStringLiteral("incomingStart"), static_cast<qint64>(incomingClip.timelineStartFrame)},
        {QStringLiteral("incomingEnd"),
         static_cast<qint64>(incomingClip.timelineStartFrame + incomingDuration.frame)},
        {QStringLiteral("kind"), QString::fromLatin1(project::transitionKindName(found->kind))}};
    // 数式の変形は描画と preview の状態を足す (Blend には無い)。
    const auto status = mathTransformStatus(*found);
    for (auto it = status.cbegin(); it != status.cend(); ++it)
        selected.insert(it.key(), it.value());
    if (found->kind == project::TransitionKind::MathTransform)
        selected.insert(QStringLiteral("spanRejection"),
                        mathTransformRejection_.revision == currentRevision_ &&
                                mathTransformRejection_.transitionId == found->id
                            ? mathTransformRejection_.message
                            : QString());
    return selected;
}

bool MvmController::setTransitionSpan(qint64 framesBeforeCut, qint64 framesAfterCut,
                                      bool keepTotal) {
    if (selectedTransitionId_.empty()) {
        setStatus(QStringLiteral("長さを変えるトランジションが選択されていません"));
        return false;
    }
    const std::string id = selectedTransitionId_;
    const auto current =
        std::find_if(project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
                     [&](const auto& transition) { return transition.id == id; });
    const bool mathTransform = current != project_.timelineTransitions.end() &&
                               current->kind == project::TransitionKind::MathTransform;
    // 数式の変形は、断った理由を inspector にも残す (status は次の操作で消える)。
    const auto reject = [&](const QString& message) {
        setStatus(message);
        if (mathTransform) {
            mathTransformRejection_ = {{}, {}, id, currentRevision_, message};
            notifyTimelineTransitions();
        }
        return false;
    };
    // 数値欄・ドラッグの値は素材 frame に乗るとは限らない (30fps 素材を 60fps timeline に置くと
    // 2 frame 単位)。最も近い置ける長さへ吸着させ、吸着したことは status に出す。
    const auto fitted = project::nearestTransitionSpan(
        project_, id, framesBeforeCut, framesAfterCut,
        keepTotal ? project::SpanFitMode::KeepTotal : project::SpanFitMode::EachSide,
        project::LinkMode::Linked);
    if (!fitted.success)
        return reject(QString::fromStdString(fitted.error));
    // 吸着した結果が今の値と同じなら編集ではない (上限で止まっただけ)。applyTimelineEdit は
    // 再生を止めるので、何も変わらない操作では入らない。
    if (current != project_.timelineTransitions.end() &&
        current->framesBeforeCut == fitted.framesBeforeCut &&
        current->framesAfterCut == fitted.framesAfterCut) {
        const bool requestedSame =
            framesBeforeCut == fitted.framesBeforeCut && framesAfterCut == fitted.framesAfterCut;
        if (requestedSame)
            return reject(QStringLiteral("トランジションの長さは変わっていません"));
        return reject(
            mathTransform
                ? QStringLiteral("数式の変形はこれ以上変えられません (後ろの数式 clip の"
                                 "尺・前の数式 clip の Write・区間の見た目の範囲の端です)")
                : QStringLiteral("トランジションはこれ以上変えられません "
                                 "(素材の余白・フレーム・不透明度の範囲の端です)"));
    }
    QString status = QStringLiteral("トランジションを") +
                     QString::number(fitted.framesBeforeCut + fitted.framesAfterCut) +
                     QStringLiteral("フレームにしました");
    if (fitted.framesBeforeCut != framesBeforeCut || fitted.framesAfterCut != framesAfterCut)
        status += (mathTransform ? QStringLiteral(" (変形を置ける範囲に合わせて cut の前 ")
                                 : QStringLiteral(" (素材のフレームに合わせて cut の前 ")) +
                  QString::number(fitted.framesBeforeCut) + QStringLiteral(" / 後 ") +
                  QString::number(fitted.framesAfterCut) + QStringLiteral(")");
    const bool applied = applyTimelineEdit(
        [&](project::Project& candidate) {
            const auto changed = project::setTimelineTransitionSpan(
                candidate, id, fitted.framesBeforeCut, fitted.framesAfterCut,
                project::LinkMode::Linked);
            project::TimelineEditResult result;
            result.success = changed.success;
            result.error = changed.error;
            return result;
        },
        std::string{}, status);
    if (!applied)
        return mathTransform && !busy_ ? reject(statusText_) : false;
    notifyTimelineTransitions();
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::deleteSelection() {
    if (!selectedSubtitleIds_.empty())
        return deleteSelectedSubtitles();
    if (!selectedTransitionId_.empty()) {
        const std::string id = selectedTransitionId_;
        const bool deleted = applyTimelineEdit(
            [&](project::Project& candidate) {
                return project::deleteTimelineTransition(candidate, id);
            },
            std::string{}, QStringLiteral("トランジションを削除しました"));
        if (deleted) {
            selectedTransitionId_.clear();
            notifyTimelineTransitions();
            Q_EMIT stateChanged();
        }
        return deleted;
    }
    if (!selectedEditOutgoing_.empty()) {
        setStatus(QStringLiteral("編集点は削除できません"));
        return false;
    }
    return deleteCurrentClip();
}

bool MvmController::deleteCurrentClip() {
    if (busy_)
        return false;
    // 候補の作成と検証は再生を止める前に行う (applyTimelineEdit と同じ)。受理されない操作・
    // 変更の無い操作だけで再生を止めない。
    const std::string currentId = currentClipId();
    if (currentId.empty()) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }
    std::vector<std::string> deletedIds = selectedClipIds_;
    if (std::find(deletedIds.begin(), deletedIds.end(), currentId) == deletedIds.end())
        deletedIds = {currentId};

    project::Project candidate = project_;
    const int firstDeletedIndex = currentClipIndex_;
    int deletedCount = 0;
    for (const auto& id : deletedIds) {
        const int index = indexOfClipId(candidate.timelineClips, id);
        if (index < 0)
            continue; // link相手の削除で同時に消えたclip。
        const std::size_t before = candidate.timelineClips.size();
        const project::TimelineEditResult deleted = project::deleteTimelineClip(candidate, index);
        if (!deleted.success) {
            setStatus(QString::fromStdString(deleted.error));
            return false;
        }
        deletedCount += static_cast<int>(before - candidate.timelineClips.size());
    }
    if (deletedCount == 0) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }
    if (!pauseTimeline())
        return false;

    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    const QString resetFailure = resetAfterClipRemoval();
    if (!resetFailure.isEmpty()) {
        setStatus(QStringLiteral("clipは削除しましたが、") + resetFailure);
        return true;
    }

    if (!project_.timelineClips.empty()) {
        const int nextIndex =
            std::min(firstDeletedIndex, static_cast<int>(project_.timelineClips.size()) - 1);
        if (!selectClip(nextIndex)) {
            const QString previewFailure = statusText_;
            setStatus(QString::number(deletedCount) +
                      QStringLiteral("個のclipは削除しましたが、次のclipをPreviewできません: ") +
                      previewFailure);
        } else {
            setStatus(QString::number(deletedCount) + QStringLiteral("個のclipを削除しました"));
        }
        return true;
    }

    setStatus(QString::number(deletedCount) +
              QStringLiteral("個のclipを削除し、timelineが空になりました"));
    return true;
}

QString MvmController::resetAfterClipRemoval() {
    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    const bool previewReset = resetPreviewEngine();
    const QString resetFailure = previewReset ? QString() : statusText_;
    currentSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = -1;
    std::erase_if(selectedClipIds_, [&](const std::string& id) {
        return indexOfClipId(project_.timelineClips, id) < 0;
    });
    Q_EMIT stateChanged();
    return resetFailure;
}

bool MvmController::deleteTimelineClip(const QString& clipId) {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }
    setCurrentClipSelection(index);
    return deleteCurrentClip();
}

bool MvmController::unlinkTimelineClip(const QString& clipId) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto unlinked = project::unlinkTimelineClip(candidate, clipId.toStdString());
    if (!unlinked.success) {
        setStatus(QString::fromStdString(unlinked.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    setTimelineSelection({clipId.toStdString()});
    return refreshPreviewAfterSavedEdit(clipId.toStdString(),
                                        QStringLiteral("clipのリンクを解除しました"));
}

bool MvmController::addTrack(const QString& trackKind) {
    if (busy_)
        return false;
    project::TrackKind kind;
    if (trackKind == QStringLiteral("video"))
        kind = project::TrackKind::Video;
    else if (trackKind == QStringLiteral("audio"))
        kind = project::TrackKind::Audio;
    else {
        setStatus(QStringLiteral("追加するtrack種別が不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto added = project::addTrack(candidate, kind);
    if (!added.success) {
        setStatus(QString::fromStdString(added.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    setStatus(QStringLiteral("trackを追加しました"));
    return true;
}

bool MvmController::removeTrack(const QString& trackKind, int trackIndex) {
    if (busy_)
        return false;
    // 候補の作成と検証は再生を止める前に行う (applyTimelineEdit と同じ)。受理されない操作・
    // 変更の無い操作だけで再生を止めない。
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("削除するtrackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto removed = project::removeTrack(candidate, track);
    if (!removed.success) {
        setStatus(QString::fromStdString(removed.error));
        return false;
    }
    // track を消すと後続 track の index が繰り上がる。preview cache は track index を
    // key にしているので、止めてから組み直さないと stale な対応が残る。
    if (!pauseTimeline())
        return false;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    // index の対応が変わったので cache を捨ててから現在位置で組み直す。
    trackSources_.clear();
    audioSources_.clear();
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("trackは削除しましたが、Previewを初期化できません: ") + failure);
        return true;
    }
    const std::string selectedId =
        (currentClipIndex_ >= 0 &&
         currentClipIndex_ < static_cast<int>(project_.timelineClips.size()))
            ? project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id
            : std::string{};
    currentClipIndex_ = -1;
    return refreshPreviewAfterSavedEdit(selectedId, QStringLiteral("trackを削除しました"));
}

bool MvmController::setAudioTrackMix(int index, double gainDb, double pan, bool commit) {
    if (busy_ || !projectLockHeld_ || index < 0 || index >= audioTrackCount() ||
        !project::isValidAudioMix(gainDb, pan)) {
        setStatus(QStringLiteral("トラック音量またはパンが不正です"));
        return false;
    }
    const auto& current = project_.audioTracks[static_cast<std::size_t>(index)];
    if (current.mixerGainDb == gainDb && current.mixerPan == pan) {
        cancelAudioTrackMix(index);
        return true;
    }
    if (!pauseForTrackOutputEdit())
        return false;
    if (!commit) {
        const auto gains = project::audioMixGains(gainDb, pan);
        audioMixerBuses_[static_cast<std::size_t>(index)]->leftGain.store(
            static_cast<float>(gains.first));
        audioMixerBuses_[static_cast<std::size_t>(index)]->rightGain.store(
            static_cast<float>(gains.second));
        audioTrackModel_->setMixerValues(index, gainDb, pan);
        return refreshScrubAudioMix(index, gainDb, pan);
    }
    auto candidate = project_;
    auto& track = candidate.audioTracks[static_cast<std::size_t>(index)];
    track.mixerGainDb = gainDb;
    track.mixerPan = pan;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("ミキサー設定を保存できません: "),
                           PlaybackInvalidation::Mixer)) {
        cancelAudioTrackMix(index);
        return false;
    }
    if (!refreshScrubAudioMix(index, gainDb, pan))
        return true;
    setStatus(QStringLiteral("ミキサー設定を保存しました"));
    return true;
}

void MvmController::cancelAudioTrackMix(int index) {
    if (index < 0 || index >= audioTrackCount())
        return;
    const auto& track = project_.audioTracks[static_cast<std::size_t>(index)];
    const auto gains = project::audioMixGains(track.mixerGainDb, track.mixerPan);
    const auto& bus = audioMixerBuses_[static_cast<std::size_t>(index)];
    const bool changed = bus->leftGain.load() != static_cast<float>(gains.first) ||
                         bus->rightGain.load() != static_cast<float>(gains.second);
    audioMixerBuses_[static_cast<std::size_t>(index)]->leftGain.store(
        static_cast<float>(gains.first));
    audioMixerBuses_[static_cast<std::size_t>(index)]->rightGain.store(
        static_cast<float>(gains.second));
    audioTrackModel_->setMixerValues(index, track.mixerGainDb, track.mixerPan);
    if (changed)
        refreshScrubAudioMix(index, track.mixerGainDb, track.mixerPan);
}

bool MvmController::setAudioMixerName(int index, const QString& name) {
    if (busy_ || index < 0 || index >= audioTrackCount() || name.trimmed().isEmpty())
        return false;
    if (QString::fromStdString(project_.audioTracks[static_cast<std::size_t>(index)].mixerName) ==
        name.trimmed())
        return true;
    auto candidate = project_;
    candidate.audioTracks[static_cast<std::size_t>(index)].mixerName = name.trimmed().toStdString();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("ミキサー名を保存できません: "),
                           PlaybackInvalidation::Mixer))
        return false;
    setStatus(QStringLiteral("ミキサー名を保存しました"));
    return true;
}

QVariantMap MvmController::audioTrackMeter(int index) {
    if (index < 0 || index >= static_cast<int>(audioMixerBuses_.size()))
        return {};
    const auto& bus = audioMixerBuses_[static_cast<std::size_t>(index)];
    const auto [left, right] = audioMixerPeaks_[static_cast<std::size_t>(index)];
    return {{QStringLiteral("left"), playing_ ? linearToDb(left, -96.0) : -96.0},
            {QStringLiteral("right"), playing_ ? linearToDb(right, -96.0) : -96.0},
            {QStringLiteral("clipped"), bus->clipped.load()}};
}

void MvmController::clearAudioTrackClip(int index) {
    if (index >= 0 && index < static_cast<int>(audioMixerBuses_.size()))
        audioMixerBuses_[static_cast<std::size_t>(index)]->clipped.store(false);
}

bool MvmController::setTrackMuted(const QString& trackKind, int trackIndex, bool muted) {
    return setTracksMuted(trackKind, {trackIndex}, muted);
}

bool MvmController::setTracksMuted(const QString& trackKind, const QVariantList& trackIndices,
                                   bool muted) {
    if (busy_)
        return false;
    project::TrackRef first;
    std::vector<int> indices;
    for (const auto& value : trackIndices) {
        bool ok = false;
        const int index = value.toInt(&ok);
        project::TrackRef track;
        if (!ok || !resolveTrackRef(trackKind, index, track)) {
            setStatus(QStringLiteral("trackが不正です"));
            return false;
        }
        first = track;
        indices.push_back(index);
    }
    project::Project candidate = project_;
    const auto changed = project::setTracksMuted(candidate, first.kind, indices, muted);
    if (!changed.success) {
        setStatus(QString::fromStdString(changed.error));
        return false;
    }
    const bool video = first.kind == project::TrackKind::Video;
    if (!pauseForTrackOutputEdit())
        return false;
    return commitTrackOutputEdit(std::move(candidate),
                                 video ? (muted ? QStringLiteral("trackを非表示にしました")
                                                : QStringLiteral("trackを表示しました"))
                                       : (muted ? QStringLiteral("trackをミュートしました")
                                                : QStringLiteral("trackのミュートを解除しました")));
}

bool MvmController::setTrackSolo(const QString& trackKind, int trackIndex, bool solo) {
    if (busy_)
        return false;
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto changed = project::setTrackSolo(candidate, track, solo);
    if (!changed.success) {
        setStatus(QString::fromStdString(changed.error));
        return false;
    }
    if (!pauseForTrackOutputEdit())
        return false;
    return commitTrackOutputEdit(std::move(candidate),
                                 solo ? QStringLiteral("trackをソロにしました")
                                      : QStringLiteral("trackのソロを解除しました"));
}

bool MvmController::pauseForTrackOutputEdit() {
    // 通常再生は止めない (Premiere と同じく再生しながら切り替える)。通常再生は毎 tick
    // handOffPlaybackSources が Project から layer / audio を引き直すので、隠す・消音は次の
    // tick で外れ、表示・解除で足りない source は既存の組み直し (その frame から再生を続ける)
    // で用意される。シャトルは開始時に鳴らす clip を決めて持つので、従来どおり止める。
    if (shuttleRate_ != 0)
        return pauseTimeline();
    return true;
}

bool MvmController::commitTrackOutputEdit(project::Project candidate, const QString& doneStatus) {
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    if (playing_) {
        setStatus(doneStatus);
        return true;
    }
    QString error;
    if (!syncPreviewSourcesAt(playheadFrame_, error)) {
        setStatus(QStringLiteral(
                      "trackの表示・音声の設定は保存されましたが、Previewの更新に失敗しました: ") +
                  error);
        return true;
    }
    setStatus(doneStatus);
    return true;
}

bool MvmController::hasGapAt(const QString& trackKind, int trackIndex, qint64 frame) const {
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track))
        return false;
    return project::gapAt(project_, track, frame).found;
}

bool MvmController::hasClipAt(const QString& trackKind, int trackIndex, qint64 frame) const {
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track))
        return false;
    return project::timelineClipIndexAt(project_, track, frame) >= 0;
}

bool MvmController::rippleDeleteGap(const QString& trackKind, int trackIndex, qint64 frame) {
    if (busy_)
        return false;
    // 候補の作成と検証は再生を止める前に行う (applyTimelineEdit と同じ)。受理されない操作・
    // 変更の無い操作だけで再生を止めない。
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto rippled = project::rippleDeleteGap(candidate, track, frame);
    if (!rippled.success) {
        setStatus(QString::fromStdString(rippled.error));
        return false;
    }
    if (!pauseTimeline())
        return false;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    const std::string selectedId =
        (currentClipIndex_ >= 0 &&
         currentClipIndex_ < static_cast<int>(project_.timelineClips.size()))
            ? project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id
            : std::string{};
    return refreshPreviewAfterSavedEdit(selectedId, QStringLiteral("空白をリップル削除しました"));
}

} // namespace mvm::app
