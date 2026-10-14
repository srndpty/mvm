// P3-7B.2: 書き出しの結果の文を、preview の確認 (pollPreviewState) が上書きしない。
//
//   mvm_test_text_ui_input --status-ownership-ui
//
// 実 controller・実 window (製品の Main.qml)・製品の書き出し (mvm::app::exportTimeline)
// を使う。数式の 描画だけ偽の backend。素材の clip が無い数式だけの Project で、書き出しの結果
// (成功・取消・timeout・ 最終の名前へ置けない失敗) が出た後に、preview
// が使える状態へ変わった時の処理を、決まった順序で 起こす (markPreviewNotReadyForTest →
// pollPreviewStateForTest)。controller の状態の文と、Main.qml の 状態の表示 (statusTextLabel)
// に、結果の文が残ることを確かめる。
//
// 修正の前 (P3-7B.1 まで) は、この処理が状態の表示を「素材を追加してください」で上書きした
// (controller の記録: 書き出しの失敗の文の 0.18 秒後)。
//
// 利用者の新しい操作 (seek・別の書き出し・別の Project を開く・失敗する操作) は、結果の文を置き換え
// られること、置き換えた後は preview の確認の文がまた出ること (結果の文が居座らない) も確かめる。

#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "app/timeline_export.h"
#include "math_fake_backend.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "test_window_focus.h"
#include "waveform_cache.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <QCoreApplication>
#include <QEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
namespace project = mvm::project;
using mvm::app::MvmController;

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

QUrl urlOf(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
}

std::string text(const QString& value) {
    return value.toStdString();
}

// 数式だけの Project (V1 は空、V2・V3 に数式)。素材の clip は無い。
project::Project mathOnlyProject() {
    auto out = project::createDefaultProject();
    out.outputWidth = 640;
    out.outputHeight = 360;
    while (out.videoTracks.size() < 3) {
        auto track = out.videoTracks.front();
        track.name = "V" + std::to_string(out.videoTracks.size() + 1);
        out.videoTracks.push_back(track);
    }
    const auto math = [](const std::string& id, int track, const std::string& source) {
        project::TimelineClip clip;
        clip.kind = project::TimelineClipKind::Math;
        clip.id = id;
        clip.name = id;
        clip.sourceFpsNum = 60;
        clip.sourceFpsDen = 1;
        clip.sourceFrameCount = clip.sourceOutFrame = 30;
        clip.track = {project::TrackKind::Video, track};
        clip.math.source = source;
        clip.math.fontSize = 96;
        clip.math.color = "#FFFF0000";
        return clip;
    };
    out.timelineClips.push_back(math("m1", 1, "SIZE80x40 a"));
    out.timelineClips.push_back(math("m2", 2, "SIZE78x38 b"));
    return out;
}

struct Hooks {
    std::atomic<bool> hold{false};
    std::atomic<bool> holding{false};
    std::atomic<bool> timeout{false}; // 次の書き出しの timeout を 1 ms にする (GUI thread が決める)
};

struct Session {
    std::shared_ptr<Hooks> hooks = std::make_shared<Hooks>();
    mvm::test::FakeMathBackend backend;
    std::unique_ptr<MvmController> controller;
    mvm::app::WaveformCache cache;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;
    QString failure;

    ~Session() { close(); }

    void close() {
        engine.reset();
        window = nullptr;
        if (controller) {
            controller->shutdown();
            controller.reset();
        }
    }

