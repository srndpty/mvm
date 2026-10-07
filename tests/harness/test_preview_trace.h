#ifndef MVM_TEST_PREVIEW_TRACE_H
#define MVM_TEST_PREVIEW_TRACE_H

// 因果切り分け用の観測だけを行う。要求・描画・フォーカスは変更しない。
#include "mvm_controller.h"

#include <atomic>
#include <cstdio>
#include <memory>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QQuickWindow>
#include <QTimer>

namespace mvm::test {
inline bool traceRequest(app::MvmController& controller, const char* label,
                         const std::function<bool()>& operation) {
    if (!qEnvironmentVariableIsSet("MVM_TEST_PREVIEW_TRACE"))
        return operation();
    const auto before = controller.previewEngineForTest()->status();
    const bool accepted = operation();
    if (qEnvironmentVariableIsSet("MVM_TEST_PREVIEW_TRACE")) {
        const auto after = controller.previewEngineForTest()->status();
        std::fprintf(stderr, "要求観測 %s: 受理=%d state=%d->%d frame=%lld->%lld status=%s\n",
                     label, accepted, static_cast<int>(before.state), static_cast<int>(after.state),
                     static_cast<long long>(before.position.outputFrame),
                     static_cast<long long>(after.position.outputFrame),
                     qUtf8Printable(controller.statusText()));
    }
    return accepted;
}

inline void tracePreview(app::MvmController& controller, QQuickWindow* window, const char* label) {
    if (!qEnvironmentVariableIsSet("MVM_TEST_PREVIEW_TRACE"))
        return;

    struct Observation {
        QElapsedTimer elapsed;
        QString previous;
        std::atomic<unsigned long long> swaps{0};
    };

    auto observation = std::make_shared<Observation>();
    observation->elapsed.start();
    QObject::connect(
        window, &QQuickWindow::frameSwapped, &controller, [observation] { ++observation->swaps; },
        Qt::DirectConnection);
    auto* timer = new QTimer(&controller);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &controller,
        [&controller, window = QPointer<QQuickWindow>(window), observation,
         label = QByteArray(label)] {
            if (!window || !controller.previewEngineForTest())
                return;
            const auto status = controller.previewEngineForTest()->status();
            const auto desired = status.latestAcceptedDesiredComposition;
            const auto presented = status.lastPresentedComposition;
            const QString record =
                QStringLiteral(
                    "state=%1 frame=%2 head=%3 playing=%4 shuttle=%5 desired=%6/%7 presented=%8/%9 "
                    "latest=%10 visible=%11 exposed=%12 active=%13 swaps=%14 status=%15")
                    .arg(static_cast<int>(status.state))
                    .arg(status.position.outputFrame)
                    .arg(controller.playheadFrame())
                    .arg(controller.playing())
                    .arg(controller.shuttleRate())
                    .arg(desired ? desired->id.value : 0)
                    .arg(desired ? desired->revision : 0)
                    .arg(presented ? presented->id.value : 0)
                    .arg(presented ? presented->revision : 0)
                    .arg(controller.previewPresentedLatest())
                    .arg(window->isVisible())
                    .arg(window->isExposed())
                    .arg(window->isActive())
                    .arg(observation->swaps.load())
                    .arg(controller.statusText());
            if (record == observation->previous)
                return;
            observation->previous = record;
            std::fprintf(stderr, "状態観測 %s %lldms: %s\n", label.constData(),
                         static_cast<long long>(observation->elapsed.elapsed()),
                         qUtf8Printable(record));
        });
    timer->start();
    std::fprintf(stderr, "状態観測 %s: Quick API=%d animation=%s render=%s\n", label,
                 static_cast<int>(QQuickWindow::graphicsApi()),
                 qgetenv("QSG_USE_SIMPLE_ANIMATION_DRIVER").constData(),
                 qgetenv("QSG_RENDER_LOOP").constData());
}
} // namespace mvm::test
#endif
