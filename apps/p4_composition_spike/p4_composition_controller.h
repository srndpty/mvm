#ifndef MVM_APPS_P4_COMPOSITION_CONTROLLER_H
#define MVM_APPS_P4_COMPOSITION_CONTROLLER_H

#include "app/preview/compositor_rhi_item.h"
#include "app/preview/display_target_contract.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "media/gpu_preview/phase4_composition_catalog.h"
#include "media/gpu_preview/shutdown_sequence.h"
#include "media/gpu_preview/transition_probe_reference.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <optional>

namespace mvm::app {

struct P4Config {
    QString sourceA;
    QString sourceB;
    QString metricsPath;
    gpu::Phase4ScheduleKind workload = gpu::Phase4ScheduleKind::Smoke;
    int durationSeconds = 10;
    int warmupSeconds = 1;
    int displayTimeoutMs = 3000;
};

// Phase 4 / C のcontract smoke raw producer。
//
// Phase 3 の integrated playback path (SourceDecodeWorker x2 / AudioDecodeWorker /
// WasapiAudioSink / IAudioClock master / CompositorRhiItem) をそのまま使う。
// 新しい decode / audio / render architecture は作らない。
//
// **最終PASSは独立checkerだけが判定する。** rawのformal_verdictと
// smoke_contract_verdictは常にNOT_RUNであり、producer集計をauthorityにしない。
class P4CompositionController final : public QObject {
    Q_OBJECT
public:
    explicit P4CompositionController(P4Config config, QObject* parent = nullptr);
    void attach(CompositorRhiItem* item);
    int exitCode() const { return exitCode_; }

Q_SIGNALS:
    void finished();

private:
    enum class Phase {
        WaitDevice,
        DisplayPreflight,
        Start,
        WaitWarmupDisplay,
        Warmup,
        WaitMeasurementDisplay,
        Measure,
        ShutdownWait,
        Done
    };

    struct MeasurementBaseline {
        long long pairWait = 0;
        long long targetSuperseded = 0;
        long long staleA = 0;
        long long staleB = 0;
        long long underflow = 0;
        long long overflow = 0;
        long long markerAMismatch = 0;
        long long markerBMismatch = 0;
        long long mixedPair = 0;
        long long mixedGeneration = 0;
        long long staleEpoch = 0;
        long long ahead = 0;
        long long clockRegression = 0;
        long long qpcFallback = 0;
        long long audioClockQueryFailure = 0;
    };

    void tick();
    bool validateSchedule();
    bool openPipelines();
    bool prepareCpuReferences();
    bool pollTransitionProbes();
    bool startAtFrameZero(bool measurementStart);
    bool pollFirstDisplay(bool measurementStart);
    DisplayEnvironmentSnapshot captureDisplayEnvironment() const;
    void startShutdown(const QString& reason, bool failure);
    bool writeMetrics() const;

    P4Config config_;
    CompositorRhiItem* item_ = nullptr;
    std::shared_ptr<CompositorSpikeState> state_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB_;
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker_;
    std::shared_ptr<audio::AudioMasterClock> audioClock_;
    std::shared_ptr<audio::WasapiAudioSink> audioSink_;
    QTimer timer_;
    QElapsedTimer phaseTimer_;
    Phase phase_ = Phase::WaitDevice;
    int exitCode_ = 0;
    QString shutdownReason_;

    QString canonicalSchedule_;
    QString canonicalScheduleSha256_;
    std::vector<gpu::CompositionScheduleEntry> scheduleEntries_;
    std::vector<long long> transitionBoundaries_;
    std::optional<gpu::CompositionSchedule> schedule_;

    unsigned long long measurementLedgerBaseline_ = 0;
    gpu::CompositionDisplayExpectation displayExpectation_;
    gpu::CompositionEpoch measurementBaselineEpoch_{};
    gpu::SourceGeneration baselineGenerationA_{};
    gpu::SourceGeneration baselineGenerationB_{};
    gpu::ResourceEpoch baselineResourceEpochA_{};
    gpu::ResourceEpoch baselineResourceEpochB_{};
    MeasurementBaseline measurementBaseline_;
    long long observedMeasurementEndSample_ = -1;
    bool coordinatorConfigured_ = false;
    bool warmupComplete_ = false;
    bool firstMeasurementDisplaySeen_ = false;
    DisplayEnvironmentSnapshot displayEnvironmentStart_;
    DisplayEnvironmentSnapshot displayEnvironmentEnd_;
    bool displayPreflightPassed_ = false;
    gpu::ShutdownSequenceResult shutdownSequence_;
    gpu::Phase4CpuReferenceSet cpuReferences_;
};

} // namespace mvm::app
#endif
