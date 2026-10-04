#ifndef MVM_APPS_AUDIO_ADJUSTMENT_JOB_H
#define MVM_APPS_AUDIO_ADJUSTMENT_JOB_H
#include "media/audio_analysis/loudness_meter.h"
#include "project/audio_adjustment.h"
#include "project/project.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace mvm::app {
// 解析した PCM の出自。size と 100ns の更新時刻に加え、内容の SHA-256 を authority にする。
struct AudioFileIdentity {
    std::string key;
    std::uint64_t size = 0;
    std::uint64_t mtime100ns = 0;
    std::string contentSha256;
    bool exists = false;
    bool hashed = false;
};

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
    std::string projectionHash;
    std::vector<AudioFileIdentity> files;
    std::string fingerprintText;
    project::AudioAdjustmentSettings settings;
};

// hashContent が false のときは size と更新時刻だけを読む。running が false なら内容 hash
// を中断する。
bool inspectAudioFile(const std::filesystem::path& path, AudioFileIdentity& identity,
                      bool hashContent, const std::atomic<bool>* running, std::string& error);
// inspectAudioFile が identity の key にする正規化した絶対 path。
std::string audioFileKey(const std::filesystem::path& path);
// key が同じ path を 1 件にまとめ、key の順に並べる。
std::vector<std::filesystem::path> uniqueAudioFiles(std::vector<std::filesystem::path> paths);
std::string audioProjectionHash(const project::Project& project);
std::string audioProjectionHash(const project::Project& project,
                                const project::AudioAdjustmentSettings& settings);
std::string formatAudioInputFingerprint(const std::string& projectionHash,
                                        const std::vector<AudioFileIdentity>& files);
bool parseAudioInputFingerprint(const std::string& text, std::string& projectionHash,
                                std::vector<AudioFileIdentity>& files);

// decode / open の前で worker を止める試験用。nullptr で無効。待っている間は cancel を見ない。
void setAudioAdjustmentOpenGateForTest(std::atomic<bool>* gate);
int audioAdjustmentOpenGateWaitersForTest();
int audioContentHashJobsStartedForTest();
// 内容 SHA-256 を始めた回数と、保護 handle を開いた回数 (素材単位であることの試験用)。
int audioFullHashesStartedForTest();
int audioFileLocksOpenedForTest();
// 対象 track を指定した射影 hash を計算した回数 (全 clip を走査する処理の回数の試験用)。
int audioProjectionHashesForTest();

struct AudioContentHashResult {
    enum class Status { Complete, Cancelled, Missing, ReadError };
    Status status = Status::Cancelled;
    std::vector<AudioFileIdentity> files;
    std::shared_ptr<void> locks;
};

// 内容 hash は GUI を止めない。ready になる前に破棄しない。
class AudioContentHashJob final {
public:
    explicit AudioContentHashJob(std::vector<std::filesystem::path> paths, bool lockFiles = false);
    ~AudioContentHashJob();

    void cancel() { running_ = false; }

    bool ready() const;

    AudioContentHashResult take();

private:
    std::atomic<bool> running_{true};
    std::future<AudioContentHashResult> future_;
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
