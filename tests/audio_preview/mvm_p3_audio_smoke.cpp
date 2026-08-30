#include "media/audio_preview/audio_clock.h"
#include "media/audio_preview/audio_decode_worker.h"
#include "media/audio_preview/wasapi_audio_sink.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace mvm::audio;

namespace {

struct Args {
    std::string mode;
    std::string source;
    std::string output;
    int durationSeconds = 15;
    int seekCount = 64;
    unsigned int seed = 20260808;
};

std::int64_t qpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t qpcFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index =
        static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

bool parse(int argc, char** argv, Args& args) {
    if (argc < 4)
        return false;
    args.mode = argv[1];
    args.source = argv[2];
    args.output = argv[3];
    for (int i = 4; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 >= argc)
            return false;
        const std::string value = argv[++i];
        try {
            if (key == "--duration-seconds")
                args.durationSeconds = std::stoi(value);
            else if (key == "--seek-count")
                args.seekCount = std::stoi(value);
            else if (key == "--seed")
                args.seed = static_cast<unsigned int>(std::stoul(value));
            else
                return false;
        } catch (...) {
            return false;
        }
    }
    return (args.mode == "playback" || args.mode == "seek" || args.mode == "pause-resume" ||
            args.mode == "fixture") &&
           args.durationSeconds > 0 && args.seekCount > 0;
}

void writeJson(const Args& args, const AudioDecoderSnapshot& decoder,
               const AudioQueueSnapshot& queue, const WasapiSnapshot& sink,
               const AudioClockSnapshot& clock, bool pass, const std::string& detail,
               double elapsed, const std::vector<double>& drift, int exactCount, int seekTimeouts,
               double seekP95, const std::vector<AudioSeekCompletion>& seekCompletions,
               bool pauseFrozen, bool pauseContinuous, int markerMatches) {
    std::ofstream out(args.output, std::ios::binary);
    const double driftMax =
        drift.empty() ? 0.0 : *std::max_element(drift.begin(), drift.end(), [](double a, double b) {
            return std::abs(a) < std::abs(b);
        });
    out << std::fixed << std::setprecision(6) << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"phase\": \"P3-A\",\n"
        << "  \"mode\": \"" << args.mode << "\",\n"
        << "  \"pass\": " << (pass ? "true" : "false") << ",\n"
        << "  \"detail\": \"" << detail << "\",\n"
        << "  \"source_sample_rate\": " << decoder.sourceFormat.sampleRate << ",\n"
        << "  \"source_channels\": " << decoder.sourceFormat.channels << ",\n"
        << "  \"source_sample_format\": \"" << decoder.sourceFormat.sampleFormat << "\",\n"
        << "  \"device_sample_rate\": " << sink.deviceFormat.sampleRate << ",\n"
        << "  \"device_channels\": " << sink.deviceFormat.channels << ",\n"
        << "  \"device_sample_format\": \"" << sink.deviceFormat.sampleFormat << "\",\n"
        << "  \"queue_target_ms\": " << kQueueTargetMs << ",\n"
        << "  \"queue_hard_max_ms\": " << kQueueHardMaxMs << ",\n"
        << "  \"audio_preroll_ms\": " << kAudioPrerollMs << ",\n"
        << "  \"elapsed_seconds\": " << elapsed << ",\n"
        << "  \"audio_rendered_samples\": " << sink.audioRenderedSamples << ",\n"
        << "  \"endpoint_prefill_frames\": " << sink.endpointPrefillFrames << ",\n"
        << "  \"endpoint_first_media_sample\": " << sink.endpointFirstMediaSample << ",\n"
        << "  \"endpoint_start_device_position\": " << sink.endpointStartDevicePosition << ",\n"
        << "  \"clock_anchor_media_sample\": " << sink.clockAnchorMediaSample << ",\n"
        << "  \"clock_anchor_device_position\": " << sink.clockAnchorDevicePosition << ",\n"
        << "  \"expected_elapsed_samples\": " << std::llround(elapsed * kInternalSampleRate)
        << ",\n"
        << "  \"queued_duration_ms\": " << queue.queuedDurationMs << ",\n"
        << "  \"high_watermark_ms\": " << queue.highWatermarkMs << ",\n"
        << "  \"underflow_count\": " << queue.underflowCount << ",\n"
        << "  \"underflow_samples\": " << queue.underflowSamples << ",\n"
        << "  \"overflow_reject_count\": " << queue.overflowRejectCount << ",\n"
        << "  \"stale_generation_reject_count\": " << queue.staleGenerationRejectCount << ",\n"
        << "  \"clock_regression_count\": " << clock.clockRegressionCount << ",\n"
        << "  \"clock_generation_mismatch_count\": " << clock.clockGenerationMismatchCount << ",\n"
        << "  \"clock_query_failure_count\": " << clock.clockQueryFailureCount << ",\n"
        << "  \"drift_p50_ms\": " << percentile(drift, 0.50) << ",\n"
        << "  \"drift_p95_ms\": " << percentile(drift, 0.95) << ",\n"
        << "  \"drift_max_absolute_ms\": " << std::abs(driftMax) << ",\n"
        << "  \"exact_seek_count\": " << exactCount << ",\n"
        << "  \"seek_requested_count\": " << (args.mode == "seek" ? args.seekCount : 0) << ",\n"
        << "  \"seek_timeout_count\": " << seekTimeouts << ",\n"
        << "  \"seek_latency_p95_ms\": " << seekP95 << ",\n"
        << "  \"seeks\": [";
    for (std::size_t index = 0; index < seekCompletions.size(); ++index) {
        const auto& seek = seekCompletions[index];
        out << (index == 0 ? "\n" : ",\n") << "    {\"requested_sample\": " << seek.requestedSample
            << ", \"first_output_sample\": " << seek.firstOutputSample
            << ", \"seek_generation\": " << seek.seekGeneration.value
            << ", \"discarded_preroll_samples\": " << seek.discardedPrerollSamples
            << ", \"latency_ms\": " << seek.latencyMs << "}";
    }
    out << (seekCompletions.empty() ? "]" : "\n  ]") << ",\n"
        << "  \"pause_clock_frozen\": " << (pauseFrozen ? "true" : "false") << ",\n"
        << "  \"pause_resume_continuous\": " << (pauseContinuous ? "true" : "false") << ",\n"
        << "  \"audio_marker_matches\": " << markerMatches << ",\n"
        << "  \"device_failure_count\": " << sink.deviceFailureCount << ",\n"
        << "  \"audio_render_thread_join_leak\": " << sink.audioRenderThreadJoinLeak << ",\n"
        << "  \"audio_decode_thread_join_leak\": " << decoder.audioDecodeThreadJoinLeak << ",\n"
        << "  \"audio_device_release_before_join\": " << sink.audioDeviceReleaseBeforeJoin << ",\n"
        << "  \"audio_lifecycle_violation\": " << sink.audioLifecycleViolation << "\n"
        << "}\n";
}

