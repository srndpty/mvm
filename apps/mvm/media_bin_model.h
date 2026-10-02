#ifndef MVM_APPS_MVM_MEDIA_BIN_MODEL_H
#define MVM_APPS_MVM_MEDIA_BIN_MODEL_H

#include "project/project.h"

#include <vector>

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace mvm::app {

// 表示用の整形。該当しない列は空文字列を返す。
//   rate     : "23.976 fps" / "60.00 fps" / "48000 Hz"
//   duration : 動画は HH:MM:SS:FF (公称 fps)、音声は HH:MM:SS.mmm
//   size     : "1920 × 1080"
QString formatMediaRate(const project::MediaItem& item);
QString formatMediaDuration(const project::MediaItem& item);
QString formatMediaSize(const project::MediaItem& item);

// プロジェクトパネルのリスト表示。folder の木を、展開中の folder だけ開いた
// 1 次元の行へ平坦化して公開する。並びは folder が先、同種内は名前順。
class MediaBinModel : public QAbstractListModel {
    Q_OBJECT
    // QML からは mvmController.mediaBinModel 経由でだけ触る。型名は公開しない。
    QML_ANONYMOUS
    Q_PROPERTY(int entryCount READ entryCount NOTIFY entryCountChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

public:
    enum Role {
        EntryIdRole = Qt::UserRole + 1,
        EntryKindRole, // "folder" / "video" / "audio" / "image"
        NameRole,
        DepthRole,
        ParentIdRole,
        ExpandedRole,
        HasChildrenRole,
        RateTextRole,
        DurationTextRole,
        SizeTextRole,
        MediaPathRole,
        InUseRole,
    };

    explicit MediaBinModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setProject(const project::Project& project);

    QString filterText() const { return filterText_; }

    void setFilterText(const QString& text);

    // folder / item の総数 (折りたたみに関係しない)。空表示の判定に使う。
    int entryCount() const { return static_cast<int>(folders_.size() + items_.size()); }

    Q_INVOKABLE void setExpanded(const QString& folderId, bool expanded);
    Q_INVOKABLE void toggleExpanded(const QString& folderId);
    Q_INVOKABLE int rowOfEntry(const QString& entryId) const;
    Q_INVOKABLE QString entryIdAt(int row) const;
    // entry の親 folder の id。root 直下なら空文字列。
    Q_INVOKABLE QString parentFolderOf(const QString& entryId) const;
    // 読み込み先などを決めるため、entry が folder ならそれ自身、item なら親 folder を返す。
    Q_INVOKABLE QString containingFolderOf(const QString& entryId) const;

Q_SIGNALS:
    void entryCountChanged();
    void filterTextChanged();

private:
    struct Row {
        QString id;
        QString kind;
        QString name;
        int depth = 0;
        QString parentId;
        bool expanded = false;
        bool hasChildren = false;
        QString rateText;
        QString durationText;
        QString sizeText;
        QString mediaPath;
        bool inUse = false;
    };

    void appendChildren(const std::string& parentId, int depth, QList<Row>& rows,
                        bool ancestorMatches = false) const;

    std::vector<project::MediaFolder> folders_;
    std::vector<project::MediaItem> items_;
    QSet<QString> inUseItems_;
    QSet<QString> expandedFolders_;
    QList<Row> rows_;
    QString filterText_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MEDIA_BIN_MODEL_H
