#include "core/source_frame_mapping.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using mvm::core::FrameRate;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 期待値は MLT を実行して得た対応 (docs/premiere-like-editing.md §16.7) を手で書き写したもの。
// 実装の式から作らない。
void displayedFrameMatchesMlt() {
    // 25fps 素材を 60fps へ置く。MLT (avformat producer) は位置 p に frame floor(p * 25 / 60 + 1/2)
    // を出した。p = 6 は 2.5 で、上へ丸める。
    const std::vector<std::int64_t> pal = {0, 0, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5};
    for (std::int64_t p = 0; p < static_cast<std::int64_t>(pal.size()); ++p) {
        const auto frame = mvm::core::sourceFrameAtOutputPosition(p, {25, 1}, {60, 1});
        require(frame && *frame == pal[static_cast<std::size_t>(p)],
                "25fps素材の60fps位置 " + std::to_string(p) + " がMLTと一致しません");
    }
    // 50fps 素材を 60fps へ置き、producer 位置 4..9 を出すと MLT は 3,4,5,6,7,8 を出した。
    const std::vector<std::int64_t> fifty = {3, 4, 5, 6, 7, 8};
    for (std::int64_t p = 4; p < 10; ++p) {
        const auto frame = mvm::core::sourceFrameAtOutputPosition(p, {50, 1}, {60, 1});
        require(frame && *frame == fifty[static_cast<std::size_t>(p - 4)],
                "50fps素材の60fps位置 " + std::to_string(p) + " がMLTと一致しません");
    }
    // 同じ fps なら位置と frame は 1:1。
    for (std::int64_t p = 0; p < 10; ++p) {
        const auto frame = mvm::core::sourceFrameAtOutputPosition(p, {30000, 1001}, {30000, 1001});
        require(frame && *frame == p, "同じfpsで位置とframeが1:1になりません");
    }
}

void boundaryRounding() {
    // 30 * 60 * 1001 / 30000 = 60.06
    require(mvm::core::convertFrameBoundary(30, {30000, 1001}, {60, 1}, true) == 61,
            "ceilの境界が一致しません");
    require(mvm::core::convertFrameBoundary(30, {30000, 1001}, {60, 1}, false) == 60,
            "floorの境界が一致しません");
    require(mvm::core::convertFrameBoundary(61, {60, 1}, {30000, 1001}, false) == 30,
            "逆向きの境界が一致しません");
    require(!mvm::core::convertFrameBoundary(-1, {60, 1}, {60, 1}, true), "負の境界を受理しました");
    require(!mvm::core::convertFrameBoundary(1, {0, 1}, {60, 1}, true), "不正なfpsを受理しました");
}

// firstOutputPositionOfSourceFrame は sourceFrameAtOutputPosition の逆でなければならない。
// 2 つの関数を互いに照合するだけで、どちらの式も期待値の計算には使わない。
void inverseIsConsistent() {
    const std::vector<std::pair<FrameRate, FrameRate>> rates =
        {
            {{25, 1}, {60, 1}},    {{50, 1}, {60, 1}}, {{24000, 1001}, {60, 1}},
            {{120, 1}, {60, 1}},   {{30, 1}, {60, 1}}, {{60, 1}, {30000, 1001}},
            {{3000, 73}, {60, 1}}, // 30fps 素材の 73% 相当
    };
    for (const auto& [source, output] : rates) {
        for (std::int64_t p = 0; p < 500; ++p) {
            const auto frame = mvm::core::sourceFrameAtOutputPosition(p, source, output);
            require(frame.has_value(), "表示frameを計算できません");
            const auto begin = mvm::core::firstOutputPositionOfSourceFrame(*frame, source, output);
            const auto end =
                mvm::core::firstOutputPositionOfSourceFrame(*frame + 1, source, output);
            require(begin && end && *begin <= p && p < *end,
                    "表示区間が表示frameの逆になっていません: " + std::to_string(source.num) + "/" +
                        std::to_string(source.den) + " p=" + std::to_string(p));
        }
    }
    require(mvm::core::firstOutputPositionOfSourceFrame(0, {25, 1}, {60, 1}) == 0,
            "frame 0の表示区間が0から始まりません");
}

void overflowIsRejected() {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    // 2 p R.den = 2 max * max^2 は 128 bit を超える。
    require(!mvm::core::sourceFrameAtOutputPosition(maximum, {maximum, 1}, {1, maximum}),
            "表示frameのoverflowを受理しました");
    require(!mvm::core::firstOutputPositionOfSourceFrame(maximum, {1, maximum}, {maximum, 1}),
            "表示区間のoverflowを受理しました");
    require(!mvm::core::convertFrameBoundary(maximum, {1, maximum}, {maximum, 1}, true),
            "境界のoverflowを受理しました");
}

} // namespace

int main() {
    int passed = 0;
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"displayedFrameMatchesMlt", displayedFrameMatchesMlt},
        {"boundaryRounding", boundaryRounding},
        {"inverseIsConsistent", inverseIsConsistent},
        {"overflowIsRejected", overflowIsRejected},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << "\n";
            return 1;
        }
    }
    std::cout << "source frame mapping: " << passed << "/" << tests.size() << " 通過\n";
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}
