#include "timeline_clip_model.h"

#include "clip_keyframe_values.h"
#include "project/timeline_edit.h"

#include <algorithm>

#include <QHash>
#include <QVariantMap>

namespace mvm::app {

TimelineClipModel::TimelineClipModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineClipModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QString TimelineClipModel::clipIdAt(int row) const {
    return row >= 0 && row < items_.size() ? items_[row].id : QString();
}

QVariant TimelineClipModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};
    const auto& item = items_[index.row()];
    switch (role) {
    case ClipIdRole:
        return item.id;
    case DisplayNameRole:
        return item.name;
    case KindRole:
        return item.kind;
    case TimelineStartFrameRole:
        return item.timelineStartFrame;
    case TimelineDurationFramesRole:
        return item.timelineDurationFrames;
    case SourceInFrameRole:
        return item.sourceInFrame;
    case SourceOutFrameRole:
        return item.sourceOutFrame;
    case SourceFrameCountRole:
        return item.sourceFrameCount;
    case SourceFpsNumRole:
        return item.sourceFpsNum;
    case SourceFpsDenRole:
        return item.sourceFpsDen;
    case PreviewSupportedRole:
        return item.previewSupported;
    case TrackKindRole:
        return item.trackKind;
    case TrackIndexRole:
        return item.trackIndex;
    case LinkedRole:
        return item.linked;
    case LinkGroupIdRole:
        return item.linkGroupId;
    case SelectedRole:
        return item.selected;
    case MediaPathRole:
        return item.mediaPath;
    case AutomationKeysRole: {
        const auto keys = clipKeyframeValues(item.automationKeys);
        return keys;
    }
    case AutomationBaseRole:
        return item.automationBase;
    case SpeedRole:
        return item.speed;
    case FrameHoldRole:
        return item.frameHold;
    case PreservePitchRole:
        return item.preservePitch;
    case ClipEnabledRole:
        return item.enabled;
    case ClipRowRole:
        return index.row();
    case SubtitleLinkedRole:
        return item.subtitleLinked;
    default:
        return {};
    }
}

QHash<int, QByteArray> TimelineClipModel::roleNames() const {
    return {{ClipIdRole, "clipId"},
            {DisplayNameRole, "displayName"},
            {KindRole, "clipKind"},
            {TimelineStartFrameRole, "timelineStartFrame"},
            {TimelineDurationFramesRole, "timelineDurationFrames"},
            {SourceInFrameRole, "sourceInFrame"},
            {SourceOutFrameRole, "sourceOutFrame"},
            {SourceFrameCountRole, "sourceFrameCount"},
            {SourceFpsNumRole, "sourceFpsNum"},
            {SourceFpsDenRole, "sourceFpsDen"},
            {PreviewSupportedRole, "previewSupported"},
            {TrackKindRole, "trackKind"},
            {TrackIndexRole, "trackIndex"},
            {LinkedRole, "linked"},
            {LinkGroupIdRole, "linkGroupId"},
            {SelectedRole, "selected"},
            {MediaPathRole, "mediaPath"},
            {AutomationKeysRole, "automationKeys"},
            {AutomationBaseRole, "automationBase"},
            {SpeedRole, "speed"},
            {FrameHoldRole, "frameHold"},
            {PreservePitchRole, "preservePitch"},
            {ClipEnabledRole, "clipEnabled"},
            {ClipRowRole, "clipRow"},
            {SubtitleLinkedRole, "subtitleLinked"}};
}

QList<int> TimelineClipModel::changedRoles(const Item& before, const Item& after) {
    QList<int> roles;
    const auto add = [&roles](bool changed, int role) {
        if (changed)
            roles.append(role);
    };
    add(before.id != after.id, ClipIdRole);
    add(before.name != after.name, DisplayNameRole);
    add(before.kind != after.kind, KindRole);
    add(before.timelineStartFrame != after.timelineStartFrame, TimelineStartFrameRole);
    add(before.timelineDurationFrames != after.timelineDurationFrames, TimelineDurationFramesRole);
    add(before.sourceInFrame != after.sourceInFrame, SourceInFrameRole);
    add(before.sourceOutFrame != after.sourceOutFrame, SourceOutFrameRole);
    add(before.sourceFrameCount != after.sourceFrameCount, SourceFrameCountRole);
    add(before.sourceFpsNum != after.sourceFpsNum, SourceFpsNumRole);
    add(before.sourceFpsDen != after.sourceFpsDen, SourceFpsDenRole);
    add(before.previewSupported != after.previewSupported, PreviewSupportedRole);
    add(before.trackKind != after.trackKind, TrackKindRole);
    add(before.trackIndex != after.trackIndex, TrackIndexRole);
    add(before.linked != after.linked, LinkedRole);
    add(before.linkGroupId != after.linkGroupId, LinkGroupIdRole);
    add(before.selected != after.selected, SelectedRole);
    add(before.mediaPath != after.mediaPath, MediaPathRole);
    add(before.automationKeys != after.automationKeys, AutomationKeysRole);
    add(before.automationBase != after.automationBase, AutomationBaseRole);
    add(before.speed != after.speed, SpeedRole);
    add(before.frameHold != after.frameHold, FrameHoldRole);
    add(before.preservePitch != after.preservePitch, PreservePitchRole);
    add(before.enabled != after.enabled, ClipEnabledRole);
    add(before.subtitleLinked != after.subtitleLinked, SubtitleLinkedRole);
    return roles;
}

