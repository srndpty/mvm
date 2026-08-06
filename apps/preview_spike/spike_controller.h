/*
 * mvm Phase 1 / P1 - preview_spike の制御と計測
 *
 * **製品 UI ではない。** P1 の exit criteria を測るための最小アプリである。
 * タイムライン・Project Model・undo/redo・音声・文字・export は無い。
 *
 * GUI thread 上で動く。decode は DecodeWorker が別スレッドで行う。
 */

#ifndef MVM_APPS_PREVIEW_SPIKE_SPIKE_CONTROLLER_H
#define MVM_APPS_PREVIEW_SPIKE_SPIKE_CONTROLLER_H

#include "app/preview/preview_rhi_item.h"
#include "media/gpu_preview/decode_worker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

namespace mvm::app {

// 計測モードの設定。すべて CLI から与える。
struct MeasureConfig {
    bool enabled = false;
    QString mediaPath;
    QString jsonPath;
    QString label;
    int warmupMs = 5000;
    int measureMs = 60000;
    int seekCount = 1000;
    unsigned int seed = 20260806;
    std::vector<long long> markerFrames;
    bool quitWhenDone = true;
};

class SpikeController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString deviceText READ deviceText NOTIFY statusChanged)
    Q_PROPERTY(QString counterText READ counterText NOTIFY statusChanged)
    Q_PROPERTY(qlonglong requestedFrame READ requestedFrame NOTIFY statusChanged)
    Q_PROPERTY(qlonglong displayedFrame READ displayedFrame NOTIFY statusChanged)
    Q_PROPERTY(qlonglong frameCount READ frameCount NOTIFY statusChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY statusChanged)
    Q_PROPERTY(double fps READ fps NOTIFY statusChanged)

public:
    explicit SpikeController(QObject* parent = nullptr);
    ~SpikeController() override;

    void attach(PreviewRhiItem* item);
    void setMeasureConfig(const MeasureConfig& cfg) { measure_ = cfg; }
    // 計測モードの終了コード。0 = 正常。
    int exitCode() const { return exitCode_; }

    QString mediaPath() const { return mediaPath_; }
    QString statusText() const { return status_; }
    QString deviceText() const { return deviceText_; }
    QString counterText() const { return counterText_; }
    qlonglong requestedFrame() const { return requestedFrame_; }
    qlonglong displayedFrame() const;
    qlonglong frameCount() const;
    bool playing() const;
    double fps() const { return fps_; }

public Q_SLOTS:
    void openMedia(const QString& path);
    void play();
    void pause();
    void stepForward();
    void seekTo(qlonglong frame);

Q_SIGNALS:
    void statusChanged();
    void finished();

private:
    void tick();
    void updateDeviceText();

    // --- 計測 ---------------------------------------------------------------
    enum class Phase { Idle, WaitDevice, Warmup, Measure, Markers, Seeks, Done };
    void advanceMeasurement();
    bool runMarkerChecks();
    void runSeekBenchmark();
    bool writeJson();

    PreviewRhiItem* item_ = nullptr;
    std::shared_ptr<gpu::PreviewState> state_;
    std::unique_ptr<gpu::DecodeWorker> worker_;

    QTimer timer_;
    QString mediaPath_;
    QString status_ = QStringLiteral("初期化中");
    QString deviceText_;
    QString counterText_;
    qlonglong requestedFrame_ = 0;
    double fps_ = 0.0;

    // fps 表示用 (UI のみ。判定には使わない)
    QElapsedTimer fpsTimer_;
    long long lastDisplayedCount_ = 0;

    // --- 計測状態 -----------------------------------------------------------
    MeasureConfig measure_;
    Phase phase_ = Phase::Idle;
    // advanceMeasurement への再入防止。marker / seek のフェーズは
    // processEvents を回すので、その間に QTimer が発火しうる。
    bool inPhase_ = false;
    QElapsedTimer phaseTimer_;
    int exitCode_ = 0;

    long long startupLatencyTicks_ = 0;
    double startupLatencyMs_ = -1.0;

    long long measureStartTicks_ = 0;
    double measureElapsedMs_ = 0.0;
    long long displayedAtStart_ = 0;
    long long repeatedAtStart_ = 0;
    long long presentAtStart_ = 0;
    long long decodedAtStart_ = 0;
    long long loopCount_ = 0;
    // 計測区間が終わった時点で確定させる値。
    // marker 検査と seek 計測でも decode は進むため、後から読むと汚れる。
    long long displayedInWindow_ = 0;
    long long repeatedInWindow_ = 0;
    long long presentsInWindow_ = 0;
    long long decodedInWindow_ = 0;

    unsigned long long cpuUserStart_ = 0;
    unsigned long long cpuKernelStart_ = 0;
    unsigned long long cpuUserEnd_ = 0;
    unsigned long long cpuKernelEnd_ = 0;
    unsigned long long qtDevicePointer_ = 0;
    long long workingSetAtEnd_ = 0;
    long long privateUsageAtEnd_ = 0;

    std::vector<gpu::SeekSample> seekSamples_;
    struct MarkerResult {
        long long requested = -1;
        long long displayed = -1;
        long long marker = -1;
        bool syncOk = false;
        QString error;
    };
    std::vector<MarkerResult> markerResults_;
    QStringList errors_;
};

}  // namespace mvm::app

#endif  // MVM_APPS_PREVIEW_SPIKE_SPIKE_CONTROLLER_H
