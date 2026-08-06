/*
 * mvm Phase 1 / P1.1 - decode thread の駆動
 *
 * Qt に依存しない。命令は mailbox 1 つに集約する
 * (再生 / 一時停止 / 1 コマ送り / seek / 停止)。
 *
 * decode thread の責務はこれだけである。
 *   - 再生中なら次の frame を decode して queue へ submit する
 *   - queue が満杯なら待つ (落とさない。backpressure をかける)
 *   - seek 要求が来たら decoder を seek し、queue の generation を進める
 *
 * --------------------------------------------------------------------------
 * P1.1 §2: GUI から decoder 内部を無排他で読まない
 * --------------------------------------------------------------------------
 * P1 では `info()` / `decodeAdapter()` / `lastError()` が
 * decode thread が書き換えている実体への **const 参照**を返していた。
 * 「読むだけだから安全」ではない。std::string の書き換え中に読めば
 * 壊れた文字列を読むし、参照が指す先が再確保されれば dangling になる。
 *
 * したがって:
 *   - decoder_ には decoderMutex_ を持たずに触れない
 *   - GUI へ返すのは **値のコピー** (DecoderSnapshot) だけ
 *   - mutable object への const 参照は返さない
 *
 * snapshot は decode thread か decoderMutex_ 下でのみ更新する。
 */

#ifndef MVM_GPU_PREVIEW_DECODE_WORKER_H
#define MVM_GPU_PREVIEW_DECODE_WORKER_H

#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/preview_state.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mvm::gpu {

// seek 1 回の実測。p50 / p95 / max を出すために全件を持つ。
struct SeekSample {
    long long requestedFrame = -1;
    long long landedFrame = -1;
    // decode が終わった時点まで (診断)
    double decodeReadyMs = 0.0;
    // 実際に画面へ出た時点まで (**正式 gate はこちら**)
    double displayedMs = 0.0;
    bool ok = false;
    bool displayed = false;
};

// GUI へ渡す decoder の状態。**すべて値**。参照もポインタも持たない。
struct DecoderSnapshot {
    bool open = false;
    bool running = false;
    VideoStreamInfo info;
    AdapterInfo adapter;
    unsigned long long decodeDevicePointer = 0;
    GenerationId generation{};
    unsigned long long resourceEpoch = 0;
    long long decodedFrameCount = 0;
    long long decodeErrorCount = 0;
    long long softwareFrameRejectCount = 0;
    long long seekBackoffCount = 0;
    std::string lastError;
};

class DecodeWorker {
public:
    explicit DecodeWorker(PreviewState& state);
    ~DecodeWorker();

    DecodeWorker(const DecodeWorker&) = delete;
    DecodeWorker& operator=(const DecodeWorker&) = delete;

    // decoder を開いて decode thread を起動する。
    // device が ready でなければ失敗する (fail-closed)。
    bool start(const std::string& utf8Path, std::string& err);
    void stop();

    void play();
    void pause();
    void stepForward();

    // 同期 seek。呼び出し元 (GUI thread) をブロックして待つ。
    // decode が終わるまでの時間を返す (displayed までは呼び出し側が測る)。
    bool seekBlocking(long long frameNumber, double& decodeReadyMs, std::string& err);

    bool playing() const { return playing_.load(std::memory_order_relaxed); }

    bool eof() const { return eof_.load(std::memory_order_relaxed); }

    // **値のコピーを返す。** const 参照は返さない (§2)。
    DecoderSnapshot snapshot() const;

private:
    void run();
    // decoderMutex_ を保持した状態で呼ぶこと。
    void refreshSnapshotLocked();

    PreviewState& state_;
    std::unique_ptr<FFmpegD3D11Decoder> decoder_;

    std::thread thread_;
    mutable std::mutex mutex_; // 命令 mailbox
    // decoder 本体の排他。seekBlocking (GUI thread) と
    // decode loop が同時に decoder を触らないようにする。
    mutable std::mutex decoderMutex_;
    // snapshot 専用。decoderMutex_ を待たずに GUI が読めるようにする
    // (GUI の 100ms tick が decode を止めないため)。
    mutable std::mutex snapshotMutex_;
    DecoderSnapshot snapshot_;
    std::condition_variable wake_;

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    int stepsPending_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_DECODE_WORKER_H
