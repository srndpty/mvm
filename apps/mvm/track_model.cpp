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
    case MixerNameRole:
        return item.mixerName;
    case MixerGainRole:
        return item.mixerGain;
    case MixerPanRole:
        return item.mixerPan;
    case TrackIndexRole:
        return index.row();
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackModel::roleNames() const {
    return {{TrackNameRole, "trackName"}, {MutedRole, "trackMuted"},
            {SoloRole, "trackSolo"},      {OutputEnabledRole, "trackOutputEnabled"},
            {TrackKindRole, "trackKind"}, {TrackIndexRole, "trackIndex"},
            {MixerNameRole, "mixerName"}, {MixerGainRole, "mixerGainDb"},
            {MixerPanRole, "mixerPan"}};
}

void TrackModel::setMixerValues(int row, double gain, double pan) {
    if (row < 0 || row >= items_.size())
        return;
    items_[row].mixerGain = gain;
    items_[row].mixerPan = pan;
    Q_EMIT dataChanged(index(row, 0), index(row, 0), {MixerGainRole, MixerPanRole});
}

void TrackModel::setProject(const project::Project& project) {
    QList<Item> next;
    const auto& tracks = project::tracksOfKind(project, kind_);
    for (int index = 0; index < static_cast<int>(tracks.size()); ++index) {
        const auto& track = tracks[static_cast<std::size_t>(index)];
        next.append({QString::fromStdString(track.name), track.muted, track.solo,
                     project::isTrackOutputEnabled(project, {kind_, index}),
                     track.mixerName.empty() ? QStringLiteral("音声 %1").arg(index + 1)
                                             : QString::fromStdString(track.mixerName),
                     track.mixerGainDb, track.mixerPan});
    }
    // 行数が変わらなければ作り直さず、変わった行だけを通知する。reset すると QML の delegate
    // (目玉の Canvas を含む) が全部作り直され、mute / solo を押すたびにヘッダが一瞬消える。
    if (next.size() != items_.size()) {
        beginResetModel();
        items_ = std::move(next);
        endResetModel();
        return;
    }
    for (int row = 0; row < items_.size(); ++row) {
        if (items_[row] == next[row])
            continue;
        items_[row] = next[row];
        Q_EMIT dataChanged(index(row, 0), index(row, 0));
    }
}

} // namespace mvm::app
