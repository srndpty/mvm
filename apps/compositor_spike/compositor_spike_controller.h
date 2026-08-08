#ifndef MVM_APPS_COMPOSITOR_SPIKE_CONTROLLER_H
#define MVM_APPS_COMPOSITOR_SPIKE_CONTROLLER_H

#include "app/preview/compositor_rhi_item.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <vector>

namespace mvm::app {

enum class CompositorMode { Playback, Seek, Layout };

struct CompositorSpikeConfig {
    QString sourceA;
    QString sourceB;
    QString metricsPath;
    int warmupSeconds = 5;
    int measureSeconds = 10;
    unsigned int seed = 20260808;
    int seekCount = 64;
    int displayTimeoutMs = 2000;
    CompositorMode mode = CompositorMode::Playback;
    gpu::GpuCompletionBackend completion = gpu::GpuCompletionBackend::Fence;
    QString testFault;
};

class CompositorSpikeController final : public QObject {
    Q_OBJECT
public:
    explicit CompositorSpikeController(CompositorSpikeConfig config, QObject* parent = nullptr);
    void attach(CompositorRhiItem* item);
    int exitCode() const { return exitCode_; }

Q_SIGNALS:
    void finished();

private:
    enum class Phase { WaitDevice, Warmup, Measure, SeekStart, SeekWait, LayoutStart, LayoutWait,
                       ShutdownWait, Done };
    void tick();
    bool startWorkers();
    void startSeek();
    void pollSeek();
    void startLayoutChange();
    void pollLayoutChange();
    void beginShutdown(const QString& reason, bool failure);
    bool writeMetrics();

    CompositorSpikeConfig config_;
    CompositorRhiItem* item_ = nullptr;
    std::shared_ptr<CompositorSpikeState> state_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB_;
    QTimer timer_;
    QElapsedTimer phaseTimer_;
    Phase phase_ = Phase::WaitDevice;
    int exitCode_ = 0;
    long long displayedAtMeasureStart_ = 0;
    long long droppedAtMeasureStart_ = 0;
    long long scheduledAtMeasureStart_ = 0;
    long long clearsAtMeasureStart_ = 0;
    long long presentsAtMeasureStart_ = 0;
    double measureElapsedSeconds_ = 0;
    std::vector<long long> seekTargets_;
    size_t seekIndex_ = 0;
    unsigned long long waitBaseline_ = 0;
    gpu::CompositionDisplayExpectation waitExpectation_;
    QElapsedTimer waitTimer_;
    long long seekRequestStartQpc_ = 0;
    std::vector<double> seekDecodeReadyMs_;
    std::vector<double> seekDisplayedMs_;
    int seekMismatch_ = 0;
    int seekTimeout_ = 0;
    size_t layoutIndex_ = 0;
    int layoutMismatch_ = 0;
    QString shutdownReason_;
};

} // namespace mvm::app
#endif
