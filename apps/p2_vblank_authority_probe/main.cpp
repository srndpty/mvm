// P2-D5-2/F3-A: window output VBlank authority probe。
//
// formal opportunity ordinalのauthorityを、windowが実際に載っているdisplay
// outputのphysical VBlank sequenceへ移せるかを、実装を変更する前に実測で確かめる。
// DwmGetCompositionTimingInfo(NULL)のcRefresh / rateRefreshはdiagnostic-onlyとして
// 併記するだけで、判定には使わない。
#include "app/preview/compositor_rhi_item.h"
#include "media/gpu_preview/qpc_clock.h"
#include "media/gpu_preview/window_output_vblank_observer.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QTimer>

#include <windows.h>

#include <dwmapi.h>

#include <cstdio>

using namespace mvm::app;

namespace {

struct DwmDiagnostic {
    bool available = false;
    unsigned long long refreshCount = 0;
    long long qpcVBlank = 0;
    long long qpcRefreshPeriod = 0;
    long long rateRefreshNumerator = 0;
    long long rateRefreshDenominator = 0;
    long long sampledQpc = 0;
};

DwmDiagnostic captureDwmDiagnostic() {
    DwmDiagnostic result;
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    // Windows 8.1以降、この関数のhwndはNULL必須。window固有のtimingは得られない。
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)))
        return result;
    result.available = true;
    result.refreshCount = timing.cRefresh;
    result.qpcVBlank = static_cast<long long>(timing.qpcVBlank);
    result.qpcRefreshPeriod = static_cast<long long>(timing.qpcRefreshPeriod);
    result.rateRefreshNumerator = timing.rateRefresh.uiNumerator;
    result.rateRefreshDenominator = timing.rateRefresh.uiDenominator;
    result.sampledQpc = mvm::gpu::qpcTicks();
    return result;
}

QJsonObject dwmJson(const DwmDiagnostic& value) {
    return {{"available", value.available},
            {"refresh_count", static_cast<qint64>(value.refreshCount)},
            {"qpc_vblank", value.qpcVBlank},
            {"qpc_refresh_period", value.qpcRefreshPeriod},
            {"rate_refresh_numerator", value.rateRefreshNumerator},
            {"rate_refresh_denominator", value.rateRefreshDenominator},
            {"sampled_qpc", value.sampledQpc}};
}

QJsonObject identityJson(const mvm::gpu::WindowOutputIdentity& value) {
    return {{"available", value.available},
            {"monitor_handle", QString::number(value.monitorHandle)},
            {"output_index", static_cast<qint64>(value.outputIndex)},
            {"adapter_luid_low", static_cast<qint64>(value.adapterLuidLow)},
            {"adapter_luid_high", static_cast<qint64>(value.adapterLuidHigh)},
            {"gdi_device_name", QString::fromStdString(value.gdiDeviceName)},
            {"output_device_name", QString::fromStdString(value.outputDeviceName)},
            {"refresh_numerator", value.refreshNumerator},
            {"refresh_denominator", value.refreshDenominator},
            {"desktop_left", value.desktopLeft},
            {"desktop_top", value.desktopTop},
            {"desktop_right", value.desktopRight},
            {"desktop_bottom", value.desktopBottom}};
}

const char* sequenceStatusName(mvm::gpu::VBlankSequenceStatus status) {
    switch (status) {
    case mvm::gpu::VBlankSequenceStatus::Ok: return "OK";
    case mvm::gpu::VBlankSequenceStatus::Empty: return "EMPTY";
    case mvm::gpu::VBlankSequenceStatus::Invalid: return "INVALID";
    case mvm::gpu::VBlankSequenceStatus::OrdinalRegression: return "ORDINAL_REGRESSION";
    case mvm::gpu::VBlankSequenceStatus::OrdinalGap: return "ORDINAL_GAP";
    case mvm::gpu::VBlankSequenceStatus::QpcRegression: return "QPC_REGRESSION";
    }
    return "UNKNOWN";
}

