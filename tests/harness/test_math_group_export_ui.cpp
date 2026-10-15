// P3-6C.1: 製品の Main.qml の書き出し設定から、数式の高速化 (数式 group) を選ぶ。
//
//   mvm_test_text_ui_input --math-group-export-ui
//
// 実 controller・実 window・製品の書き出し (mvm::app::exportTimeline) を使う。数式の描画だけ偽の
// backend。書き出し先の指定 (FileDialog の accepted)、checkbox、「書き出す」、「キャンセル」は、
// 製品の QML の部品を操作する。checkbox の状態ではなく、書き出しの要求・結果の記録・選ばれた
// graph・ 出力の byte 列を確かめる。
//
// 書き出しの関数の前に試験の runner を挟み、controller が作った要求を記録する。上限の超過・実行時の
// 失敗・timeout は、その runner が要求の写しに診断用の入力を足して起こす (QML からは届かない入力)。

#include "app/preview/test_window_mode.h"
#include "app/timeline_export.h"
#include "math_fake_backend.h"
#include "media/math/math_group_compose.h"
#include "media/mlt/mvm_mlt_export.h"
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
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

namespace {
namespace project = mvm::project;
using mvm::app::MathGroupOptimization;
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

void pump(int milliseconds = 100) {
    pumpUntil([] { return false; }, milliseconds);
}

constexpr int kFrames = 60;

// V1 の種類: 0 = clip が無い (判定は ELIGIBLE)、1 = 透明な余白のある数式 (INELIGIBLE)、
// 2 = 1 を拡大したもの (UNKNOWN)、3 = 数式 clip が 1 本も無い (文字だけ)。
project::Project fixtureProject(int base) {
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
        clip.sourceFrameCount = clip.sourceOutFrame = kFrames;
        clip.track = {project::TrackKind::Video, track};
        clip.math.source = source;
        clip.math.fontSize = 96;
        clip.math.color = "#FFFF0000";
        return clip;
    };
    if (base == 3) {
        project::TimelineClip title;
        title.kind = project::TimelineClipKind::Text;
        title.id = "title";
        title.name = "title";
        title.text.content = "title";
        title.sourceFpsNum = 60;
        title.sourceFrameCount = title.sourceOutFrame = kFrames;
        out.timelineClips.push_back(title);
        return out;
    }
    if (base != 0) {
        auto clip = math("base", 0, "SIZE60x30 c");
        if (base == 2)
            clip.effects.scaleXPercent = clip.effects.scaleYPercent = 150;
        out.timelineClips.push_back(clip);
    }
    out.timelineClips.push_back(math("m1", 1, "SIZE80x40 a"));
    out.timelineClips.push_back(math("m2", 2, "SIZE78x38 b"));
    return out;
}

// 試験の runner が記録するものと、次の書き出しへ足す診断用の入力。
struct Hooks {
    enum class Inject { None, Budget, RuntimeFault, Timeout };

    std::atomic<int> runs{0};
    std::atomic<int> modeSeen{-1};
    std::atomic<bool> observerSupplied{false};
    std::atomic<bool> validatorSupplied{false};
    std::atomic<int> timeoutSeen{0};
    std::atomic<bool> hold{false};    // true の間、書き出しの関数を呼ばずに待つ
    std::atomic<bool> holding{false}; // 待ちに入った
    Inject inject = Inject::None;     // 書き出しの開始の前に GUI thread が決める
    mvm::app::MathGroupExportReport report;
    long long liveTransitions = -1, activeCallbacks = -1, rasterLiveBytes = -1;
};

struct Session {
    std::shared_ptr<Hooks> hooks = std::make_shared<Hooks>();
    mvm::test::FakeMathBackend backend;
    std::unique_ptr<MvmController> controller;
    mvm::app::WaveformCache cache;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;
    QString failure; // 最後の exportFailed

    ~Session() { close(); }

