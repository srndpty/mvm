#include "media/gpu_preview/decode_worker.h"

#include "media/gpu_preview/qpc_clock.h"

#include <chrono>

namespace mvm::gpu {

DecodeWorker::DecodeWorker(PreviewState& state) : state_(state) {}

DecodeWorker::~DecodeWorker() {
    stop();
}

bool DecodeWorker::start(const std::string& utf8Path, std::string& err) {
    stop();

    if (!state_.deviceReady.load(std::memory_order_acquire)) {
        // device が無い状態で software decode へ落ちない。
        err = "D3D11 device がまだ準備できていません";
        return false;
    }

    decoder_ = std::make_unique<FFmpegD3D11Decoder>(state_.device, &state_.counters);
    if (!decoder_->open(utf8Path, err)) {
        decoder_.reset();
        return false;
    }
    info_ = decoder_->info();

    state_.queue.restart();
    state_.queue.clear();
    state_.queue.setExpectedDevice(state_.device.device());
    state_.queue.setCurrentGeneration(decoder_->generation());

    eof_.store(false);
    playing_.store(false);
    {
        std::lock_guard<std::mutex> g(mutex_);
        stepsPending_ = 1; // 開いた直後に 1 枚出す (黒いままにしない)
        lastError_.clear();
    }

    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void DecodeWorker::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable())
            thread_.join();
        return;
    }
    state_.queue.stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();

    std::lock_guard<std::mutex> g(decoderMutex_);
    decoder_.reset();
}

void DecodeWorker::play() {
    playing_.store(true);
    wake_.notify_all();
}

void DecodeWorker::pause() {
    playing_.store(false);
    wake_.notify_all();
}

void DecodeWorker::stepForward() {
    {
        std::lock_guard<std::mutex> g(mutex_);
        stepsPending_++;
    }
    wake_.notify_all();
}

bool DecodeWorker::seekBlocking(long long frameNumber, double& elapsedMs, std::string& err) {
    elapsedMs = 0.0;
    std::lock_guard<std::mutex> g(decoderMutex_);
    if (!decoder_) {
        err = "decoder が開かれていません";
        return false;
    }

    const long long t0 = qpcTicks();

    // 表示側の generation を先に進める。
    // decode 中に飛ぶ前のフレームが submit されても弾かれる。
    if (!decoder_->seek(frameNumber, err)) {
        state_.queue.setCurrentGeneration(decoder_->generation());
        elapsedMs = qpcMsBetween(t0, qpcTicks());
        return false;
    }
    state_.queue.setCurrentGeneration(decoder_->generation());

    // seek は「目標フレームを decode し終えた時点」で完了とする。
    // packet を投げただけを seek 完了と呼ぶと、実測が実態と合わない。
    DecodedGpuFrame frame;
    const DecodeStatus st = decoder_->requestFrame(frame, err);
    elapsedMs = qpcMsBetween(t0, qpcTicks());
    if (st != DecodeStatus::Ok) {
        if (err.empty())
            err = std::string("seek 後の decode が ") + toString(st) + " でした";
        return false;
    }
    if (frame.frameNumber != frameNumber) {
        err = "seek 先が要求と違います (要求 " + std::to_string(frameNumber) + " / 着地 " +
              std::to_string(frame.frameNumber) + ")";
        return false;
    }

    // 表示させる。満杯でも seek 結果は捨てない (最新なので詰め替える)。
    if (state_.queue.submitFrame(frame) == SubmitResult::RejectedQueueFull) {
        state_.queue.clear();
        state_.queue.submitFrame(frame);
    }
    eof_.store(false);
    return true;
}

void DecodeWorker::run() {
    while (running_.load(std::memory_order_relaxed)) {
        bool doDecode = false;
        {
            std::unique_lock<std::mutex> g(mutex_);
            if (stepsPending_ > 0) {
                stepsPending_--;
                doDecode = true;
            } else if (playing_.load(std::memory_order_relaxed) &&
                       !eof_.load(std::memory_order_relaxed)) {
                doDecode = true;
            } else {
                wake_.wait_for(g, std::chrono::milliseconds(20));
                continue;
            }
        }
        if (!doDecode)
            continue;

        // 表示が追いつくまで待つ。**落として詰め込まない。**
        // ここで捨てると「decode は速いが表示は遅い」を隠してしまう。
        if (!state_.queue.waitForSpace(50))
            continue;

        std::lock_guard<std::mutex> g(decoderMutex_);
        if (!decoder_)
            break;

        DecodedGpuFrame frame;
        std::string err;
        const DecodeStatus st = decoder_->requestFrame(frame, err);
        if (st == DecodeStatus::Eof) {
            eof_.store(true);
            playing_.store(false);
            continue;
        }
        if (st == DecodeStatus::Again)
            continue;
        if (st != DecodeStatus::Ok) {
            std::lock_guard<std::mutex> g2(mutex_);
            lastError_ = err;
            playing_.store(false);
            continue;
        }

        const SubmitResult sr = state_.queue.submitFrame(frame);
        if (sr == SubmitResult::RejectedDeviceMismatch) {
            // 致命的。device 共有が壊れている。黙って続けない。
            std::lock_guard<std::mutex> g2(mutex_);
            lastError_ = "decode texture が表示側の device と一致しません";
            playing_.store(false);
        }
    }
}

const AdapterInfo& DecodeWorker::decodeAdapter() const {
    static const AdapterInfo empty;
    return decoder_ ? decoder_->decodeAdapter() : empty;
}

unsigned long long DecodeWorker::decodeDevicePointer() const {
    return decoder_ ? decoder_->decodeDevicePointer() : 0;
}

long long DecodeWorker::decodedFrameCount() const {
    return decoder_ ? decoder_->decodedFrameCount() : 0;
}

long long DecodeWorker::decodeErrorCount() const {
    return decoder_ ? decoder_->decodeErrorCount() : 0;
}

long long DecodeWorker::softwareFrameRejectCount() const {
    return decoder_ ? decoder_->softwareFrameRejectCount() : 0;
}

long long DecodeWorker::seekBackoffCount() const {
    return decoder_ ? decoder_->seekBackoffCount() : 0;
}

} // namespace mvm::gpu
