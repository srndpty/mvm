#ifndef MVM_APP_SUBTITLE_MODEL_H
#define MVM_APP_SUBTITLE_MODEL_H
#include "project/project.h"

#include <QAbstractListModel>

namespace mvm::app {
class SubtitleListModel : public QAbstractListModel {
public:
    enum Role { CueId = Qt::UserRole + 1, StartFrame, EndFrame, Content, Linked };

    explicit SubtitleListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(cues_.size());
    }

    QHash<int, QByteArray> roleNames() const override {
        return {{CueId, "cueId"},
                {StartFrame, "startFrame"},
                {EndFrame, "endFrame"},
                {Content, "content"},
                {Linked, "linked"}};
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
            return {};
        const auto& cue = cues_[static_cast<std::size_t>(index.row())];
        switch (role) {
        case CueId:
            return QString::fromStdString(cue.id);
        case StartFrame:
            return QVariant::fromValue<qlonglong>(cue.startFrame);
        case EndFrame:
            return QVariant::fromValue<qlonglong>(cue.endFrame);
        case Content:
            return QString::fromStdString(cue.content);
        case Linked:
            return !cue.linkClipId.empty();
        default:
            return {};
        }
    }

    // 編集のたびに model を作り直さない。作り直すと timeline と字幕パネルの全 delegate
    // (操作を持つ) を毎回作り直し、字幕 400 件で 1 回の移動に約 0.4 秒かかった (release)。
    // ID の並びが一致する先頭と末尾は行を保って値の変わった行だけを知らせ、間だけを
    // 行の削除・挿入にする (TimelineClipModel::setProject と同じ方式)。
    void setCues(const std::vector<project::SubtitleCue>& cues) {
        if (cues_ == cues)
            return;
        const auto oldSize = cues_.size();
        const auto newSize = cues.size();
        std::size_t prefix = 0;
        while (prefix < oldSize && prefix < newSize && cues_[prefix].id == cues[prefix].id)
            ++prefix;
        std::size_t suffix = 0;
        while (suffix < oldSize - prefix && suffix < newSize - prefix &&
               cues_[oldSize - 1 - suffix].id == cues[newSize - 1 - suffix].id)
            ++suffix;
        const auto oldMiddle = oldSize - prefix - suffix;
        const auto newMiddle = newSize - prefix - suffix;
        if (oldMiddle > 0) {
            beginRemoveRows({}, static_cast<int>(prefix), static_cast<int>(prefix + oldMiddle - 1));
            cues_.erase(cues_.begin() + static_cast<std::ptrdiff_t>(prefix),
                        cues_.begin() + static_cast<std::ptrdiff_t>(prefix + oldMiddle));
            endRemoveRows();
        }
        if (newMiddle > 0) {
            beginInsertRows({}, static_cast<int>(prefix), static_cast<int>(prefix + newMiddle - 1));
            cues_.insert(cues_.begin() + static_cast<std::ptrdiff_t>(prefix),
                         cues.begin() + static_cast<std::ptrdiff_t>(prefix),
                         cues.begin() + static_cast<std::ptrdiff_t>(prefix + newMiddle));
            endInsertRows();
        }
        // 残した行は値の変わった行だけを知らせる (delegate を作り直さない)。
        for (std::size_t row = 0; row < newSize; ++row) {
            if (cues_[row] == cues[row])
                continue;
            cues_[row] = cues[row];
            const auto changed = index(static_cast<int>(row), 0);
            Q_EMIT dataChanged(changed, changed);
        }
    }

private:
    std::vector<project::SubtitleCue> cues_;
};
} // namespace mvm::app
#endif
