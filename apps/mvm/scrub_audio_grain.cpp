#include "scrub_audio_grain.h"

#include "media/audio_preview/audio_types.h"

#include <algorithm>
#include <utility>

namespace mvm::app {

void applyScrubGrainFade(std::vector<float>& pcm, std::int64_t fadeSamples) {
    const auto samples = static_cast<std::int64_t>(pcm.size()) / audio::kInternalChannels;
    if (samples <= 0 || fadeSamples <= 0)
        return;
    for (std::int64_t i = 0; i < samples; ++i) {
        // 端から何 sample 目か。0 番目と最後の sample は gain 0 になる。
        const std::int64_t edge = std::min(i, samples - 1 - i);
        if (edge >= fadeSamples)
            continue;
        const float gain = static_cast<float>(edge) / static_cast<float>(fadeSamples);
        for (int channel = 0; channel < audio::kInternalChannels; ++channel)
            pcm[static_cast<std::size_t>(i * audio::kInternalChannels + channel)] *= gain;
    }
}

void ScrubTargetLatch::set(std::int64_t frame) {
    if (latest_ == frame)
        return;
    latest_ = frame;
    ++revision_;
}

std::optional<ScrubTarget> ScrubTargetLatch::take() {
    if (!latest_ || revision_ == takenRevision_)
        return std::nullopt;
    takenRevision_ = revision_;
    return ScrubTarget{*latest_, revision_};
}

void ScrubGrainStream::replace(std::vector<float> grain) {
    const auto channels = static_cast<std::size_t>(audio::kInternalChannels);
    const std::size_t remaining = (grain_.size() - offset_) / channels;
    const std::size_t tail =
        std::min(remaining, static_cast<std::size_t>(std::max<std::int64_t>(0, crossfadeSamples_)));
    if (tail > 0) {
        if (grain.size() < tail * channels)
            grain.resize(tail * channels, 0.0F);
        // gain は直前に鳴らした sample と連続する 1 から始め、tail 分で 0 へ近づける。
        for (std::size_t i = 0; i < tail; ++i) {
            const float gain = static_cast<float>(tail - i) / static_cast<float>(tail);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                float& sample = grain[i * channels + channel];
                sample = std::clamp(sample + grain_[offset_ + i * channels + channel] * gain, -1.0F,
                                    1.0F);
            }
        }
    }
    grain_ = std::move(grain);
    offset_ = 0;
}

std::int64_t ScrubGrainStream::fill(float* destination, std::int64_t count) {
    if (count <= 0)
        return 0;
    const auto total = static_cast<std::size_t>(count) * audio::kInternalChannels;
    const std::size_t available = grain_.size() - offset_;
    const std::size_t copied = std::min(total, available);
    std::copy_n(grain_.begin() + static_cast<std::ptrdiff_t>(offset_), copied, destination);
    std::fill(destination + copied, destination + total, 0.0F);
    offset_ += copied;
    return static_cast<std::int64_t>(copied) / audio::kInternalChannels;
}

ScrubGrainScheduler::ScrubGrainScheduler(ScrubGrainMaker maker, std::int64_t crossfadeSamples)
    : maker_(std::move(maker)), stream_(crossfadeSamples) {}

ScrubGrainScheduler::~ScrubGrainScheduler() {
    requestStop();
    join();
}

void ScrubGrainScheduler::start() {
    {
        std::lock_guard lock(mutex_);
        if (running_)
            return;
        running_ = true;
    }
    thread_ = std::thread(&ScrubGrainScheduler::run, this);
}

void ScrubGrainScheduler::requestStop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    changed_.notify_all();
}

void ScrubGrainScheduler::join() {
    if (thread_.joinable())
        thread_.join();
}

void ScrubGrainScheduler::setTarget(std::int64_t frame) {
    {
        std::lock_guard lock(mutex_);
        target_.set(frame);
    }
    changed_.notify_all();
}

std::int64_t ScrubGrainScheduler::fill(float* destination, std::int64_t count) {
    std::lock_guard lock(mutex_);
    return stream_.fill(destination, count);
}

std::string ScrubGrainScheduler::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void ScrubGrainScheduler::run() {
    while (true) {
        std::optional<ScrubTarget> target;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] {
                if (!running_)
                    return true;
                target = target_.take();
                return target.has_value();
            });
            if (!running_)
                return;
        }
        std::vector<float> pcm;
        std::string error;
        const bool made = maker_(target->frame, pcm, error);
        std::lock_guard lock(mutex_);
        if (!running_)
            return;
        if (!made) {
            error_ = error.empty() ? "scrub音声を生成できません" : error;
            return;
        }
        // decode 中に位置が動いていたら、古い位置の音は一度も鳴らさずに捨てる。
        if (!target_.isLatest(target->revision)) {
            discarded_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        stream_.replace(std::move(pcm));
        published_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace mvm::app
