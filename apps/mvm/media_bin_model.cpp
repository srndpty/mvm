#include "media_bin_model.h"

#include "project/media_bin.h"

#include <algorithm>

namespace mvm::app {
namespace {

QString twoDigits(std::int64_t value) {
    return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

QString hms(std::int64_t totalSeconds) {
    return twoDigits(totalSeconds / 3600) + u':' + twoDigits(totalSeconds / 60 % 60) + u':' +
           twoDigits(totalSeconds % 60);
}

} // namespace

QString formatMediaRate(const project::MediaItem& item) {
    switch (item.kind) {
    case project::MediaKind::Video: {
        if (item.fpsNum <= 0 || item.fpsDen <= 0)
            return {};
        // 23.976 / 29.97 / 60.00 のように、小数 3 桁から末尾の 0 を 2 桁まで落とす。
        QString text = QString::number(
            static_cast<double>(item.fpsNum) / static_cast<double>(item.fpsDen), 'f', 3);
        if (text.endsWith(u'0'))
            text.chop(1);
        return text + QStringLiteral(" fps");
    }
    case project::MediaKind::Audio:
        return QString::number(item.sampleRate) + QStringLiteral(" Hz");
    case project::MediaKind::Image:
        return {};
    }
    return {};
}

QString formatMediaDuration(const project::MediaItem& item) {
    switch (item.kind) {
    case project::MediaKind::Video: {
        if (item.fpsNum <= 0 || item.fpsDen <= 0)
            return {};
        // non-drop の timecode。frame 桁は公称 fps (23.976 なら 24) で数える。
        const std::int64_t nominal = (item.fpsNum + item.fpsDen - 1) / item.fpsDen;
        return hms(item.frameCount / nominal) + u':' + twoDigits(item.frameCount % nominal);
    }
    case project::MediaKind::Audio: {
        if (item.sampleRate <= 0)
            return {};
        const std::int64_t milliseconds = item.durationSamples * 1000 / item.sampleRate;
        return hms(milliseconds / 1000) + u'.' +
               QStringLiteral("%1").arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
    }
    case project::MediaKind::Image:
        return {};
    }
    return {};
}

QString formatMediaSize(const project::MediaItem& item) {
    if (item.kind == project::MediaKind::Audio || item.width <= 0 || item.height <= 0)
        return {};
    return QStringLiteral("%1 × %2").arg(item.width).arg(item.height);
}

MediaBinModel::MediaBinModel(QObject* parent) : QAbstractListModel(parent) {}

int MediaBinModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MediaBinModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
        return {};
    const auto& row = rows_[index.row()];
    switch (role) {
    case EntryIdRole:
        return row.id;
    case EntryKindRole:
        return row.kind;
    case NameRole:
        return row.name;
    case DepthRole:
        return row.depth;
    case ParentIdRole:
        return row.parentId;
    case ExpandedRole:
        return row.expanded;
    case HasChildrenRole:
        return row.hasChildren;
    case RateTextRole:
        return row.rateText;
    case DurationTextRole:
        return row.durationText;
    case SizeTextRole:
        return row.sizeText;
    case MediaPathRole:
        return row.mediaPath;
    case InUseRole:
        return row.inUse;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaBinModel::roleNames() const {
    return {{EntryIdRole, "entryId"},
            {EntryKindRole, "entryKind"},
            {NameRole, "name"},
            {DepthRole, "depth"},
            {ParentIdRole, "parentId"},
            {ExpandedRole, "expanded"},
            {HasChildrenRole, "hasChildren"},
            {RateTextRole, "rateText"},
            {DurationTextRole, "durationText"},
            {SizeTextRole, "sizeText"},
            {MediaPathRole, "mediaPath"},
            {InUseRole, "inUse"}};
}

void MediaBinModel::setProject(const project::Project& project) {
    const int previousCount = entryCount();
    folders_ = project.mediaFolders;
    items_ = project.mediaItems;
    inUseItems_.clear();
    for (const auto& item : items_) {
        if (project::isMediaItemInUse(project, item))
            inUseItems_.insert(QString::fromStdString(item.id));
    }
    // 消えた folder の展開状態は持ち越さない。同じ id が将来再利用されても開かない。
    QSet<QString> liveFolders;
    for (const auto& folder : folders_)
        liveFolders.insert(QString::fromStdString(folder.id));
    expandedFolders_.intersect(liveFolders);

    beginResetModel();
    rows_.clear();
    appendChildren({}, 0, rows_);
    endResetModel();
    if (previousCount != entryCount())
        Q_EMIT entryCountChanged();
}

void MediaBinModel::appendChildren(const std::string& parentId, int depth, QList<Row>& rows) const {
    std::vector<const project::MediaFolder*> folders;
    for (const auto& folder : folders_) {
        if (folder.parentId == parentId)
            folders.push_back(&folder);
    }
    std::vector<const project::MediaItem*> items;
    for (const auto& item : items_) {
        if (item.folderId == parentId)
            items.push_back(&item);
    }
    const auto byName = [](const auto* left, const auto* right) {
        const int order = QString::fromStdString(left->name)
                              .localeAwareCompare(QString::fromStdString(right->name));
        return order != 0 ? order < 0 : left->id < right->id;
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(items.begin(), items.end(), byName);

    const QString parent = QString::fromStdString(parentId);
    for (const auto* folder : folders) {
        const QString id = QString::fromStdString(folder->id);
        const bool hasChildren =
            std::any_of(folders_.begin(), folders_.end(),
                        [&](const auto& child) { return child.parentId == folder->id; }) ||
            std::any_of(items_.begin(), items_.end(),
                        [&](const auto& child) { return child.folderId == folder->id; });
        Row row;
        row.id = id;
        row.kind = QStringLiteral("folder");
        row.name = QString::fromStdString(folder->name);
        row.depth = depth;
        row.parentId = parent;
        row.expanded = expandedFolders_.contains(id);
        row.hasChildren = hasChildren;
        rows.append(row);
        if (row.expanded)
            appendChildren(folder->id, depth + 1, rows);
    }
    for (const auto* item : items) {
        Row row;
        row.id = QString::fromStdString(item->id);
        row.kind = QString::fromLatin1(project::mediaKindName(item->kind));
        row.name = QString::fromStdString(item->name);
        row.depth = depth;
        row.parentId = parent;
        row.rateText = formatMediaRate(*item);
        row.durationText = formatMediaDuration(*item);
        row.sizeText = formatMediaSize(*item);
        row.mediaPath = QString::fromStdWString(item->mediaPath.wstring());
        row.inUse = inUseItems_.contains(row.id);
        rows.append(row);
    }
}

void MediaBinModel::setExpanded(const QString& folderId, bool expanded) {
    const int row = rowOfEntry(folderId);
    if (row < 0 || rows_[row].kind != QStringLiteral("folder") || rows_[row].expanded == expanded)
        return;
    if (expanded)
        expandedFolders_.insert(folderId);
    else
        expandedFolders_.remove(folderId);

    // 開閉で変わるのは folder 直後の子孫の塊だけなので、reset せずに行を出し入れする。
    // reset するとリストのスクロール位置が失われる。
    QList<Row> next;
    appendChildren({}, 0, next);
    const auto delta = next.size() - rows_.size();
    if (delta > 0) {
        beginInsertRows({}, row + 1, static_cast<int>(row + delta));
        rows_ = std::move(next);
        endInsertRows();
    } else if (delta < 0) {
        beginRemoveRows({}, row + 1, static_cast<int>(row - delta));
        rows_ = std::move(next);
        endRemoveRows();
    } else {
        rows_ = std::move(next);
    }
    Q_EMIT dataChanged(index(row, 0), index(row, 0), {ExpandedRole});
}

void MediaBinModel::toggleExpanded(const QString& folderId) {
    setExpanded(folderId, !expandedFolders_.contains(folderId));
}

int MediaBinModel::rowOfEntry(const QString& entryId) const {
    for (int row = 0; row < rows_.size(); ++row) {
        if (rows_[row].id == entryId)
            return row;
    }
    return -1;
}

QString MediaBinModel::entryIdAt(int row) const {
    return row >= 0 && row < rows_.size() ? rows_[row].id : QString();
}

QString MediaBinModel::parentFolderOf(const QString& entryId) const {
    const std::string id = entryId.toStdString();
    for (const auto& folder : folders_) {
        if (folder.id == id)
            return QString::fromStdString(folder.parentId);
    }
    for (const auto& item : items_) {
        if (item.id == id)
            return QString::fromStdString(item.folderId);
    }
    return {};
}

QString MediaBinModel::containingFolderOf(const QString& entryId) const {
    const std::string id = entryId.toStdString();
    for (const auto& folder : folders_) {
        if (folder.id == id)
            return entryId;
    }
    for (const auto& item : items_) {
        if (item.id == id)
            return QString::fromStdString(item.folderId);
    }
    return {};
}

} // namespace mvm::app
