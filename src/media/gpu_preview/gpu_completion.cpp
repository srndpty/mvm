#include "media/gpu_preview/gpu_completion.h"

#include "media/gpu_preview/qpc_clock.h"

#include <cstdio>
#include <d3d11_4.h>
#include <thread>

namespace mvm::gpu {

const char* toString(GpuCompletionBackend b) {
    switch (b) {
    case GpuCompletionBackend::None:
        return "none";
    case GpuCompletionBackend::Fence:
        return "fence";
    case GpuCompletionBackend::EventQuery:
        return "event_query";
    }
    return "unknown";
}

const char* toString(SubmissionStatus st) {
    switch (st) {
    case SubmissionStatus::Ok:
        return "ok";
    case SubmissionStatus::NotTracked:
        return "not_tracked";
    case SubmissionStatus::Failed:
        return "failed";
    }
    return "unknown";
}

const char* toString(CompletionPollStatus st) {
    switch (st) {
    case CompletionPollStatus::Ok:
        return "ok";
    case CompletionPollStatus::Failed:
        return "failed";
    case CompletionPollStatus::DeviceRemoved:
        return "device_removed";
    }
    return "unknown";
}

// --------------------------------------------------------------------------
// EventQueryLedger (純粋)
// --------------------------------------------------------------------------
EventQueryLedger::EventQueryLedger(size_t maxSlots) : maxSlots_(maxSlots == 0 ? 1 : maxSlots) {}

EventQueryLedger::Slot EventQueryLedger::acquireFreeSlot() {
    if (free_.empty())
        return nullptr;
    Slot s = free_.front();
    free_.pop_front();
    return s;
}

void EventQueryLedger::addSlot(Slot slot) {
    if (slot)
        free_.push_back(slot);
}

size_t EventQueryLedger::totalSlots() const {
    return pending_.size() + free_.size();
}

size_t EventQueryLedger::inFlight() const {
    return pending_.size();
}

bool EventQueryLedger::atCapacity() const {
    return totalSlots() >= maxSlots_ && free_.empty();
}

unsigned long long EventQueryLedger::confirmSubmission(Slot slot) {
    // **slot がなければ serial を確定しない。**
    // 確定してしまうと、追跡できない serial を「完了した」と扱う経路が生まれる。
    if (!slot)
        return 0;
    submitted_++;
    pending_.push_back(Pending{submitted_, slot});
    return submitted_;
}

bool EventQueryLedger::advanceCompleted(const std::function<int(Slot)>& isComplete) {
    // 先頭から順にしか進めない。**terminal serial を飛ばさない。**
    // GPU の完了順は投入順なので、先頭が未完了なら後ろも未完了とみなす。
    while (!pending_.empty()) {
        const int r = isComplete(pending_.front().slot);
        if (r < 0)
            return false; // fatal。completed は進めない
        if (r == 0)
            break; // まだ完了していない
        completed_ = pending_.front().serial;
        free_.push_back(pending_.front().slot);
        pending_.pop_front();
    }
    return true;
}

std::deque<EventQueryLedger::Slot> EventQueryLedger::takeAllSlots() {
    std::deque<Slot> all;
    for (auto& p : pending_)
        all.push_back(p.slot);
    pending_.clear();
    for (auto* s : free_)
        all.push_back(s);
    free_.clear();
    return all;
}

// --------------------------------------------------------------------------
// GpuRetirementQueue
// --------------------------------------------------------------------------
void GpuRetirementQueue::retire(unsigned long long serial, std::shared_ptr<void> payload) {
    if (!payload)
        return;
    std::lock_guard<std::mutex> g(mutex_);
    items_.push_back(Item{serial, std::move(payload)});
    if (items_.size() > depthPeak_)
        depthPeak_ = items_.size();
}

size_t GpuRetirementQueue::poll(unsigned long long completedSerial) {
    // 解放は payload の破棄 (deleter 実行) で行う。deleter が D3D11 を
    // 触ることがあるので、mutex を持ったまま実行しない。ローカルへ移してから抜ける。
    std::deque<std::shared_ptr<void>> toRelease;
    {
        std::lock_guard<std::mutex> g(mutex_);
        lastCompleted_ = completedSerial;
        while (!items_.empty() && items_.front().serial <= completedSerial) {
            toRelease.push_back(std::move(items_.front().payload));
            items_.pop_front();
        }
    }
    return toRelease.size(); // ここで toRelease が破棄され、payload が解放される
}

bool GpuRetirementQueue::drain(const std::function<unsigned long long()>& polledCompleted,
                               int timeoutMs) {
    const long long start = qpcTicks();
    for (;;) {
        poll(polledCompleted());
        {
            std::lock_guard<std::mutex> g(mutex_);
            if (items_.empty())
                return true;
        }
        if (qpcMsBetween(start, qpcTicks()) >= static_cast<double>(timeoutMs)) {
            std::lock_guard<std::mutex> g(mutex_);
            timeoutCount_++;
            return false; // fail-closed。残りは呼び出し側が扱う
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

size_t GpuRetirementQueue::depthCurrent() const {
    std::lock_guard<std::mutex> g(mutex_);
    return items_.size();
}

size_t GpuRetirementQueue::depthPeak() const {
    std::lock_guard<std::mutex> g(mutex_);
    return depthPeak_;
}

long long GpuRetirementQueue::retirementTimeoutCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return timeoutCount_;
}

long long GpuRetirementQueue::forcedGpuWaitCount() const {
    std::lock_guard<std::mutex> g(mutex_);
    return forcedWaitCount_;
}

long long GpuRetirementQueue::payloadsReleasedBeforeCompletion() const {
    std::lock_guard<std::mutex> g(mutex_);
    return releasedBeforeCompletion_;
}

size_t GpuRetirementQueue::releaseWithoutCompletion() {
    std::deque<std::shared_ptr<void>> toRelease;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> g(mutex_);
        n = items_.size();
        for (auto& it : items_)
            toRelease.push_back(std::move(it.payload));
        items_.clear();
        releasedBeforeCompletion_ += static_cast<long long>(n);
    }
    return n;
}

GpuRetirementQueue::~GpuRetirementQueue() {
    // ここに残っているのは「GPU 完了を確認できていない payload」である。
    // 黙って解放すると contract 違反が観測できなくなる。数えてから解放する。
    releaseWithoutCompletion();
}

void GpuRetirementQueue::noteForcedGpuWait() {
    std::lock_guard<std::mutex> g(mutex_);
    forcedWaitCount_++;
}

// --------------------------------------------------------------------------
// GpuCompletionTracker
// --------------------------------------------------------------------------
namespace {
constexpr size_t kMaxEventQueries = 256; // event query backend の上限 (有界に保つ)
}

GpuCompletionTracker::~GpuCompletionTracker() {
    release();
}

void GpuCompletionTracker::noteFatal(const std::string& reason) {
    if (!fatal_) {
        fatal_ = true;
        fatalReason_ = reason;
    }
}

bool GpuCompletionTracker::initialize(SharedD3D11Device& device, std::string& err,
                                      GpuCompletionBackend preferred) {
    release();
    if (!device.valid()) {
        err = "共有 D3D11 device が未初期化です";
        return false;
    }
    shared_ = &device;

    std::lock_guard<D3D11Lock> guard(shared_->lock());

    // preferred == EventQuery のときは fence を試さない。
    // fallback 経路を実際に走らせるためのテスト用オプションである。
    // --- 第一候補: ID3D11Fence ---------------------------------------------
    if (preferred != GpuCompletionBackend::EventQuery) {
        ID3D11Device5* dev5 = nullptr;
        ID3D11DeviceContext4* ctx4 = nullptr;
        HRESULT fenceHr = shared_->device()->QueryInterface(__uuidof(ID3D11Device5),
                                                            reinterpret_cast<void**>(&dev5));
        if (SUCCEEDED(fenceHr) && dev5) {
            fenceHr = shared_->context()->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                                         reinterpret_cast<void**>(&ctx4));
            if (SUCCEEDED(fenceHr) && ctx4) {
                fenceHr = dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                            reinterpret_cast<void**>(&fence_));
                if (SUCCEEDED(fenceHr) && fence_) {
                    context4_ = ctx4;
                    ctx4 = nullptr;
                    backend_ = GpuCompletionBackend::Fence;
                }
            }
        }
        if (dev5)
            dev5->Release();
        if (ctx4)
            ctx4->Release();

        if (backend_ == GpuCompletionBackend::Fence)
            return true;
    }

