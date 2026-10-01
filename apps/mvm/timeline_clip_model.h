#ifndef MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
#define MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H

#include "project/project.h"

#include <QAbstractListModel>
#include <QSet>
#include <QSortFilterProxyModel>

#include <cstdint>
#include <limits>
#include <QStringList>
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
        // この model での行番号。絞り込んだ proxy の delegate が、controller へ全 clip の
        // 行番号を渡すために使う (proxy の index は絞り込んだ後の番号)。
        ClipRowRole,
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

    // 編集のたびに model を作り直さない。clip の並びの共通の先頭・末尾は行を保ったまま値の
    // 変わった role だけを dataChanged で知らせ、間だけを行の削除・挿入にする。作り直すと
    // timeline の全 delegate (操作・波形・メニューを持つ) を毎回作り直すことになる。
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

    // 値が変わった role。
    static QList<int> changedRoles(const Item& before, const Item& after);

    QList<Item> items_;
};

// timeline の表示範囲 (± 余白) に掛かる clip と、固定する clip (押している・操作中) だけを通す。
// 選択中の clip は固定しない (全選択で全 clip の delegate を作らない)。群のドラッグで範囲外から
// 見えてくる clip は、QML がドラッグ量だけ広げた範囲を渡して作る。
// timeline の Repeater はこれを model にし、clip が数千あっても delegate の数を表示範囲に
// 比例する数に抑える。範囲は setVisibleRange で受け、余白の内側を見ている間は絞り直さない
// (スクロールのたびに全 clip を判定し直さない)。範囲を受けるまでは固定する clip だけを通す。
class TimelineClipWindowModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ANONYMOUS
    // 表示範囲の外でも delegate を残す clip。押している clip の delegate をスクロールで
    // 消すと、操作の途中で release が届かなくなる。
    Q_PROPERTY(QStringList pinnedClipIds READ pinnedClipIds WRITE setPinnedClipIds NOTIFY
                   pinnedClipIdsChanged)

public:
    explicit TimelineClipWindowModel(QObject* parent = nullptr);

    // 表示している frame の範囲 [startFrame, endFrame)。
    Q_INVOKABLE void setVisibleRange(double startFrame, double endFrame);
    bool hasWindow() const { return windowValid_; }
    double windowStartFrame() const { return windowStart_; }
    double windowEndFrame() const { return windowEnd_; }

    QStringList pinnedClipIds() const { return pinnedClipIds_; }
    void setPinnedClipIds(const QStringList& clipIds);

signals:
    void pinnedClipIdsChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    bool windowValid_ = false;
    double windowStart_ = 0.0;
    double windowEnd_ = 0.0;
    QStringList pinnedClipIds_;
    QSet<QString> pinned_;
};

// 再生位置に掛かる文字 clip と、固定する文字 clip (preview 上でドラッグしている) だけを通す。
// preview の文字 layer は再生位置で見えている文字にしか要らないので、字幕のように文字 clip が
// 数千あっても、delegate (preview 全面) は再生位置に掛かる数 + 固定する数に収まる。
// 再生位置は毎 frame 変わるので、通す組が変わらない範囲 [stableFrom, stableUntil) を絞り込みの
// ついでに求めておき、その内側の移動では絞り直さない (文字 clip の境界を跨いだときだけ全行を
// 判定し直す)。
class TextClipFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QStringList pinnedClipIds READ pinnedClipIds WRITE setPinnedClipIds NOTIFY
                   pinnedClipIdsChanged)

public:
    explicit TextClipFilterModel(QObject* parent = nullptr);

    void setPlayheadFrame(qint64 frame);
    qint64 playheadFrame() const { return playhead_; }
    // 再生位置の移動で全行を判定し直した回数 (試験用)。
    std::uint64_t playheadRefilterCountForTest() const { return playheadRefilterCount_; }

    QStringList pinnedClipIds() const { return pinnedClipIds_; }
    void setPinnedClipIds(const QStringList& clipIds);

signals:
    void pinnedClipIdsChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    qint64 playhead_ = 0;
    // playhead_ がこの範囲にある間は、通す文字 clip の組が変わらない。行の追加・変更で
    // filterAcceptsRow が呼ばれるたびに狭めるだけなので (行が消えても広げない)、実際より
    // 狭いことはあっても広いことはない。
    mutable qint64 stableFrom_ = std::numeric_limits<qint64>::min();
    mutable qint64 stableUntil_ = std::numeric_limits<qint64>::max();
    std::uint64_t playheadRefilterCount_ = 0;
    QStringList pinnedClipIds_;
    QSet<QString> pinned_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_TIMELINE_CLIP_MODEL_H
