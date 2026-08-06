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

// submission の結果 (P1.2 §4)。
//
// **serial だけを返してはいけない。** P1.1 は Signal / CreateQuery / End の
// 失敗を無視して serial を進めていた。追跡できていない serial で retire すると、
// 「GPU 完了を待った」ことにならないまま frame を解放する。
enum class SubmissionStatus {
    Ok = 0,     // serial は有効。完了を追跡できる
    NotTracked, // 追跡手段を確保できなかった (query 枯渇など)。serial は無効
    Failed,     // API が失敗した。以後は fail-closed
};

const char* toString(SubmissionStatus s);

// 決して完了しない serial。追跡できなかった submission の payload を
// これで retire すると、poll では絶対に解放されず、shutdown の drain で
// payloads_released_before_completion として必ず表面化する。
// **黙って解放するより、表に出す方を選ぶ。**
inline constexpr unsigned long long kNeverCompletingSerial = ~0ULL;

struct SubmissionResult {
    SubmissionStatus status = SubmissionStatus::Failed;
    // status == Ok のときだけ意味を持つ。それ以外は 0。
    unsigned long long serial = 0;

    bool tracked() const { return status == SubmissionStatus::Ok && serial != 0; }
};

// poll の結果。
enum class CompletionPollStatus {
    Ok = 0,        // completed は有効
    Failed,        // GetData が失敗した (fatal)
    DeviceRemoved, // device が失われた
};

const char* toString(CompletionPollStatus s);

struct CompletionPollResult {
    CompletionPollStatus status = CompletionPollStatus::Ok;
    unsigned long long completed = 0;
};

// --------------------------------------------------------------------------
// EventQueryLedger (純粋。D3D11 に依存しない)
// --------------------------------------------------------------------------
// event query backend の「slot 割り当て」と「completed serial の確定」だけを
// 取り出したもの。D3D11 の呼び出しは呼び出し側が注入する。
//
// なぜ分けるか: この state machine のバグ (serial を飛ばす / 上限で壊れる) は
// **実 GPU では再現させにくい**。純粋な状態機械にして決定論的に検査する。
//
// 契約:
//   - slot を確保できたときだけ serial を確定する (確保できなければ NotTracked)
//   - completed は先頭から順にしか進まない。**terminal serial を飛ばさない**
//   - 完了した slot は再利用する
//   - 上限に達したら poll して空きを作る。それでも無ければ fail-closed
class EventQueryLedger {
public:
    // slot は呼び出し側が用意する不透明な識別子 (実体は ID3D11Query*)。
    using Slot = void*;

    explicit EventQueryLedger(size_t maxSlots = 256);

    // 未使用 slot を 1 つ取り出す。無ければ nullptr。
    Slot acquireFreeSlot();
    // 新しく作った slot を登録する (acquire が nullptr を返したとき)。
    void addSlot(Slot slot);
    // 現在確保している slot の総数 (未使用 + 使用中)。
    size_t totalSlots() const;
    size_t inFlight() const;
    bool atCapacity() const;

    // serial を 1 つ進めて slot に結び付ける。**End が成功した後に呼ぶ。**
    unsigned long long confirmSubmission(Slot slot);

    // 先頭から順に「完了したか」を尋ね、完了した分だけ completed を進める。
    // isComplete は 1 (完了) / 0 (未完了) / -1 (失敗) を返す。
    // 失敗したら false を返し、completed は進めない (fail-closed)。
    bool advanceCompleted(const std::function<int(Slot)>& isComplete);

    unsigned long long submittedSerial() const { return submitted_; }

    unsigned long long completedSerial() const { return completed_; }

    // 使用中 slot を全部返す (shutdown 時の解放用)。
    std::deque<Slot> takeAllSlots();

private:
    struct Pending {
        unsigned long long serial = 0;
        Slot slot = nullptr;
    };

    std::deque<Pending> pending_;
    std::deque<Slot> free_;
    size_t maxSlots_;
    unsigned long long submitted_ = 0;
    unsigned long long completed_ = 0;
};

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
    // **必ず 0**。GPU 完了を確認できないまま手放した payload の数。
    //
    // 名前が payload なのは、ここに入るのが frame の lifetime token だけでは
    // ないからである (SRV / texture holder も同じ queue に入る)。
    // P1.1 の frames_released_before_completion は実態と合っていなかった (§6)。
    long long payloadsReleasedBeforeCompletion() const;

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
    //
    // preferred に EventQuery を渡すと fence を試さない。
    // **fallback 経路を実際に走らせるためのテスト用オプション**であり、
    // 通常経路では使わない (P1.2 §4)。
    bool initialize(SharedD3D11Device& device, std::string& err,
                    GpuCompletionBackend preferred = GpuCompletionBackend::Fence);
    void release();

    // API が失敗して以後追跡できない状態。fail-closed の判断に使う。
    bool fatal() const { return fatal_; }

    const std::string& fatalReason() const { return fatalReason_; }

    long long untrackedSubmissionCount() const { return untracked_; }

    long long deviceRemovedCount() const { return deviceRemoved_; }

    bool ready() const { return backend_ != GpuCompletionBackend::None; }

    GpuCompletionBackend backend() const { return backend_; }

    // 直前に発行した描画コマンドの完了を GPU へ signal させる。
    // **draw の直後に 1 回だけ呼ぶ。** D3D11 lock は内部で取る。
    //
    // 戻り値の status を必ず見ること。Ok 以外の serial で retire してはいけない。
    SubmissionResult signalSubmission();

    // 現在完了している serial を poll する。blocking wait しない。
    CompletionPollResult polledCompleted();

    // 呼び出し側の便宜用。失敗時は「何も完了していない」を返す
    // (完了を過大に見せない = fail-closed 側へ倒す)。
    unsigned long long polledCompletedSerial();

    // **shutdown の drain 直前に 1 度だけ呼ぶ。**
    //
    // event query は DONOTFLUSH で poll しているので、以降 GPU へ何も
    // 投入されないと最後の query が永久に未完了のままになる。
    // 実測: event query backend の soak で drain が timeout し、
    // payload が 3 件残った。
    //
    // これは「毎 frame Flush して見かけ上解決する」ことではない。
    // **終端で 1 度だけ**、もう後続の描画が来ないことが確定した時点で行う。
    // 通常の render 経路からは呼ばない。
    void flushForShutdown();

    // これまでに signal した最大 serial。
    unsigned long long submittedSerial() const { return submitted_; }

    unsigned long long completedSerial() const { return completed_; }

private:
    void releaseEventQueries();

    SharedD3D11Device* shared_ = nullptr;
    GpuCompletionBackend backend_ = GpuCompletionBackend::None;

    // --- fence backend ---
    ID3D11Fence* fence_ = nullptr;
    ID3D11DeviceContext4* context4_ = nullptr;

    // --- event query backend ---
    EventQueryLedger ledger_{256};

    void noteFatal(const std::string& reason);

    bool fatal_ = false;
    std::string fatalReason_;
    long long untracked_ = 0;
    long long deviceRemoved_ = 0;

    unsigned long long submitted_ = 0;
    unsigned long long completed_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_GPU_COMPLETION_H
