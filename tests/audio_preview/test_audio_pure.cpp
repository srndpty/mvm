#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_frame_queue.h"

#include <array>
#include <cmath>
#include <iostream>

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
    check(queue.setGeneration({2}), "generation を前進できる");
    check(queue.push(chunk(1, 6, 2)) == AudioQueuePushResult::RejectedStaleGeneration,
          "旧 generation を拒否する negative test");
    check(queue.push(chunk(3, 6, 2)) == AudioQueuePushResult::RejectedFutureGeneration,
          "future generation を拒否する negative test");
    check(!queue.setGeneration({1}), "generation regression を拒否する negative test");
    const auto queueMetrics = queue.snapshot();
    check(queueMetrics.overflowRejectCount == 1 && queueMetrics.staleGenerationRejectCount == 1 &&
              queueMetrics.futureGenerationRejectCount == 1,
          "reject counter が空振りせず増える");

    AudioMasterClock clock;
    check(clock.start({48000, 1000, 7, 10000000, {2}}), "clock anchor を受理する");
    check(clock.update(1000 + 5000000, 8, {2}), "device absolute position を写像する");
    auto snap = clock.snapshot();
    check(snap.mediaSamplePosition == 72000, "整数写像が 0.5 秒を正しく加える");
    check(!clock.update(999, 9, {2}), "device position regression を拒否する");
    check(!clock.update(1000 + 6000000, 10, {1}), "旧 generation clock を拒否する");
    clock.pause();
    const auto frozen = clock.snapshot().mediaSamplePosition;
    check(!clock.update(1000 + 7000000, 11, {2}) && clock.snapshot().mediaSamplePosition == frozen,
          "pause 中は media clock が凍結する");
    snap = clock.snapshot();
    check(snap.clockRegressionCount == 1 && snap.clockGenerationMismatchCount == 1,
          "clock fail-closed counter が増える");

    if (failures == 0)
        std::cout << "PASS: audio queue/clock contract\n";
    return failures == 0 ? 0 : 1;
}
