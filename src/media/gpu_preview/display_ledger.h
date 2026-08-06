/*
 * mvm Phase 1 / P1.2 - 表示された frame の記録 (§1)
 *
 * --------------------------------------------------------------------------
 * 直そうとしている race
 * --------------------------------------------------------------------------
 * P1.1 の seekAndWaitForDisplay は
 *
 *     waiting = false        (前回の待機を降ろす)
 *     seekBlocking()         (ここで frame が queue へ入る)
 *     waiting = true         (ここで初めて待機を登録する)
 *
 * という順序だった。**submit 後・arm 前に render thread が描いてしまうと、
 * その display は誰にも記録されない。** 待機側は次の display が来るまで待ち、
 * 静止画なら永久に来ないので timeout する。
 *
 * seek の直後は「新しい frame が 1 枚だけ入って、すぐ描かれる」状況なので、
 * この窓に入る確率は低くない。timeout は fail-closed なので
 * 「表示されなかった」と誤って報告する。
 *
 * --------------------------------------------------------------------------
 * 直し方
 * --------------------------------------------------------------------------
 * render thread は **待機の有無に関わらず、すべての display を記録する。**
 * 待機側は
 *
 *     baseline = ledger.currentSequence()   (seek より前に取る)
 *     seek()
 *     ledger.waitForDisplay(baseline, ...)  (baseline より後の記録を探す)
 *
 * とする。arm する前に描かれていても、記録は残っているので取りこぼさない。
 *
 * 「古い display を新しい request の成功に使わない」ことは
 * **sequence > baseline** で保証する。baseline を seek より前に取るので、
 * seek 以前の display は必ず baseline 以下になる。
 * さらに source / generation / composition epoch / frame 番号も照合する。
 *
 * sleep には依存しない。記録済みなら即座に返り、未記録なら
 * condition_variable で待つ。
 */

#ifndef MVM_GPU_PREVIEW_DISPLAY_LEDGER_H
#define MVM_GPU_PREVIEW_DISPLAY_LEDGER_H

#include "media/gpu_preview/gpu_frame.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace mvm::gpu {

// 1 回の display の記録。render thread が作り、待機側が照合する。
struct DisplayRecord {
    unsigned long long sequence = 0; // 単調増加。0 は「記録なし」
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    CompositionEpoch compositionEpoch{};
    long long displayedFrame = -1;
    long long displayedQpc = 0;
};

// 待機の一致条件。4 つすべてが一致した記録だけを成功とする。
struct DisplayWaitKey {
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    CompositionEpoch compositionEpoch{};
    long long requestedFrame = -1;
};

class DisplayLedger {
public:
    // 直近 historyDepth 件を保持する。
    // seek 1 回につき描かれるのは数枚なので、これで十分に足りる。
    // 無限に持たない (計測中は毎 frame 記録されるため)。
    explicit DisplayLedger(size_t historyDepth = 64);

    // --- render thread ------------------------------------------------------
    // **すべての display を記録する。** 待機の有無を見ない。
    // 戻り値はこの display に付いた sequence。
    unsigned long long recordDisplay(const DecodedGpuFrame& frame, CompositionEpoch epoch,
                                     long long displayedQpc);

    // --- 待機側 (GUI thread) ------------------------------------------------
    // seek を出す **前に** 呼ぶ。これより後の display だけを成功と見なす。
    unsigned long long currentSequence() const;

    // baseline より後で key に一致する display を待つ。
    // 見つかれば true。timeoutMs 以内に来なければ false (fail-closed)。
    // すでに記録済みなら待たずに即 true を返す (ここが race 修正の要点)。
    bool waitForDisplay(unsigned long long baselineSequence, const DisplayWaitKey& key,
                        int timeoutMs, DisplayRecord& out) const;

    // 待機を打ち切る (shutdown 用)。以後の wait は即 false。
    void abort();
    void restart();

    long long recordedCount() const;

private:
    // baseline より後で key に一致する記録を探す。mutex 保持下で呼ぶ。
    bool findLocked(unsigned long long baselineSequence, const DisplayWaitKey& key,
                    DisplayRecord& out) const;

    mutable std::mutex mutex_;
    mutable std::condition_variable recorded_;
    std::deque<DisplayRecord> history_;
    size_t historyDepth_;
    unsigned long long sequence_ = 0;
    long long recordedCount_ = 0;
    bool aborted_ = false;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_DISPLAY_LEDGER_H
