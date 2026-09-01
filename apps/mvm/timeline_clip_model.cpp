#include "timeline_clip_model.h"

#include "project/timeline_edit.h"

namespace mvm::app {

TimelineClipModel::TimelineClipModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineClipModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
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
    case VideoTrackRole:
        return item.videoTrack;
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
            {VideoTrackRole, "videoTrack"}};
}

void TimelineClipModel::setProject(const project::Project& project) {
    beginResetModel();
    items_.clear();
    items_.reserve(static_cast<qsizetype>(project.timelineClips.size()));
    for (const auto& clip : project.timelineClips) {
        const auto duration = project::timelineClipDuration(project, clip);
        items_.append({QString::fromStdString(clip.id), QString::fromStdString(clip.name),
                       QString::fromLatin1(project::timelineClipKindName(clip.kind)),
                       clip.timelineStartFrame, duration.success ? duration.frame : 0,
                       clip.sourceInFrame, clip.sourceOutFrame, clip.sourceFrameCount,
                       clip.sourceFpsNum, clip.sourceFpsDen,
                       project::sourceRateMatchesTimelineRate(project, clip),
                       static_cast<int>(clip.videoTrack)});
    }
    endResetModel();
}

} // namespace mvm::app