void TimelineClipModel::setProject(const project::Project& project) {
    QList<Item> next;
    next.reserve(static_cast<qsizetype>(project.timelineClips.size()));
    QSet<QString> subtitleLinkedClips;
    if (project.subtitles)
        for (const auto& cue : project.subtitles->cues)
            if (!cue.linkClipId.empty())
                subtitleLinkedClips.insert(QString::fromStdString(cue.linkClipId));
    for (const auto& clip : project.timelineClips) {
        const auto duration = project::timelineClipDuration(project, clip);
        const bool audio = clip.kind == project::TimelineClipKind::Audio;
        const auto& automationKeys = audio ? clip.effects.volumeKeys : clip.effects.opacityKeys;
        next.append({QString::fromStdString(clip.id),
                     QString::fromStdString(clip.name),
                     QString::fromLatin1(project::timelineClipKindName(clip.kind)),
                     clip.timelineStartFrame,
                     duration.success ? duration.frame : 0,
                     clip.sourceInFrame,
                     clip.sourceOutFrame,
                     clip.sourceFrameCount,
                     clip.sourceFpsNum,
                     clip.sourceFpsDen,
                     duration.success,
                     QString::fromLatin1(project::trackKindName(clip.track.kind)),
                     clip.track.index,
                     !clip.linkGroupId.empty(),
                     QString::fromStdString(clip.linkGroupId),
                     false,
                     QString::fromStdWString(clip.mediaPath.wstring()),
                     automationKeys,
                     audio ? clip.effects.volumePercent : clip.effects.opacityPercent,
                     static_cast<double>(clip.speedNum) / static_cast<double>(clip.speedDen),
                     clip.frameHold.has_value(),
                     clip.preservePitch,
                     clip.enabled,
                     subtitleLinkedClips.contains(QString::fromStdString(clip.id))});
    }
    // 選択は setSelectedClipIds が持つ。同じ clip の選択は引き継ぎ、作り直した後に選択が
    // 一瞬外れて見えないようにする。
    {
        QHash<QString, bool> selected;
        selected.reserve(items_.size());
        for (const auto& item : items_)
            selected.insert(item.id, item.selected);
        for (auto& item : next)
            item.selected = selected.value(item.id, false);
    }

    // clip ID の並びが一致する先頭と末尾は行を保つ。間 (追加・削除・並べ替えのあった範囲) は
    // 古い行を消して新しい行を入れる。
    const qsizetype oldSize = items_.size();
    const qsizetype newSize = next.size();
    qsizetype prefix = 0;
    while (prefix < oldSize && prefix < newSize && items_[prefix].id == next[prefix].id)
        ++prefix;
    qsizetype suffix = 0;
    while (suffix < oldSize - prefix && suffix < newSize - prefix &&
           items_[oldSize - 1 - suffix].id == next[newSize - 1 - suffix].id)
        ++suffix;
    const qsizetype oldMiddle = oldSize - prefix - suffix;
    const qsizetype newMiddle = newSize - prefix - suffix;
    if (oldMiddle > 0) {
        beginRemoveRows({}, static_cast<int>(prefix), static_cast<int>(prefix + oldMiddle - 1));
        items_.remove(prefix, oldMiddle);
        endRemoveRows();
    }
    if (newMiddle > 0) {
        beginInsertRows({}, static_cast<int>(prefix), static_cast<int>(prefix + newMiddle - 1));
        for (qsizetype offset = 0; offset < newMiddle; ++offset)
            items_.insert(prefix + offset, next[prefix + offset]);
        endInsertRows();
    }
    // 残した行は値の変わった role だけを知らせる (delegate を作り直さない)。
    for (qsizetype row = 0; row < newSize; ++row) {
        if (row >= prefix && row < prefix + newMiddle)
            continue;
        auto roles = changedRoles(items_[row], next[row]);
        // 間の行数が変わると、後ろの行の番号 (clipRow) が変わる。
        if (oldMiddle != newMiddle && row >= prefix + newMiddle)
            roles.append(ClipRowRole);
        if (roles.isEmpty())
            continue;
        items_[row] = std::move(next[row]);
        const auto changed = index(static_cast<int>(row), 0);
        Q_EMIT dataChanged(changed, changed, roles);
    }
}

QVariantList TimelineClipModel::clipSpans() const {
    QVariantList spans;
    spans.reserve(items_.size());
    for (const auto& item : items_) {
        spans.append(QVariantMap{
            {QStringLiteral("clipId"), item.id},
            {QStringLiteral("trackKind"), item.trackKind},
            {QStringLiteral("trackIndex"), item.trackIndex},
            {QStringLiteral("start"), item.timelineStartFrame},
            {QStringLiteral("end"), item.timelineStartFrame + item.timelineDurationFrames},
            {QStringLiteral("linkGroupId"), item.linkGroupId}});
    }
    return spans;
}