    void close() {
        if (engine) {
            engine.reset();
            window = nullptr;
        }
        if (controller) {
            controller->shutdown();
            controller.reset();
        }
    }

    bool open(const std::filesystem::path& path, const project::Project& initial) {
        auto runner = [hooks = hooks](const project::Project& exportProject,
                                      const mvm::app::TimelineExportRequest& request) {
            ++hooks->runs;
            hooks->modeSeen.store(static_cast<int>(request.mathGroupOptimization));
            hooks->observerSupplied.store(request.exportObserver != nullptr);
            hooks->validatorSupplied.store(static_cast<bool>(request.encoderFrameValidator));
            hooks->timeoutSeen.store(request.timeoutMs);
            hooks->holding.store(true);
            while (hooks->hold.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            hooks->holding.store(false);
            mvm::app::TimelineExportResult result;
            MvmExportObserver observer{};
            if (hooks->inject == Hooks::Inject::None) {
                // controller が作った要求を、そのまま製品の書き出しの関数へ渡す。
                result = mvm::app::exportTimeline(exportProject, request);
            } else {
                auto input = request;
                if (hooks->inject == Hooks::Inject::Budget) {
                    observer.diagnostic_math_group_raster_budget_bytes = 15000;
                    input.exportObserver = &observer;
                } else if (hooks->inject == Hooks::Inject::RuntimeFault) {
                    observer.diagnostic_math_group_fault = 4; // 実行時に下地を拒否する
                    input.exportObserver = &observer;
                } else {
                    input.timeoutMs = 1;
                }
                result = mvm::app::exportTimeline(exportProject, input);
            }
            hooks->report = result.mathGroup;
            hooks->liveTransitions = mvm_mlt_math_group_live();
            hooks->activeCallbacks = mvm_mlt_math_group_active();
            hooks->rasterLiveBytes = mvm::math::preparedMathRasterLiveBytes();
            return result;
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
        properties.insert(QStringLiteral("mvmController"), QVariant::fromValue(controller.get()));
        properties.insert(QStringLiteral("waveformCache"), QVariant::fromValue(&cache));
        if (!mvm::app::testFixedWindowRequested())
            properties.insert(QStringLiteral("flags"), mvm::test::backgroundWindowFlags());
        engine->setInitialProperties(properties);
        engine->load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        window = engine->rootObjects().isEmpty()
                     ? nullptr
                     : qobject_cast<QQuickWindow*>(engine->rootObjects().first());
        return window != nullptr;
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

    // 見えている部品だけを返す (閉じた dialog の部品は返さない)。
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

    void click(QQuickItem* item, QPointF local) {
        QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(local).toPoint());
    }

    // 書き出し先を指定して、書き出し設定を開く (製品の FileDialog の accepted)。
    bool openSettings(const std::filesystem::path& output) {
        auto* dialog = window->findChild<QObject*>(QStringLiteral("exportFileDialog"));
        if (!dialog ||
            !dialog->setProperty("selectedFile",
                                 QUrl::fromLocalFile(QString::fromStdWString(output.wstring()))) ||
            !QMetaObject::invokeMethod(dialog, "accepted"))
            return false;
        return pumpUntil([&] { return find(QStringLiteral("exportAcceptButton")) != nullptr; },
                         5000);
    }

    QQuickItem* checkBox() { return find(QStringLiteral("exportMathGroupCheckBox")); }

    // checkbox を実際に click して on にする。
    bool turnOn() {
        auto* box = checkBox();
        if (!box || box->property("checked").toBool())
            return false;
        click(box, QPointF(12, box->height() / 2));
        return pumpUntil([&] { return box->property("checked").toBool(); }, 2000);
    }

    bool accept() {
        auto* button = find(QStringLiteral("exportAcceptButton"));
        if (!button)
            return false;
        failure.clear();
        click(button, QPointF(button->width() / 2, button->height() / 2));
        return pumpUntil([&] { return controller->exporting() || !failure.isEmpty(); }, 5000) &&
               controller->exporting();
    }

    bool finished(int timeoutMs = 120000) {
        return pumpUntil([&] { return !controller->exporting(); }, timeoutMs);
    }

    // 設定を開く → (選ぶ) → 書き出す → 終わるまで待つ。
    bool exportOnce(const std::filesystem::path& output, bool accelerate) {
        if (!openSettings(output))
            return false;
        if (accelerate && !turnOn())
            return false;
        return accept() && finished();
    }

    void dismissFailure() {
        if (auto* dialog = window->findChild<QObject*>(QStringLiteral("exportFailureDialog")))
            QMetaObject::invokeMethod(dialog, "close");
        pump(150);
    }

    // 証拠用: MVM_P36C1_EVIDENCE_DIR があれば、描画した window を PNG に保存する (検査はしない)。
    void shot(const char* name) {
        const QString evidence = qEnvironmentVariable("MVM_P36C1_EVIDENCE_DIR");
        if (evidence.isEmpty())
            return;
        pump(150);
        window->grabWindow().save(evidence + QLatin1Char('/') + QString::fromLatin1(name) +
                                  QStringLiteral(".png"));
    }

    QVariant outcome(const char* key) {
        return controller->exportMathGroupOutcome().value(QString::fromLatin1(key));
    }
};

QByteArray fileBytes(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool leftBehind(const std::filesystem::path& output) {
    auto temporary = output;
    temporary += ".mvmtmp";
    std::error_code ignored;
    return std::filesystem::exists(output, ignored) || std::filesystem::exists(temporary, ignored);
}

bool settingsMentionExport() {
    QSettings saved;
    saved.sync();
    for (const auto& key : saved.allKeys()) {
        const auto lower = key.toLower();
        if (lower.contains(QStringLiteral("mathgroup")) ||
            lower.contains(QStringLiteral("export")) || lower.contains(QStringLiteral("accelerat")))
            return true;
    }
    return false;
}

} // namespace

int runMathGroupExportUi(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const std::filesystem::path paths[4] = {directory / L"eligible.mvm",
                                            directory / L"ineligible.mvm",
                                            directory / L"unknown.mvm", directory / L"no-math.mvm"};
    for (int base = 0; base < 4; ++base)
        check(project::saveProjectJson(fixtureProject(base), paths[base]).success,
              "fixture の Project を保存する");
    const auto urlOf = [](const std::filesystem::path& path) {
        return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
    };
    const int kOff = static_cast<int>(MathGroupOptimization::Off);
    const int kAuto = static_cast<int>(MathGroupOptimization::Auto);
    QByteArray eligibleOff;

    {
        Session s;
        if (!s.open(paths[0], fixtureProject(0)))
            return 3;
        QString reason;
        if (!QTest::qWaitForWindowExposed(s.window) ||
            !mvm::test::focusWithoutForeground(s.window) ||
            !mvm::test::isolatedFromUserInput(s.window, reason)) {
            std::fprintf(stderr, "PROTOCOL_INVALID: 試験を操作から隔離できません: %s\n",
                         qUtf8Printable(reason));
            return 4;
        }
        auto& hooks = *s.hooks;
        check(s.mathReady(), "数式の描画が ready になる");
        check(!settingsMentionExport(), "開始時: 設定 file に書き出しの選択が無い");

        // --- A. 既定は off。選択は見えていて、説明がある ---
        const auto offPath = directory / L"eligible-off.mp4";
        check(s.openSettings(offPath), "A: 書き出し設定を開く");
        auto* box = s.checkBox();
        auto* hint = s.find(QStringLiteral("exportMathGroupHint"));
        check(box && !box->property("checked").toBool() &&
                  box->property("text").toString() ==
                      QStringLiteral("数式の書き出しを高速化（試験的）"),
              "A: 数式の Project では選択が見え、既定は off");
        const QString hintText = hint ? hint->property("text").toString() : QString();
        check(hint && hintText.contains(QStringLiteral("この書き出しだけ")) &&
                  hintText.contains(QStringLiteral("従来の方法で書き出します")) &&
                  hintText.contains(QStringLiteral("書き出しを止めます")) &&
                  hintText.contains(QStringLiteral("自動ではやり直しません")),
              "A: 対象外は従来の方法、失敗は止める、という説明がある");
        s.shot("export-settings-default-off");
        check(s.accept() && s.finished(),
              "A: 既定のまま書き出す: " + s.controller->statusText().toStdString());
        eligibleOff = fileBytes(offPath);
        check(hooks.modeSeen == kOff && !hooks.observerSupplied && !hooks.validatorSupplied &&
                  hooks.timeoutSeen == 600000 && !hooks.report.evaluated &&
                  !hooks.report.mathGroupGraph && hooks.report.composedFrames == 0 &&
                  !eligibleOff.isEmpty(),
              "A: 既定の要求は Off (判定しない、従来の graph、observer なし、timeout は製品の値)");
        check(s.outcome("outcome") == QStringLiteral("off") &&
                  !s.controller->statusText().contains(QStringLiteral("高速化")),
              "A: 結果は off で、高速化の文を出さない");

        // --- B. 明示の on、ELIGIBLE → 数式 group の graph ---
        const auto onPath = directory / L"eligible-on.mp4";
        check(s.exportOnce(onPath, true),
              "B: on にして書き出す: " + s.controller->statusText().toStdString());
        check(hooks.modeSeen == kAuto && !hooks.observerSupplied && !hooks.validatorSupplied &&
                  hooks.timeoutSeen == 600000,
              "B: on の要求は Auto (observer なし、timeout は製品の値)");
        check(hooks.report.evaluated && hooks.report.decision == "ELIGIBLE" &&
                  hooks.report.mathGroupGraph && hooks.report.groupedClips == 2 &&
                  hooks.report.composedFrames > 0 && hooks.report.errors == 0 &&
                  hooks.report.affineTransitions == 0,
              "B: 判定 ELIGIBLE、数式 group の graph で実際に合成する: 判定 '" +
                  hooks.report.decision + "' (" + hooks.report.reason + ")");
        check(s.outcome("outcome") == QStringLiteral("used") &&
                  s.controller->statusText().startsWith(QStringLiteral(
                      "数式の高速化を使いました（数式 clip 2 本）。書き出しました: ")),
              "B: 使ったことを利用者へ報告する: " + s.controller->statusText().toStdString());
        check(fileBytes(onPath) == eligibleOff, "B: 出力は off と同じ byte 列");
        check(hooks.liveTransitions == 0 && hooks.activeCallbacks == 0 &&
                  hooks.rasterLiveBytes == 0,
              "B: transition・callback・準備済みの画像が残らない");

        // --- C. 設定を開き直すと off に戻る。D. くり返し ---
        const auto againPath = directory / L"eligible-on-again.mp4";
        check(s.openSettings(againPath) && s.checkBox() &&
                  !s.checkBox()->property("checked").toBool(),
              "C: on で書き出した後に開き直すと off に戻っている");
        const bool turnedOn = s.turnOn();
        s.shot("export-settings-on");
        check(turnedOn && s.accept() && s.finished() && hooks.modeSeen == kAuto &&
                  hooks.report.mathGroupGraph && fileBytes(againPath) == eligibleOff &&
                  hooks.rasterLiveBytes == 0,
              "D: もう一度 on で書き出しても同じ出力");

        // --- E. 書き出しの最中に checkbox を変えても、その書き出しは変わらない ---
        const auto heldOff = directory / L"eligible-held-off.mp4";
        hooks.hold = true;
        check(s.openSettings(heldOff) && s.accept() &&
                  pumpUntil([&] { return hooks.holding.load(); }, 5000),
              "E: off で書き出しを始める (worker は待ち)");
        auto* closedBox = s.window->findChild<QObject*>(QStringLiteral("exportMathGroupCheckBox"));
        check(closedBox && closedBox->setProperty("checked", true),
              "E: 最中に checkbox を on にする");
        s.controller->setMathGroupExportOptimization(
            true); // 内部の選択も、UI の書き出しには効かない
        hooks.hold = false;
        check(s.finished() && hooks.modeSeen == kOff && !hooks.report.evaluated &&
                  s.outcome("outcome") == QStringLiteral("off") &&
                  fileBytes(heldOff) == eligibleOff,
              "E: 始めた書き出しは Off のまま");
        const auto internalSet = directory / L"eligible-internal-set.mp4";
        check(s.exportOnce(internalSet, false) && hooks.modeSeen == kOff,
              "E: 内部の選択が on でも、UI で選ばなければ Off");
        s.controller->setMathGroupExportOptimization(false);
        const auto heldOn = directory / L"eligible-held-on.mp4";
        hooks.hold = true;
        check(s.openSettings(heldOn) && s.turnOn() && s.accept() &&
                  pumpUntil([&] { return hooks.holding.load(); }, 5000),
              "E: on で書き出しを始める (worker は待ち)");
        check(closedBox && closedBox->setProperty("checked", false),
              "E: 最中に checkbox を off にする");
        hooks.hold = false;
        check(s.finished() && hooks.modeSeen == kAuto && hooks.report.mathGroupGraph &&
                  s.outcome("outcome") == QStringLiteral("used") &&
                  fileBytes(heldOn) == eligibleOff,
              "E: 始めた書き出しは Auto のまま");

        // --- F. 画像の上限の超過 → 従来の graph ---
        const auto budgetPath = directory / L"eligible-budget.mp4";
        hooks.inject = Hooks::Inject::Budget;
        check(s.exportOnce(budgetPath, true),
              "F: 上限を超える書き出しが成功する: " + s.controller->statusText().toStdString());
        hooks.inject = Hooks::Inject::None;
        check(hooks.modeSeen == kAuto && hooks.report.preparationRejected &&
                  hooks.report.preparationReason == "prepared-raster-budget-exceeded" &&
                  !hooks.report.mathGroupGraph && hooks.report.composedFrames == 0,
              "F: 準備を取りやめ、従来の graph を選ぶ");
        check(s.outcome("outcome") == QStringLiteral("budget") &&
                  s.controller->statusText().startsWith(
                      QStringLiteral("数式の高速化は使いませんでした（準備する画像が上限を"
                                     "超えたため）。従来の方法で書き出しました: ")) &&
                  !s.controller->statusText().contains(QStringLiteral("高速化を使いました")) &&
                  fileBytes(budgetPath) == eligibleOff && hooks.rasterLiveBytes == 0,
              "F: 高速化を使ったと報告しない。出力は off と同じ: " +
                  s.controller->statusText().toStdString());

        // --- G. 実行時の失敗 → 書き出しを止める。自動ではやり直さない。off で書き出せる ---
        const auto faultPath = directory / L"eligible-fault.mp4";
        hooks.inject = Hooks::Inject::RuntimeFault;
        const int runsBefore = hooks.runs;
        check(s.openSettings(faultPath) && s.turnOn() && s.accept() && s.finished(),
              "G: 実行時に失敗する書き出しが終わる");
        hooks.inject = Hooks::Inject::None;
        pump(200);
        check(hooks.runs == runsBefore + 1, "G: 自動ではやり直さない (書き出しは 1 回)");
        check(hooks.report.mathGroupGraph && hooks.report.errors > 0 &&
                  s.outcome("outcome") == QStringLiteral("failed"),
              "G: 数式 group の実行時の失敗として記録する");
        check(
            s.failure.startsWith(QStringLiteral("書き出しに失敗しました: ")) &&
                s.failure.contains(QStringLiteral("合成器が拒否")) &&
                s.failure.contains(QStringLiteral("「数式の書き出しを高速化（試験的）」を外すと")),
            "G: 元の error を残し、外して書き出せることを示す: " + s.failure.toStdString());
        auto* failureDialog = s.window->findChild<QObject*>(QStringLiteral("exportFailureDialog"));
        check(failureDialog && failureDialog->property("visible").toBool() &&
                  failureDialog->property("message").toString() == s.failure,
              "G: 失敗の dialog に同じ文を出す");
        s.shot("export-failure-optimized-render");
        check(!leftBehind(faultPath) && hooks.liveTransitions == 0 && hooks.activeCallbacks == 0 &&
                  hooks.rasterLiveBytes == 0,
              "G: 最終 file・一時 file・transition・画像が残らない");
        s.dismissFailure();
        const auto retryPath = directory / L"eligible-retry-off.mp4";
        check(s.openSettings(retryPath) && s.checkBox() &&
                  !s.checkBox()->property("checked").toBool() && s.accept() && s.finished() &&
                  hooks.modeSeen == kOff && fileBytes(retryPath) == eligibleOff,
              "G: 開き直すと off で、そのまま従来の方法で書き出せる");

        // --- H. 取消 (製品のキャンセルボタン) ---
        const auto cancelPath = directory / L"eligible-cancel.mp4";
        hooks.hold = true;
        check(s.openSettings(cancelPath) && s.turnOn() && s.accept() &&
                  pumpUntil([&] { return hooks.holding.load(); }, 5000),
              "H: on で書き出しを始める");
        auto* cancel = s.find(QStringLiteral("exportCancelButton"));
        if (cancel)
            s.click(cancel, QPointF(cancel->width() / 2, cancel->height() / 2));
        check(cancel && pumpUntil([&] { return s.controller->exportCancelling(); }, 3000),
              "H: キャンセルボタンを click する");
        hooks.hold = false;
        check(s.finished() && hooks.modeSeen == kAuto &&
                  s.controller->statusText() == QStringLiteral("書き出しをキャンセルしました") &&
                  s.failure.isEmpty() && s.outcome("outcome") == QStringLiteral("incomplete") &&
                  !leftBehind(cancelPath) && hooks.liveTransitions == 0 &&
                  hooks.rasterLiveBytes == 0,
              "H: 取消で終わり、file が残らない: " + s.controller->statusText().toStdString());

        // --- I. consumer の待ちの timeout ---
        const auto timeoutPath = directory / L"eligible-timeout.mp4";
        hooks.inject = Hooks::Inject::Timeout;
        check(s.openSettings(timeoutPath) && s.turnOn() && s.accept() && s.finished(),
              "I: timeout する書き出しが終わる");
        hooks.inject = Hooks::Inject::None;
        check(hooks.modeSeen == kAuto && s.failure.contains(QStringLiteral("timeout")) &&
                  !s.failure.contains(QStringLiteral("高速化")) &&
                  s.outcome("outcome") == QStringLiteral("incomplete") &&
                  !leftBehind(timeoutPath) && hooks.liveTransitions == 0 &&
                  hooks.rasterLiveBytes == 0,
              "I: timeout として報告し (高速化の失敗にしない)、file が残らない: " +
                  s.failure.toStdString());
        s.dismissFailure();

        // --- J. 別の Project を開く: 選択は持ち越さない。対象外は従来の graph ---
        const char* expectedDecision[] = {"", "INELIGIBLE", "UNKNOWN"};
        const char* expectedText[] = {
            "", "一番下の映像 track に透明な部分があるため",
            "一番下の映像 track の clip に拡大・回転などの変形があるため"};
        const char* name[] = {"", "V1 に alpha 0", "V1 を拡大"};
        for (int base = 1; base <= 2; ++base) {
            const std::string label = std::string("J (") + name[base] + "): ";
            check(s.controller->openProject(urlOf(paths[base])), label + "Project を開く");
            s.controller->setMathPreflightForTest(s.backend.preflight());
            check(s.mathReady(), label + "数式の描画が ready になる");
            const auto off = directory / (L"switch-" + std::to_wstring(base) + L"-off.mp4");
            check(s.openSettings(off) && s.checkBox() &&
                      !s.checkBox()->property("checked").toBool() && s.accept() && s.finished() &&
                      hooks.modeSeen == kOff,
                  label + "開いた直後の既定は off: " + s.controller->statusText().toStdString());
            const auto offBytes = fileBytes(off);
            const auto on = directory / (L"switch-" + std::to_wstring(base) + L"-on.mp4");
            check(s.exportOnce(on, true) && hooks.modeSeen == kAuto, label + "on で書き出す");
            s.shot(base == 1 ? "status-fallback-ineligible" : "status-fallback-unknown");
            check(hooks.report.evaluated && hooks.report.decision == expectedDecision[base] &&
                      !hooks.report.mathGroupGraph && hooks.report.composedFrames == 0 &&
                      hooks.report.groups == 0,
                  label + "判定 " + expectedDecision[base] + " で従来の graph を選ぶ: 判定 '" +
                      hooks.report.decision + "' (" + hooks.report.reason + ")");
            check(s.outcome("outcome") == QStringLiteral("conventional") &&
                      s.controller->statusText().startsWith(
                          QStringLiteral("数式の高速化は使いませんでした（") +
                          QString::fromUtf8(expectedText[base]) +
                          QStringLiteral("）。従来の方法で書き出しました: ")) &&
                      !s.controller->statusText().contains(QStringLiteral("高速化を使いました")),
                  label +
                      "高速化を使ったと報告しない: " + s.controller->statusText().toStdString());
            check(!offBytes.isEmpty() && fileBytes(on) == offBytes,
                  label + "出力は off と同じ byte 列");
        }
        // 数式 clip が無い Project: 選択を見せない。要求は Off。
        check(s.controller->openProject(urlOf(paths[3])), "J: 数式の無い Project を開く");
        const auto plain = directory / L"no-math.mp4";
        check(s.openSettings(plain) && s.checkBox() == nullptr &&
                  s.find(QStringLiteral("exportMathGroupHint")) == nullptr,
              "J: 数式の無い Project では選択を見せない");
        if (auto* hidden = s.window->findChild<QObject*>(QStringLiteral("exportMathGroupCheckBox")))
            hidden->setProperty("checked", true); // 見えていない checkbox は効かない
        check(s.accept() && s.finished() && hooks.modeSeen == kOff &&
                  s.outcome("outcome") == QStringLiteral("off"),
              "J: 数式の無い Project の要求は Off: " + s.controller->statusText().toStdString());
        check(!settingsMentionExport(), "J: 設定 file に書き出しの選択を保存していない");
    }

    // --- K. 起動し直す (controller と QML を作り直す): 既定は off ---
    {
        Session s;
        if (!s.open(paths[0], fixtureProject(0)))
            return 3;
        check(QTest::qWaitForWindowExposed(s.window) && mvm::test::focusWithoutForeground(s.window),
              "K: 作り直した window を表示する");
        auto& hooks = *s.hooks;
        check(s.mathReady(), "K: 数式の描画が ready になる");
        check(!s.controller->mathGroupExportOptimization() &&
                  s.controller->exportMathGroupOutcome().isEmpty() && !settingsMentionExport(),
              "K: 前の選択も結果も残っていない");
        const auto restarted = directory / L"restart-off.mp4";
        check(s.openSettings(restarted) && s.checkBox() &&
                  !s.checkBox()->property("checked").toBool() && s.accept() && s.finished() &&
                  hooks.modeSeen == kOff && fileBytes(restarted) == eligibleOff,
              "K: 起動し直した後の既定は off で、出力は同じ");
    }

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
