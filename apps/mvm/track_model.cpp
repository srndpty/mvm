#include "track_model.h"

namespace mvm::app {

TrackModel::TrackModel(project::TrackKind kind, QObject* parent)
    : QAbstractListModel(parent), kind_(kind) {}

int TrackModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant TrackModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};
    const auto& item = items_[index.row()];
    switch (role) {
    case TrackNameRole:
        return item.name;
    case MutedRole:
        return item.muted;
    case SoloRole:
        return item.solo;
    case OutputEnabledRole:
        return item.outputEnabled;
    case TrackKindRole:
        return QString::fromLatin1(project::trackKindName(kind_));
    case TrackIndexRole:
        return index.row();
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackModel::roleNames() const {
    return {{TrackNameRole, "trackName"},
            {MutedRole, "trackMuted"},
            {SoloRole, "trackSolo"},
            {OutputEnabledRole, "trackOutputEnabled"},
            {TrackKindRole, "trackKind"},
            {TrackIndexRole, "trackIndex"}};
}

void TrackModel::setProject(const project::Project& project) {
    beginResetModel();
    items_.clear();
    const auto& tracks = project::tracksOfKind(project, kind_);
    for (int index = 0; index < static_cast<int>(tracks.size()); ++index) {
        const auto& track = tracks[static_cast<std::size_t>(index)];
        items_.append({QString::fromStdString(track.name), track.muted, track.solo,
                       project::isTrackOutputEnabled(project, {kind_, index})});
    }
    endResetModel();
}

} // namespace mvm::app
