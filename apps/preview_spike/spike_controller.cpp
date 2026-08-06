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
#include <string>

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

} // namespace

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
    return worker_ ? worker_->snapshot().info.frameCount : -1;
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
    gpu::SeekSample s;
    QString err;
    if (!seekAndWaitForDisplay(frame, s, err))
        status_ = QStringLiteral("seek 失敗: ") + err;
    else
        status_ = QStringLiteral("seek %1 (decode %2ms / 表示 %3ms)")
                      .arg(frame)
                      .arg(s.decodeReadyMs, 0, 'f', 1)
                      .arg(s.displayedMs, 0, 'f', 1);
    Q_EMIT statusChanged();
}

// --------------------------------------------------------------------------
// §1 (P1.2) request-to-display seek: race を残さない待ち方
// --------------------------------------------------------------------------
// **旧経路 (P1.1)**
//     waiting = false
//     seekBlocking()      <- ここで frame が queue に入る
//     waiting = true      <- arm。この前に描かれると記録が残らず取りこぼす
//
// **新経路 (P1.2)**
//     baseline = ledger.currentSequence()   <- seek より前に取る
//     seekBlocking()
//     ledger.waitForDisplay(baseline, key)  <- 記録済みなら即座に見つかる
//
// render thread は待機の有無に関わらず全 display を記録するので、
// arm のタイミングに依存しない。
bool SpikeController::seekAndWaitForDisplay(long long frame, gpu::SeekSample& sample,
                                            QString& err) {
    sample = gpu::SeekSample{};
    sample.requestedFrame = frame;
    if (!worker_ || !state_) {
        err = QStringLiteral("worker がありません");
        return false;
    }

    // **seek を出す前に baseline を取る。**
    // これより後の display だけを成功と見なすので、
    // 「古い display を新しい request の成功にする」ことが構造的に起きない。
    const unsigned long long baseline = state_->displayLedger.currentSequence();
    const long long t0 = gpu::qpcTicks();

    double decodeReadyMs = 0.0;
    std::string serr;
    if (!worker_->seekBlocking(frame, decodeReadyMs, serr)) {
        sample.decodeReadyMs = decodeReadyMs;
        err = QString::fromStdString(serr);
        return false;
    }
    sample.decodeReadyMs = decodeReadyMs;

    const gpu::DecoderSnapshot snap = worker_->snapshot();
    gpu::DisplayWaitKey key;
    key.sourceId = snap.sourceId;
    key.sourceGeneration = snap.sourceGeneration;
    key.compositionEpoch = state_->queue.compositionEpoch();
    key.requestedFrame = frame;

    // 有限時間だけ待つ。**無制限に processEvents を回さない。**
    // 待つのは ledger 側の condition_variable であり、sleep ではない。
    gpu::DisplayRecord record;
    QElapsedTimer wait;
    wait.start();
    bool got = false;
    while (wait.elapsed() < measure_.displayTimeoutMs) {
        const int remaining =
            static_cast<int>(measure_.displayTimeoutMs - wait.elapsed());
        // GUI thread は render thread の update を回す必要があるので、
        // ledger の待ちは短く刻み、その合間に processEvents を挟む。
        if (state_->displayLedger.waitForDisplay(baseline, key, std::min(remaining, 5), record)) {
            got = true;
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }

    if (!got) {
        err = QStringLiteral("frame %1 が %2ms 以内に表示されませんでした")
                  .arg(frame)
                  .arg(measure_.displayTimeoutMs);
        return false;
    }

    sample.landedFrame = record.displayedFrame;
    sample.displayedMs = gpu::qpcMsBetween(t0, record.displayedQpc);
    sample.ok = true;
    sample.displayed = true;
    return true;
}

void SpikeController::updateDeviceText() {
    if (!state_)
        return;
    const gpu::DecoderSnapshot snap = worker_ ? worker_->snapshot() : gpu::DecoderSnapshot{};
    std::lock_guard<std::mutex> g(state_->infoMutex);
    const gpu::AdapterInfo qt = state_->device.adapter();
    const gpu::AdapterInfo dec = snap.adapter;

    deviceText_ =
        QStringLiteral("backend=%1  gpu完了=%2  adapter=%3  FL=0x%4\nQt LUID=%5:%6  decode "
                       "LUID=%7:%8  %9")
            .arg(QString::fromStdString(state_->rhiBackend))
            .arg(QString::fromStdString(state_->gpuCompletionBackend))
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

    const gpu::DecoderSnapshot snap = worker_ ? worker_->snapshot() : gpu::DecoderSnapshot{};

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
            "CPU full-frame readback=%5  marker band=%6  GPU copy=%7  device lost=%8\n"
            "retirement depth=%9 (peak %10)  SRV=%11")
            .arg(snap.decodedFrameCount)
            .arg(displayed)
            .arg(state_->repeatedPresents.load(std::memory_order_relaxed))
            .arg(state_->queue.depth())
            .arg(state_->counters.fullFrameReadbacks())
            .arg(state_->counters.markerBandReadbacks())
            .arg(state_->counters.gpuCopies())
            .arg(state_->deviceLostCount.load(std::memory_order_relaxed))
            .arg(state_->retirement.depthCurrent())
            .arg(state_->retirement.depthPeak())
            .arg(state_->converter.srvCacheEntries());

    // --- §3 device change: 停止順序を GUI が主導する -----------------------
    // render thread は「検出した」ことしか伝えてこない。
    // **ここで decode thread を止めて join し切ってから** teardown を許可する。
    if (state_->deviceChange.detected()) {
        if (worker_) {
            worker_->stop(); // thread::join まで行う
        }
        state_->deviceChange.noteWorkerStopped();
        state_->deviceChange.noteFailClosed();
        status_ = QStringLiteral("device が変わったため停止しました (P1.2 は自動復帰しない): ") +
                  QString::fromStdString(state_->deviceChange.reason());
        errors_ << status_;
        if (exitCode_ == 0)
            exitCode_ = 7; // device change による fail-closed 停止
        if (measure_.enabled) {
            phase_ = Phase::Done;
            writeJson();
            Q_EMIT finished();
            return;
        }
    }

    if (state_->initFailed.load(std::memory_order_relaxed) && exitCode_ == 0) {
        {
            std::lock_guard<std::mutex> g(state_->infoMutex);
            status_ = QStringLiteral("初期化失敗: ") + QString::fromStdString(state_->initError);
        }
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
    // marker / color / seek のフェーズは processEvents を回す。
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
        if (startupLatencyMs_ < 0 && state_->uniqueDisplayed.load(std::memory_order_relaxed) > 0)
            startupLatencyMs_ = gpu::qpcMsBetween(startupLatencyTicks_, gpu::qpcTicks());
        if (worker_->eof()) {
            gpu::SeekSample s;
            QString e;
            seekAndWaitForDisplay(0, s, e);
            play();
        }
        if (phaseTimer_.elapsed() < measure_.warmupMs)
            return;

        // 計測区間の基準を取り直す。warm-up の値を混ぜない。
        displayedAtStart_ = state_->uniqueDisplayed.load(std::memory_order_relaxed);
        repeatedAtStart_ = state_->repeatedPresents.load(std::memory_order_relaxed);
        presentAtStart_ = state_->presentCount.load(std::memory_order_relaxed);
        decodedAtStart_ = worker_->snapshot().decodedFrameCount;
        submittedAtStart_ = state_->queue.submittedCount();
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
            gpu::SeekSample s;
            QString e;
            seekAndWaitForDisplay(0, s, e);
            play();
        }
        if (phaseTimer_.elapsed() < measure_.measureMs)
            return;

        measureElapsedMs_ = gpu::qpcMsBetween(measureStartTicks_, gpu::qpcTicks());
        state_->collectIntervals.store(false, std::memory_order_relaxed);
        pause();
        // 計測区間の値をここで確定させる。
        // このあとの marker / color / seek でも decode は進むので、
        // JSON を書く時点で読むと計測区間の値ではなくなる。
        displayedInWindow_ = state_->uniqueDisplayed.load(std::memory_order_relaxed) -
                             displayedAtStart_;
        repeatedInWindow_ =
            state_->repeatedPresents.load(std::memory_order_relaxed) - repeatedAtStart_;
        presentsInWindow_ = state_->presentCount.load(std::memory_order_relaxed) - presentAtStart_;
        decodedInWindow_ = worker_->snapshot().decodedFrameCount - decodedAtStart_;
        submittedInWindow_ = state_->queue.submittedCount() - submittedAtStart_;
        processCpuTimes(cpuUserEnd_, cpuKernelEnd_);
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),
                             sizeof pm);
        workingSetAtEnd_ = static_cast<long long>(pm.WorkingSetSize);
        privateUsageAtEnd_ = static_cast<long long>(pm.PrivateUsage);
        phase_ = Phase::Markers;
        phaseTimer_.restart();
        return;
    }

    case Phase::Markers: {
        runMarkerChecks();
        phase_ = Phase::Colors;
        phaseTimer_.restart();
        return;
    }

    case Phase::Colors: {
        runColorPatchDiagnostic();
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
    const long long total = worker_->snapshot().info.frameCount;

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

        MarkerResult r;
        r.requested = f;
        gpu::SeekSample sample;
        QString serr;
        if (!seekAndWaitForDisplay(f, sample, serr)) {
            r.error = serr;
            markerResults_.push_back(r);
            allOk = false;
            std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
            state_->markerProbe.requested = false;
            continue;
        }

        // 表示が済んでいるので marker 読み取りも済んでいるはずだが、
        // render thread の完了は別なので有限時間だけ待つ。
        QElapsedTimer wait;
        wait.start();
        bool done = false;
        while (wait.elapsed() < 3000) {
            {
                std::lock_guard<std::mutex> g(state_->markerProbe.mutex);
                if (state_->markerProbe.done) {
                    done = true;
                    r.displayed = state_->markerProbe.displayedFrame;
                    r.marker = state_->markerProbe.markerValue;
                    r.syncOk = state_->markerProbe.syncOk;
                    r.error = QString::fromStdString(state_->markerProbe.error);
                }
            }
            if (done)
                break;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
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

void SpikeController::runColorPatchDiagnostic() {
    // **これは診断であって色の合否判定ではない。**
    // 正式な color patch 検査は mvm_test_gpu_decode color が
    // 既知の YUV fixture に対して行う (§6)。
    // ここでは「表示経路がどの行列 / レンジを選んだか」を記録するだけ。
    if (measure_.colorPatchWidth <= 0 || measure_.colorPatchHeight <= 0)
        return;

    {
        std::lock_guard<std::mutex> g(state_->colorPatch.mutex);
        state_->colorPatch.requested = true;
        state_->colorPatch.done = false;
        state_->colorPatch.expectedFrame = 0;
        state_->colorPatch.patchWidth = measure_.colorPatchWidth;
        state_->colorPatch.patchHeight = measure_.colorPatchHeight;
        state_->colorPatch.error.clear();
    }

    gpu::SeekSample sample;
    QString serr;
    if (!seekAndWaitForDisplay(0, sample, serr)) {
        colorDiagError_ = serr;
        std::lock_guard<std::mutex> g(state_->colorPatch.mutex);
        state_->colorPatch.requested = false;
        return;
    }

    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 3000) {
        bool done = false;
        {
            std::lock_guard<std::mutex> g(state_->colorPatch.mutex);
            if (state_->colorPatch.done) {
                done = true;
                colorDiagDone_ = true;
                colorDiagSpace_ = QString::fromLatin1(gpu::toString(state_->colorPatch.colorSpace));
                colorDiagRange_ = QString::fromLatin1(gpu::toString(state_->colorPatch.colorRange));
                colorDiagSpaceInferred_ = state_->colorPatch.colorSpaceInferred;
                colorDiagRangeInferred_ = state_->colorPatch.colorRangeInferred;
                colorDiagError_ = QString::fromStdString(state_->colorPatch.error);
            }
        }
        if (done)
            return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    colorDiagError_ = QStringLiteral("color patch の読み取りがタイムアウトしました");
    std::lock_guard<std::mutex> g(state_->colorPatch.mutex);
    state_->colorPatch.requested = false;
}

void SpikeController::runSeekBenchmark() {
    const long long total = worker_->snapshot().info.frameCount;
    if (total <= 1 || measure_.seekCount <= 0)
        return;

    // seed 固定。run 間で同じ点を測る。
    QRandomGenerator rng(measure_.seed);
    seekSamples_.reserve(static_cast<size_t>(measure_.seekCount));

    for (int i = 0; i < measure_.seekCount; i++) {
        const long long target = rng.bounded(static_cast<quint32>(total));
        gpu::SeekSample s;
        QString serr;
        seekAndWaitForDisplay(target, s, serr);
        if (!s.ok && errors_.size() < 20)
            errors_ << QStringLiteral("seek %1 失敗: %2").arg(target).arg(serr);
        seekSamples_.push_back(s);
    }
}

bool SpikeController::writeJson() {
    if (measure_.jsonPath.isEmpty())
        return true;

    const gpu::DecoderSnapshot snap = worker_ ? worker_->snapshot() : gpu::DecoderSnapshot{};

    std::vector<double> intervals;
    {
        std::lock_guard<std::mutex> g(state_->intervalMutex);
        intervals = state_->frameIntervalsMs;
    }
    std::vector<double> seekDisplayedMs;
    std::vector<double> seekDecodeMs;
    long long seekFail = 0;
    long long seekDisplayMismatch = 0;
    for (const auto& s : seekSamples_) {
        seekDecodeMs.push_back(s.decodeReadyMs);
        if (s.ok) {
            seekDisplayedMs.push_back(s.displayedMs);
            if (s.landedFrame != s.requestedFrame)
                seekDisplayMismatch++;
        } else {
            seekFail++;
        }
    }

    const long long displayed = displayedInWindow_;
    const long long repeated = repeatedInWindow_;
    const long long presents = presentsInWindow_;
    const long long decoded = decodedInWindow_;
    const long long submitted = submittedInWindow_;

    const double elapsedSec = measureElapsedMs_ / 1000.0;
    const double effectiveFps = elapsedSec > 0 ? static_cast<double>(displayed) / elapsedSec : 0.0;

    // §7: queue に残っているものを無条件に dropped と呼ばない。
    // dropped は「表示期限を過ぎて意図的に捨てた」ものだけ。
    const long long pendingAtEnd = static_cast<long long>(state_->queue.depth());
    const long long dropped = state_->displayDeadlineDrops.load(std::memory_order_relaxed);
    const double dropRate =
        decoded > 0 ? static_cast<double>(dropped) / static_cast<double>(decoded) : 0.0;
    const double repeatRate =
        presents > 0 ? static_cast<double>(repeated) / static_cast<double>(presents) : 0.0;
    const double presentRateHz =
        elapsedSec > 0 ? static_cast<double>(presents) / elapsedSec : 0.0;

    const double cpuMs =
        static_cast<double>((cpuUserEnd_ - cpuUserStart_) + (cpuKernelEnd_ - cpuKernelStart_)) /
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
            markerMismatch++; // 検査できなかったものを「一致」にしない
            continue;
        }
        markerChecked++;
        if (!m.syncOk || m.marker != m.requested || m.displayed != m.requested)
            markerMismatch++;
    }

    const gpu::AdapterInfo qtAdapter = state_->device.adapter();
    const gpu::AdapterInfo decAdapter = snap.adapter;

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
    kv("schema", QStringLiteral("mvm-p1-preview-3"));
    kv("label", measure_.label);
    kv("media", mediaPath_);
    kv("timestamp", QDateTime::currentDateTime().toString(Qt::ISODate));
    kv("codec", QString::fromStdString(snap.info.codecName));
    kv("hwaccel", QString::fromStdString(snap.info.hwaccelName));
    kvi("width", snap.info.width);
    kvi("height", snap.info.height);
    kvi("frame_count", snap.info.frameCount);
    kv("pixel_format", QString::fromLatin1(gpu::toString(snap.info.pixelFormat)));
    kv("color_space", QString::fromLatin1(gpu::toString(snap.info.colorSpace)));
    kv("color_range", QString::fromLatin1(gpu::toString(snap.info.colorRange)));

    // --- device 共有 --------------------------------------------------------
    {
        std::lock_guard<std::mutex> g(state_->infoMutex);
        kv("rhi_backend", QString::fromStdString(state_->rhiBackend));
        kv("gpu_completion_backend", QString::fromStdString(state_->gpuCompletionBackend));
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

    // same_device と same_adapter は **別の意味**である。
    //   same_device  : 同一 ID3D11Device。zero-copy が成立している
    //   same_adapter : 同じ GPU。device が別でも GPU copy で繋げる
    const unsigned long long ffmpegDevice = snap.decodeDevicePointer;
    o << "  \"ffmpeg_d3d11_device\": \"0x" << Qt::hex << ffmpegDevice << Qt::dec << "\",\n";
    kvb("same_device", ffmpegDevice != 0 && ffmpegDevice == qtDevicePointer_);
    kvb("same_adapter", decAdapter.valid && decAdapter.sameAdapterAs(qtAdapter));
    kvb("multithread_protected", state_->device.multithreadProtected());
    kvi("device_lost_count", state_->deviceLostCount.load(std::memory_order_relaxed));

    // --- device lifecycle (P1.2 §3) -----------------------------------------
    // **device_change_handled は「完全な復帰が成立した回数」である。**
    // P1.2 は復帰を実装していないので、常に 0 になる。
    // 検出は detected、停止は fail_closed で数える。
    kvi("device_change_detected_count", state_->deviceChange.detectedCount());
    kvi("device_change_handled_count", state_->deviceChange.handledCount());
    kvi("device_change_fail_closed_count", state_->deviceChange.failClosedCount());
    kv("device_change_state", QString::fromLatin1(gpu::toString(state_->deviceChange.state())));
    kv("device_change_reason", QString::fromStdString(state_->deviceChange.reason()));
    kv("device_recovery_support", QStringLiteral("none (P1.2 は fail-closed のみ)"));

    // --- §1 GPU completion / retirement -------------------------------------
    kvi("gpu_submitted_serial", static_cast<long long>(state_->completion.submittedSerial()));
    kvi("gpu_completed_serial",
        static_cast<long long>(state_->completion.polledCompletedSerial()));
    // 追跡できなかった submission (P1.2 §4)。0 でなければ
    // 「GPU 完了を待った」とは言えない。
    kvi("untracked_submission_count",
        state_->untrackedSubmissionCount.load(std::memory_order_relaxed));
    kvi("completion_poll_failure_count",
        state_->completionPollFailureCount.load(std::memory_order_relaxed));
    kvi("gpu_completion_device_removed_count", state_->completion.deviceRemovedCount());
    kvb("gpu_completion_fatal", state_->completion.fatal());
    kv("gpu_completion_fatal_reason",
       QString::fromStdString(state_->completion.fatalReason()));
    kvi("retirement_depth_current", static_cast<long long>(state_->retirement.depthCurrent()));
    kvi("retirement_depth_peak", static_cast<long long>(state_->retirement.depthPeak()));
    kvi("retirement_timeout_count", state_->retirement.retirementTimeoutCount());
    kvi("forced_gpu_wait_count", state_->retirement.forcedGpuWaitCount());
    // **frame だけでなく SRV も同じ queue に入る。** 名前を実態に合わせる (§6)。
    kvi("payloads_released_before_completion",
        state_->retirement.payloadsReleasedBeforeCompletion());

    // --- §4 resource epoch / SRV cache --------------------------------------
    kvi("resource_epoch", static_cast<long long>(snap.resourceEpoch.value));
    kvi("source_id", static_cast<long long>(snap.sourceId.value));
    kvi("source_generation", static_cast<long long>(snap.sourceGeneration.value));
    kvi("composition_epoch",
        static_cast<long long>(state_->queue.compositionEpoch().value));
    kvi("srv_cache_entries_current", static_cast<long long>(state_->converter.srvCacheEntries()));
    kvi("srv_cache_entries_peak", static_cast<long long>(state_->converter.srvCacheEntriesPeak()));
    kvi("retired_srv_entries", state_->converter.retiredSrvEntries());
    // **cache 内の (epoch, texture) の異なる組み合わせ数**である。
    // 「今 open している decoder の数」ではない (§6)。
    kvi("srv_cache_texture_groups",
        static_cast<long long>(state_->converter.srvCacheTextureGroups()));

    // --- 計測 ---------------------------------------------------------------
    kvi("warmup_ms", measure_.warmupMs);
    kvn("measure_elapsed_ms", measureElapsedMs_);
    kvi("decoded_frames", decoded);
    kvi("submitted_frames", submitted);
    kvi("displayed_frames", displayed);
    kvi("present_calls", presents);
    kvi("pending_at_end", pendingAtEnd);
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

    // --- §7 frame accounting ------------------------------------------------
    kvi("stale_rejected", state_->queue.rejectedStaleCount());
    kvi("future_rejected", state_->queue.rejectedFutureCount());
    kvi("invalid_rejected", state_->queue.rejectedInvalidCount());
    kvi("device_rejected", state_->queue.rejectedDeviceMismatchCount());
    kvi("generation_regression_rejected", state_->queue.generationRegressionCount());
    kvi("decode_failed", snap.decodeErrorCount);
    kvi("render_failed", state_->renderErrorCount.load(std::memory_order_relaxed));
    kvi("retired_not_completed", static_cast<long long>(state_->retirement.depthCurrent()));

    // --- §5 seek ------------------------------------------------------------
    kvi("seek_count", static_cast<long long>(seekSamples_.size()));
    kvi("seek_failures", seekFail);
    kvi("seek_display_mismatch", seekDisplayMismatch);
    kvn("seek_decode_ready_p50_ms", percentile(seekDecodeMs, 0.50));
    kvn("seek_decode_ready_p95_ms", percentile(seekDecodeMs, 0.95));
    kvn("seek_decode_ready_max_ms", maxOf(seekDecodeMs));
    kvn("seek_displayed_p50_ms", percentile(seekDisplayedMs, 0.50));
    kvn("seek_displayed_p95_ms", percentile(seekDisplayedMs, 0.95));
    kvn("seek_displayed_max_ms", maxOf(seekDisplayedMs));
    kvi("seek_backoff_count", snap.seekBackoffCount);

    kvi("marker_checked", markerChecked);
    kvi("marker_mismatch", markerMismatch);

    // --- color patch (診断。正式判定は mvm_test_gpu_decode color) -----------
    kvb("color_patch_read", colorDiagDone_);
    kv("color_patch_space", colorDiagSpace_);
    kv("color_patch_range", colorDiagRange_);
    kvb("color_patch_space_inferred", colorDiagSpaceInferred_);
    kvb("color_patch_range_inferred", colorDiagRangeInferred_);
    kv("color_patch_error", colorDiagError_);

    // --- readback -----------------------------------------------------------
    kvi("cpu_full_frame_readback_count", state_->counters.fullFrameReadbacks());
    kvi("marker_band_readback_count", state_->counters.markerBandReadbacks());
    kvi("color_patch_readback_count", state_->counters.colorPatchReadbacks());
    kvi("gpu_copy_count", state_->counters.gpuCopies());

    kvi("decode_errors", snap.decodeErrorCount);
    kvi("software_frame_rejects", snap.softwareFrameRejectCount);
    kvi("render_errors", state_->renderErrorCount.load(std::memory_order_relaxed));
    kvi("queue_full_events", state_->queue.queueFullCount());

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

} // namespace mvm::app
