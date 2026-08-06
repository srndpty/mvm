/*
 * mvm Phase 1 / P1.2 - device 変更時の停止順序 (§3)
 *
 * --------------------------------------------------------------------------
 * P1.1 の何が危なかったか
 * --------------------------------------------------------------------------
 * render thread は device の変化を検出すると、その場で
 *
 *     deviceReady = false
 *     queue.stop()
 *     retirement.drain()
 *     converter.release()
 *     completion.release()
 *     device.release()
 *
 * まで一気にやっていた。**このとき decode thread はまだ動いている。**
 * decode thread は同じ SharedD3D11Device と ReadbackCounters を握って
 * FFmpeg の decode を回しているので、その足元で device を Release すると
 * 未定義動作になる。queue.stop() は「submit を止める」だけで、
 * decode 自体を止めはしない。
 *
 * --------------------------------------------------------------------------
 * P1.2 の順序
 * --------------------------------------------------------------------------
 *   1. render thread: device 変化を検出 -> deviceReady=false / queue.stop()
 *                     -> noteDetected() で GUI へ通知。**teardown しない**
 *   2. GUI thread   : detected を見て DecodeWorker::stop() (thread join まで)
 *                     -> noteWorkerStopped()
 *   3. render thread: workerStopped を確認してから teardown
 *   4. 明示的な終了コードで停止する
 *
 * **P1.2 では自動復帰しない。** 新しい device で開き直す実装は持たない。
 * したがって deviceChangeHandledCount は「完全な復帰が成立した回数」であり、
 * P1.2 では **常に 0** である。検出は detected、停止は fail_closed で数える。
 */

#ifndef MVM_GPU_PREVIEW_DEVICE_CHANGE_H
#define MVM_GPU_PREVIEW_DEVICE_CHANGE_H

#include <condition_variable>
#include <mutex>
#include <string>

namespace mvm::gpu {

enum class DeviceChangeState {
    None = 0,
    Detected,      // render thread が検出。decode thread はまだ動いている
    WorkerStopped, // GUI thread が decode thread を join し終えた
    TornDown,      // render thread が resource を破棄した
};

const char* toString(DeviceChangeState s);

class DeviceChangeCoordinator {
public:
    // --- render thread ------------------------------------------------------
    // device の変化を検出した。**ここでは resource を壊さない。**
    void noteDetected(const std::string& reason);

    // teardown してよいか。decode thread の join が済んでいるときだけ true。
    bool mayTeardown() const;

    // teardown が済んだ。
    void noteTornDown();

    // --- GUI thread ---------------------------------------------------------
    bool detected() const;
    // DecodeWorker::stop() (join まで) を終えてから呼ぶ。
    void noteWorkerStopped();

    // --- 共通 ---------------------------------------------------------------
    DeviceChangeState state() const;
    std::string reason() const;

    long long detectedCount() const;
    long long failClosedCount() const;
    // **完全な復帰が成立したときだけ増やす。** P1.2 では常に 0。
    long long handledCount() const;

    // 復帰まで成功した場合だけ呼ぶ。P1.2 では呼び出し箇所が無い。
    void noteFullyRecovered();

    // 停止 (fail-closed) を 1 件記録する。
    void noteFailClosed();

    void reset();

private:
    mutable std::mutex mutex_;
    DeviceChangeState state_ = DeviceChangeState::None;
    std::string reason_;
    long long detected_ = 0;
    long long failClosed_ = 0;
    long long handled_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_DEVICE_CHANGE_H
