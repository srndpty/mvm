/*
 * mvm Phase 1 / P1 - decode thread の駆動
 *
 * Qt に依存しない。命令は mailbox 1 つに集約する
 * (再生 / 一時停止 / 1 コマ送り / seek / 停止)。
 *
 * decode thread の責務はこれだけである。
 *   - 再生中なら次の frame を decode して queue へ submit する
 *   - queue が満杯なら待つ (落とさない。backpressure をかける)
 *   - seek 要求が来たら decoder を seek し、queue の generation を進める
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
    double elapsedMs = 0.0;
    bool ok = false;
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
    // 計測はこの経路で行う。非同期にすると「いつ着地したか」が測れない。
    bool seekBlocking(long long frameNumber, double& elapsedMs, std::string& err);

    bool playing() const { return playing_.load(std::memory_order_relaxed); }

    bool eof() const { return eof_.load(std::memory_order_relaxed); }

    const VideoStreamInfo& info() const { return info_; }

    const AdapterInfo& decodeAdapter() const;
    unsigned long long decodeDevicePointer() const;
    long long decodedFrameCount() const;
    long long decodeErrorCount() const;
    long long softwareFrameRejectCount() const;
    long long seekBackoffCount() const;

    const std::string& lastError() const { return lastError_; }

private:
    void run();

    PreviewState& state_;
    std::unique_ptr<FFmpegD3D11Decoder> decoder_;
    VideoStreamInfo info_;

    std::thread thread_;
    mutable std::mutex mutex_; // 命令 mailbox
    // decoder 本体の排他。seekBlocking (GUI thread) と
    // decode loop が同時に decoder を触らないようにする。
    std::mutex decoderMutex_;
    std::condition_variable wake_;

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> eof_{false};
    int stepsPending_ = 0;
    std::string lastError_;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_DECODE_WORKER_H
