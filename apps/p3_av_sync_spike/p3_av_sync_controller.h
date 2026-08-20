#ifndef MVM_APPS_P3_AV_SYNC_CONTROLLER_H
#define MVM_APPS_P3_AV_SYNC_CONTROLLER_H

#include "app/preview/compositor_rhi_item.h"
#include "app/preview/display_target_contract.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/wasapi_audio_sink.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

namespace mvm::app {

enum class P3AvMode { Playback, Seek, PauseResume };

struct P3AvConfig {
    QString sourceA;
    QString sourceB;
    QString metricsPath;
    P3AvMode mode = P3AvMode::Playback;
    int durationSeconds = 15;
    int seekCount = 64;
    unsigned int seed = 20260808;
    int displayTimeoutMs = 3000;
    bool formalContract = false;
    bool formalContractC2 = false;
    int warmupSeconds = 5;
};

class P3AvSyncController final : public QObject {
    Q_OBJECT
public:
    explicit P3AvSyncController(P3AvConfig config, QObject* parent = nullptr);
    void attach(CompositorRhiItem* item);
    int exitCode() const { return exitCode_; }

Q_SIGNALS:
    void finished();

private:
    enum class Phase { WaitDevice, DisplayPreflight, Start, WaitDisplay, Warmup, Playback, PauseStart, PauseWait,
                       ResumePlayback, ShutdownWait, Done };
    struct SeekRecord {
        long long requestedFrame = -1;
        long long requestedAudioSample = -1;
        unsigned long long audioGeneration = 0;
        unsigned long long videoGenerationA = 0;
        unsigned long long videoGenerationB = 0;
        long long firstAudioSample = -1;
        long long firstDisplayedVideoFrame = -1;
        double audioSeekReadyMs = -1;
        double videoAReadyMs = -1;
        double videoBReadyMs = -1;
        double allMediaReadyMs = -1;
        double resumeToFirstVideoMs = -1;
        double requestToFirstVideoMs = -1;
        bool firstDisplayApplicationAvProjectionValid = false;
        double firstDisplayApplicationAvDeltaMs = 0.0;
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
    bool openPipelines();
    bool startAtFrame(long long targetFrame, bool measurementStart = false);
    bool pollFirstDisplay();
    void captureSeekTimeoutStageEvidence();
    DisplayEnvironmentSnapshot captureDisplayEnvironment() const;
    void setPhase(Phase phase);
    void startShutdown(const QString& reason, bool failure);
    bool writeMetrics() const;

    P3AvConfig config_;
    CompositorRhiItem* item_ = nullptr;
    std::shared_ptr<CompositorSpikeState> state_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerA_;
    std::shared_ptr<gpu::SourceDecodeWorker> workerB_;
    std::shared_ptr<audio::AudioDecodeWorker> audioWorker_;
    std::shared_ptr<audio::AudioMasterClock> audioClock_;
    std::shared_ptr<audio::WasapiAudioSink> audioSink_;
    QTimer timer_;
    QElapsedTimer phaseTimer_;
    QElapsedTimer runTimer_;
    Phase phase_ = Phase::WaitDevice;
    int exitCode_ = 0;
    QString shutdownReason_;
    std::vector<long long> seekTargets_;
    std::vector<SeekRecord> seeks_;
    size_t seekIndex_ = 0;
    unsigned long long displayBaseline_ = 0;
    gpu::CompositionDisplayExpectation displayExpectation_;
    long long requestStartQpc_ = 0;
    long long audioStartQpc_ = 0;
    long long pauseAudioSample_ = -1;
    long long pauseVideoFrame_ = -1;
    bool pauseFrozen_ = false;
    bool pauseVideoAdvanceZero_ = false;
    bool pauseGenerationStable_ = false;
    bool coordinatorConfigured_ = false;
    bool warmupResetComplete_ = false;
    MeasurementBaseline measurementBaseline_;
    long long observedMeasurementEndSample_ = -1;
    long long pauseDisplayCount_ = -1;
    long long pauseGeneration_ = -1;
    long long pauseQpcFallback_ = -1;
    long long seekTimeoutCount_ = 0;
    long long seekBusyAcceptanceCount_ = 0;
    long long seekGenerationMismatchCount_ = 0;
    long long seekStaleCompletionCount_ = 0;
    QString seekTimeoutDiagnostic_;
    QJsonObject seekTimeoutStageEvidence_;
    long long seekStaleABaseline_ = 0;
    long long seekStaleBBaseline_ = 0;
    long long seekPairWaitBaseline_ = 0;
    DisplayEnvironmentSnapshot displayEnvironmentStart_;
    DisplayEnvironmentSnapshot displayEnvironmentEnd_;
    bool displayPreflightPassed_ = false;
    bool formalWorkloadStarted_ = false;
};

} // namespace mvm::app
#endif
