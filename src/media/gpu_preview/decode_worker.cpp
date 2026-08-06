#include "media/gpu_preview/decode_worker.h"

#include "media/gpu_preview/qpc_clock.h"

#include <chrono>

namespace mvm::gpu {

DecodeWorker::DecodeWorker(PreviewState& state) : state_(state) {}

DecodeWorker::~DecodeWorker() {
    stop();
}

// decoderMutex_ を保持した状態で呼ぶこと。
// decoder_ を読むのはこの関数と decode 経路だけにする (§2)。
void DecodeWorker::refreshSnapshotLocked() {
    DecoderSnapshot s;
    s.running = running_.load(std::memory_order_relaxed);
    if (decoder_) {
        s.open = true;
        s.info = decoder_->info();
        s.adapter = decoder_->decodeAdapter();
        s.decodeDevicePointer = decoder_->decodeDevicePointer();
        s.generation = decoder_->generationId();
        s.resourceEpoch = decoder_->resourceEpoch();
        s.decodedFrameCount = decoder_->decodedFrameCount();
        s.decodeErrorCount = decoder_->decodeErrorCount();
        s.softwareFrameRejectCount = decoder_->softwareFrameRejectCount();
        s.seekBackoffCount = decoder_->seekBackoffCount();
    }
    std::lock_guard<std::mutex> g(snapshotMutex_);
    // lastError は decode ループが別に書き込む。上書きして消さない。
    s.lastError = snapshot_.lastError;
    snapshot_ = std::move(s);
}

DecoderSnapshot DecodeWorker::snapshot() const {
    std::lock_guard<std::mutex> g(snapshotMutex_);
    return snapshot_; // **値のコピー**を返す。参照は返さない
}

bool DecodeWorker::start(const std::string& utf8Path, std::string& err) {
    stop();

    if (!state_.deviceReady.load(std::memory_order_acquire)) {
        // device が無い状態で software decode へ落ちない。
        err = "D3D11 device がまだ準備できていません";
        return false;
    }

    {
        std::lock_guard<std::mutex> g(snapshotMutex_);
        snapshot_ = DecoderSnapshot{};
    }

    auto decoder = std::make_unique<FFmpegD3D11Decoder>(state_.device, &state_.counters);
    if (!decoder->open(utf8Path, err))
        return false;

    {
        std::lock_guard<std::mutex> g(decoderMutex_);
        decoder_ = std::move(decoder);
    }

    state_.queue.restart();
    state_.queue.clear();
    state_.queue.setExpectedDevice(state_.device.device());
    {
        std::lock_guard<std::mutex> g(decoderMutex_);
        state_.queue.setCurrentGeneration(decoder_->generationId());
    }

    eof_.store(false);
    playing_.store(false);
    {
        std::lock_guard<std::mutex> g(mutex_);
        stepsPending_ = 1; // 開いた直後に 1 枚出す (黒いままにしない)
    }

    running_.store(true);
    {
        std::lock_guard<std::mutex> g(decoderMutex_);
        refreshSnapshotLocked();
    }
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
    refreshSnapshotLocked();
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

bool DecodeWorker::seekBlocking(long long frameNumber, double& decodeReadyMs, std::string& err) {
    decodeReadyMs = 0.0;
    std::lock_guard<std::mutex> g(decoderMutex_);
    if (!decoder_) {
        err = "decoder が開かれていません";
        return false;
    }

    const long long t0 = qpcTicks();

    // 表示側の generation を先に進める。
    // decode 中に飛ぶ前のフレームが submit されても弾かれる。
    if (!decoder_->seek(frameNumber, err)) {
        state_.queue.setCurrentGeneration(decoder_->generationId());
        decodeReadyMs = qpcMsBetween(t0, qpcTicks());
        refreshSnapshotLocked();
        return false;
    }
    state_.queue.setCurrentGeneration(decoder_->generationId());

    // seek は「目標フレームを decode し終えた時点」を decode-ready とする。
    // packet を投げただけを seek 完了と呼ぶと、実測が実態と合わない。
    // **画面に出るまで**は呼び出し側が別に測る (§5)。
    DecodedGpuFrame frame;
    const DecodeStatus st = decoder_->requestFrame(frame, err);
    decodeReadyMs = qpcMsBetween(t0, qpcTicks());
    if (st != DecodeStatus::Ok) {
        if (err.empty())
            err = std::string("seek 後の decode が ") + toString(st) + " でした";
        refreshSnapshotLocked();
        return false;
    }
    if (frame.frameNumber != frameNumber) {
        err = "seek 先が要求と違います (要求 " + std::to_string(frameNumber) + " / 着地 " +
              std::to_string(frame.frameNumber) + ")";
        refreshSnapshotLocked();
        return false;
    }

    // 表示させる。満杯でも seek 結果は捨てない (最新なので詰め替える)。
    if (state_.queue.submitFrame(frame) == SubmitResult::RejectedQueueFull) {
        state_.queue.clear();
        state_.queue.submitFrame(frame);
    }
    eof_.store(false);
    refreshSnapshotLocked();
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
            refreshSnapshotLocked();
            continue;
        }
        if (st == DecodeStatus::Again)
            continue;
        if (st != DecodeStatus::Ok) {
            {
                std::lock_guard<std::mutex> g2(snapshotMutex_);
                snapshot_.lastError = err;
            }
            playing_.store(false);
            refreshSnapshotLocked();
            continue;
        }

        const SubmitResult sr = state_.queue.submitFrame(frame);
        if (sr == SubmitResult::RejectedDeviceMismatch) {
            // 致命的。device 共有が壊れている。黙って続けない。
            {
                std::lock_guard<std::mutex> g2(snapshotMutex_);
                snapshot_.lastError = "decode texture が表示側の device と一致しません";
            }
            playing_.store(false);
        }
        refreshSnapshotLocked();
    }
}

} // namespace mvm::gpu