    bool open(const std::filesystem::path& path, const project::Project& initial) {
        auto runner = [hooks = hooks](const project::Project& exportProject,
                                      const mvm::app::TimelineExportRequest& request) {
            hooks->holding.store(true);
            while (hooks->hold.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            hooks->holding.store(false);
            auto input = request;
            if (hooks->timeout.load())
                input.timeoutMs = 1;
            return mvm::app::exportTimeline(exportProject, input);
        };
        controller = std::make_unique<MvmController>(
            path, std::filesystem::path{}, initial, nullptr, std::move(runner),
            MvmController::ExportThreadFactory{},
            MvmController::FileRevealer{
                [](const std::filesystem::path&, QString&) { return true; }});
        controller->setMathPreflightForTest(backend.preflight());
        QObject::connect(controller.get(), &MvmController::exportFailed,
                         [this](const QString& message) { failure = message; });
        engine = std::make_unique<QQmlApplicationEngine>();
        auto properties = mvm::app::testFixedWindowInitialProperties();
        // 製品 (main.cpp) と同じく、preview の描画先を controller へ渡してから表示する。
        properties.insert(QStringLiteral("visible"), false);
        properties.insert(QStringLiteral("mvmController"), QVariant::fromValue(controller.get()));
        properties.insert(QStringLiteral("waveformCache"), QVariant::fromValue(&cache));
        if (!mvm::app::testFixedWindowRequested())
            properties.insert(QStringLiteral("flags"), mvm::test::backgroundWindowFlags());
        engine->setInitialProperties(properties);
        engine->load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        window = engine->rootObjects().isEmpty()
                     ? nullptr
                     : qobject_cast<QQuickWindow*>(engine->rootObjects().first());
        if (!window)
            return false;
        auto* surface =
            window->findChild<mvm::app::PreviewEngineRhiItem*>(QStringLiteral("previewSurface"));
        if (!surface)
            return false;
        controller->attachPreview(surface);
        window->setVisible(true);
        return true;
    }

    bool mathReady() {
        return pumpUntil(
            [&] {
                for (const auto& clip : controller->projectForTest().timelineClips)
                    if (clip.kind == project::TimelineClipKind::Math &&
                        controller->mathClipData(QString::fromStdString(clip.id))
                                .value(QStringLiteral("state"))
                                .toString() != QStringLiteral("ready"))
                        return false;
                return true;
            },
            30000);
    }

    QQuickItem* find(const QString& name) {
        if (QQuickTest::qIsPolishScheduled(window))
            window->grabWindow();
        QQuickItem* found = nullptr;
        const std::function<void(QQuickItem*)> visit = [&](QQuickItem* item) {
            if (found)
                return;
            if (item->objectName() == name && item->isVisible()) {
                found = item;
                return;
            }
            for (auto* child : item->childItems())
                visit(child);
        };
        visit(window->contentItem());
        return found;
    }

    // Main.qml の状態の表示に出ている文。
    QString shownStatus() {
        auto* label = find(QStringLiteral("statusTextLabel"));
        return label ? label->property("text").toString() : QStringLiteral("(状態の表示が無い)");
    }

    void click(QQuickItem* item) {
        QTest::mouseClick(
            window, Qt::LeftButton, {},
            item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }

    bool openSettings(const std::filesystem::path& output) {
        auto* dialog = window->findChild<QObject*>(QStringLiteral("exportFileDialog"));
        if (!dialog || !dialog->setProperty("selectedFile", urlOf(output)) ||
            !QMetaObject::invokeMethod(dialog, "accepted"))
            return false;
        return pumpUntil([&] { return find(QStringLiteral("exportAcceptButton")) != nullptr; },
                         5000);
    }

    bool turnOn() {
        auto* box = find(QStringLiteral("exportMathGroupCheckBox"));
        if (!box || box->property("checked").toBool())
            return false;
        QTest::mouseClick(window, Qt::LeftButton, {},
                          box->mapToScene(QPointF(12, box->height() / 2)).toPoint());
        return pumpUntil([&] { return box->property("checked").toBool(); }, 2000);
    }

    bool accept() {
        auto* button = find(QStringLiteral("exportAcceptButton"));
        if (!button)
            return false;
        failure.clear();
        click(button);
        return pumpUntil([&] { return controller->exporting() || !failure.isEmpty(); }, 5000) &&
               controller->exporting();
    }

    bool finished() {
        return pumpUntil([&] { return !controller->exporting(); }, 120000);
    }

    bool exportOnce(const std::filesystem::path& output, bool accelerate) {
        return openSettings(output) && (!accelerate || turnOn()) && accept() && finished();
    }

    void dismissFailure() {
        if (auto* dialog = window->findChild<QObject*>(QStringLiteral("exportFailureDialog")))
            QMetaObject::invokeMethod(dialog, "close");
        pumpUntil([] { return false; }, 150);
    }

    // preview が使える状態へ変わった時の処理を 1 回起こす (修正の前に結果の文を上書きした処理)。
    bool deliverReadinessPoll() {
        // seek の直後などで engine がまだ使える状態でなければ、使える状態になるまで待つ (待つ間の
        // timer の poll はそのまま届く)。
        pumpUntil(
            [&] {
                return controller->previewEngineForTest()->status().state ==
                       mvm::preview::PreviewEngineState::ReadyPaused;
            },
            5000);
        controller->markPreviewNotReadyForTest();
        controller->pollPreviewStateForTest();
        return controller->previewReady();
    }
};

// 書き出しの結果 (status) が、preview の確認の後も controller と Main.qml の両方に残る。
void expectResultSurvives(Session& s, const QString& result, const std::string& label) {
    check(s.controller->statusText() == result && s.shownStatus() == result,
          label + ": 書き出しの直後に結果の文が出ている: '" + text(s.shownStatus()) + "'");
    const auto suppressedBefore = s.controller->suppressedRoutineStatusCountForTest();
    const bool transitioned = s.deliverReadinessPoll();
    check(transitioned, label + ": preview が使える状態へ変わった時の処理が起きた");
    // 出どころ: preview の確認は定常の文を出そうとし、書き出しの結果があるので出さなかった。
    const QString suppressed = s.controller->lastSuppressedRoutineStatusForTest();
    check(s.controller->suppressedRoutineStatusCountForTest() > suppressedBefore &&
              (suppressed == QStringLiteral("素材を追加してください") ||
               suppressed.endsWith(QStringLiteral(" を表示しています"))),
          label + ": preview の確認の定常の文を出さなかった記録がある: '" + text(suppressed) + "'");
    pumpUntil([] { return false; }, 300); // 続けて timer の poll も届ける
    check(s.controller->statusText() == result,
          label + ": preview の確認の後も controller の文は結果のまま: '" +
              text(s.controller->statusText()) + "'");
    check(s.shownStatus() == result,
          label + ": preview の確認の後も Main.qml の状態の表示は結果のまま: '" +
              text(s.shownStatus()) + "'");
}

} // namespace

int runStatusOwnershipUi(const std::filesystem::path& directory) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto projectPath = directory / L"math-only.mvm";
    const auto otherPath = directory / L"other.mvm";
    check(project::saveProjectJson(mathOnlyProject(), projectPath).success &&
              project::saveProjectJson(mathOnlyProject(), otherPath).success,
          "fixture の Project を保存する");
    const QString noMedia = QStringLiteral("素材を追加してください");

