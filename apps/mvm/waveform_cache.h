#ifndef MVM_APPS_MVM_WAVEFORM_CACHE_H
#define MVM_APPS_MVM_WAVEFORM_CACHE_H

#include "core/waveform_peaks.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace mvm::app {

// timeline の音声波形を素材 path ごとに 1 度だけ生成して持つ。
// decode は worker thread で行い、GUI thread は完成した peak を読むだけにする。
class WaveformCache final : public QObject {
    Q_OBJECT

public:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State state = State::Loading;
        std::shared_ptr<const core::WaveformPeaks> peaks;
        QString error;
    };

    explicit WaveformCache(QObject* parent = nullptr);
    // 生成中の job を中断し、終わるまで待つ。job は this を参照するため。
    ~WaveformCache() override;

    // 未要求の素材なら生成を開始し、現在の状態を返す。
    Entry request(const QString& mediaPath);

Q_SIGNALS:
    void entryChanged(const QString& mediaPath);

private:
    void finish(const QString& mediaPath, Entry entry);

    QThreadPool pool_;
    std::atomic<bool> cancel_{false};
    QHash<QString, Entry> entries_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_WAVEFORM_CACHE_H
