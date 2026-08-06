#include "media/gpu_preview/display_ledger.h"

#include <chrono>

namespace mvm::gpu {

DisplayLedger::DisplayLedger(size_t historyDepth)
    : historyDepth_(historyDepth == 0 ? 1 : historyDepth) {}

unsigned long long DisplayLedger::recordDisplay(const DecodedGpuFrame& frame,
                                                CompositionEpoch epoch, long long displayedQpc) {
    unsigned long long seq = 0;
    {
        std::lock_guard<std::mutex> g(mutex_);
        seq = ++sequence_;
        DisplayRecord r;
        r.sequence = seq;
        r.sourceId = frame.sourceId;
        r.sourceGeneration = frame.sourceGeneration;
        r.compositionEpoch = epoch;
        r.displayedFrame = frame.frameNumber;
        r.displayedQpc = displayedQpc;
        history_.push_back(r);
        while (history_.size() > historyDepth_)
            history_.pop_front();
        recordedCount_++;
    }
    // 待機側は「まだ arm していない」かもしれないが、記録は済んでいる。
    // 通知を取りこぼしても waitForDisplay が履歴を先に見るので落ちない。
    recorded_.notify_all();
    return seq;
}

unsigned long long DisplayLedger::currentSequence() const {
    std::lock_guard<std::mutex> g(mutex_);
    return sequence_;
}

bool DisplayLedger::findLocked(unsigned long long baselineSequence, const DisplayWaitKey& key,
                               DisplayRecord& out) const {
    // 新しい方から探す。同じ frame が複数回描かれていても、
    // baseline より後で最初に見つかったものを使えば十分である。
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->sequence <= baselineSequence)
            break; // それより前は baseline 以前。**古い display を成功にしない**
        if (it->sourceId != key.sourceId)
            continue;
        if (it->sourceGeneration != key.sourceGeneration)
            continue;
        if (it->compositionEpoch != key.compositionEpoch)
            continue;
        if (it->displayedFrame != key.requestedFrame)
            continue;
        out = *it;
        return true;
    }
    return false;
}

bool DisplayLedger::waitForDisplay(unsigned long long baselineSequence, const DisplayWaitKey& key,
                                   int timeoutMs, DisplayRecord& out) const {
    std::unique_lock<std::mutex> g(mutex_);
    if (aborted_)
        return false;

    // **待つ前にまず履歴を見る。** arm より前に描かれていてもここで拾える。
    // これが P1.1 の race を消している部分である。
    if (findLocked(baselineSequence, key, out))
        return true;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    while (!aborted_) {
        if (recorded_.wait_until(g, deadline) == std::cv_status::timeout) {
            // timeout でも最後にもう一度見る (通知と deadline が競ることがある)
            return findLocked(baselineSequence, key, out);
        }
        if (findLocked(baselineSequence, key, out))
            return true;
    }
    return false;
}

void DisplayLedger::abort() {
    {
        std::lock_guard<std::mutex> g(mutex_);
        aborted_ = true;
    }
    recorded_.notify_all();
}

void DisplayLedger::restart() {
    std::lock_guard<std::mutex> g(mutex_);
    aborted_ = false;
}

long long DisplayLedger::recordedCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return recordedCount_;
}

} // namespace mvm::gpu