    Session s;
    if (!s.open(projectPath, mathOnlyProject()))
        return 3;
    QString reason;
    if (!QTest::qWaitForWindowExposed(s.window) || !mvm::test::focusWithoutForeground(s.window) ||
        !mvm::test::isolatedFromUserInput(s.window, reason)) {
        std::fprintf(stderr, "PROTOCOL_INVALID: 試験を操作から隔離できません: %s\n",
                     qUtf8Printable(reason));
        return 4;
    }
    check(pumpUntil([&] { return s.controller->previewReady(); }, 30000), "preview の初期化");
    check(s.mathReady(), "数式の描画が ready になる");

    // --- 前提: 書き出しの無い状態では、preview の確認は自分の文を出す
    // (以前と同じ。初回は自動の初期 seek の「… を表示しています」、2
    // 回目は「素材を追加してください」) ---
    const QString initial = s.controller->statusText();
    check(s.deliverReadinessPoll() &&
              (s.controller->statusText() == noMedia ||
               s.controller->statusText().endsWith(QStringLiteral(" を表示しています"))) &&
              s.shownStatus() == s.controller->statusText(),
          "前提: 書き出しの無い状態では、preview の確認が定常の文を出す ('" + text(initial) +
              "' → '" + text(s.controller->statusText()) + "')");

    // --- 書き出しの最中は、preview の確認が「書き出しています…」を置き換えない ---
    s.hooks->hold = true;
    check(s.openSettings(directory / L"during.mp4") && s.accept() &&
              pumpUntil([&] { return s.hooks->holding.load(); }, 5000),
          "最中: 書き出しを始める (worker は待ち)");
    s.deliverReadinessPoll();
    check(s.controller->statusText() == QStringLiteral("書き出しています…"),
          "最中: preview の確認の後も「書き出しています…」: '" + text(s.controller->statusText()) +
              "'");
    s.hooks->hold = false;
    check(s.finished(), "最中: 書き出しが終わる");

    // --- 1. 従来の方法 (選択なし) の成功 ---
    const auto plain = directory / L"plain.mp4";
    check(s.exportOnce(plain, false), "1: 従来の方法で書き出す");
    const QString plainResult = s.controller->statusText();
    check(plainResult.startsWith(QStringLiteral("書き出しました: ")),
          "1: 成功の文: '" + text(plainResult) + "'");
    expectResultSurvives(s, plainResult, "1 (従来の方法の成功)");

