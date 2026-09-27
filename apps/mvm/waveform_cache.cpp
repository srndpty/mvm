#include "waveform_cache.h"

#include "media/audio_waveform/audio_waveform_decoder.h"

#include <QMetaObject>

namespace mvm::app {

WaveformCache::WaveformCache(QObject* parent) : QObject(parent) {
    // 素材を一度に大量に追加しても、decode が preview の CPU を食い尽くさないようにする。
    pool_.setMaxThreadCount(2);
}

WaveformCache::~WaveformCache() {
    cancel_.store(true, std::memory_order_relaxed);
    pool_.clear();
    pool_.waitForDone();
}

WaveformCache::Entry WaveformCache::request(const QString& mediaPath) {
    if (mediaPath.isEmpty())
        return {State::Failed, {}, QStringLiteral("素材の path がありません")};
    const auto found = entries_.constFind(mediaPath);
    if (found != entries_.constEnd())
        return found.value();

    entries_.insert(mediaPath, Entry{});
    const std::string utf8Path = mediaPath.toStdString();
    pool_.start([this, mediaPath, utf8Path] {
        auto result = audio::decodeAudioWaveform(utf8Path, &cancel_);
        if (result.cancelled)
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
            this, [this, mediaPath, entry = std::move(entry)]() mutable {
                finish(mediaPath, std::move(entry));
            },
            Qt::QueuedConnection);
    });
    return entries_.value(mediaPath);
}

void WaveformCache::finish(const QString& mediaPath, Entry entry) {
    if (entry.state == State::Failed)
        qWarning("波形を生成できません: %s: %s", qUtf8Printable(mediaPath),
                 qUtf8Printable(entry.error));
    entries_.insert(mediaPath, std::move(entry));
    Q_EMIT entryChanged(mediaPath);
}

} // namespace mvm::app
