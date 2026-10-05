#ifndef MVM_TESTS_MATH_TEST_TRANSFORM_H
#define MVM_TESTS_MATH_TEST_TRANSFORM_H

// 数式の変形の backend の試験で、偽の Manim (fake_math_tex_cli) と試験が共有する取り決め。
//
// - 端点の静止の mask の模様: 試験が静止の mask として渡し、偽の Manim が frame 0 と終状態に
//   同じ模様を描く。0 の画素も含み、位置が 1 画素ずれると一致しない。
// - 偽の被覆率の画像: 偽の Manim は PNG の代わりに "MVMFAKECOV <w> <h>\n" + w*h byte の alpha を
//   .png の名前で書き、試験の loader がそれを読む。PNG の decode は backend の外
//   (MathCoverageLoader) の責務なので、backend の検査だけを試す。実 PNG の decode は実 Manim の
//   試験 (mvm_math_transform_smoke) が通す。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mvm::test {

inline std::uint8_t mathTestEndpointAlpha(bool target, int x, int y) {
    return static_cast<std::uint8_t>(target ? (x * 5 + y * 29 + 7) % 256 : (x * 37 + y * 11) % 256);
}

inline std::string mathTestCoverageBytes(int width, int height,
                                         const std::vector<std::uint8_t>& alpha) {
    char header[64] = {};
    std::snprintf(header, sizeof header, "MVMFAKECOV %d %d\n", width, height);
    return std::string(header) + std::string(alpha.begin(), alpha.end());
}

// 形が違えば false。
inline bool parseMathTestCoverage(const std::string& bytes, int& width, int& height,
                                  std::vector<std::uint8_t>& alpha) {
    const auto newline = bytes.find('\n');
    if (bytes.rfind("MVMFAKECOV ", 0) != 0 || newline == std::string::npos ||
        std::sscanf(bytes.c_str(), "MVMFAKECOV %d %d", &width, &height) != 2 || width <= 0 ||
        height <= 0)
        return false;
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (bytes.size() - newline - 1 != size)
        return false;
    alpha.assign(bytes.begin() + static_cast<std::ptrdiff_t>(newline) + 1, bytes.end());
    return true;
}

} // namespace mvm::test

#endif
