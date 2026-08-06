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

#include <d3d11.h>
#include <mutex>
#include <string>

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
class D3D11Lock {
public:
    void lock() { mutex_.lock(); }

    void unlock() { mutex_.unlock(); }

    static void lockCallback(void* ctx) { static_cast<D3D11Lock*>(ctx)->lock(); }

    static void unlockCallback(void* ctx) { static_cast<D3D11Lock*>(ctx)->unlock(); }

private:
    std::recursive_mutex mutex_;
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
