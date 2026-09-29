#include "waveform_view.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mvm::app {

WaveformView::WaveformView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
}

void WaveformView::setCache(QObject* cache) {
    auto* typed = qobject_cast<WaveformCache*>(cache);
    if (cache && !typed) {
        qWarning("WaveformView.cache には WaveformCache を渡してください");
        return;
    }
    if (cache_ == typed)
        return;
    if (cache_)
        disconnect(cache_, nullptr, this, nullptr);
    cache_ = typed;
    if (cache_) {
        connect(cache_, &WaveformCache::entryChanged, this, [this](const QString& key) {
            if (key.isEmpty() || key == WaveformCache::sourceKey(mediaPath_))
                refreshEntry();
        });
    }
    Q_EMIT cacheChanged();
    refreshEntry();
}

void WaveformView::setMediaPath(const QString& mediaPath) {
    if (mediaPath_ == mediaPath)
        return;
    mediaPath_ = mediaPath;
    Q_EMIT mediaPathChanged();
    refreshEntry();
}

void WaveformView::setStartSeconds(double seconds) {
    if (startSeconds_ == seconds)
        return;
    startSeconds_ = seconds;
    Q_EMIT startSecondsChanged();
    update();
}

void WaveformView::setSecondsPerPixel(double seconds) {
    if (secondsPerPixel_ == seconds)
        return;
    secondsPerPixel_ = seconds;
    Q_EMIT secondsPerPixelChanged();
    update();
}

void WaveformView::setColor(const QColor& color) {
    if (color_ == color)
        return;
    color_ = color;
    Q_EMIT colorChanged();
    update();
}

void WaveformView::refreshEntry() {
    // mediaPath が空 (video clip の delegate など) の間は decode を要求しない。
    if (cache_ && !mediaPath_.isEmpty())
        entry_ = cache_->request(mediaPath_);
    else
        entry_ = WaveformCache::Entry{};
    Q_EMIT peaksChanged();
    update();
}

void WaveformView::paint(QPainter* painter) {
    const double w = width();
    const double h = height();
    if (w <= 0.0 || h <= 0.0)
        return;
    if (entry_.state == WaveformCache::State::Failed) {
        // 波形が無いことを無音と見分けられるよう、失敗は明示する。
        painter->setPen(QColor(QStringLiteral("#f0b870")));
        QFont font = painter->font();
        font.setPixelSize(10);
        painter->setFont(font);
        painter->drawText(QRectF(4.0, 0.0, w - 8.0, h), Qt::AlignLeft | Qt::AlignBottom,
                          QStringLiteral("波形を取得できません"));
        return;
    }
    const auto peaks = entry_.peaks;
    if (!peaks || peaks->channels <= 0 || !(secondsPerPixel_ > 0.0))
        return;

    // Premiere と同じく、各 channel 行の下端を基線にして振幅の大きさを上へ描く。
    const int channels = peaks->channels;
    const double rowHeight = h / channels;
    QColor guide = color_;
    guide.setAlphaF(0.25);
    painter->setPen(QPen(guide, 1.0));
    for (int channel = 0; channel < channels; ++channel) {
        // 1 pixel の線が行の外へはみ出さないよう、基線は下端の半 pixel 内側に引く。
        const double baseline = rowHeight * (channel + 1) - 0.5;
        painter->drawLine(QPointF(0.0, baseline), QPointF(w, baseline));
    }

    const int columns = static_cast<int>(std::ceil(w));
    std::vector<QLineF> lines;
    lines.reserve(static_cast<std::size_t>(columns) * static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        const double bottom = rowHeight * (channel + 1);
        const double fullHeight = std::max(1.0, rowHeight - 1.0);
        for (int x = 0; x < columns; ++x) {
            const double begin = startSeconds_ + x * secondsPerPixel_;
            const auto column =
                core::waveformColumn(*peaks, channel, begin, begin + secondsPerPixel_);
            if (!column.valid)
                continue;
            const double amplitude = std::max(std::abs(static_cast<double>(column.minimum)),
                                              std::abs(static_cast<double>(column.maximum)));
            // 無音でも基線から 1 pixel は描き、音のある区間と区別しやすくする。
            const double top = bottom - std::max(1.0, std::min(1.0, amplitude) * fullHeight);
            lines.emplace_back(x + 0.5, top, x + 0.5, bottom);
        }
    }
    painter->setPen(QPen(color_, 1.0));
    painter->drawLines(lines.data(), static_cast<int>(lines.size()));
}

} // namespace mvm::app
