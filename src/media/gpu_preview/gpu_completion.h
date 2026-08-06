/*
 * mvm Phase 1 / P1.1 - GPU 完了に基づく frame / resource の retire
 *
 * これまでの「直近 retainDepth 枚を保持」方式を廃止する (§1)。
 * retainDepth は「たぶん GPU は読み終わっているだろう」という推測であり、
 * 負荷が上がると保証が崩れる。ここでは **GPU の完了 serial** を根拠にする。
 *
 * 契約:
 *   - draw を発行するたびに submission serial を得る (signalSubmission)
 *   - 描画に使った lifetime token / SRV / texture は、その serial が
 *     完了するまで GpuRetirementQueue が保持する
 *   - render ごとに完了済み serial を **poll** して解放する
 *     (per-frame で GPU 完了を blocking wait しない)
 *   - shutdown だけは有限 timeout で drain する。timeout は fail-closed
 *   - Flush を毎 frame 呼んで見かけ上解決しない
 *
 * GpuRetirementQueue は D3D11 を直接触らない (shared_ptr<void> だけを扱う)。
 * だから GPU 無しで単体テストできる。GPU 完了の取得は GpuCompletionTracker。
 */

#ifndef MVM_GPU_PREVIEW_GPU_COMPLETION_H
#define MVM_GPU_PREVIEW_GPU_COMPLETION_H

#include "media/gpu_preview/d3d11_shared_device.h"

// ID3D11Fence / ID3D11DeviceContext4 / ID3D11Query を実体で使う。
//
// **前方宣言では書けない。** namespace mvm::gpu の中で
// `struct ID3D11Fence*` と書くと、グローバルの COM interface ではなく
// mvm::gpu::ID3D11Fence という別の不完全型を宣言してしまう。
// 実際にそれで `invalid use of incomplete type` になった。
#include <cstddef>
#include <d3d11_4.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace mvm::gpu {

// GPU 完了の取得手段。JSON に出す (gpu_completion_backend)。
enum class GpuCompletionBackend {
    None = 0,
    Fence,      // ID3D11Fence + ID3D11DeviceContext4::Signal (第一候補)
    EventQuery, // D3D11_QUERY_EVENT + End / GetData (fallback)
};

const char* toString(GpuCompletionBackend b);

// --------------------------------------------------------------------------
// GpuRetirementQueue (純粋。GPU に依存しない)
// --------------------------------------------------------------------------
// 「この serial が完了するまで解放しない」対象を貯めておく。
// 対象は frame の lifetime token でも、SRV / texture を握った holder でも、
// shared_ptr<void> でありさえすればよい。
class GpuRetirementQueue {
public:
    // serial が完了するまで payload を保持する。
    // payload は必ず有効であること (空を retire しても意味が無い)。
    void retire(unsigned long long serial, std::shared_ptr<void> payload);

    // completedSerial 以下の serial の payload を解放する。
    // per-frame で呼ぶ。blocking wait はしない。
    // 返り値は解放した件数。
    size_t poll(unsigned long long completedSerial);

    // shutdown 用。completed を返す関数を渡し、空になるまで poll する。
    // timeoutMs 以内に空にならなければ **false** を返す (fail-closed)。
    // timeout したら retirement_timeout_count を 1 増やす。
    // 残った payload は破棄せずに保持したまま返す (呼び出し側が扱いを決める)。
    bool drain(const std::function<unsigned long long()>& polledCompleted, int timeoutMs);

    size_t depthCurrent() const;
    size_t depthPeak() const;
    long long retirementTimeoutCount() const;
    long long forcedGpuWaitCount() const;
    // **必ず 0**。完了していない serial を poll が解放したら増える (設計上起きない)。
    long long framesReleasedBeforeCompletion() const;

    // 診断用に forced wait を 1 回数える (呼び出し側が blocking wait を
    // 余儀なくされたときだけ使う。通常経路では呼ばない)。
    void noteForcedGpuWait();

    // GPU 完了を確認できないまま payload を手放す。**契約違反の経路**。
    // drain が timeout した後など、これ以上待てないときにだけ呼ぶ。
    // 手放した件数を framesReleasedBeforeCompletion() に足す。
    // JSON では必ず 0 であることを検査する (0 でなければ不合格)。
    size_t releaseWithoutCompletion();

    // デストラクタでも未完了分を数える。黙って消えると
    // 「0 件だった」と「数え忘れた」の区別がつかない。
    ~GpuRetirementQueue();

private:
    struct Item {
        unsigned long long serial = 0;
        std::shared_ptr<void> payload;
    };

    mutable std::mutex mutex_;
    std::deque<Item> items_; // serial 昇順で積まれる
    size_t depthPeak_ = 0;
    long long timeoutCount_ = 0;
    long long forcedWaitCount_ = 0;
    long long releasedBeforeCompletion_ = 0;
    unsigned long long lastCompleted_ = 0;
};

// --------------------------------------------------------------------------
// GpuCompletionTracker (GPU 依存)
// --------------------------------------------------------------------------
class GpuCompletionTracker {
public:
    GpuCompletionTracker() = default;
    ~GpuCompletionTracker();

    GpuCompletionTracker(const GpuCompletionTracker&) = delete;
    GpuCompletionTracker& operator=(const GpuCompletionTracker&) = delete;

    // fence を試し、無ければ event query へ降りる。両方失敗したら false。
    bool initialize(SharedD3D11Device& device, std::string& err);
    void release();

    bool ready() const { return backend_ != GpuCompletionBackend::None; }

    GpuCompletionBackend backend() const { return backend_; }

    // 直前に発行した描画コマンドの完了を GPU へ signal させ、その serial を返す。
    // **draw の直後に 1 回だけ呼ぶ。** D3D11 lock は内部で取る。
    unsigned long long signalSubmission();

    // 現在完了している serial を poll する。blocking wait しない。
    unsigned long long polledCompleted();

    // これまでに signal した最大 serial。
    unsigned long long submittedSerial() const { return submitted_; }

private:
    void releaseEventQueries();

    SharedD3D11Device* shared_ = nullptr;
    GpuCompletionBackend backend_ = GpuCompletionBackend::None;

    // --- fence backend ---
    ID3D11Fence* fence_ = nullptr;
    ID3D11DeviceContext4* context4_ = nullptr;

    // --- event query backend ---
    struct PendingQuery {
        unsigned long long serial = 0;
        ID3D11Query* query = nullptr;
    };

    std::deque<PendingQuery> pendingQueries_;
    std::deque<ID3D11Query*> freeQueries_;

    unsigned long long submitted_ = 0;
    unsigned long long completed_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_GPU_COMPLETION_H
