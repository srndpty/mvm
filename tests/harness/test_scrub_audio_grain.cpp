#include "scrub_audio_grain.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool near(float actual, float expected) {
    return std::abs(actual - expected) < 1e-6F;
}

// 左右同じ値の stereo grain。
std::vector<float> grainOf(std::initializer_list<float> values) {
    std::vector<float> pcm;
    for (const float value : values) {
        pcm.push_back(value);
        pcm.push_back(value);
    }
    return pcm;
}

void testFade() {
    // 10 sample (stereo) の全 1.0 に fade 4 を掛ける。期待 gain は手計算:
    // 端からの距離 0,1,2,3 → 0, 0.25, 0.5, 0.75、中央 (距離 4) は 1。
    std::vector<float> pcm(20, 1.0F);
    mvm::app::applyScrubGrainFade(pcm, 4);
    const float expected[10] = {0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 1.0F, 0.75F, 0.5F, 0.25F, 0.0F};
    for (int i = 0; i < 10; ++i) {
        expect(near(pcm[static_cast<std::size_t>(i * 2)], expected[i]), "fade の左 channel");
        expect(near(pcm[static_cast<std::size_t>(i * 2 + 1)], expected[i]), "fade の右 channel");
    }
}

void testLatch() {
    mvm::app::ScrubTargetLatch latch;
    expect(!latch.take(), "位置が一度も来ていなければ grain を作らない");
    latch.set(10);
    const auto first = latch.take();
    expect(first && first->frame == 10, "新しい位置で grain を作る");
    // drag を止めた (同じ位置が来続ける) ときに鳴り続けないこと。
    expect(!latch.take(), "取り出した後は同じ位置で grain を作らない");
    latch.set(10);
    expect(!latch.take(), "同じ位置を再設定しても grain を作らない");
    expect(first && latch.isLatest(first->revision), "同じ位置の再設定では最新のまま");

    latch.set(11);
    expect(first && !latch.isLatest(first->revision),
           "位置が動いたら取り出し済みの位置は最新でない");
    latch.set(12);
    const auto latest = latch.take();
    expect(latest && latest->frame == 12, "take までに来た位置は最新だけを使う");

    // A → B → A と素早く動いた場合も「動いた」として扱う。
    latch.set(13);
    latch.set(12);
    const auto back = latch.take();
    expect(back && back->frame == 12, "元の位置へ戻る往復でも grain を作る");
    expect(latest && back && back->revision != latest->revision, "往復後は別の revision になる");
}

void testStream() {
    mvm::app::ScrubGrainStream stream(0);
    std::vector<float> out(8, 9.0F);
    expect(stream.fill(out.data(), 4) == 0, "grain が無ければ無音");
    expect(near(out[0], 0.0F) && near(out[7], 0.0F), "無音を書く");

    stream.replace(grainOf({1, 2, 3, 4, 5, 6}));
    expect(stream.fill(out.data(), 4) == 4, "grain の先頭 4 sample");
    expect(near(out[0], 1.0F) && near(out[6], 4.0F), "grain の値をそのまま書く");
    expect(stream.fill(out.data(), 4) == 2, "残り 2 sample");
    expect(near(out[0], 5.0F) && near(out[2], 6.0F) && near(out[4], 0.0F) && near(out[7], 0.0F),
           "尽きた分は無音で埋める");
    expect(stream.fill(out.data(), 4) == 0, "grain を鳴らし終えたら無音");
}

void testCrossfade() {
    // 振幅 0.7 の途中で差し替える。古い grain を 0 へ切ると click になるので、
    // 続き 4 sample を gain 4/4, 3/4, 2/4, 1/4 で新しい grain (ここでは無音) へ重ねる。
    mvm::app::ScrubGrainStream stream(4);
    stream.replace(grainOf({0.7F, 0.7F, 0.7F, 0.7F, 0.7F, 0.7F, 0.7F, 0.7F, 0.7F, 0.7F}));
    std::vector<float> out(16, 9.0F);
    stream.fill(out.data(), 2);
    stream.replace(grainOf({0, 0}));
    expect(stream.fill(out.data(), 8) == 4, "crossfade 分だけ grain が延びる");
    const float expected[8] = {0.7F, 0.525F, 0.35F, 0.175F, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i)
        expect(near(out[static_cast<std::size_t>(i * 2)], expected[i]), "古い grain を fade-out");

    // 新しい grain とは加算する。
    mvm::app::ScrubGrainStream mixed(2);
    mixed.replace(grainOf({0.5F, 0.5F, 0.5F}));
    mixed.fill(out.data(), 1);
    mixed.replace(grainOf({0.1F, 0.2F, 0.3F}));
    mixed.fill(out.data(), 3);
    expect(near(out[0], 0.6F) && near(out[2], 0.45F) && near(out[4], 0.3F),
           "新しい grain へ重ねる");

    // 鳴らし終えた grain からは何も重ねない。
    mvm::app::ScrubGrainStream finished(4);
    finished.replace(grainOf({0.7F}));
    finished.fill(out.data(), 1);
    finished.replace(grainOf({0.1F}));
    expect(finished.fill(out.data(), 2) == 1 && near(out[0], 0.1F), "尽きた grain は重ねない");
}

// decode 中に位置が動いたら、古い位置の grain を一度も鳴らさないこと。
void testStaleGrainIsDiscarded() {
    std::mutex mutex;
    std::condition_variable changed;
    bool firstEntered = false;
    bool releaseFirst = false;
    const auto maker = [&](std::int64_t frame, std::vector<float>& pcm, std::string&) {
        if (frame == 100) {
            std::unique_lock lock(mutex);
            firstEntered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return releaseFirst; });
        }
        pcm = grainOf({static_cast<float>(frame) / 1000.0F});
        return true;
    };
    mvm::app::ScrubGrainScheduler scheduler(maker, 0);
    scheduler.start();
    scheduler.setTarget(100);
    {
        std::unique_lock lock(mutex);
        expect(changed.wait_for(lock, std::chrono::seconds(2), [&] { return firstEntered; }),
               "位置 100 の decode が始まらない");
    }
    scheduler.setTarget(200);
    {
        std::lock_guard lock(mutex);
        releaseFirst = true;
    }
    changed.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (scheduler.publishedCount() + scheduler.discardedCount() < 2 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::vector<float> out(4, 9.0F);
    const auto filled = scheduler.fill(out.data(), 2);
    scheduler.requestStop();
    scheduler.join();
    expect(scheduler.discardedCount() == 1, "decode 中に古くなった grain を捨てる");
    expect(scheduler.publishedCount() == 1, "最新の位置の grain だけを鳴らす");
    expect(filled == 1 && near(out[0], 0.2F), "鳴るのは位置 200 の grain");
    expect(scheduler.error().empty(), "error にしない");
}

} // namespace

int main() {
    testFade();
    testLatch();
    testStream();
    testCrossfade();
    testStaleGrainIsDiscarded();
    if (failures != 0) {
        std::fprintf(stderr, "scrub音声grain: %d件失敗\n", failures);
        return 1;
    }
    std::puts("scrub音声grain: PASS");
    return 0;
}
