#include "audio_adjustment_job.h"

#include "clip_sample_reader.h"
#include "core/checked_output_timebase.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace mvm::app {
AudioAdjustmentResult analyzeAudioAdjustment(project::Project source,
                                             const project::AudioAdjustmentSettings& settings,
                                             const std::atomic<bool>& running,
                                             std::atomic<int>& progress) {
    AudioAdjustmentResult combined;
    if (!project::validateAudioAdjustmentSettings(
            settings, static_cast<int>(source.audioTracks.size()), combined.error))
        return combined;
    combined.candidate = source;
    // 解析対象は明示指定。mute / solo と前回の自動調整を測定入力から除く。
    for (auto& track : source.audioTracks) {
        track.muted = false;
        track.solo = false;
    }
    for (auto& clip : source.timelineClips) {
        clip.effects.normalizationGainDb = 0;
        clip.effects.duckingDb = 0;
        clip.effects.duckingKeys.clear();
    }
    ShuttleAudioPlan plan;
    if (!planShuttleAudio(source, 1, 0, plan, combined.error))
        return combined;
    std::erase_if(plan.clips, [&](const auto& clip) {
        const int track = clip.clip.track.index;
        return track != settings.bgmTrack &&
               std::find(settings.voiceTracks.begin(), settings.voiceTracks.end(), track) ==
                   settings.voiceTracks.end();
    });
    if (plan.clips.empty() ||
        std::none_of(plan.clips.begin(), plan.clips.end(), [&](const auto& clip) {
            return clip.clip.track.index == settings.bgmTrack;
        })) {
        combined.error = "BGM トラックに解析できる音声クリップがありません";
        return combined;
    }
    if (settings.duck && std::none_of(plan.clips.begin(), plan.clips.end(), [&](const auto& clip) {
            return clip.clip.track.index != settings.bgmTrack;
        })) {
        combined.error = "声トラックに解析できる音声クリップがありません";
        return combined;
    }
    const auto timebase =
        core::CheckedOutputTimebase::create(plan.timelineFpsNum, plan.timelineFpsDen, 48000);
    if (!timebase) {
        combined.error = "解析する音声の時間軸が不正です";
        return combined;
    }
    std::int64_t total = 0;
    std::atomic<std::int64_t> completed{0};
    for (const auto& clip : plan.clips)
        total += clip.timelineEndSample - clip.timelineStartSample;
    auto* candidate = &combined.candidate;
    std::atomic<bool> failed{false};
    const auto analyzeClip = [&](std::size_t index) {
        AudioAdjustmentResult result;
        // 各 worker は自分の decoder・測定器・PCM だけを保持する。
        ClipSampleReaders reader;
        reader.reset(1, 48000);
        const auto& clip = plan.clips[index];
        auto target = std::find_if(candidate->timelineClips.begin(),
                                   candidate->timelineClips.end(),
                                   [&](const auto& item) { return item.id == clip.clip.id; });
        if (target == candidate->timelineClips.end()) {
            result.error = "解析したクリップがありません";
            return result;
        }
        audio::LoudnessMeter meter;
        if (!meter.start(result.error))
            return result;

        struct Window {
            std::int64_t start;
            std::int64_t end;
            double rms;
        };

        std::vector<Window> windows;
        const bool voice = clip.clip.track.index != settings.bgmTrack;
        const auto pan = project::audioMixGains(0, clip.segment.mixerPan);
        std::vector<float> pcm;
        // PCM は 200 ms ずつ渡し、検出窓は従来どおり 20 ms に分ける。
        constexpr std::int64_t blockSamples = 9600;
        for (auto start = clip.timelineStartSample; start < clip.timelineEndSample;
             start += blockSamples) {
            if (!running || failed) {
                result.cancelled = !running;
                return result;
            }
            const auto count = std::min(blockSamples, clip.timelineEndSample - start);
            if (!reader.read(clip, 0, start + clip.sourceOffset, count, pcm, running,
                             result.error)) {
                result.cancelled = !running;
                if (result.error.empty() && !result.cancelled)
                    result.error = "解析する音声を読み出せません";
                return result;
            }
            // 同一 frame のゲインは一定。共通評価を frame 境界でだけ呼び出す。
            for (std::int64_t sample = 0; sample < count;) {
                const auto frame = timebase.value().schedulerOutputFrame(start + sample);
                const auto gain =
                    frame ? project::renderSegmentGain(clip.segment, plan.timelineFpsNum,
                                                       plan.timelineFpsDen, frame.value())
                          : std::nullopt;
                if (!gain) {
                    result.error = "解析する音声のゲインを評価できません";
                    return result;
                }
                const auto boundary = timebase.value().firstAudioSample(frame.value() + 1);
                if (!boundary || boundary.value() <= start + sample) {
                    result.error = "解析する音声の frame 境界を評価できません";
                    return result;
                }
                const auto end = std::min(count, boundary.value() - start);
                const float left = static_cast<float>(*gain * pan.first);
                const float right = static_cast<float>(*gain * pan.second);
                for (; sample < end; ++sample) {
                    const auto at = static_cast<std::size_t>(sample) * 2;
                    pcm[at] *= left;
                    pcm[at + 1] *= right;
                }
            }
            if (!meter.push(pcm.data(), static_cast<std::size_t>(count), result.error))
                return result;
            if (voice && settings.duck)
                for (std::int64_t first = 0; first < count; first += 960) {
                    const auto end = std::min(first + 960, count);
                    double squaresLeft = 0, squaresRight = 0;
                    for (auto sample = first; sample < end; ++sample) {
                        const auto at = static_cast<std::size_t>(sample) * 2;
                        squaresLeft += static_cast<double>(pcm[at]) * static_cast<double>(pcm[at]);
                        squaresRight += static_cast<double>(pcm[at + 1]) * static_cast<double>(pcm[at + 1]);
                    }
                    windows.push_back({start + first, start + end,
                        std::sqrt(std::max(squaresLeft, squaresRight) / static_cast<double>(end - first))});
                }
            const auto done = completed.fetch_add(count, std::memory_order_relaxed) + count;
            const auto percent = static_cast<int>(static_cast<double>(done) / static_cast<double>(total) * 100);
            auto previous = progress.load(std::memory_order_relaxed);
            while (previous < percent && !progress.compare_exchange_weak(previous, percent, std::memory_order_relaxed)) {}
        }
        AudioAdjustmentClipResult measured;
        measured.clipId = target->id;
        measured.name = target->name;
        if (!meter.finish(measured.measurement, result.error))
            return result;
        measured.correctionDb = target->effects.normalizationGainDb;
        if (settings.normalize && measured.measurement.measurable) {
            const double wanted = (voice ? settings.voiceLufs : settings.bgmLufs) -
                                  measured.measurement.integratedLufs;
            const double ceiling = -1 - measured.measurement.truePeakDb;
            measured.peakLimited = wanted > ceiling;
            const double correction = std::min(wanted, ceiling);
            measured.correctionDb = std::clamp(correction, -96.0, 60.0);
            measured.gainLimited = correction != measured.correctionDb;
            target->effects.normalizationGainDb = measured.correctionDb;
        }
        if (settings.duck && voice) {
            const double normalized = std::pow(10.0, target->effects.normalizationGainDb / 20);
            const double threshold = std::pow(10.0, settings.thresholdDb / 20);
            for (const auto& window : windows)
                if (window.rms * normalized >= threshold)
                    result.ranges.push_back({window.start, window.end});
        }
        result.clips.push_back(measured);
        result.success = true;
        return result;
    };
    // スレッド数と常駐メモリを制限し、結果は元の clip 順に取りまとめる。
    const auto concurrency = std::min(plan.clips.size(),
        static_cast<std::size_t>(std::clamp(std::thread::hardware_concurrency(), 1U, 4U)));
    std::vector<AudioAdjustmentResult> outputs(plan.clips.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::future<void>> workers;
    for (std::size_t worker = 0; worker < concurrency; ++worker)
        workers.push_back(std::async(std::launch::async, [&] {
            try {
                while (running && !failed) {
                    const auto index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= plan.clips.size())
                        break;
                    outputs[index] = analyzeClip(index);
                    if (!outputs[index].success)
                        failed = true;
                }
            } catch (...) {
                failed = true;
                throw;
            }
        }));
    for (auto& worker : workers)
        worker.get();
    if (failed) {
        for (const auto& output : outputs)
            if (!output.error.empty()) {
                combined.error = output.error;
                break;
            }
        combined.cancelled = !running;
        if (combined.error.empty() && !combined.cancelled)
            combined.error = "音声解析を完了できません";
        return combined;
    }
    if (!running) {
        combined.cancelled = true;
        return combined;
    }
    for (auto& output : outputs) {
        combined.clips.insert(combined.clips.end(), output.clips.begin(), output.clips.end());
        combined.ranges.insert(combined.ranges.end(), output.ranges.begin(), output.ranges.end());
    }
    combined.ranges = project::mergeAudioDetectedRanges(std::move(combined.ranges));
    if (settings.duck)
        for (auto& clip : combined.candidate.timelineClips)
            if (clip.enabled && clip.kind == project::TimelineClipKind::Audio &&
                clip.track.index == settings.bgmTrack) {
                const auto duration = project::timelineClipDuration(combined.candidate, clip);
                if (!duration.success) {
                    combined.error = duration.error;
                    return combined;
                }
                clip.effects.duckingDb = 0;
                clip.effects.duckingKeys = project::makeDuckingKeys(
                    combined.ranges, settings, clip.timelineStartFrame, duration.frame,
                    plan.timelineFpsNum, plan.timelineFpsDen);
            }
    const auto valid = project::validateTimeline(combined.candidate);
    if (!valid.success) {
        combined.error = valid.error;
        return combined;
    }
    combined.success = true;
    progress = 100;
    return combined;
}

AudioAdjustmentJob::AudioAdjustmentJob(project::Project source,
                                       project::AudioAdjustmentSettings settings) {
    future_ = std::async(std::launch::async, [this, source = std::move(source),
                                              settings = std::move(settings)]() mutable {
        try {
            return analyzeAudioAdjustment(std::move(source), settings, running_, progress_);
        } catch (const std::exception&) {
            AudioAdjustmentResult result;
            result.error = "音声解析中に処理を継続できなくなりました";
            return result;
        }
    });
}

AudioAdjustmentJob::~AudioAdjustmentJob() {
    cancel();
    if (future_.valid())
        future_.wait();
}

bool AudioAdjustmentJob::ready() const {
    return future_.valid() &&
           future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

AudioAdjustmentResult AudioAdjustmentJob::take() {
    return future_.get();
}
} // namespace mvm::app