bool writeJson(const QString& path, const QJsonObject& root) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0)
        return false;
    return file.commit();
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    const qsizetype metricsIndex = args.indexOf(QStringLiteral("--metrics"));
    const qsizetype durationIndex = args.indexOf(QStringLiteral("--duration-ms"));
    const qsizetype preflightIndex = args.indexOf(QStringLiteral("--preflight-vblanks"));
    if (metricsIndex < 0 || metricsIndex + 1 >= args.size() || durationIndex < 0 ||
        durationIndex + 1 >= args.size()) {
        std::fprintf(stderr, "使い方: mvm_p2_vblank_authority_probe --metrics <json> "
                             "--duration-ms <ms> [--preflight-vblanks <n>]\n");
        return 2;
    }
    bool durationOk = false;
    const int durationMs = args[durationIndex + 1].toInt(&durationOk);
    if (!durationOk || durationMs <= 0)
        return 2;
    long long preflightVBlanks = 120;
    if (preflightIndex >= 0 && preflightIndex + 1 < args.size()) {
        bool preflightOk = false;
        preflightVBlanks = args[preflightIndex + 1].toLongLong(&preflightOk);
        if (!preflightOk || preflightVBlanks < 2)
            return 2;
    }
    const QString metricsPath = args[metricsIndex + 1];

    qmlRegisterType<CompositorRhiItem>("mvm.compositor", 1, 0, "CompositorSurface");
    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral("qrc:/mvm/p2_vblank_authority_probe/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 5;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    auto* item = window ? window->findChild<CompositorRhiItem*>("compositorSurface") : nullptr;
    if (!window || !item)
        return 5;

    auto observer = std::make_shared<mvm::gpu::WindowOutputVBlankObserver>();
    auto finish = [&app, metricsPath, observer](const QJsonObject& root, int code) {
        observer->stop();
        if (!writeJson(metricsPath, root))
            app.exit(6);
        else
            app.exit(code);
    };

    QElapsedTimer elapsed;
    elapsed.start();
    auto* poll = new QTimer(&app);
    poll->setInterval(50);
    QObject::connect(poll, &QTimer::timeout, &app, [&, poll]() mutable {
        if (!window->isExposed()) {
            if (elapsed.elapsed() < 10000)
                return;
            poll->stop();
            finish(QJsonObject{{"schema", "mvm-p2-vblank-authority-probe-1"},
                               {"probe_pass", false},
                               {"error", "windowがexposeされません"}},
                   3);
            return;
        }
        poll->stop();

        const auto hwnd = reinterpret_cast<void*>(window->winId());
        const auto startResolve = mvm::gpu::resolveWindowOutput(hwnd);
        const auto dwmStart = captureDwmDiagnostic();
        if (!startResolve.ok) {
            finish(QJsonObject{{"schema", "mvm-p2-vblank-authority-probe-1"},
                               {"probe_pass", false},
                               {"error", QString::fromStdString(startResolve.error)},
                               {"dwm_diagnostic_start", dwmJson(dwmStart)}},
                   3);
            return;
        }
        std::string observerError;
        if (!observer->start(hwnd, observerError)) {
            finish(QJsonObject{{"schema", "mvm-p2-vblank-authority-probe-1"},
                               {"probe_pass", false},
                               {"error", QString::fromStdString(observerError)},
                               {"window_output_start", identityJson(startResolve.identity)},
                               {"dwm_diagnostic_start", dwmJson(dwmStart)}},
                   3);
            return;
        }

        auto* done = new QTimer(&app);
        done->setSingleShot(true);
        done->setInterval(durationMs);
        QObject::connect(done, &QTimer::timeout, &app, [&, dwmStart, startResolve]() {
            const auto dwmStop = captureDwmDiagnostic();
            observer->stop();
            const auto endResolve = mvm::gpu::resolveWindowOutput(
                reinterpret_cast<void*>(window->winId()));
            const auto samples = observer->ring().snapshot();
            const auto identity = startResolve.identity;
            const long long qpcFrequency = static_cast<long long>(mvm::gpu::qpcFrequency());

            const auto sequence =
                mvm::gpu::vblankSequenceStatus(samples.data(), samples.size());
            mvm::gpu::VBlankCadenceResult preflight;
            const std::size_t preflightCount =
                samples.size() < static_cast<std::size_t>(preflightVBlanks)
                    ? samples.size()
                    : static_cast<std::size_t>(preflightVBlanks);
            const bool preflightConsistent = mvm::gpu::vblankCadenceConsistent(
                samples.data(), preflightCount, identity.refreshNumerator,
                identity.refreshDenominator, qpcFrequency, preflight);
            mvm::gpu::VBlankCadenceResult full;
            const bool fullConsistent = mvm::gpu::vblankCadenceConsistent(
                samples.data(), samples.size(), identity.refreshNumerator,
                identity.refreshDenominator, qpcFrequency, full);

            const bool outputStable = endResolve.ok &&
                                      mvm::gpu::sameWindowOutput(identity, endResolve.identity);
            const bool observerClean =
                observer->ring().overflowCount() == 0 && observer->waitFailureCount() == 0;

            // diagnostic-only: DWM composition clockのcadence。判定には使わない。
            QJsonValue dwmObservedHz = QJsonValue::Null;
            if (dwmStart.available && dwmStop.available && dwmStop.sampledQpc > dwmStart.sampledQpc
                && dwmStop.refreshCount >= dwmStart.refreshCount) {
                const double seconds = static_cast<double>(dwmStop.sampledQpc -
                                                           dwmStart.sampledQpc) /
                                       static_cast<double>(qpcFrequency);
                if (seconds > 0)
                    dwmObservedHz = static_cast<double>(dwmStop.refreshCount -
                                                        dwmStart.refreshCount) /
                                    seconds;
            }
            QJsonValue outputObservedHz = QJsonValue::Null;
            if (samples.size() >= 2) {
                const double seconds =
                    static_cast<double>(samples.back().qpc - samples.front().qpc) /
                    static_cast<double>(qpcFrequency);
                if (seconds > 0)
                    outputObservedHz =
                        static_cast<double>(samples.back().ordinal - samples.front().ordinal) /
                        seconds;
            }

            QJsonArray head;
            for (std::size_t index = 0; index < samples.size() && index < 8; ++index)
                head.append(QJsonObject{{"ordinal", samples[index].ordinal},
                                        {"qpc", samples[index].qpc}});

            mvm::gpu::VBlankIntervalReport intervals;
            const bool intervalsOk = mvm::gpu::vblankIntervalReport(
                samples.data(), samples.size(), identity.refreshNumerator,
                identity.refreshDenominator, qpcFrequency, intervals);

            // authority validityの判定はpreflight窓の整合で行う。full窓の差は
            // panelの実cadenceとdisplay modeの公称値の差を含むためdiagnosticとする。
            const bool pass = sequence == mvm::gpu::VBlankSequenceStatus::Ok &&
                              preflightConsistent && outputStable && observerClean &&
                              intervalsOk && intervals.longIntervalCount == 0 &&
                              samples.size() >= 2;
            const QJsonObject root{
                {"schema", "mvm-p2-vblank-authority-probe-1"},
                {"probe_pass", pass},
                {"error", QString{}},
                {"configured_duration_ms", durationMs},
                {"configured_preflight_vblanks", static_cast<qint64>(preflightVBlanks)},
                {"qpc_frequency", qpcFrequency},
                {"window_output_start", identityJson(identity)},
                {"window_output_end", identityJson(endResolve.identity)},
                {"window_output_stable", outputStable},
                {"vblank_sample_count", static_cast<qint64>(samples.size())},
                {"vblank_sequence_status", QString::fromLatin1(sequenceStatusName(sequence))},
                {"vblank_ring_overflow_count", observer->ring().overflowCount()},
                {"vblank_wait_failure_count", observer->waitFailureCount()},
                {"vblank_head", head},
                {"preflight_vblank_cadence_consistent", preflightConsistent},
                {"preflight_observed_intervals", preflight.observedIntervals},
                {"preflight_elapsed_qpc", preflight.elapsedQpc},
                {"preflight_deviation_numerator", preflight.deviationNumerator},
                {"preflight_tolerance_unit", preflight.toleranceUnit},
                {"full_vblank_cadence_consistent", fullConsistent},
                {"vblank_interval_report_ok", intervalsOk},
                {"vblank_interval_count", intervals.intervalCount},
                {"vblank_long_interval_count", intervals.longIntervalCount},
                {"vblank_max_interval_qpc", intervals.maxIntervalQpc},
                {"vblank_min_interval_qpc", intervals.minIntervalQpc},
                {"vblank_nominal_period_qpc", intervals.nominalPeriodQpc},
                {"full_observed_intervals", full.observedIntervals},
                {"full_elapsed_qpc", full.elapsedQpc},
                {"full_deviation_numerator", full.deviationNumerator},
                {"full_tolerance_unit", full.toleranceUnit},
                {"window_output_observed_hz", outputObservedHz},
                {"dwm_diagnostic_start", dwmJson(dwmStart)},
                {"dwm_diagnostic_stop", dwmJson(dwmStop)},
                {"dwm_diagnostic_observed_hz", dwmObservedHz}};
            finish(root, pass ? 0 : 3);
        });
        done->start();
    });
    poll->start();
    return app.exec();
}
