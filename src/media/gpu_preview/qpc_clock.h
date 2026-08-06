/*
 * mvm Phase 1 / P1 - 経過時間の実測
 *
 * Phase 0 で「Sleep の回数から経過時間を計算して 6 秒が 12.3 秒だった」
 * という事故があった (AGENTS.md の罠一覧)。
 * 経過時間は必ず QueryPerformanceCounter の実測値を使う。
 */

#ifndef MVM_GPU_PREVIEW_QPC_CLOCK_H
#define MVM_GPU_PREVIEW_QPC_CLOCK_H

#include <windows.h>

namespace mvm::gpu {

inline double qpcFrequency() {
    static const double f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);
        return static_cast<double>(li.QuadPart);
    }();
    return f;
}

inline long long qpcTicks() {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

inline double qpcMsBetween(long long from, long long to) {
    return static_cast<double>(to - from) * 1000.0 / qpcFrequency();
}

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_QPC_CLOCK_H
