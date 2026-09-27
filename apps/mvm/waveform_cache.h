#ifndef MVM_APPS_MVM_WAVEFORM_CACHE_H
#define MVM_APPS_MVM_WAVEFORM_CACHE_H

#include "core/waveform_peaks.h"
#include "media/audio_waveform/audio_waveform_decoder.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace mvm::app {

// timeline の音声波形を素材ごとに生成して持つ。
// decode は worker thread で行い、GUI thread は完成した peak を読むだけにする。
//
// 素材の identity は path だけでなく size と更新時刻も含める。同じ path の素材が
// 差し替えられた場合や、失敗後に file が現れた場合に、次の request で作り直す。
//
// 完成した peak は byte budget を超えたら、どの WaveformView も参照していない
// ものから古い順に捨てる。表示中の波形は budget を超えても捨てない。
class WaveformCache final : public QObject {
    Q_OBJECT

public:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        State state = State::Loading;
        std::shared_ptr<const core::WaveformPeaks> peaks;
        QString error;
    };

    using DecodeFunction = std::function<audio::AudioWaveformResult(
        const std::string& utf8Path, const std::atomic<bool>* cancel)>;

    static constexpr std::size_t kDefaultBudgetBytes = std::size_t{256} * 1024 * 1024;

    // decode を省略すると audio::decodeAudioWaveform を使う。差し替えはテスト用。
    explicit WaveformCache(QObject* parent = nullptr);
    WaveformCache(std::size_t budgetBytes, DecodeFunction decode, QObject* parent = nullptr);
    // 生成中の job を中断し、終わるまで待つ。job は this を参照するため。
    ~WaveformCache() override;

    // 素材が未要求か、前回から identity が変わっていれば生成を開始する。
    // 現在の状態を返す。
    Entry request(const QString& mediaPath);

    // 同じ素材を指す path を同じ値へ正規化する。entryChanged の引数はこの値。
    static QString sourceKey(const QString& mediaPath);

    // 表示中の全 view に request し直させる。外部で差し替えられた素材を、
    // timeline の編集を待たずに検出するため、アプリが前面へ戻ったときに呼ぶ。
    void revalidateAll();

    // 診断・テスト用。
    std::size_t readyBytes() const;
    int entryCount() const { return static_cast<int>(records_.size()); }

Q_SIGNALS:
    // sourceKey が空なら全素材が対象。
    void entryChanged(const QString& sourceKey);

private:
    struct SourceIdentity {
        bool exists = false;
        qint64 size = -1;
        qint64 lastModifiedMs = -1;
        bool operator==(const SourceIdentity&) const = default;
    };

    struct Record {
        Entry entry;
        SourceIdentity identity;
        std::uint64_t ticket = 0;
        std::uint64_t lastUse = 0;
        std::size_t bytes = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    static SourceIdentity identityOf(const QString& mediaPath);
    void finish(const QString& key, std::uint64_t ticket, Entry entry);
    void evictUnused(const QString& keep);

    std::size_t budgetBytes_;
    DecodeFunction decode_;
    QThreadPool pool_;
    QHash<QString, Record> records_;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t useClock_ = 0;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_WAVEFORM_CACHE_H
