#ifndef MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H
#define MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mvm::app {

// scrub 音声は、drag 位置が変わるたびにその位置から等速で短い断片 (grain) を鳴らす。
// 位置が変わらなければ新しい grain は作らず、鳴らし終えたら無音になる (Premiere と同じ)。
// ここは WASAPI にも decoder にも依存しない部分で、sample は 48kHz stereo interleave。
inline constexpr std::int64_t kScrubGrainSamples = 3840;    // 80 ms
inline constexpr std::int64_t kScrubGrainFadeSamples = 240; // 5 ms

// grain の先頭と末尾に線形 fade を掛ける。
// 長さが fade 2 回分に満たない grain は、中央で折り返す三角形になる。
void applyScrubGrainFade(std::vector<float>& pcm, std::int64_t fadeSamples);

struct ScrubTarget {
    std::int64_t frame = 0;
    // set で位置が変わるたびに増える。frame の値だけで比べると、A → B → A と
    // 素早く動いたとき「一度動いた」ことまで消えてしまうので別に持つ。
    std::uint64_t revision = 0;
};

// 最新の scrub 位置を受け取り、前回取り出してから動いたときだけ取り出させる。
class ScrubTargetLatch final {
public:
    // 直前と同じ位置なら何もしない (drag を止めても mouse move は来続ける)。
    void set(std::int64_t frame);
    // 前回 take してから位置が動いていれば、最新の位置を返す。
    std::optional<ScrubTarget> take();

    // revision が今も最新の位置か。decode 中に位置が動いたら false になる。
    bool isLatest(std::uint64_t revision) const { return revision == revision_; }

private:
    std::optional<std::int64_t> latest_;
    std::uint64_t revision_ = 0;
    std::uint64_t takenRevision_ = 0;
};

// 出力 stream へ流す grain の残り。
// 新しい grain が来たら古い grain の未再生分は捨てるが、途中で 0 へ落とすと click になるので、
// 古い grain の続き crossfadeSamples 分を fade-out しながら新しい grain の先頭へ重ねる。
class ScrubGrainStream final {
public:
    explicit ScrubGrainStream(std::int64_t crossfadeSamples = kScrubGrainFadeSamples)
        : crossfadeSamples_(crossfadeSamples) {}

    void replace(std::vector<float> grain);
    // count sample を destination へ書き、grain 由来の sample 数を返す。尽きた分は無音。
    std::int64_t fill(float* destination, std::int64_t count);

private:
    std::int64_t crossfadeSamples_ = 0;
    std::vector<float> grain_;
    std::size_t offset_ = 0;
};

// frame の位置から grain を作る。decode を含むので時間がかかってよい。
using ScrubGrainMaker =
    std::function<bool(std::int64_t frame, std::vector<float>& pcm, std::string& error)>;

// grain の decode を専用 thread で行い、出力側 (fill) を decode 待ちで止めない。
// decode 中に位置が動いたら、その grain は鳴らさずに捨てて最新の位置へ進む (latest wins)。
class ScrubGrainScheduler final {
public:
    explicit ScrubGrainScheduler(ScrubGrainMaker maker,
                                 std::int64_t crossfadeSamples = kScrubGrainFadeSamples);
    ~ScrubGrainScheduler();
    ScrubGrainScheduler(const ScrubGrainScheduler&) = delete;
    ScrubGrainScheduler& operator=(const ScrubGrainScheduler&) = delete;

    void start();
    // thread を待たずに止める合図だけを出す。
    void requestStop();
    void join();

    void setTarget(std::int64_t frame);
    std::int64_t fill(float* destination, std::int64_t count);

    std::uint64_t publishedCount() const { return published_.load(); }

    std::uint64_t discardedCount() const { return discarded_.load(); }

    std::string error() const;

private:
    void run();

    ScrubGrainMaker maker_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool running_ = false;
    ScrubTargetLatch target_;
    ScrubGrainStream stream_;
    std::string error_;
    std::atomic<std::uint64_t> published_{0};
    std::atomic<std::uint64_t> discarded_{0};
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_SCRUB_AUDIO_GRAIN_H