void TimelineClipModel::setSelectedClipIds(const QSet<QString>& clipIds) {
    if (items_.isEmpty())
        return;
    bool changed = false;
    for (auto& item : items_) {
        const bool selected = clipIds.contains(item.id);
        changed = changed || item.selected != selected;
        item.selected = selected;
    }
    if (changed)
        Q_EMIT dataChanged(index(0, 0), index(static_cast<int>(items_.size()) - 1, 0),
                           {SelectedRole});
}

// 位置・尺が変わった行は、QSortFilterProxyModel が dataChanged で絞り直す
// (dynamicSortFilter。test_timeline_clip_model が編集で出入りする clip で確かめている)。
TimelineClipWindowModel::TimelineClipWindowModel(QObject* parent) : QSortFilterProxyModel(parent) {}

void TimelineClipWindowModel::setRoles(int idRole, int startRole, int lengthRole,
                                       bool lengthIsEnd) {
    beginFilterChange();
    idRole_ = idRole;
    startRole_ = startRole;
    lengthRole_ = lengthRole;
    lengthIsEnd_ = lengthIsEnd;
    endFilterChange(Direction::Rows);
}

void TimelineClipWindowModel::setVisibleRange(double startFrame, double endFrame) {
    if (!(endFrame >= startFrame))
        return;
    const double span = std::max(1.0, endFrame - startFrame);
    const bool covered = windowValid_ && startFrame >= windowStart_ && endFrame <= windowEnd_;
    // 拡大した後に広すぎる範囲を残すと、表示範囲に比例しない数の delegate を持ち続ける。
    const bool tooWide = windowValid_ && windowEnd_ - windowStart_ > 5.0 * span;
    if (covered && !tooWide)
        return;
    // 左右に表示幅 1 つ分の余白を持つ。スクロールでその内側を見ている間は絞り直さない。
    beginFilterChange();
    windowValid_ = true;
    windowStart_ = startFrame - span;
    windowEnd_ = endFrame + span;
    endFilterChange(Direction::Rows);
}

void TimelineClipWindowModel::setPinnedClipIds(const QStringList& clipIds) {
    if (clipIds == pinnedClipIds_)
        return;
    beginFilterChange();
    pinnedClipIds_ = clipIds;
    pinned_ = QSet<QString>(clipIds.begin(), clipIds.end());
    pinned_.remove(QString());
    endFilterChange(Direction::Rows);
    Q_EMIT pinnedClipIdsChanged();
}

bool TimelineClipWindowModel::filterAcceptsRow(int sourceRow,
                                               const QModelIndex& sourceParent) const {
    const auto row = sourceModel()->index(sourceRow, 0, sourceParent);
    if (pinned_.contains(row.data(idRole_).toString()))
        return true;
    if (!windowValid_)
        return false;
    const auto start = static_cast<double>(row.data(startRole_).toLongLong());
    const auto length = row.data(lengthRole_).toLongLong();
    const auto duration = static_cast<double>(
        std::max<qint64>(1, lengthIsEnd_ ? length - row.data(startRole_).toLongLong() : length));
    return start < windowEnd_ && start + duration > windowStart_;
}

TextClipFilterModel::TextClipFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {}

void TextClipFilterModel::setPlayheadFrame(qint64 frame) {
    if (frame == playhead_)
        return;
    if (frame >= stableFrom_ && frame < stableUntil_) {
        playhead_ = frame;
        return;
    }
    beginFilterChange();
    playhead_ = frame;
    stableFrom_ = std::numeric_limits<qint64>::min();
    stableUntil_ = std::numeric_limits<qint64>::max();
    ++playheadRefilterCount_;
    endFilterChange(Direction::Rows);
}

void TextClipFilterModel::setPinnedClipIds(const QStringList& clipIds) {
    if (clipIds == pinnedClipIds_)
        return;
    beginFilterChange();
    pinnedClipIds_ = clipIds;
    pinned_ = QSet<QString>(clipIds.begin(), clipIds.end());
    pinned_.remove(QString());
    endFilterChange(Direction::Rows);
    Q_EMIT pinnedClipIdsChanged();
}

bool TextClipFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const auto row = sourceModel()->index(sourceRow, 0, sourceParent);
    if (row.data(TimelineClipModel::KindRole) != QStringLiteral("text"))
        return false;
    const qint64 start = row.data(TimelineClipModel::TimelineStartFrameRole).toLongLong();
    const qint64 end =
        start +
        std::max<qint64>(1, row.data(TimelineClipModel::TimelineDurationFramesRole).toLongLong());
    // 再生位置の前にある端は範囲の下限を、後ろにある端は上限を狭める。
    for (const qint64 edge : {start, end}) {
        if (edge <= playhead_)
            stableFrom_ = std::max(stableFrom_, edge);
        else
            stableUntil_ = std::min(stableUntil_, edge);
    }
    if (pinned_.contains(row.data(TimelineClipModel::ClipIdRole).toString()))
        return true;
    return start <= playhead_ && playhead_ < end;
}

} // namespace mvm::app
