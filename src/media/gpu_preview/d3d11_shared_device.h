/*
 * mvm Phase 1 / P1 - Qt と FFmpeg が共有する D3D11 device
 *
 * P1 の主要仮説はここにある。
 * 「Qt Quick が使う ID3D11Device と FFmpeg の decode device を同一にできるか」。
 *
 * このクラスは Qt からもらった device / context を受け取り、
 *   - 共有に必要な設定 (ID3D10Multithread) を施し
 *   - adapter の実体 (LUID / description / feature level) を調べ
 *   - decode thread と render thread の直列化用 lock を提供する
 * ところまでを持つ。Qt にも FFmpeg にも依存しない。
 */

#ifndef MVM_GPU_PREVIEW_D3D11_SHARED_DEVICE_H
#define MVM_GPU_PREVIEW_D3D11_SHARED_DEVICE_H

#include <array>
#include <atomic>
#include <d3d11.h>
#include <mutex>
#include <string>
#include <vector>

namespace mvm::gpu {

// adapter の実体。設定値ではなく、device から遡って取得した値だけを入れる。
struct AdapterInfo {
    bool valid = false;
    unsigned int luidLow = 0;
    int luidHigh = 0;
    unsigned int vendorId = 0;
    unsigned int deviceId = 0;
    int featureLevel = 0; // D3D_FEATURE_LEVEL の生値
    std::string description;

    bool sameAdapterAs(const AdapterInfo& other) const {
        return valid && other.valid && luidLow == other.luidLow && luidHigh == other.luidHigh;
    }
};

// ID3D11Device から adapter 情報を取り出す。
// FFmpeg 側の adapter は **decode texture から GetDevice で遡って**調べる。
// 設定値を照合しても「本当に同じ adapter で decode されたか」は分からない。
bool queryAdapterInfo(ID3D11Device* device, AdapterInfo& out, std::string& err);

// 再帰 mutex。ID3D11DeviceContext は thread-safe ではないので、
// decode thread と render thread の両方がこれを取る。
// FFmpeg の AVD3D11VADeviceContext::lock / unlock にも同じものを渡す
// (FFmpeg は再帰的にロックする。非再帰 mutex では自己デッドロックする)。
enum class D3D11LockRole { Unknown = 0, Render = 1, DecoderA = 2, DecoderB = 3 };

struct D3D11LockTimingSnapshot {
    std::vector<double> renderWaitUs;
    std::vector<double> decoderAWaitUs;
    std::vector<double> decoderBWaitUs;
};

class D3D11Lock {
public:
    void lock() {
        if (!diagnosticsEnabled_.load(std::memory_order_relaxed)) {
            mutex_.lock();
            return;
        }
        LARGE_INTEGER begin{};
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&begin);
        mutex_.lock();
        QueryPerformanceCounter(&end);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const double waitUs = static_cast<double>(end.QuadPart - begin.QuadPart) * 1000000.0 /
                              static_cast<double>(frequency.QuadPart);
        const D3D11LockRole role = currentRole();
        if (role == D3D11LockRole::Unknown)
            return;
        std::lock_guard<std::mutex> timingLock(timingMutex_);
        waits_[static_cast<size_t>(role)].push_back(waitUs);
    }

    void unlock() { mutex_.unlock(); }

    static void lockCallback(void* ctx) { static_cast<D3D11Lock*>(ctx)->lock(); }

    static void unlockCallback(void* ctx) { static_cast<D3D11Lock*>(ctx)->unlock(); }

    static D3D11LockRole setCurrentRole(D3D11LockRole role) {
        D3D11LockRole& current = currentRoleStorage();
        const D3D11LockRole previous = current;
        current = role;
        return previous;
    }

    void beginDiagnostics() {
        std::lock_guard<std::mutex> lock(timingMutex_);
        for (auto& values : waits_) {
            values.clear();
            values.reserve(8192);
        }
        diagnosticsEnabled_.store(true, std::memory_order_release);
    }

    D3D11LockTimingSnapshot endDiagnostics() {
        diagnosticsEnabled_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(timingMutex_);
        return {waits_[1], waits_[2], waits_[3]};
    }

private:
    static D3D11LockRole& currentRoleStorage() {
        static thread_local D3D11LockRole role = D3D11LockRole::Unknown;
        return role;
    }

    static D3D11LockRole currentRole() { return currentRoleStorage(); }

    std::recursive_mutex mutex_;
    std::atomic<bool> diagnosticsEnabled_{false};
    std::mutex timingMutex_;
    std::array<std::vector<double>, 4> waits_;
};

class D3D11LockRoleScope {
public:
    explicit D3D11LockRoleScope(D3D11LockRole role) : previous_(D3D11Lock::setCurrentRole(role)) {}

    ~D3D11LockRoleScope() { D3D11Lock::setCurrentRole(previous_); }

private:
    D3D11LockRole previous_;
};

class SharedD3D11Device {
public:
    SharedD3D11Device() = default;
    ~SharedD3D11Device();

    SharedD3D11Device(const SharedD3D11Device&) = delete;
    SharedD3D11Device& operator=(const SharedD3D11Device&) = delete;

    // Qt (QRhiD3D11NativeHandles) から得た device / context を受け取る。
    // 参照を AddRef して保持し、context に
    // ID3D10Multithread::SetMultithreadProtected(TRUE) を設定する。
    //
    // **SetMultithreadProtected に失敗したら成功を返さない。**
    // 保護なしで共有すると、動いてしまう run と壊れる run が混ざり、
    // 一番診断しづらい形の不具合になる。
    bool adopt(ID3D11Device* device, ID3D11DeviceContext* context, std::string& err);

    void release();

    bool valid() const { return device_ != nullptr; }

    ID3D11Device* device() const { return device_; }

    ID3D11DeviceContext* context() const { return context_; }

    const AdapterInfo& adapter() const { return adapter_; }

    D3D11Lock& lock() { return lock_; }

    // multithread protection が実際に有効になったか (診断用に JSON へ出す)。
    bool multithreadProtected() const { return multithreadProtected_; }

    // device lost の検出。GetDeviceRemovedReason が S_OK 以外なら失われている。
    bool deviceLost(long& reasonOut) const;

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    AdapterInfo adapter_;
    D3D11Lock lock_;
    bool multithreadProtected_ = false;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_D3D11_SHARED_DEVICE_H
