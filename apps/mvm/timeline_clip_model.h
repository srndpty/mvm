#ifndef MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
#define MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H

#include "project/project.h"

#include <QAbstractListModel>
#include <QSet>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace mvm::app {

class TimelineClipModel : public QAbstractListModel {
    Q_OBJECT
    // QML からは mvmController.timelineModel 経由でだけ触る。型名は公開しない。
    QML_ANONYMOUS

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
        TrackKindRole,
        TrackIndexRole,
        LinkedRole,
        LinkGroupIdRole,
        SelectedRole,
        MediaPathRole,
        AutomationKeysRole,
        AutomationBaseRole,
        // 再生速度の倍率 (1.0 が等速)。表示と波形の素材秒の換算だけに使う。
        // frame の換算は C++ 側 (project::clipTimebase) で行い、QML で有理数を再現しない。
        SpeedRole,
        FrameHoldRole,
        PreservePitchRole,
        // 名前は clipEnabled。enabled にすると delegate の Item.enabled を隠し、
        // 無効にした clip の MouseArea まで止まる (選び直せなくなる)。
        ClipEnabledRole,
    };

    explicit TimelineClipModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 範囲外の row は空文字列。Repeater の itemAt(index) と組で使う。
    Q_INVOKABLE QString clipIdAt(int row) const;
    // 全 clip の {clipId, trackKind, trackIndex, start, end, linkGroupId}。
    // drag 中の表示で隣接 clip やリンク相手を探すのに使う。delegate の item から
    // 読むと QML の静的検査で型が QQuickItem にしかならない。
    Q_INVOKABLE QVariantList clipSpans() const;

    void setProject(const project::Project& project);
    void setSelectedClipIds(const QSet<QString>& clipIds);

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
        QString trackKind;
        int trackIndex = 0;
        bool linked = false;
        QString linkGroupId;
        bool selected = false;
        QString mediaPath;
        QVariantList automationKeys;
        double automationBase = 100.0;
        double speed = 1.0;
        bool frameHold = false;
        bool preservePitch = false;
        bool enabled = true;
    };

    QList<Item> items_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
