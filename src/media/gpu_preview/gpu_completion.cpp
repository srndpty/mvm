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

long long GpuRetirementQueue::framesReleasedBeforeCompletion() const {
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

bool GpuCompletionTracker::initialize(SharedD3D11Device& device, std::string& err) {
    release();
    if (!device.valid()) {
        err = "共有 D3D11 device が未初期化です";
        return false;
    }
    shared_ = &device;

    std::lock_guard<D3D11Lock> guard(shared_->lock());

    // --- 第一候補: ID3D11Fence ---------------------------------------------
    ID3D11Device5* dev5 = nullptr;
    ID3D11DeviceContext4* ctx4 = nullptr;
    HRESULT hr =
        shared_->device()->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&dev5));
    if (SUCCEEDED(hr) && dev5) {
        hr = shared_->context()->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                                reinterpret_cast<void**>(&ctx4));
        if (SUCCEEDED(hr) && ctx4) {
            hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                   reinterpret_cast<void**>(&fence_));
            if (SUCCEEDED(hr) && fence_) {
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

    // --- fallback: D3D11_QUERY_EVENT ---------------------------------------
    // event query は「End した時点までの GPU コマンドが完了したか」を返す。
    // fence が使えない環境 (古い driver / feature level) 向けの退避。
    // ここで 1 個作れることを確かめておく (作れないなら fail-closed)。
    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query* probe = nullptr;
    hr = shared_->device()->CreateQuery(&qd, &probe);
    if (FAILED(hr) || !probe) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "GPU 完了追跡を用意できません (fence も event query も不可, 0x%08lX)",
                      static_cast<unsigned long>(hr));
        err = buf;
        shared_ = nullptr;
        return false;
    }
    freeQueries_.push_back(probe);
    backend_ = GpuCompletionBackend::EventQuery;
    return true;
}

void GpuCompletionTracker::releaseEventQueries() {
    for (auto& p : pendingQueries_)
        if (p.query)
            p.query->Release();
    pendingQueries_.clear();
    for (auto* q : freeQueries_)
        if (q)
            q->Release();
    freeQueries_.clear();
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
}

unsigned long long GpuCompletionTracker::signalSubmission() {
    if (backend_ == GpuCompletionBackend::None || !shared_)
        return 0;

    std::lock_guard<D3D11Lock> guard(shared_->lock());
    submitted_++;

    if (backend_ == GpuCompletionBackend::Fence) {
        // GPU が queue 上のここまでを終えたら fence に submitted_ を書かせる。
        context4_->Signal(fence_, submitted_);
        return submitted_;
    }

    // event query: 未使用の query を End する。
    ID3D11Query* q = nullptr;
    if (!freeQueries_.empty()) {
        q = freeQueries_.front();
        freeQueries_.pop_front();
    } else if (pendingQueries_.size() < kMaxEventQueries) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_EVENT;
        shared_->device()->CreateQuery(&qd, &q);
    }
    if (!q) {
        // query を作れない / 上限。serial は進めるが query を積まない。
        // 次の poll で completed が追いつく (古い query が完了する) のを待つ。
        return submitted_;
    }
    shared_->context()->End(q);
    pendingQueries_.push_back(PendingQuery{submitted_, q});
    return submitted_;
}

unsigned long long GpuCompletionTracker::polledCompleted() {
    if (backend_ == GpuCompletionBackend::None || !shared_)
        return 0;

    std::lock_guard<D3D11Lock> guard(shared_->lock());

    if (backend_ == GpuCompletionBackend::Fence) {
        completed_ = fence_->GetCompletedValue();
        return completed_;
    }

    // event query: 先頭から順に、完了しているものだけ completed を進める。
    // **DONOTFLUSH** を渡す。ここで flush すると「毎 frame Flush で
    // 見かけ上解決」に等しくなり、計測を汚す (§1)。
    while (!pendingQueries_.empty()) {
        PendingQuery& p = pendingQueries_.front();
        const HRESULT hr =
            shared_->context()->GetData(p.query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr != S_OK)
            break; // まだ完了していない。以降も未完了とみなす
        completed_ = p.serial;
        freeQueries_.push_back(p.query);
        pendingQueries_.pop_front();
    }
    return completed_;
}

} // namespace mvm::gpu