    // --- fallback: D3D11_QUERY_EVENT ---------------------------------------
    // event query は「End した時点までの GPU コマンドが完了したか」を返す。
    // fence が使えない環境 (古い driver / feature level) 向けの退避。
    // ここで 1 個作れることを確かめておく (作れないなら fail-closed)。
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query* probe = nullptr;
    HRESULT hr = shared_->device()->CreateQuery(&qd, &probe);
    if (FAILED(hr) || !probe) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "GPU 完了追跡を用意できません (fence も event query も不可, 0x%08lX)",
                      static_cast<unsigned long>(hr));
        err = buf;
        shared_ = nullptr;
        return false;
    }
    ledger_.addSlot(probe);
    backend_ = GpuCompletionBackend::EventQuery;
    return true;
}

void GpuCompletionTracker::releaseEventQueries() {
    for (auto* slot : ledger_.takeAllSlots())
        if (slot)
            static_cast<ID3D11Query*>(slot)->Release();
    ledger_ = EventQueryLedger{256};
}

void GpuCompletionTracker::release() {
    if (fence_) {
        fence_->Release();
        fence_ = nullptr;
    }
    if (context4_) {
        context4_->Release();
        context4_ = nullptr;
    }
    releaseEventQueries();
    backend_ = GpuCompletionBackend::None;
    shared_ = nullptr;
    submitted_ = 0;
    completed_ = 0;
    fatal_ = false;
    fatalReason_.clear();
    untracked_ = 0;
    deviceRemoved_ = 0;
}