    // --- 2. 数式の高速化を使った成功 ---
    check(s.exportOnce(directory / L"accelerated.mp4", true), "2: 数式の高速化で書き出す");
    const QString usedResult = s.controller->statusText();
    check(usedResult.startsWith(
              QStringLiteral("数式の高速化を使いました（数式 clip 2 本）。書き出しました: ")),
          "2: 成功の文: '" + text(usedResult) + "'");
    expectResultSurvives(s, usedResult, "2 (数式の高速化の成功)");

    // --- 3. 取消 ---
    s.hooks->hold = true;
    check(s.openSettings(directory / L"cancel.mp4") && s.accept() &&
              pumpUntil([&] { return s.hooks->holding.load(); }, 5000),
          "3: 書き出しを始める (worker は待ち)");
    auto* cancel = s.find(QStringLiteral("exportCancelButton"));
    if (cancel)
        s.click(cancel);
    check(cancel && pumpUntil([&] { return s.controller->exportCancelling(); }, 3000),
          "3: キャンセルボタンを押す");
    s.hooks->hold = false;
    check(s.finished(), "3: 取消で終わる");
    expectResultSurvives(s, QStringLiteral("書き出しをキャンセルしました"), "3 (取消)");

    // --- 4. timeout ---
    s.hooks->timeout = true;
    check(s.openSettings(directory / L"timeout.mp4") && s.accept() && s.finished(),
          "4: timeout する書き出し");
    s.hooks->timeout = false;
    const QString timeoutResult = s.controller->statusText();
    check(timeoutResult.startsWith(QStringLiteral("書き出しに失敗しました: ")) &&
              timeoutResult.contains(QStringLiteral("timeout")) && s.failure == timeoutResult,
          "4: timeout の失敗の文と通知: '" + text(timeoutResult) + "'");
    expectResultSurvives(s, timeoutResult, "4 (timeout)");
    s.dismissFailure();

    // --- 5. 最終の名前へ置けない失敗 (書き出し先を他が掴んでいる) ---
    {
        const HANDLE held = CreateFileW(plain.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE, "5: 書き出し先 (1 の出力) を掴む");
        check(s.openSettings(plain) && s.accept() && s.finished(), "5: 同じ名前へ書き出す");
        if (held != INVALID_HANDLE_VALUE)
            CloseHandle(held);
        const QString renameResult = s.controller->statusText();
        check(renameResult.startsWith(QStringLiteral("書き出しに失敗しました: ")) &&
                  renameResult.contains(QStringLiteral("rename")),
              "5: 最終の名前へ置けない失敗の文: '" + text(renameResult) + "'");
        expectResultSurvives(s, renameResult, "5 (最終の名前へ置けない)");
        s.dismissFailure();
    }

    // --- 6. preview の engine を作り直す (自動の初期 seek が「… を表示しています」を書く経路) ---
    check(s.exportOnce(directory / L"before-reset.mp4", false), "6: 書き出す");
    const QString beforeReset = s.controller->statusText();
    check(s.controller->resetPreviewEngineForTest(), "6: preview の engine を作り直す");
    check(pumpUntil([&] { return s.controller->previewReady(); }, 30000),
          "6: preview がまた使える");
    pumpUntil([] { return false; }, 500);
    check(s.controller->statusText() == beforeReset && s.shownStatus() == beforeReset,
          "6: engine の作り直しの後も結果の文のまま: '" + text(s.controller->statusText()) + "'");
    check(s.controller->lastSuppressedRoutineStatusForTest().endsWith(
              QStringLiteral(" を表示しています")) ||
              s.controller->lastSuppressedRoutineStatusForTest() == noMedia,
          "6: 自動の初期 seek の文を出さなかった記録: '" +
              text(s.controller->lastSuppressedRoutineStatusForTest()) + "'");

