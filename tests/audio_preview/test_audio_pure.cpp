#include "core/checked_output_timebase.h"
#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_frame_queue.h"
#include "media/audio_preview/audio_video_scheduler.h"
#include "media/audio_preview/runtime_attribution.h"
#include "media/audio_preview/wasapi_audio_sink.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

using namespace mvm::audio;

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

AudioChunk chunk(std::uint64_t generation, std::int64_t start, std::int64_t count) {
    auto pcm = std::make_shared<std::vector<float>>(
        static_cast<std::size_t>(count) * kInternalChannels, 0.25f);
    return {{1},
            {generation},
            {1},
            start,
            count,
            start,
            {1, kInternalSampleRate},
            kInternalSampleRate,
            kInternalChannels,
            std::move(pcm),
            0};
}
} // namespace

int main() {
    AudioFrameQueue queue({1}, {1}, 10);
    check(queue.push(chunk(1, 0, 6)) == AudioQueuePushResult::Accepted, "正しい chunk を受理する");
    check(queue.push(chunk(1, 6, 5)) == AudioQueuePushResult::RejectedOverflow,
          "overflow を暗黙 drop せず拒否する");

    std::array<float, 8> output{};
    const auto consumed = queue.consume(output.data(), 4, {1});
    check(consumed.audioSamples == 4 && consumed.firstSample == 0 &&
              consumed.lastSampleExclusive == 4,
          "partial consume の sample identity を保つ");
    check(consumed.queuedSamplesBeforeConsume == 6 && consumed.queuedSamplesAfterConsume == 2,
          "consume と同じqueue lock内でbefore/afterを記録する");
    check(consumed.queueLastAvailableSampleExclusive == 6,
          "consume時点のqueue末尾sample exclusiveを記録する");
    check(queue.setGeneration({2}), "generation を前進できる");
    check(queue.push(chunk(1, 6, 2)) == AudioQueuePushResult::RejectedStaleGeneration,
          "旧 generation を拒否する negative test");
    check(queue.push(chunk(3, 6, 2)) == AudioQueuePushResult::RejectedFutureGeneration,
          "future generation を拒否する negative test");
    check(!queue.setGeneration({1}), "generation regression を拒否する negative test");
    const auto emptyGeneration = queue.consume(output.data(), 1, {1});
    check(emptyGeneration.queueLastAvailableSampleExclusive == -1,
          "generation不一致をqueue末尾として報告しない negative test");
    const auto queueMetrics = queue.snapshot();
    check(queueMetrics.overflowRejectCount == 1 && queueMetrics.staleGenerationRejectCount == 1 &&
              queueMetrics.futureGenerationRejectCount == 1,
          "reject counter が空振りせず増える");

    AudioMasterClock clock;
    check(clock.start({48000, 1000, {7}, 10000000, {2}}), "clock anchor を受理する");
    check(clock.update(1000 + 5000000, {8}, {2}), "device absolute position を写像する");
    auto snap = clock.snapshot();
    check(snap.mediaSamplePosition == 72000, "整数写像が 0.5 秒を正しく加える");
    check(!clock.update(999, {9}, {2}), "device position regression を拒否する");
    check(!clock.update(1000 + 6000000, {10}, {1}), "旧 generation clock を拒否する");
    clock.pause();
    const auto frozen = clock.snapshot().mediaSamplePosition;
    check(!clock.update(1000 + 7000000, {11}, {2}) &&
              clock.snapshot().mediaSamplePosition == frozen,
          "pause 中は media clock が凍結する");
    snap = clock.snapshot();
    check(snap.clockRegressionCount == 1 && snap.clockGenerationMismatchCount == 1,
          "clock fail-closed counter が増える");

    Qpc100ns converted;
    check(qpcTicksTo100ns({0}, 10'000'000, converted) && converted.value == 0,
          "QPC tick 0 を 100ns へ変換する");
    check(qpcTicksTo100ns({3'579'545}, 3'579'545, converted) && converted.value == 10'000'000,
          "QPC 1 秒を 100ns へ変換する");
    check(qpcTicksTo100ns({9'000'000'000'000'000ULL}, 10'000'000, converted) &&
              converted.value == 9'000'000'000'000'000ULL,
          "large QPC counter を overflow なしで変換する");
    check(qpcTicksTo100ns({std::numeric_limits<std::uint64_t>::max() - 1},
                          std::numeric_limits<std::uint64_t>::max(), converted) &&
              converted.value == 9'999'999,
          "中間積が uint64 を超える QPC 比率も整数で変換する");
    Qpc100ns roundedA;
    Qpc100ns roundedB;
    check(qpcTicksTo100ns({1}, 3, roundedA) && roundedA.value == 3'333'333 &&
              qpcTicksTo100ns({2}, 3, roundedB) && roundedB.value == 6'666'666 &&
              roundedA.value < roundedB.value,
          "QPC 変換は floor rounding で単調になる");
    check(!qpcTicksTo100ns({1}, 0, converted), "frequency 0 を拒否する negative test");

    AudioClockSnapshot projectionSnapshot;
    projectionSnapshot.deviceFrequency = 10'000'000;
    projectionSnapshot.qpcPosition100ns = {20'000'000};
    projectionSnapshot.mediaSamplePosition = 48'000;
    projectionSnapshot.running = true;
    projectionSnapshot.generation = {4};
    auto projection = projectAtQpc100ns(projectionSnapshot, {25'000'000}, {4});
    check(projection.valid && projection.mediaSample == 72'000 &&
              projection.extrapolatedSamples == 24'000,
          "timestamp observation から render 時刻へ audio sample を射影する");
    projectionSnapshot.running = false;
    check(!projectAtQpc100ns(projectionSnapshot, {25'000'000}, {4}).valid,
          "停止 clock の projection を拒否する negative test");
    projectionSnapshot.running = true;
    check(!projectAtQpc100ns(projectionSnapshot, {25'000'000}, {3}).valid,
          "generation mismatch の projection を拒否する negative test");
    check(!projectAtQpc100ns(projectionSnapshot, {19'999'999}, {4}).valid,
          "observation より過去の projection を拒否する negative test");

    check(scheduleVideoForAudio(0, -1, -1, 100).action == AudioVideoScheduleAction::Request,
          "sample 0 は frame 0 を request する");
    check(scheduleVideoForAudio(799, 0, 0, 100).action == AudioVideoScheduleAction::Hold,
          "同じ frame 区間では hold する");
    const auto catchUp = scheduleVideoForAudio(3 * 800, 0, 0, 100);
    check(catchUp.action == AudioVideoScheduleAction::CatchUp && catchUp.targetFrame == 3 &&
              catchUp.skippedFrames == 2,
          "複数 frame 前進を明示的な catch-up とする");
    const auto pendingSeek = scheduleVideoForAudio(2384 * 800, 2382, -1, 3000, 2383);
    check(pendingSeek.action == AudioVideoScheduleAction::Request &&
              pendingSeek.targetFrame == 2383 && pendingSeek.skippedFrames == 0,
          "seek直後はclockが1 frame進んでもexact targetを先に要求する");
    check(scheduleVideoForAudio(2384 * 800, 2383, 2383, 3000, 2383).action ==
              AudioVideoScheduleAction::Invalid,
          "表示済みseek targetをpendingとして再要求しない");
    check(scheduleVideoForAudio(800, 2, 2, 100).action == AudioVideoScheduleAction::ClockRegression,
          "audio clock regression を fail-closed にする");
    check(scheduleVideoForAudio(100 * 800, 99, 99, 100).action == AudioVideoScheduleAction::End,
          "video end boundary を検出する");
    check(!formalVideoTargetForSample(0, kFormalPlaybackSamples).measurementEnded &&
              formalVideoTargetForSample(0, kFormalPlaybackSamples).frameNumber == 0,
          "formal sample 0 は frame 0 へ写像する");
    check(formalVideoTargetForSample(799, kFormalPlaybackSamples).frameNumber == 0 &&
              formalVideoTargetForSample(800, kFormalPlaybackSamples).frameNumber == 1,
          "formal frame 境界を整数 sample で写像する");
    check(formalVideoTargetForSample(2'879'999, kFormalPlaybackSamples).frameNumber == 3'599,
          "formal 終端直前は frame 3599 へ写像する");
    check(formalVideoTargetForSample(2'880'000, kFormalPlaybackSamples).measurementEnded,
          "formal end sample では測定終了とする negative endpoint test");
    check(scheduleVideoForAudio(-1, -1, -1, 100).action == AudioVideoScheduleAction::Invalid,
          "負の audio sample を拒否する negative test");
    check(!isVideoAheadViolation(2, 800) && isVideoAheadViolation(3, 800),
          "1 frame を超える video ahead を検出する");
    const auto injectedTimebase = mvm::core::CheckedOutputTimebase::create(3, 2, 10);
    check(injectedTimebase &&
              scheduleVideoForAudio(20, 0, 0, 100, -1, injectedTimebase.value()).targetFrame == 3 &&
              formalVideoTargetForSample(9, 100, injectedTimebase.value()).frameNumber == 1 &&
              !isVideoAheadViolation(2, 9, injectedTimebase.value()) &&
              isVideoAheadViolation(3, 9, injectedTimebase.value()),
          "checked timebase の明示注入を scheduler 全経路で使用する");
    check(acceptsVideoMasterSource(VideoMasterSource::AudioDeviceClock) &&
              !acceptsVideoMasterSource(VideoMasterSource::Qpc),
          "QPC master fallback attempt を拒否する negative test");

    FirstFailureCapture<AudioUnderflowFirstSnapshot> firstUnderflow;
    check(!firstUnderflow.snapshot().has_value(), "failure未発生時はsnapshotを公開しない");
    AudioUnderflowFirstSnapshot underflowA;
    underflowA.occurrenceQpc = 100;
    underflowA.requestedSampleStart = 48000;
    underflowA.actuallyConsumedSamples = 128;
    underflowA.queueLastAvailableSampleExclusive = 48128;
    underflowA.endpointBufferFrames = 960;
    underflowA.consumeTraceCount = 1;
    underflowA.consumeTrace[0].requestedSampleStart = 48000;
    AudioUnderflowFirstSnapshot underflowB = underflowA;
    underflowB.occurrenceQpc = 200;
    underflowB.actuallyConsumedSamples = 0;
    check(firstUnderflow.capture(underflowA), "最初のunderflow snapshotをpublishする");
    check(!firstUnderflow.capture(underflowB),
          "2回目のunderflow snapshotによる上書きを拒否する negative test");
    const auto publishedUnderflow = firstUnderflow.snapshot();
    check(publishedUnderflow && publishedUnderflow->occurrenceQpc == 100 &&
              publishedUnderflow->actuallyConsumedSamples == 128 &&
              publishedUnderflow->queueLastAvailableSampleExclusive == 48128 &&
              publishedUnderflow->endpointBufferFrames == 960 &&
              publishedUnderflow->consumeTraceCount == 1 &&
              publishedUnderflow->consumeTrace[0].requestedSampleStart == 48000,
          "最初のunderflow値を保持する");

    FirstFailureCapture<ClockRegressionFirstSnapshot> firstClockRegression;
    ClockRegressionFirstSnapshot clockRegressionA;
    clockRegressionA.site = ClockRegressionSite::SchedulerDecision;
    clockRegressionA.previousFrame = 42;
    ClockRegressionFirstSnapshot clockRegressionB = clockRegressionA;
    clockRegressionB.site = ClockRegressionSite::DisplayProjectionInvalid;
    clockRegressionB.previousFrame = 99;
    check(firstClockRegression.capture(clockRegressionA),
          "最初のclock regression snapshotをpublishする");
    check(!firstClockRegression.capture(clockRegressionB),
          "2回目のclock regression snapshotによる上書きを拒否する negative test");
    const auto publishedClockRegression = firstClockRegression.snapshot();
    check(publishedClockRegression &&
              publishedClockRegression->site == ClockRegressionSite::SchedulerDecision &&
              publishedClockRegression->previousFrame == 42,
          "最初のclock regression siteと値を保持する");

    // endpoint session volume。範囲外の要求はCOM/endpointへ触る前に弾き、
    // 「設定できなかったので全音量で鳴らす」へ縮退させない。
    // 期待値は実装helperを呼ばず、契約 (0.0〜1.0) から直接与える。
    {
        AudioFrameQueue volumeQueue({1}, {1});
        AudioMasterClock volumeClock;
        WasapiAudioSink volumeSink(volumeQueue, volumeClock);
        std::string volumeError;
        check(!volumeSink.open(volumeError, 1.5F) && !volumeError.empty(),
              "1.0超のsession volumeを拒否する negative test");
        volumeError.clear();
        check(!volumeSink.open(volumeError, -0.1F) && !volumeError.empty(),
              "負のsession volumeを拒否する negative test");
        volumeError.clear();
        check(!volumeSink.open(volumeError, std::numeric_limits<float>::quiet_NaN()) &&
                  !volumeError.empty(),
              "NaNのsession volumeを拒否する negative test");
        check(kVerificationSessionVolume > 0.0F && kVerificationSessionVolume < 1.0F,
              "検証用session volumeがunity未満の有効値である");
        check(volumeSink.snapshot().sessionVolume == 1.0F,
              "open失敗時にsession volumeを適用済みとして記録しない negative test");
    }

    if (failures == 0)
        std::cout << "PASS: audio queue/clock contract\n";
    return failures == 0 ? 0 : 1;
}
