#include "spike_controller.h"

#include "media/gpu_preview/qpc_clock.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QScreen>
#include <QTextStream>
#include <QUrl>

#include <psapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>

namespace mvm::app {
namespace {

QString jsonEscape(const QString& s) {
    QString out;
    out.reserve(s.size() + 8);
    for (const QChar c : s) {
        switch (c.unicode()) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    if (v.size() == 1)
        return v.front();
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<size_t>(std::floor(idx));
    const auto hi = static_cast<size_t>(std::ceil(idx));
    const double t = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
}

double maxOf(const std::vector<double>& v) {
    return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
}

unsigned long long fileTimeTo100ns(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void processCpuTimes(unsigned long long& userOut, unsigned long long& kernelOut) {
    FILETIME c{}, e{}, k{}, u{};
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        kernelOut = fileTimeTo100ns(k);
        userOut = fileTimeTo100ns(u);
    }
}

}  // namespace

SpikeController::SpikeController(QObject* parent) : QObject(parent) {
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &SpikeController::tick);
}

SpikeController::~SpikeController() {
    // decode thread を先に止める。表示側より後に止めると
    // 解放済みの queue へ submit しうる。
    worker_.reset();
}

void SpikeController::attach(PreviewRhiItem* item) {
    item_ = item;
    state_ = item->state();
    worker_ = std::make_unique<gpu::DecodeWorker>(*state_);
    fpsTimer_.start();
    phaseTimer_.start();
    if (measure_.enabled)
        phase_ = Phase::WaitDevice;
    timer_.start();
}

qlonglong SpikeController::displayedFrame() const {
    return state_ ? state_->displayedFrameNumber.load(std::memory_order_relaxed) : -1;
}

qlonglong SpikeController::frameCount() const {
    return worker_ ? worker_->info().frameCount : -1;
}

bool SpikeController::playing() const {
    return worker_ && worker_->playing();
}

void SpikeController::openMedia(const QString& path) {
    if (!worker_ || !state_)
        return;
    QString local = path;
    if (local.startsWith(QStringLiteral("file:")))
        local = QUrl(local).toLocalFile();

    if (!state_->deviceReady.load(std::memory_order_acquire)) {
        status_ = QStringLiteral("D3D11 device がまだ準備できていません");
        Q_EMIT statusChanged();
        return;
    }

    startupLatencyTicks_ = gpu::qpcTicks();

    std::string err;
    if (!worker_->start(local.toStdString(), err)) {
        // fail-closed。software decode へは落ちない。
        status_ = QStringLiteral("開けません: ") + QString::fromStdString(err);
        errors_ << status_;
        exitCode_ = 3;
        Q_EMIT statusChanged();
        return;
    }

    mediaPath_ = local;
    requestedFrame_ = 0;
    status_ = QStringLiteral("読み込み完了");
    updateDeviceText();
    Q_EMIT statusChanged();
}

void SpikeController::play() {
    if (worker_)
        worker_->play();
    Q_EMIT statusChanged();
}

void SpikeController::pause() {
    if (worker_)
        worker_->pause();
    Q_EMIT statusChanged();
}

void SpikeController::stepForward() {
    if (worker_)
        worker_->stepForward();
    requestedFrame_++;
    Q_EMIT statusChanged();
}

void SpikeController::seekTo(qlonglong frame) {
    if (!worker_)
        return;
    requestedFrame_ = frame;
    double ms = 0.0;
    std::string err;
    if (!worker_->seekBlocking(frame, ms, err))
        status_ = QStringLiteral("seek 失敗: ") + QString::fromStdString(err);
    else
        status_ = QStringLiteral("seek %1 (%2 ms)").arg(frame).arg(ms, 0, 'f', 1);
    Q_EMIT statusChanged();
}

void SpikeController::updateDeviceText() {
    if (!state_)
        return;
    std::lock_guard<std::mutex> g(state_->infoMutex);
    const gpu::AdapterInfo& qt = state_->device.adapter();
    const gpu::AdapterInfo& dec = worker_ ? worker_->decodeAdapter() : qt;

    deviceText_ =
        QStringLiteral("backend=%1  adapter=%2  FL=0x%3\nQt LUID=%4:%5  decode LUID=%6:%7  %8")
            .arg(QString::fromStdString(state_->rhiBackend))
            .arg(QString::fromStdString(qt.description))
            .arg(qt.featureLevel, 0, 16)
            .arg(qt.luidHigh)
            .arg(qt.luidLow)
            .arg(dec.valid ? QString::number(dec.luidHigh) : QStringLiteral("-"))
            .arg(dec.valid ? QString::number(dec.luidLow) : QStringLiteral("-"))
            .arg(dec.valid ? (dec.sameAdapterAs(qt) ? QStringLiteral("同一 adapter")
                                                    : QStringLiteral("**別 adapter**"))
                           : QStringLiteral("(未 decode)"));
}

void SpikeController::tick() {
    if (!state_)
        return;

    // UI 表示用の fps。判定には使わない (判定は計測フェーズの実測値)。
    const long long displayed = state_->uniqueDisplayed.load(std::memory_order_relaxed);
    const qint64 elapsed = fpsTimer_.elapsed();
    if (elapsed >= 500) {
        fps_ = static_cast<double>(displayed - lastDisplayedCount_) * 1000.0 /
               static_cast<double>(elapsed);
        lastDisplayedCount_ = displayed;
        fpsTimer_.restart();
    }

    counterText_ =
        QStringLiteral(
            "decoded=%1  displayed=%2  repeated=%3  queue=%4\n"
            "CPU full-frame readback=%5  marker band readback=%6  GPU copy=%7  device lost=%8")
            .arg(worker_ ? worker_->decodedFrameCount() : 0)
            .arg(displayed)
            .arg(state_->repeatedPresents.load(std::memory_order_relaxed))
            .arg(state_->queue.depth())
            .arg(state_->counters.fullFrameReadbacks())
            .arg(state_->counters.markerBandReadbacks())
            .arg(state_->counters.gpuCopies())
            .arg(state_->deviceLostCount.load(std::memory_order_relaxed));

    if (state_->initFailed.load(std::memory_order_relaxed) && exitCode_ == 0) {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        status_ = QStringLiteral("初期化失敗: ") + QString::fromStdString(state_->initError);
        errors_ << status_;
        exitCode_ = 5;
        if (measure_.enabled) {
            phase_ = Phase::Done;
            writeJson();
            Q_EMIT finished();
            return;
        }
    }

    updateDeviceText();
    if (item_)
        item_->refreshStatus();
    Q_EMIT statusChanged();

    if (measure_.enabled)
        advanceMeasurement();
}

// --------------------------------------------------------------------------
// 計測
// --------------------------------------------------------------------------

void SpikeController::advanceMeasurement() {
    // marker 検査と seek 計測は processEvents を回す。
    // その間に QTimer が発火してここへ再入すると、同じフェーズを二重に走らせる。
    if (inPhase_)
        return;
    struct Guard {
        bool& f;
        ~Guard() { f = false; }
    } guard{inPhase_};
    inPhase_ = true;

    switch (phase_) {
    case Phase::WaitDevice: {
        if (!state_->deviceReady.load(std::memory_order_acquire)) {
            if (phaseTimer_.elapsed() > 15000) {
                errors_ << QStringLiteral("D3D11 device が 15 秒以内に準備できませんでした");
                exitCode_ = 5;
                phase_ = Phase::Done;
                writeJson();
                Q_EMIT finished();
            }
            return;
        }
        openMedia(measure_.mediaPath);
        if (exitCode_ != 0) {
            phase_ = Phase::Done;
            writeJson();
            Q_EMIT finished();
            return;
        }
        play();
        phase_ = Phase::Warmup;
        phaseTimer_.restart();
        return;
    }

    case Phase::Warmup: {
        // startup latency = 開いてから最初の 1 枚が **表示された** まで。
        if (startupLatencyMs_ < 0 &&
            state_->uniqueDisplayed.load(std::memory_order_relaxed) > 0) {
            startupLatencyMs_ = gpu::qpcMsBetween(startupLatencyTicks_, gpu::qpcTicks());
        }
        if (worker_->eof()) {
            seekTo(0);
            play();
        }
        if (phaseTimer_.elapsed() < measure_.warmupMs)
            return;

        // 計測区間の基準を取り直す。warm-up の値を混ぜない。
        displayedAtStart_ = state_->uniqueDisplayed.load(std::memory_order_relaxed);
        repeatedAtStart_ = state_->repeatedPresents.load(std::memory_order_relaxed);
        presentAtStart_ = state_->presentCount.load(std::memory_order_relaxed);
        decodedAtStart_ = worker_->decodedFrameCount();
        {
            std::lock_guard<std::mutex> g(state_->intervalMutex);
            state_->frameIntervalsMs.clear();
        }
        processCpuTimes(cpuUserStart_, cpuKernelStart_);
        state_->collectIntervals.store(true, std::memory_order_relaxed);
        measureStartTicks_ = gpu::qpcTicks();
        phase_ = Phase::Measure;
        phaseTimer_.restart();
        return;
    }

    case Phase::Measure: {
        if (worker_->eof()) {
            // 素材が尽きたら先頭へ戻して測り続ける。回数は記録する。
            loopCount_++;
            seekTo(0);
            play();
        }
        if (phaseTimer_.elapsed() < measure_.measureMs)
            return;

        measureElapsedMs_ = gpu::qpcMsBetween(measureStartTicks_, gpu::qpcTicks());
        state_->collectIntervals.store(false, std::memory_order_relaxed);
        pause();
        // 計測区間の値をここで確定させる。
        // このあとの marker 検査と seek 計測でも decode は進むので、
        // JSON を書く時点で読むと計測区間の値ではなくなる。
        displayedInWindow_ = state_->uniqueDisplayed.load(std::memory_order_relaxed) -
                             displayedAtStart_;
        repeatedInWindow_ = state_->repeatedPresents.load(std::memory_order_relaxed) -
                            repeatedAtStart_;
        presentsInWindow_ = state_->presentCount.load(std::memory_order_relaxed) -
                            presentAtStart_;
        decodedInWindow_ = worker_->decodedFrameCount() - decodedAtStart_;
        processCpuTimes(cpuUserEnd_, cpuKernelEnd_);
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
        workingSetAtEnd_ = static_cast<long long>(pm.WorkingSetSize);
        privateUsageAtEnd_ = static_cast<long long>(pm.PrivateUsage);
        phase_ = Phase::Markers;
        phaseTimer_.restart();
        return;
    }

    case Phase::Markers: {
        runMarkerChecks();
        phase_ = Phase::Seeks;
        phaseTimer_.restart();
        return;
    }

    case Phase::Seeks: {
        runSeekBenchmark();
        phase_ = Phase::Done;
        writeJson();
        Q_EMIT finished();
        return;
    }

    case Phase::Idle:
    case Phase::Done:
        return;
    }
}

bool SpikeController::runMarkerChecks() {
    bool allOk = true;
    const long long total = worker_->info().frameCount;

    for (long long f : measure_.markerFrames) {
        if (total > 0 && f >= total) {
            // 素材に無いフレームは「検査しなかった」ことを残す。
            // 黙って飛ばすと「全部一致した」と報告してしまう。
            MarkerResult r;
            r.requested = f;
            r.error = QStringLiteral("素材のフレーム数 %1 を超えるため検査せず").arg(total);
            markerResults_.push_back(r);
            continue;
        }

        // 依頼を先に立ててから seek する。
        // 逆順にすると、seek 結果のフレームが読まれる前に描かれてしまう。
        {
            std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
            state_->markerProbe.requested = true;
            state_->markerProbe.done = false;
            state_->markerProbe.expectedFrame = f;
            state_->markerProbe.markerValue = -1;
            state_->markerProbe.syncOk = false;
            state_->markerProbe.error.clear();
        }

        double ms = 0.0;
        std::string err;
        MarkerResult r;
        r.requested = f;
        if (!worker_->seekBlocking(f, ms, err)) {
            r.error = QString::fromStdString(err);
            markerResults_.push_back(r);
            allOk = false;
            std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
            state_->markerProbe.requested = false;
            continue;
        }

        // 描画されるまで待つ。render thread が動くよう event を回す。
        QElapsedTimer wait;
        wait.start();
        bool done = false;
        while (wait.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
            if (state_->markerProbe.done) {
                done = true;
                r.displayed = state_->markerProbe.displayedFrame;
                r.marker = state_->markerProbe.markerValue;
                r.syncOk = state_->markerProbe.syncOk;
                r.error = QString::fromStdString(state_->markerProbe.error);
                break;
            }
        }
        if (!done) {
            r.error = QStringLiteral("marker の読み取りがタイムアウトしました");
            std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
            state_->markerProbe.requested = false;
        }
        if (!r.syncOk || r.marker != f || r.displayed != f)
            allOk = false;
        markerResults_.push_back(r);
    }
    return allOk;
}

void SpikeController::runSeekBenchmark() {
    const long long total = worker_->info().frameCount;
    if (total <= 1 || measure_.seekCount <= 0)
        return;

    // seed 固定。run 間で同じ点を測る。
    QRandomGenerator rng(measure_.seed);
    seekSamples_.reserve(static_cast<size_t>(measure_.seekCount));

    for (int i = 0; i < measure_.seekCount; i++) {
        const long long target = rng.bounded(static_cast<quint32>(total));
        gpu::SeekSample s;
        s.requestedFrame = target;
        double ms = 0.0;
        std::string err;
        s.ok = worker_->seekBlocking(target, ms, err);
        s.elapsedMs = ms;
        s.landedFrame = s.ok ? target : -1;
        if (!s.ok && errors_.size() < 20)
            errors_ << QStringLiteral("seek %1 失敗: %2").arg(target).arg(
                QString::fromStdString(err));
        seekSamples_.push_back(s);

        if ((i % 50) == 0)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
}

bool SpikeController::writeJson() {
    if (measure_.jsonPath.isEmpty())
        return true;

    std::vector<double> intervals;
    {
        std::lock_guard<std::mutex> g(state_->intervalMutex);
        intervals = state_->frameIntervalsMs;
    }
    std::vector<double> seekMs;
    long long seekFail = 0;
    for (const auto& s : seekSamples_) {
        seekMs.push_back(s.elapsedMs);
        if (!s.ok)
            seekFail++;
    }

    const long long displayed = displayedInWindow_;
    const long long repeated = repeatedInWindow_;
    const long long presents = presentsInWindow_;
    const long long decoded = decodedInWindow_;

    const double elapsedSec = measureElapsedMs_ / 1000.0;
    const double effectiveFps = elapsedSec > 0 ? static_cast<double>(displayed) / elapsedSec : 0.0;

    // **dropped の定義**: decode したのに一度も表示されなかったフレーム数。
    // 表示が追いつかないときは backpressure がかかるので、正常なら
    // 計測終了時に queue に残っている数 (数枚) しか出ない。
    //
    // 「新しいフレームが無くて前の絵をもう一度出した present」は
    // **別の指標** (repeated_presents) として出す。表示リフレッシュが
    // 素材の fps より速ければ必ず起きるので、drop と混ぜてはいけない。
    const long long dropped = decoded > displayed ? decoded - displayed : 0;
    const double dropRate =
        decoded > 0 ? static_cast<double>(dropped) / static_cast<double>(decoded) : 0.0;
    const double repeatRate =
        presents > 0 ? static_cast<double>(repeated) / static_cast<double>(presents) : 0.0;
    const double presentRateHz = elapsedSec > 0 ? static_cast<double>(presents) / elapsedSec : 0.0;

    // CPU 使用率も計測区間の値を使う。
    // JSON を書く時点で読むと marker 検査と seek 計測の分が混ざる。
    const double cpuMs = static_cast<double>((cpuUserEnd_ - cpuUserStart_) +
                                             (cpuKernelEnd_ - cpuKernelStart_)) /
                         10000.0;
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const double cpuUtil = (measureElapsedMs_ > 0 && si.dwNumberOfProcessors > 0)
                               ? cpuMs / measureElapsedMs_ / si.dwNumberOfProcessors
                               : 0.0;

    long long markerMismatch = 0;
    long long markerChecked = 0;
    for (const auto& m : markerResults_) {
        if (!m.error.isEmpty()) {
            markerMismatch++;  // 検査できなかったものを「一致」にしない
            continue;
        }
        markerChecked++;
        if (!m.syncOk || m.marker != m.requested || m.displayed != m.requested)
            markerMismatch++;
    }

    gpu::AdapterInfo qtAdapter = state_->device.adapter();
    gpu::AdapterInfo decAdapter = worker_ ? worker_->decodeAdapter() : gpu::AdapterInfo{};

    QString json;
    QTextStream o(&json);
    o.setRealNumberNotation(QTextStream::FixedNotation);
    o.setRealNumberPrecision(3);

    auto kv = [&o](const char* k, const QString& v) {
        o << "  \"" << k << "\": \"" << jsonEscape(v) << "\",\n";
    };
    auto kvn = [&o](const char* k, double v) { o << "  \"" << k << "\": " << v << ",\n"; };
    auto kvi = [&o](const char* k, long long v) { o << "  \"" << k << "\": " << v << ",\n"; };
    auto kvb = [&o](const char* k, bool v) {
        o << "  \"" << k << "\": " << (v ? "true" : "false") << ",\n";
    };

    o << "{\n";
    kv("schema", QStringLiteral("mvm-p1-preview-1"));
    kv("label", measure_.label);
    kv("media", mediaPath_);
    kv("timestamp", QDateTime::currentDateTime().toString(Qt::ISODate));
    kv("codec", QString::fromStdString(worker_ ? worker_->info().codecName : std::string()));
    kv("hwaccel", QString::fromStdString(worker_ ? worker_->info().hwaccelName : std::string()));
    kvi("width", worker_ ? worker_->info().width : 0);
    kvi("height", worker_ ? worker_->info().height : 0);
    kvi("frame_count", worker_ ? worker_->info().frameCount : -1);
    kv("pixel_format",
       QString::fromLatin1(gpu::toString(worker_ ? worker_->info().pixelFormat
                                                 : gpu::GpuPixelFormat::Unknown)));
    kv("color_space", QString::fromLatin1(gpu::toString(
                          worker_ ? worker_->info().colorSpace : gpu::ColorSpace::Unknown)));
    kv("color_range", QString::fromLatin1(gpu::toString(
                          worker_ ? worker_->info().colorRange : gpu::ColorRange::Unknown)));

    // --- device 共有 --------------------------------------------------------
    {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        kv("rhi_backend", QString::fromStdString(state_->rhiBackend));
        o << "  \"qt_d3d11_device\": \"0x" << Qt::hex << state_->qtDevicePointer << Qt::dec
          << "\",\n";
        o << "  \"qt_d3d11_context\": \"0x" << Qt::hex << state_->qtContextPointer << Qt::dec
          << "\",\n";
        kvi("qt_reported_luid_low", state_->qtReportedLuidLow);
        kvi("qt_reported_luid_high", state_->qtReportedLuidHigh);
        kvi("qt_feature_level", state_->qtFeatureLevel);
        qtDevicePointer_ = state_->qtDevicePointer;
    }
    kv("adapter_description", QString::fromStdString(qtAdapter.description));
    kvi("qt_adapter_luid_low", qtAdapter.luidLow);
    kvi("qt_adapter_luid_high", qtAdapter.luidHigh);
    kvi("ffmpeg_adapter_luid_low", decAdapter.luidLow);
    kvi("ffmpeg_adapter_luid_high", decAdapter.luidHigh);
    kvb("ffmpeg_adapter_known", decAdapter.valid);

    // FFmpeg には Qt の device をそのまま渡している。だが照合するのは
    // 「渡した値」ではなく「decode 結果の texture が実際に属していた device」である。
    //
    // same_device と same_adapter は **別の意味**である。
    //   same_device  : 同一 ID3D11Device。zero-copy が成立している
    //   same_adapter : 同じ GPU。device が別でも GPU copy で繋げる
    // 同じ条件で両方を出すと、どちらが成立したのか分からなくなる。
    const unsigned long long ffmpegDevice =
        worker_ ? worker_->decodeDevicePointer() : 0ULL;
    o << "  \"ffmpeg_d3d11_device\": \"0x" << Qt::hex << ffmpegDevice << Qt::dec << "\",\n";
    kvb("same_device", ffmpegDevice != 0 && ffmpegDevice == qtDevicePointer_);
    kvb("same_adapter", decAdapter.valid && decAdapter.sameAdapterAs(qtAdapter));
    kvb("multithread_protected", state_->device.multithreadProtected());
    kvi("device_lost_count", state_->deviceLostCount.load(std::memory_order_relaxed));

    // --- 計測 ---------------------------------------------------------------
    kvi("warmup_ms", measure_.warmupMs);
    kvn("measure_elapsed_ms", measureElapsedMs_);
    kvi("decoded_frames", decoded);
    kvi("displayed_frames", displayed);
    kvi("present_calls", presents);
    kvi("dropped_frames", dropped);
    kvi("repeated_presents", repeated);
    kvi("loop_count", loopCount_);
    kvn("effective_fps", effectiveFps);
    kvn("drop_rate", dropRate);
    kvn("repeat_rate", repeatRate);
    kvn("present_rate_hz", presentRateHz);
    kvn("startup_latency_ms", startupLatencyMs_);
    kvn("frame_interval_p50_ms", percentile(intervals, 0.50));
    kvn("frame_interval_p95_ms", percentile(intervals, 0.95));
    kvn("frame_interval_max_ms", maxOf(intervals));
    kvi("frame_interval_samples", static_cast<long long>(intervals.size()));

    kvi("seek_count", static_cast<long long>(seekSamples_.size()));
    kvi("seek_failures", seekFail);
    kvn("seek_p50_ms", percentile(seekMs, 0.50));
    kvn("seek_p95_ms", percentile(seekMs, 0.95));
    kvn("seek_max_ms", maxOf(seekMs));
    kvi("seek_backoff_count", worker_ ? worker_->seekBackoffCount() : 0);

    kvi("marker_checked", markerChecked);
    kvi("marker_mismatch", markerMismatch);

    // --- readback -----------------------------------------------------------
    kvi("cpu_full_frame_readback_count", state_->counters.fullFrameReadbacks());
    kvi("marker_band_readback_count", state_->counters.markerBandReadbacks());
    kvi("gpu_copy_count", state_->counters.gpuCopies());

    kvi("decode_errors", worker_ ? worker_->decodeErrorCount() : 0);
    kvi("software_frame_rejects", worker_ ? worker_->softwareFrameRejectCount() : 0);
    kvi("render_errors", state_->renderErrorCount.load(std::memory_order_relaxed));
    kvi("queue_full_events", state_->queue.queueFullCount());
    kvi("stale_generation_rejects", state_->queue.rejectedStaleCount());
    kvi("device_mismatch_rejects", state_->queue.rejectedDeviceMismatchCount());

    kvn("cpu_utilization", cpuUtil);
    kvi("cpu_processors", si.dwNumberOfProcessors);
    kvi("working_set_bytes", workingSetAtEnd_);
    kvi("private_usage_bytes", privateUsageAtEnd_);

    // GPU engine utilization は取得手段を持たない。0 と書かない。
    o << "  \"gpu_engine_utilization\": null,\n";
    kv("gpu_engine_utilization_note",
       QStringLiteral("P1 では取得手段を実装していない (PDH / D3DKMT を使っていない)"));

    if (QScreen* s = QGuiApplication::primaryScreen())
        kvn("display_refresh_hz", s->refreshRate());

    // --- 明細 ---------------------------------------------------------------
    o << "  \"markers\": [\n";
    for (size_t i = 0; i < markerResults_.size(); i++) {
        const auto& m = markerResults_[i];
        o << "    {\"requested\": " << m.requested << ", \"displayed\": " << m.displayed
          << ", \"marker\": " << m.marker << ", \"sync_ok\": " << (m.syncOk ? "true" : "false")
          << ", \"error\": \"" << jsonEscape(m.error) << "\"}"
          << (i + 1 < markerResults_.size() ? ",\n" : "\n");
    }
    o << "  ],\n";

    o << "  \"errors\": [";
    for (int i = 0; i < errors_.size(); i++)
        o << "\"" << jsonEscape(errors_[i]) << "\"" << (i + 1 < errors_.size() ? ", " : "");
    o << "],\n";
    kvi("exit_code", exitCode_);
    o << "  \"ok\": " << (exitCode_ == 0 ? "true" : "false") << "\n";
    o << "}\n";
    o.flush();

    QFileInfo fi(measure_.jsonPath);
    QDir().mkpath(fi.absolutePath());
    QFile f(measure_.jsonPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errors_ << QStringLiteral("JSON を書けません: ") + measure_.jsonPath;
        exitCode_ = 6;
        return false;
    }
    f.write(json.toUtf8());
    f.close();
    return true;
}

}  // namespace mvm::app