    // --- 7. 利用者の操作は、結果の文を置き換えられる。置き換えた後は preview の確認の文がまた出る
    // ---
    check(s.controller->seekTimelineFrame(0) && s.controller->statusText() != beforeReset &&
              s.controller->statusText().endsWith(QStringLiteral(" を表示しています")),
          "7: 利用者の seek が結果の文を置き換える: '" + text(s.controller->statusText()) + "'");
    // 書き出しの結果でない文 (失敗する操作の理由) の後は、preview の確認がまた自分の文を出す
    // (書き出しの結果の文を手放した後に、抑止が残らない)。
    check(!s.controller->addMediaFileToTimeline(urlOf(directory / L"無い素材.mp4")),
          "7: 失敗する操作で、書き出しの結果でない文を出す");
    const QString operationError = s.controller->statusText();
    check(s.deliverReadinessPoll() && s.controller->statusText() != operationError &&
              (s.controller->statusText() == noMedia ||
               s.controller->statusText().endsWith(QStringLiteral(" を表示しています"))) &&
              s.shownStatus() == s.controller->statusText(),
          "7: 結果を手放した後は、preview の確認の文がまた出る (居座らない): '" +
              text(s.controller->statusText()) + "'");

    // --- 8. 2 回目の書き出し: 前の結果を新しい書き出しが置き換え、新しい結果が残る ---
    check(s.exportOnce(directory / L"first.mp4", false), "8: 1 回目");
    const QString first = s.controller->statusText();
    s.hooks->hold = true;
    check(s.openSettings(directory / L"second.mp4") && s.accept() &&
              pumpUntil([&] { return s.hooks->holding.load(); }, 5000) &&
              s.controller->statusText() == QStringLiteral("書き出しています…"),
          "8: 2 回目の書き出しが前の結果を「書き出しています…」で置き換える: '" +
              text(s.controller->statusText()) + "'");
    s.hooks->hold = false;
    check(s.finished(), "8: 2 回目が終わる");
    const QString second = s.controller->statusText();
    check(second != first && second.contains(QStringLiteral("second.mp4")),
          "8: 2 回目の結果: '" + text(second) + "'");
    expectResultSurvives(s, second, "8 (2 回目の書き出し)");

    // --- 9. 結果の後の、本当の失敗 (利用者の操作) は出る ---
    const auto missing = directory / L"存在しない素材.mp4";
    check(!s.controller->addMediaFileToTimeline(urlOf(missing)) &&
              s.controller->statusText() != second && s.shownStatus() == s.controller->statusText(),
          "9: 結果の後の失敗する操作の理由が出る: '" + text(s.controller->statusText()) + "'");

    // --- 10. 別の Project を開く: 結果の文を持ち越さない ---
    check(s.exportOnce(directory / L"before-switch.mp4", false), "10: 書き出す");
    const QString beforeSwitch = s.controller->statusText();
    check(s.controller->openProject(urlOf(otherPath)), "10: 別の Project を開く");
    s.controller->setMathPreflightForTest(s.backend.preflight());
    check(pumpUntil([&] { return s.controller->previewReady(); }, 30000), "10: preview の準備");
    // 開いた直後の preview の確認は、自動の初期 seek (「… を表示しています」)
    // か「素材を追加してください」。
    check(s.deliverReadinessPoll() && s.controller->statusText() != beforeSwitch &&
              (s.controller->statusText() == noMedia ||
               s.controller->statusText().endsWith(QStringLiteral(" を表示しています"))),
          "10: 開いた後は、前の Project の結果の文を残さず、preview の確認の文が出る: '" +
              text(s.controller->statusText()) + "'");

    // --- 11. 結果の後、preview の確認が届く前に controller を閉じる ---
    s.mathReady();
    check(s.exportOnce(directory / L"before-shutdown.mp4", false), "11: 書き出す");
    // 製品 (main.cpp) と同じ順: aboutToQuit で controller を閉じ、その後で window を壊す。
    const QString beforeShutdown = s.controller->statusText();
    s.controller->markPreviewNotReadyForTest();
    // 描画先の大きさが変わった後の描き直しを積んでから閉じる。積んだ描き直しは、閉じた後の event の
    // 処理で届く (CTest の 1 回目で、window
    // の大きさの変更からこれが起き、「Previewを再描画できません」 で結果の文を上書きした)。
    s.controller->queuePreviewResizeRefreshForTest();
    s.controller->shutdown();
    s.controller->pollPreviewStateForTest(); // 閉じた後の poll
    pumpUntil([] { return false; }, 300);    // 残った queued event を処理する
    check(s.controller->statusText() == beforeShutdown,
          "11: 閉じた後の poll と残った event は、結果の文を書き換えない: '" +
              text(s.controller->statusText()) + "'");
    s.engine.reset();
    s.window = nullptr;
    s.controller.reset();
    check(true, "11: 閉じた後の poll と、残った event の処理で落ちない");

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
