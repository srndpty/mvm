#include "timeline_clip_model.h"

#include <QVariantMap>

#include "project/timeline_edit.h"

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
    case AutomationKeysRole:
        return item.automationKeys;
    case AutomationBaseRole:
        return item.automationBase;
    case SpeedRole:
        return item.speed;
    case FrameHoldRole:
        return item.frameHold;
    case PreservePitchRole:
        return item.preservePitch;
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
            {PreservePitchRole, "preservePitch"}};
}

void TimelineClipModel::setProject(const project::Project& project) {
    beginResetModel();
    items_.clear();
    items_.reserve(static_cast<qsizetype>(project.timelineClips.size()));
    for (const auto& clip : project.timelineClips) {
        const auto duration = project::timelineClipDuration(project, clip);
        const bool audio = clip.kind == project::TimelineClipKind::Audio;
        const auto& keys = audio ? clip.effects.volumeKeys : clip.effects.opacityKeys;
        QVariantList automationKeys;
        for (const auto& key : keys)
            automationKeys.append(QVariantMap{{QStringLiteral("frame"), key.frame},
                                              {QStringLiteral("value"), key.valuePercent}});
        items_.append({QString::fromStdString(clip.id), QString::fromStdString(clip.name),
                       QString::fromLatin1(project::timelineClipKindName(clip.kind)),
                       clip.timelineStartFrame, duration.success ? duration.frame : 0,
                       clip.sourceInFrame, clip.sourceOutFrame, clip.sourceFrameCount,
                       clip.sourceFpsNum, clip.sourceFpsDen,
                       duration.success,
                       QString::fromLatin1(project::trackKindName(clip.track.kind)),
                       clip.track.index, !clip.linkGroupId.empty(),
                       QString::fromStdString(clip.linkGroupId), false,
                       QString::fromStdWString(clip.mediaPath.wstring()), automationKeys,
                       audio ? clip.effects.volumePercent : clip.effects.opacityPercent,
                       static_cast<double>(clip.speedNum) / static_cast<double>(clip.speedDen),
                       clip.frameHold.has_value(), clip.preservePitch});
    }
    endResetModel();
}

QVariantList TimelineClipModel::clipSpans() const {
    QVariantList spans;
    spans.reserve(items_.size());
    for (const auto& item : items_) {
        spans.append(QVariantMap{{QStringLiteral("clipId"), item.id},
                                 {QStringLiteral("trackKind"), item.trackKind},
                                 {QStringLiteral("trackIndex"), item.trackIndex},
                                 {QStringLiteral("start"), item.timelineStartFrame},
                                 {QStringLiteral("end"),
                                  item.timelineStartFrame + item.timelineDurationFrames},
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

} // namespace mvm::app
