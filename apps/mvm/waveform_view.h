#ifndef MVM_APPS_MVM_WAVEFORM_VIEW_H
#define MVM_APPS_MVM_WAVEFORM_VIEW_H

#include "waveform_cache.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>

namespace mvm::app {

// audio clip 上の波形。channel ごとに 1 行 (mono 1 行、stereo 2 行) で描く。
// 3ch 以上の素材は decoder が stereo へ downmix するので、最大 2 行。
//
// 長い clip を高倍率で表示すると clip 幅は数十万 pixel になり得る。painted item の
// texture をその大きさで作らないよう、QML 側で viewport と重なる範囲だけに
// 配置し、左端の素材時刻 (startSeconds) と 1 pixel あたりの秒数を渡す。
class WaveformView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject* cache READ cache WRITE setCache NOTIFY cacheChanged)
    Q_PROPERTY(QString mediaPath READ mediaPath WRITE setMediaPath NOTIFY mediaPathChanged)
    Q_PROPERTY(double startSeconds READ startSeconds WRITE setStartSeconds NOTIFY
                   startSecondsChanged)
    Q_PROPERTY(double secondsPerPixel READ secondsPerPixel WRITE setSecondsPerPixel NOTIFY
                   secondsPerPixelChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY peaksChanged)

public:
    explicit WaveformView(QQuickItem* parent = nullptr);

    QObject* cache() const { return cache_; }
    void setCache(QObject* cache);
    QString mediaPath() const { return mediaPath_; }
    void setMediaPath(const QString& mediaPath);
    double startSeconds() const { return startSeconds_; }
    void setStartSeconds(double seconds);
    double secondsPerPixel() const { return secondsPerPixel_; }
    void setSecondsPerPixel(double seconds);
    QColor color() const { return color_; }
    void setColor(const QColor& color);
    int channelCount() const { return entry_.peaks ? entry_.peaks->channels : 0; }

    void paint(QPainter* painter) override;

Q_SIGNALS:
    void cacheChanged();
    void mediaPathChanged();
    void startSecondsChanged();
    void secondsPerPixelChanged();
    void colorChanged();
    void peaksChanged();

private:
    void refreshEntry();

    QPointer<WaveformCache> cache_;
    QString mediaPath_;
    double startSeconds_ = 0.0;
    double secondsPerPixel_ = 0.0;
    QColor color_{QStringLiteral("#7fd49a")};
    WaveformCache::Entry entry_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_WAVEFORM_VIEW_H