SubmissionResult GpuCompletionTracker::signalSubmission() {
    if (backend_ == GpuCompletionBackend::None || !shared_)
        return SubmissionResult{SubmissionStatus::Failed, 0};
    if (fatal_)
        return SubmissionResult{SubmissionStatus::Failed, 0};

    std::lock_guard<D3D11Lock> guard(shared_->lock());

    if (backend_ == GpuCompletionBackend::Fence) {
        // **Signal の HRESULT を検査する。** P1.1 は戻り値を捨てていた。
        // 失敗したのに serial を進めると、追跡できない serial で retire する。
        const unsigned long long next = submitted_ + 1;
        const HRESULT hr = context4_->Signal(fence_, next);
        if (FAILED(hr)) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "ID3D11DeviceContext4::Signal に失敗 (0x%08lX)",
                          static_cast<unsigned long>(hr));
            noteFatal(buf);
            return SubmissionResult{SubmissionStatus::Failed, 0};
        }
        submitted_ = next;
        return SubmissionResult{SubmissionStatus::Ok, submitted_};
    }

    // --- event query --------------------------------------------------------
    // 空きが無ければ、まず poll して完了した slot を回収する。
    EventQueryLedger::Slot slot = ledger_.acquireFreeSlot();
    if (!slot) {
        CompletionPollResult pr = polledCompleted();
        if (pr.status != CompletionPollStatus::Ok)
            return SubmissionResult{SubmissionStatus::Failed, 0};
        slot = ledger_.acquireFreeSlot();
    }
    if (!slot && !ledger_.atCapacity()) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        ID3D11Query* q = nullptr;
        const HRESULT hr = shared_->device()->CreateQuery(&qd, &q);
        if (FAILED(hr) || !q) {
            // 作れないなら追跡できない。**serial を進めない。**
            untracked_++;
            return SubmissionResult{SubmissionStatus::NotTracked, 0};
        }
        slot = q;
    }
    if (!slot) {
        // 上限に達し、poll しても空かなかった。fail-closed。
        untracked_++;
        return SubmissionResult{SubmissionStatus::NotTracked, 0};
    }

    // End は戻り値を返さないが、直前の CreateQuery が成功していることと
    // slot が有効であることが確定してから呼ぶ。
    shared_->context()->End(static_cast<ID3D11Query*>(slot));
    const unsigned long long serial = ledger_.confirmSubmission(slot);
    if (serial == 0) {
        untracked_++;
        return SubmissionResult{SubmissionStatus::NotTracked, 0};
    }
    submitted_ = serial;
    return SubmissionResult{SubmissionStatus::Ok, serial};
}

CompletionPollResult GpuCompletionTracker::polledCompleted() {
    if (backend_ == GpuCompletionBackend::None || !shared_)
        return CompletionPollResult{CompletionPollStatus::Failed, 0};

    std::lock_guard<D3D11Lock> guard(shared_->lock());

    // device が失われていれば、それ以上の完了は来ない。明示的に記録する。
    long removedReason = 0;
    if (shared_->deviceLost(removedReason)) {
        deviceRemoved_++;
        noteFatal("device が失われました (GetDeviceRemovedReason)");
        return CompletionPollResult{CompletionPollStatus::DeviceRemoved, completed_};
    }

    if (backend_ == GpuCompletionBackend::Fence) {
        completed_ = fence_->GetCompletedValue();
        return CompletionPollResult{CompletionPollStatus::Ok, completed_};
    }

    // event query: 先頭から順に、完了しているものだけ completed を進める。
    // **DONOTFLUSH** を渡す。ここで flush すると「毎 frame Flush で
    // 見かけ上解決」に等しくなり、計測を汚す (§1)。
    //
    // GetData の戻り値は 3 通りある。混ぜない。
    //   S_OK      完了
    //   S_FALSE   未完了
    //   FAILED    致命的
    bool fatalHere = false;
    const bool ok = ledger_.advanceCompleted([&](EventQueryLedger::Slot slot) -> int {
        const HRESULT hr = shared_->context()->GetData(static_cast<ID3D11Query*>(slot), nullptr, 0,
                                                       D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
            return 1;
        if (hr == S_FALSE)
            return 0;
        fatalHere = true;
        return -1;
    });
    if (!ok || fatalHere) {
        noteFatal("ID3D11DeviceContext::GetData が失敗しました");
        return CompletionPollResult{CompletionPollStatus::Failed, completed_};
    }
    completed_ = ledger_.completedSerial();
    return CompletionPollResult{CompletionPollStatus::Ok, completed_};
}

void GpuCompletionTracker::flushForShutdown() {
    if (backend_ == GpuCompletionBackend::None || !shared_)
        return;
    std::lock_guard<D3D11Lock> guard(shared_->lock());
    // 投入済みのコマンドを GPU へ押し出す。これをしないと、
    // 以降 draw が来ない状況で最後の query / fence が完了しない。
    shared_->context()->Flush();
}

unsigned long long GpuCompletionTracker::polledCompletedSerial() {
    const CompletionPollResult r = polledCompleted();
    // 失敗時に「completed が進んだ」と見せない。fail-closed 側へ倒す。
    return r.status == CompletionPollStatus::Ok ? r.completed : 0;
}

} // namespace mvm::gpu
