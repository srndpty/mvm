#ifndef MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
#define MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H

#include "project/project.h"

#include <QAbstractListModel>

namespace mvm::app {

class TimelineClipModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        ClipIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        KindRole,
        TimelineStartFrameRole,
        TimelineDurationFramesRole,
        SourceInFrameRole,
        SourceOutFrameRole,
        SourceFrameCountRole,
        SourceFpsNumRole,
        SourceFpsDenRole,
        PreviewSupportedRole,
        VideoTrackRole,
    };

    explicit TimelineClipModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProject(const project::Project& project);

private:
    struct Item {
        QString id;
        QString name;
        QString kind;
        qint64 timelineStartFrame = 0;
        qint64 timelineDurationFrames = 0;
        qint64 sourceInFrame = 0;
        qint64 sourceOutFrame = 0;
        qint64 sourceFrameCount = 0;
        qint64 sourceFpsNum = 0;
        qint64 sourceFpsDen = 1;
        bool previewSupported = false;
        int videoTrack = 0;
    };

    QList<Item> items_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
