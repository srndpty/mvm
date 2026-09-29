#include "scrub_audio_grain.h"

#include <cmath>
#include <cstdio>
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
    expect(first && *first == 10, "新しい位置で grain を作る");
    // drag を止めた (同じ位置が来続ける) ときに鳴り続けないこと。
    expect(!latch.take(), "取り出した後は同じ位置で grain を作らない");
    latch.set(10);
    expect(!latch.take(), "同じ位置を再設定しても grain を作らない");
    latch.set(11);
    latch.set(12);
    const auto latest = latch.take();
    expect(latest && *latest == 12, "decode 中に来た位置は最新だけを使う");
    latch.set(10);
    const auto back = latch.take();
    expect(back && *back == 10, "以前の位置へ戻ったら再び grain を作る");
}

void testStream() {
    mvm::app::ScrubGrainStream stream;
    std::vector<float> out(8, 9.0F);
    expect(stream.fill(out.data(), 4) == 0, "grain が無ければ無音");
    expect(near(out[0], 0.0F) && near(out[7], 0.0F), "無音を書く");

    stream.replace({1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6});
    expect(stream.fill(out.data(), 4) == 4, "grain の先頭 4 sample");
    expect(near(out[0], 1.0F) && near(out[6], 4.0F), "grain の値をそのまま書く");
    expect(stream.fill(out.data(), 4) == 2, "残り 2 sample");
    expect(near(out[0], 5.0F) && near(out[2], 6.0F) && near(out[4], 0.0F) && near(out[7], 0.0F),
           "尽きた分は無音で埋める");
    expect(stream.fill(out.data(), 4) == 0, "grain を鳴らし終えたら無音");

    stream.replace({1, 1, 2, 2, 3, 3});
    expect(stream.fill(out.data(), 1) == 1, "途中まで鳴らす");
    stream.replace({7, 7});
    expect(stream.fill(out.data(), 2) == 1, "差し替えたら古い grain の残りは捨てる");
    expect(near(out[0], 7.0F) && near(out[2], 0.0F), "新しい grain だけを鳴らす");
}

} // namespace

int main() {
    testFade();
    testLatch();
    testStream();
    if (failures != 0) {
        std::fprintf(stderr, "scrub音声grain: %d件失敗\n", failures);
        return 1;
    }
    std::puts("scrub音声grain: PASS");
    return 0;
}