bool startPipeline(const Args& args, AudioDecodeWorker& worker, AudioMasterClock& clock,
                   WasapiAudioSink& sink, std::string& error) {
    if (!worker.start(args.source, error))
        return false;
    if (!sink.open(error, kVerificationSessionVolume))
        return false;
    worker.play();
    return sink.play(0, worker.snapshot().sourceGeneration, error);
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        std::cerr << "使い方: mvm_p3_audio_smoke <playback|seek|pause-resume|fixture> "
                     "<source> <output.json> [--duration-seconds N] [--seek-count N] [--seed N]\n";
        return 2;
    }

    AudioDecodeWorker worker({1});
    AudioMasterClock clock;
    WasapiAudioSink sink(worker.queue(), clock);
    std::string error;
    bool pass = args.mode == "fixture" ? worker.start(args.source, error)
                                       : startPipeline(args, worker, clock, sink, error);
    const std::int64_t frequency = qpcFrequency();
    const std::int64_t begin = qpcNow();
    const std::int64_t startMedia = clock.snapshot().mediaSamplePosition;
    std::vector<double> drift;
    std::vector<double> seekLatencies;
    std::vector<AudioSeekCompletion> seekCompletions;
    int exactCount = 0;
    int seekTimeouts = 0;
    bool pauseFrozen = false;
    bool pauseContinuous = false;
    int markerMatches = 0;

    if (pass && args.mode == "playback") {
        while (static_cast<double>(qpcNow() - begin) / frequency < args.durationSeconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double qpcElapsed = static_cast<double>(qpcNow() - begin) / frequency;
            const auto sample = clock.snapshot();
            const double audioElapsed =
                static_cast<double>(sample.mediaSamplePosition - startMedia) / kInternalSampleRate;
            drift.push_back((audioElapsed - qpcElapsed) * 1000.0);
        }
    } else if (pass && args.mode == "pause-resume") {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        worker.pause();
        pass = sink.pause(error);
        const auto beforePause = clock.snapshot();
        const auto consumedBefore = sink.snapshot().lastConsumedSampleExclusive;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        pauseFrozen = clock.snapshot().mediaSamplePosition == beforePause.mediaSamplePosition;
        worker.play();
        if (pass)
            pass = sink.play(beforePause.mediaSamplePosition, worker.snapshot().sourceGeneration,
                             error);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const auto after = sink.snapshot();
        pauseContinuous = after.playStartFirstConsumedSample == consumedBefore;
        pass = pass && pauseFrozen && pauseContinuous;
    } else if (pass && args.mode == "seek") {
        std::mt19937 random(args.seed);
        std::uniform_int_distribution<std::int64_t> targets(0, 59LL * kInternalSampleRate);
        for (int index = 0; index < args.seekCount && pass; ++index) {
            worker.pause();
            if (!sink.pause(error) || !sink.resetForSeek(error)) {
                pass = false;
                break;
            }
            AudioSeekTicket ticket;
            const std::int64_t target = targets(random);
            if (worker.requestSeek(target, ticket, error) != AudioSeekRequestResult::Accepted) {
                pass = false;
                error = "audio seek request が拒否されました";
                break;
            }
            AudioSeekCompletion completion;
            const auto wait = worker.waitSeek(ticket, 5000, completion);
            if (wait == AudioSeekWaitResult::Timeout) {
                ++seekTimeouts;
                pass = false;
                break;
            }
            if (wait != AudioSeekWaitResult::Ready || !completion.completed ||
                completion.firstOutputSample != target) {
                pass = false;
                error =
                    completion.error.empty() ? "exact audio seek が不一致です" : completion.error;
                break;
            }
            ++exactCount;
            seekLatencies.push_back(completion.latencyMs);
            seekCompletions.push_back(completion);
            worker.play();
            if (!sink.play(target, completion.seekGeneration, error)) {
                pass = false;
                break;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (sink.snapshot().playStartFirstConsumedSample < 0 &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (sink.snapshot().playStartFirstConsumedSample != target) {
                pass = false;
                error = "renderer の first consumed sample が seek target と一致しません";
            }
        }
        pass = pass && exactCount == args.seekCount && seekTimeouts == 0;
    } else if (pass && args.mode == "fixture") {
        // 全 PCM は保存せず、固定 marker 先頭の 10 ms window だけを検査する。
        const std::int64_t markerSeconds[] = {0, 1, 5, 10, 30, 59};
        worker.pause();
        for (std::int64_t second : markerSeconds) {
            AudioSeekTicket ticket;
            const auto target = second * kInternalSampleRate;
            if (worker.requestSeek(target, ticket, error) != AudioSeekRequestResult::Accepted) {
                pass = false;
                break;
            }
            AudioSeekCompletion completion;
            if (worker.waitSeek(ticket, 5000, completion) != AudioSeekWaitResult::Ready ||
                !completion.completed) {
                pass = false;
                break;
            }
            std::vector<float> window(480 * kInternalChannels);
            const auto consumed =
                worker.queue().consume(window.data(), target, 480, completion.seekGeneration);
            float peak = 0.0f;
            for (float value : window)
                peak = std::max(peak, std::abs(value));
            if (consumed.firstSample != target || peak < 0.15f) {
                pass = false;
                error = "audio marker window が期待 pulse と一致しません";
                break;
            }
            ++markerMatches;
        }
        pass = pass && markerMatches == 6;
    }

    const double elapsed = static_cast<double>(qpcNow() - begin) / frequency;
    sink.stop();
    worker.stop();
    const auto decoderMetrics = worker.snapshot();
    const auto queueMetrics = worker.queue().snapshot();
    const auto sinkMetrics = sink.snapshot();
    const auto clockMetrics = clock.snapshot();
    pass =
        pass && decoderMetrics.joined && sinkMetrics.joined &&
        sinkMetrics.deviceFailureCount == 0 && clockMetrics.clockRegressionCount == 0 &&
        clockMetrics.clockGenerationMismatchCount == 0 &&
        clockMetrics.clockQueryFailureCount == 0 && queueMetrics.staleGenerationRejectCount == 0 &&
        sinkMetrics.audioRenderThreadJoinLeak == 0 &&
        decoderMetrics.audioDecodeThreadJoinLeak == 0 &&
        sinkMetrics.audioDeviceReleaseBeforeJoin == 0 && sinkMetrics.audioLifecycleViolation == 0;
    writeJson(args, decoderMetrics, queueMetrics, sinkMetrics, clockMetrics, pass, error, elapsed,
              drift, exactCount, seekTimeouts, percentile(seekLatencies, 0.95), seekCompletions,
              pauseFrozen, pauseContinuous, markerMatches);
    if (!pass)
        std::cerr << "FAIL: " << error << '\n';
    return pass ? 0 : 3;
}
