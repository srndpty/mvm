#ifndef MVM_APPS_AUDIO_ADJUSTMENT_JOB_H
#define MVM_APPS_AUDIO_ADJUSTMENT_JOB_H
#include "media/audio_analysis/loudness_meter.h"
#include "project/audio_adjustment.h"
#include "project/project.h"

#include <atomic>
#include <future>

namespace mvm::app {
struct AudioAdjustmentClipResult {
    std::string clipId;
    std::string name;
    audio::LoudnessMeasurement measurement;
    double correctionDb = 0;
    bool peakLimited = false;
    bool gainLimited = false;
};

struct AudioAdjustmentResult {
    bool success = false;
    bool cancelled = false;
    std::string error;
    project::Project candidate;
    std::vector<AudioAdjustmentClipResult> clips;
    std::vector<project::AudioDetectedRange> ranges;
};

AudioAdjustmentResult analyzeAudioAdjustment(project::Project source,
                                             const project::AudioAdjustmentSettings& settings,
                                             const std::atomic<bool>& running,
                                             std::atomic<int>& progress);

// GUI は完了を poll するだけ。worker の寿命と取消はこのクラスが所有する。
class AudioAdjustmentJob final {
public:
    AudioAdjustmentJob(project::Project source, project::AudioAdjustmentSettings settings);
    ~AudioAdjustmentJob();

    void cancel() { running_ = false; }

    bool ready() const;

    int progress() const { return progress_.load(); }

    AudioAdjustmentResult take();

private:
    std::atomic<bool> running_{true};
    std::atomic<int> progress_{0};
    std::future<AudioAdjustmentResult> future_;
};
} // namespace mvm::app
#endif
