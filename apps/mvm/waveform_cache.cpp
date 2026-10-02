#include "waveform_cache.h"

#include "media_source_identity.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>

#include <limits>
#include <vector>

namespace mvm::app {

WaveformCache::WaveformCache(QObject* parent)
    : WaveformCache(kDefaultBudgetBytes, {}, parent) {}

WaveformCache::WaveformCache(std::size_t budgetBytes, DecodeFunction decode, QObject* parent)
    : QObject(parent), budgetBytes_(budgetBytes), decode_(std::move(decode)) {
    if (!decode_) {
        decode_ = [](const std::string& utf8Path, const std::atomic<bool>* cancel) {
            return audio::decodeAudioWaveform(utf8Path, cancel);
        };
    }
    // 素材を一度に大量に追加しても、decode が preview の CPU を食い尽くさないようにする。
    pool_.setMaxThreadCount(2);
}

WaveformCache::~WaveformCache() {
    shuttingDown_->store(true, std::memory_order_relaxed);
    for (const auto& record : std::as_const(records_)) {
        if (record.cancel)
            record.cancel->store(true, std::memory_order_relaxed);
    }
    pool_.clear();
    pool_.waitForDone();
}

QString WaveformCache::sourceKey(const QString& mediaPath) {
    return probeMediaSource(mediaPath).key;
}

void WaveformCache::revalidateAll() {
    // size / 更新時刻 / 実体の変化は、各 view の request し直しで検出する。
    Q_EMIT entryChanged(QString());

    // size と更新時刻が一致したまま中身だけ差し替えられた素材は、内容の標本 fingerprint で
    // 検出を試みる (中央部だけの変更は取りこぼす。waveform_cache.h)。file を読むので
    // GUI thread ではなく worker で行う。
    struct Target {
        QString key;
        std::uint64_t ticket = 0;
        QString path;
        std::uint64_t fingerprint = 0;
    };
    std::vector<Target> targets;
    for (auto it = records_.cbegin(); it != records_.cend(); ++it) {
        if (!it->cancel && it->fingerprint)
            targets.push_back({it.key(), it->ticket, it->path, *it->fingerprint});
    }
    if (targets.empty())
        return;
    pool_.start([this, targets = std::move(targets), shuttingDown = shuttingDown_] {
        QList<QPair<QString, quint64>> stale;
        for (const auto& target : targets) {
            if (shuttingDown->load(std::memory_order_relaxed))
                return;
            const auto current = mediaContentFingerprint(target.path);
            if (!current || *current != target.fingerprint)
                stale.append({target.key, target.ticket});
        }
        if (stale.isEmpty())
            return;
        QMetaObject::invokeMethod(
            this, [this, stale] { dropStale(stale); }, Qt::QueuedConnection);
    });
}

void WaveformCache::dropStale(const QList<QPair<QString, quint64>>& stale) {
    for (const auto& [key, ticket] : stale) {
        const auto found = records_.find(key);
        // 検査中に作り直しが始まった record は、その結果を待つ。
        if (found == records_.end() || found->ticket != ticket)
            continue;
        records_.erase(found);
        Q_EMIT entryChanged(key);
    }
}

std::size_t WaveformCache::readyBytes() const {
    std::size_t total = 0;
    for (const auto& record : records_)
        total += record.bytes;
    return total;
}

WaveformCache::Entry WaveformCache::request(const QString& mediaPath) {
    if (mediaPath.isEmpty())
        return {State::Failed, {}, QStringLiteral("素材の path がありません")};
    const MediaSourceProbe source = probeMediaSource(mediaPath);
    const QString& key = source.key;
    const MediaSourceIdentity& identity = source.identity;
    auto found = records_.find(key);
    if (found != records_.end() && found->identity == identity) {
        found->lastUse = ++useClock_;
        return found->entry;
    }

    // 前回と identity が違う。生成中の job は結果ごと捨てる (ticket で見分ける)。
    if (found != records_.end() && found->cancel)
        found->cancel->store(true, std::memory_order_relaxed);
    Record record;
    record.identity = identity;
    record.path = mediaPath;
    record.ticket = nextTicket_++;
    record.lastUse = ++useClock_;
    record.cancel = std::make_shared<std::atomic<bool>>(false);
    records_.insert(key, record);

    pool_.start([this, key, ticket = record.ticket, cancel = record.cancel, mediaPath] {
        // decode より前に取る。decode 中に先頭・末尾が差し替えられれば、次の再検証で不一致になる。
        const auto fingerprint = mediaContentFingerprint(mediaPath);
        auto result = decode_(mediaPath.toStdString(), cancel.get());
        if (result.cancelled || cancel->load(std::memory_order_relaxed))
            return;
        Entry entry;
        if (result.success) {
            entry.state = State::Ready;
            entry.peaks = std::make_shared<const core::WaveformPeaks>(std::move(result.peaks));
        } else {
            entry.state = State::Failed;
            entry.error = QString::fromStdString(result.error);
        }
        // this は destructor が waitForDone するまで生きている。queued event は
        // this の破棄時に Qt が捨てるので、完了通知が破棄後に届くことはない。
        QMetaObject::invokeMethod(
            this,
            [this, key, ticket, fingerprint, entry = std::move(entry)]() mutable {
                finish(key, ticket, fingerprint, std::move(entry));
            },
            Qt::QueuedConnection);
    });
    return record.entry;
}

void WaveformCache::finish(const QString& key, std::uint64_t ticket,
                           std::optional<std::uint64_t> fingerprint, Entry entry) {
    auto found = records_.find(key);
    // 生成中に素材が差し替えられた場合、古い job の結果で新しい record を上書きしない。
    if (found == records_.end() || found->ticket != ticket)
        return;
    if (entry.state == State::Failed)
        qWarning("波形を生成できません: %s: %s", qUtf8Printable(key), qUtf8Printable(entry.error));
    found->bytes = entry.peaks ? core::waveformPeaksMemoryBytes(*entry.peaks) : 0;
    found->entry = std::move(entry);
    found->fingerprint = fingerprint;
    found->cancel.reset();
    evictUnused(key);
    Q_EMIT entryChanged(key);
}

void WaveformCache::evictUnused(const QString& keep) {
    std::size_t total = readyBytes();
    while (total > budgetBytes_) {
        // cache だけが peak を持っている (どの view も表示していない) ものから古い順に捨てる。
        // 完成したばかりの record は view がまだ受け取っていないので残す。
        auto victim = records_.end();
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            if (it.key() == keep || !it->entry.peaks || it->entry.peaks.use_count() != 1)
                continue;
            if (it->lastUse < oldest) {
                oldest = it->lastUse;
                victim = it;
            }
        }
        if (victim == records_.end())
            return;
        total -= victim->bytes;
        records_.erase(victim);
    }
}

} // namespace mvm::app
