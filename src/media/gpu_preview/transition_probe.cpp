#include "media/gpu_preview/transition_probe.h"

#include "media/gpu_preview/qpc_clock.h"

#include <algorithm>
#include <thread>

// MVM_ALLOW_SMALL_REGION_READBACK
// Phase 4 transition専用の1x1 staging経路。full-frame fallbackは持たない。

namespace mvm::gpu {

const char* transitionProbePointName(TransitionProbePoint point) {
    return point == TransitionProbePoint::TL ? "TL" : "BR";
}

TransitionProbeSelector::TransitionProbeSelector(std::vector<long long> boundaries)
    : boundaries_(std::move(boundaries)), selected_(boundaries_.size(), false) {}

std::optional<long long> TransitionProbeSelector::select(long long actualOutputFrame) {
    for (size_t i = 0; i < boundaries_.size(); ++i) {
        if (!selected_[i] && actualOutputFrame >= boundaries_[i]) {
            selected_[i] = true;
            ++selectedCount_;
            return boundaries_[i];
        }
    }
    return std::nullopt;
}

AsyncTransitionProbeReadback::~AsyncTransitionProbeReadback() {
    release();
}

bool AsyncTransitionProbeReadback::initialize(SharedD3D11Device& device, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shared_ || !device.valid()) {
        err = "transition probe readbackの初期化状態が不正です";
        return false;
    }
    if (!completion_.initialize(device, err, GpuCompletionBackend::Fence))
        return false;
    shared_ = &device;
    return true;
}

bool AsyncTransitionProbeReadback::issue(ID3D11Texture2D* source,
                                         const TransitionProbeRequest& request,
                                         unsigned long long& ticket, std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    ticket = 0;
    if (!shared_ || !source || request.x < 0 || request.y < 0 || pending_.size() >= 4) {
        err = "transition probe requestまたはbounded slotが不正です";
        return false;
    }
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        request.x >= static_cast<int>(sourceDesc.Width) ||
        request.y >= static_cast<int>(sourceDesc.Height)) {
        err = "transition probe source/座標がRGBA8 target契約と一致しません";
        return false;
    }
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = stagingDesc.Height = 1;
    stagingDesc.MipLevels = stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    {
        std::lock_guard<D3D11Lock> deviceLock(shared_->lock());
        const HRESULT hr = shared_->device()->CreateTexture2D(&stagingDesc, nullptr, &staging);
        if (FAILED(hr) || !staging) {
            err = "transition probe staging textureの生成に失敗しました";
            return false;
        }
        const D3D11_BOX box{static_cast<UINT>(request.x),     static_cast<UINT>(request.y),     0,
                            static_cast<UINT>(request.x + 1), static_cast<UINT>(request.y + 1), 1};
        shared_->context()->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
    }
    const SubmissionResult submission = completion_.signalSubmission();
    if (!submission.tracked()) {
        ++counters_.untrackedSubmissionCount;
        // copy済みresourceはGPU完了を追跡できないため、release()まで保持してFAILする。
        pending_.push_back({request, nextTicket_++, kNeverCompletingSerial, staging});
        err = "transition probe copyをGPU completion serialで追跡できません";
        return false;
    }
    ticket = nextTicket_++;
    pending_.push_back({request, ticket, submission.serial, staging});
    ++counters_.issuedCount;
    return true;
}

bool AsyncTransitionProbeReadback::poll(std::vector<TransitionProbeResult>& completed,
                                        std::string& err) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shared_) {
        err = "transition probe readbackが初期化されていません";
        return false;
    }
    if (testDeferCompletionPollOnce_) {
        testDeferCompletionPollOnce_ = false;
        ++counters_.notReadyPollCount;
        return true;
    }
    const CompletionPollResult observed = completion_.polledCompleted();
    if (observed.status != CompletionPollStatus::Ok) {
        ++counters_.completionFailureCount;
        err = completion_.fatalReason().empty() ? "transition probe completion pollに失敗しました"
                                                : completion_.fatalReason();
        return false;
    }
    while (!pending_.empty() && pending_.front().serial <= observed.completed) {
        Pending item = std::move(pending_.front());
        pending_.pop_front();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr;
        {
            std::lock_guard<D3D11Lock> deviceLock(shared_->lock());
            // completion確認後もDO_NOT_WAITを指定し、blocking Map経路を構造的に持たない。
            hr = shared_->context()->Map(item.staging, 0, D3D11_MAP_READ,
                                         D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (SUCCEEDED(hr)) {
                const auto* p = static_cast<const unsigned char*>(mapped.pData);
                TransitionProbeResult result;
                result.request = item.request;
                result.ticket = item.ticket;
                result.completionSerial = item.serial;
                result.completionObserved = true;
                std::copy_n(p, 4, result.rgba.begin());
                completed.push_back(std::move(result));
                shared_->context()->Unmap(item.staging, 0);
            }
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            ++counters_.notReadyPollCount;
            pending_.push_front(std::move(item));
            break;
        }
        item.staging->Release();
        if (FAILED(hr)) {
            ++counters_.completionFailureCount;
            err = "completion確認後のtransition probe DO_NOT_WAIT Mapに失敗しました";
            return false;
        }
        ++counters_.completedCount;
    }
    if (!pending_.empty())
        ++counters_.notReadyPollCount;
    return true;
}

bool AsyncTransitionProbeReadback::drain(int timeoutMs,
                                         std::vector<TransitionProbeResult>& completed,
                                         std::string& err) {
    completion_.flushForShutdown();
    const long long start = qpcTicks();
    for (;;) {
        if (!poll(completed, err))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty()) {
                counters_.pendingAfterDrainCount = 0;
                return true;
            }
        }
        if (qpcMsBetween(start, qpcTicks()) >= timeoutMs) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++counters_.retirementTimeoutCount;
            counters_.pendingAfterDrainCount = pending_.size();
            err = "transition probeを有限時間でdrainできませんでした";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void AsyncTransitionProbeReadback::releasePending() {
    for (auto& item : pending_)
        if (item.staging)
            item.staging->Release();
    pending_.clear();
}

void AsyncTransitionProbeReadback::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.empty())
        counters_.pendingAfterDrainCount = pending_.size();
    releasePending();
    completion_.release();
    shared_ = nullptr;
}

TransitionProbeCounters AsyncTransitionProbeReadback::counters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return counters_;
}

} // namespace mvm::gpu
