#ifndef MVM_APPS_MVM_TRACK_MODEL_H
#define MVM_APPS_MVM_TRACK_MODEL_H

#include "project/project.h"

#include <QAbstractListModel>

namespace mvm::app {

// video / audio いずれか一方の track 列を QML へ出す。
// 「どちらの kind か」は生成時に固定し、行ごとに取り違えられないようにする。
class TrackModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TrackNameRole = Qt::UserRole + 1,
        MutedRole,
        TrackKindRole,
        TrackIndexRole,
    };

    explicit TrackModel(project::TrackKind kind, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProject(const project::Project& project);

private:
    struct Item {
        QString name;
        bool muted = false;
    };

    project::TrackKind kind_;
    QList<Item> items_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_TRACK_MODEL_H
