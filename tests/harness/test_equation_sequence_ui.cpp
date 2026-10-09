// P3-5: 製品の Main.qml と実 controller (D3D11 の preview) で、EquationSequence を UI だけで author
// する。
//
//   mvm_test_text_ui_input --equation-sequence-ui <証拠の directory> [--scratch]
//
// 証拠の directory が存在すれば起動を拒否する (過去の証拠を上書きしない)。--scratch は CTest 用で、
// 既存の directory も消さず、新しい実行の directory を使う。描画は偽の数式 backend。
//
//   A. 二次方程式の解の公式の導出 (8 状態) を、メニュー・ボタン・編集欄のキー入力・一覧の選択・
//      combo の操作だけで作る (Project JSON は手で書かない)。判別式 b^2-4ac に outline と pulse。
//      キーボード・focus の規則、保存・閉じる・開き直し・ID・JSON の往復、直接 seek の提示、
//      破壊的 / 非破壊的な編集の Undo/Redo を確かめる。
//   B. 修復 (Invalid binding・欠落した action の対象)、backend 不在、OverBudget、読むだけの状態の
//      polling、狭い・低いパネル、長い内容 (10 状態・600 文字の式・12 部分式・長い名前と理由)。
//
// 各場面で実際に描画した window を PNG に保存し (grabWindow)、inspector の項目が親の幅の外へ
// 描かれないこと、文字が欄からはみ出さないこと、主な操作へ縦スクロールで到達できることを検査する。
// 結果は <証拠>/results.json と標準エラー。

#include "app/math_clip_render.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "app/text_raster.h"
#include "equation_sequence_editor.h"
#include "focus_release_filter.h"
#include "math_fake_backend.h"
#include "mlt_rgba_oracle.h"
#include "mvm_controller.h"
#include "project/equation_sequence.h"
#include "project/equation_sequence_edit.h"
#include "project/graph_edit.h"
#include "project/project_json.h"
#include "test_window_focus.h"
#include "waveform_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QThread>
#include <QUuid>
#include <QtQuickTest/quicktest.h>

namespace {
namespace project = mvm::project;
using mvm::app::MathRasterCache;
using mvm::app::MvmController;
using mvm::test::FakeMathBackend;

int checks = 0;
int failures = 0;
QJsonArray results;
QJsonArray shots;
std::filesystem::path evidence;

void check(bool ok, const std::string& message) {
    ++checks;
    results.append(QJsonObject{{QStringLiteral("check"), QString::fromStdString(message)},
                               {QStringLiteral("ok"), ok}});
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        // 成立した観測は追加の event drain で再評価しない。Ready の直後に次の更新が
        // Seeking を公開すると、成功を観測していたのに false を返してしまう。
        if (predicate())
            return true;
        QCoreApplication::processEvents();
        // 手動の event loop でも、一覧の作り直しで deleteLater された delegate を解放する。
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return predicate();
}

void pump(int milliseconds = 100) {
    pumpUntil([] { return false; }, milliseconds);
}

void collect(QQuickItem* item, QList<QQuickItem*>& out) {
    out.push_back(item);
    for (auto* child : item->childItems())
        collect(child, out);
}

// 二次方程式の解の公式の導出 (P3-0 の最終受け入れの段階)。
const char* const kQuadratic[] = {
    "ax^2+bx+c=0",
    "x^2+\\frac{b}{a}x+\\frac{c}{a}=0",
    "x^2+\\frac{b}{a}x=-\\frac{c}{a}",
    "x^2+\\frac{b}{a}x+\\frac{b^2}{4a^2}=\\frac{b^2}{4a^2}-\\frac{c}{a}",
    "(x+\\frac{b}{2a})^2=\\frac{b^2-4ac}{4a^2}",
    "x+\\frac{b}{2a}=\\pm\\sqrt{\\frac{b^2-4ac}{4a^2}}",
    "x+\\frac{b}{2a}=\\pm\\frac{\\sqrt{b^2-4ac}}{2a}",
    "x=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}",
};
constexpr int kStates = 8;

struct Session {
    struct Observation {
        std::mutex mutex;
        std::vector<std::pair<std::int64_t, mvm::app::EquationPreviewShown>> frames;
        std::map<std::int64_t, std::vector<std::uint8_t>> exportExpected;
        int exportCompared = 0;
        int exportMismatches = 0;
        bool graphExport = false;
        std::set<std::int64_t> graphComparedFrames;
        mvm::test::Pixel graphBackground{0, 0, 0, 255};
        // P4-5 補遺: 最初の encoder frame で worker を止め、製品の shutdown が出す取消を
        // その場で観測する。exporting() や出力の不在だけを取消の証拠にしない。
        bool holdFirstEncoderFrame = false;
        std::atomic<bool> barrierReached{false};
        std::atomic<bool> barrierReleased{false};
        std::atomic<bool> runnerReturned{false};
        // 保持中に encoder の完了待ち loop が progress で取消を受け取った。
        std::atomic<bool> cancelPolledWhileHeld{false};
        std::int64_t barrierFrame = -1;
        bool barrierFrameValid = false;
        bool barrierCancelObserved = false;
        bool preflightConfigured = false;
        std::thread::id barrierThread;
        bool runnerSuccess = false;
        bool runnerCancelled = false;
        bool runnerGraphCancelled = false;
        std::string runnerError;
    };

    std::shared_ptr<Observation> observation = std::make_shared<Observation>();
    std::unique_ptr<MvmController> controller;
    mvm::app::WaveformCache waveforms;
    std::unique_ptr<QQmlApplicationEngine> engine;
    QQuickWindow* window = nullptr;
    FakeMathBackend backend;

    ~Session() { close(); }

    void close() {
        std::fprintf(stderr, "製品 UI: controller の終了を開始\n");
        if (controller)
            controller->shutdown();
        std::fprintf(stderr, "製品 UI: QML の終了を開始\n");
        engine.reset();
        std::fprintf(stderr, "製品 UI: controller の解放を開始\n");
        controller.reset();
        window = nullptr;
        std::fprintf(stderr, "製品 UI: 終了完了\n");
    }

    mvm::app::EquationSequenceEditor& editor() { return controller->equationEditorRef(); }

    QVariantMap view() { return editor().view(); }

    const project::Project& project() { return controller->projectForTest(); }

    const project::TimelineClip* sequence() {
        for (const auto& clip : project().timelineClips)
            if (clip.kind == project::TimelineClipKind::EquationSequence)
                return &clip;
        return nullptr;
    }

    int sequenceCount() {
        int n = 0;
        for (const auto& clip : project().timelineClips)
            n += clip.kind == project::TimelineClipKind::EquationSequence;
        return n;
    }

    const project::EquationSequenceClipData& data() { return sequence()->equationSequence; }

    QQuickItem* find(const QString& name) {
        // visibility の切替でも、古い geometry のままキーやクリックを送らない。
        if (QQuickTest::qIsPolishScheduled(window))
            window->grabWindow();
        const auto locate = [&]() -> QQuickItem* {
            QList<QQuickItem*> items;
            collect(window->contentItem(), items);
            for (auto* item : items) {
                if (item->objectName() != name || !item->isVisible())
                    continue;
                bool laidOut = true;
                for (auto* ancestor = item; ancestor; ancestor = ancestor->parentItem())
                    if (ancestor->width() <= 0 || ancestor->height() <= 0)
                        laidOut = false;
                if (laidOut)
                    return item;
            }
            return nullptr;
        };
        if (auto* item = locate())
            return item;
        // 背面で未描画の layout だけを実描画へ反映する。毎回の同期 grab は不要。
        window->grabWindow();
        return locate();
    }

    QQuickItem* scroll() { return find(QStringLiteral("effectControlsScroll")); }

    QQuickItem* inspector() { return find(QStringLiteral("equationSequenceInspector")); }

    // flickable を item が見える位置までスクロールする (利用者のスクロールの代わり)。
    bool reach(QQuickItem* item) {
        auto* flick = scroll();
        if (!flick || !item)
            return false;
        QPointer<QQuickItem> target(item);
        auto* content = flick->property("contentItem").value<QQuickItem*>();
        // 非同期の状態表示で panel の高さが変わる間も、現在の geometry から到達位置を求める。
        const bool reached = pumpUntil(
            [&] {
                if (!target)
                    return false;
                const qreal top = target->mapToItem(content, QPointF(0, 0)).y();
                const qreal maxY =
                    std::max<qreal>(0, flick->property("contentHeight").toReal() - flick->height());
                const qreal bottom = top + target->height();
                qreal y = flick->property("contentY").toReal();
                if (top < y)
                    y = top - 4;
                else if (bottom > y + flick->height())
                    y = bottom - flick->height() + 4;
                flick->setProperty("contentY", std::clamp<qreal>(y, 0, maxY));
                const QRectF shown =
                    target->mapRectToScene(QRectF(0, 0, target->width(), target->height()));
                const QRectF view =
                    flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
                return target->height() > 0 && shown.top() >= view.top() - 0.5 &&
                       shown.bottom() <= view.bottom() + 0.5 && shown.left() >= view.left() - 0.5 &&
                       shown.right() <= view.right() + 0.5;
            },
            2000);
        pump(60);
        return reached && target;
    }

    QPoint center(QQuickItem* item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    }

    bool click(const QString& name) {
        auto* item = find(name);
        if (!item || !reach(item)) {
            check(false, "UI の操作へ到達できる: " + name.toStdString());
            return false;
        }
        QTest::mouseClick(window, Qt::LeftButton, {}, center(item));
        pump(120);
        return true;
    }

    void key(Qt::Key k, Qt::KeyboardModifiers modifiers = {}) {
        QTest::keyClick(window, k, modifiers);
        pump(10);
    }

    bool seek(qint64 frame) {
        {
            std::lock_guard lock(observation->mutex);
            observation->frames.clear();
        }
        // 前の提示が完了するまで新しい seek は受理されない。拒否を成功と見なさない。
        bool accepted = false;
        pumpUntil([&] {
            if (!accepted)
                accepted = controller->seekTimelineFrame(frame);
            return accepted;
        });
        check(accepted, "直接 seek: 要求が受理される (" + std::to_string(frame) +
                            "): " + controller->statusText().toStdString());
        const bool presented = accepted && pumpUntil([&] { return observed(frame); });
        check(presented, "直接 seek: engine の render thread が要求 frame を評価する");
        return presented;
    }

    bool observed(qint64 frame, const char* kind = nullptr) {
        std::lock_guard lock(observation->mutex);
        return std::any_of(observation->frames.begin(), observation->frames.end(),
                           [&](const auto& value) {
                               return value.first == frame &&
                                      (!kind || std::string(mvm::app::equationPreviewShownKindName(
                                                    value.second.kind)) == kind);
                           });
    }

    void type(const std::string& text) {
        for (char c : text)
            QTest::keyClick(window, c);
        pump(30);
    }

    QQuickItem* sourceEditor() { return find(QStringLiteral("equationSourceEditor")); }

    // 式の編集欄に focus を置く (クリック)。
    bool focusSource() {
        auto* editor = sourceEditor();
        auto* viewport = find(QStringLiteral("equationSourceScroll"));
        if (!editor || !viewport || !reach(viewport))
            return false;
        // 長文の TextArea 全体ではなく、内部スクロールの可視領域をクリックする。
        QTest::mouseClick(window, Qt::LeftButton, {},
                          viewport->mapToScene(QPointF(12, 12)).toPoint());
        pump(60);
        return editor->hasActiveFocus();
    }

    // 式を全部置き換えて Ctrl+Enter で確定する (キー入力だけ)。
    bool replaceSource(const std::string& text) {
        if (!focusSource()) {
            std::fprintf(stderr, "式の入力: focus を取得できません\n");
            return false;
        }
        key(Qt::Key_A, Qt::ControlModifier);
        type(text);
        check(sourceEditor()->property("text").toString() == QString::fromStdString(text),
              "式の入力: キー入力で下書きが指定した全文になる");
        key(Qt::Key_Return, Qt::ControlModifier);
        pump(80);
        if (view().value("state").toMap().value("source").toString() !=
            QString::fromStdString(text))
            std::fprintf(stderr, "式の確定: 入力 %s / 表示 %s / focus %d / 理由 %s\n", text.c_str(),
                         qUtf8Printable(sourceEditor()->property("text").toString()),
                         sourceEditor()->hasActiveFocus(),
                         qUtf8Printable(view().value("message").toString()));
        return true;
    }

    // 編集欄の UTF-16 の範囲 [begin, end) をキーボードで選ぶ (Ctrl+Home → → … Shift+→ …)。
    bool selectSource(int begin, int end) {
        if (!focusSource())
            return false;
        key(Qt::Key_Home, Qt::ControlModifier);
        for (int i = 0; i < begin; ++i)
            QTest::keyClick(window, Qt::Key_Right);
        for (int i = begin; i < end; ++i)
            QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
        pump(30);
        auto* editor = sourceEditor();
        return editor->property("selectionStart").toInt() == begin &&
               editor->property("selectionEnd").toInt() == end;
    }

    // 状態の一覧の行をクリックする (一覧の中を見える位置まで送る)。
    bool clickStateRow(int index) {
        auto* list = find(QStringLiteral("equationStateList"));
        if (!list)
            return false;
        reach(list);
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, index), Q_ARG(int, 4));
        pump(60);
        return click(QStringLiteral("equationStateRow_%1").arg(index));
    }

    bool clickPartRow(const QString& label) {
        const auto parts = view().value("parts").toList();
        for (int i = 0; i < parts.size(); ++i)
            if (parts[i].toMap().value("label").toString() == label) {
                auto* list = find(QStringLiteral("equationPartList"));
                reach(list);
                QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, i),
                                          Q_ARG(int, 4));
                pump(60);
                return click(QStringLiteral("equationPartRow_%1").arg(i));
            }
        check(false, "部分式の行がある: " + label.toStdString());
        return false;
    }

    // 数値欄 (DragNumberField) をダブルクリックで直接入力にし、値を打って Enter。
    bool enterNumber(const QString& name, int value) {
        auto* field = find(name);
        if (!field || !reach(field))
            return false;
        // 値の箱は label の右 (inline) にある。箱の中央をダブルクリックする。
        const QPointF boxCenter(field->width() - 30, field->height() / 2);
        QTest::mouseDClick(window, Qt::LeftButton, {}, field->mapToScene(boxCenter).toPoint());
        pump(60);
        key(Qt::Key_A, Qt::ControlModifier);
        type(std::to_string(value));
        key(Qt::Key_Return);
        pump(80);
        return true;
    }

    // combo を focus して上下キーで項目を選ぶ (activated で入力へ反映される)。
    bool chooseCombo(const QString& name, const QString& text) {
        auto* combo = find(name);
        if (!combo || !reach(combo)) {
            std::fprintf(stderr, "候補の診断: %s が表示領域にありません\n", qUtf8Printable(name));
            if (combo) {
                const auto rect =
                    combo->mapRectToScene(QRectF(0, 0, combo->width(), combo->height()));
                auto* flick = scroll();
                std::fprintf(
                    stderr,
                    "候補の geometry: %.1f %.1f %.1f %.1f / panel %.1f %.1f / scroll %.1f %.1f\n",
                    rect.x(), rect.y(), rect.width(), rect.height(), flick->width(),
                    flick->height(), flick->property("contentY").toReal(),
                    flick->property("contentHeight").toReal());
            }
            return false;
        }
        combo->forceActiveFocus(Qt::TabFocusReason);
        pump(20);
        const int count = combo->property("count").toInt();
        for (int i = 0; i < count; ++i)
            QTest::keyClick(window, Qt::Key_Up);
        pump(10);
        for (int i = 0; i <= count; ++i) {
            if (combo->property("currentText").toString() == text)
                return true;
            QTest::keyClick(window, Qt::Key_Down);
            pump(10);
        }
        const bool selected = combo->property("currentText").toString() == text;
        if (!selected)
            std::fprintf(stderr, "候補の診断: %s / 候補 %d 件 / 現在 %s / 期待 %s / focus %d\n",
                         qUtf8Printable(name), count,
                         qUtf8Printable(combo->property("currentText").toString()),
                         qUtf8Printable(text), combo->hasActiveFocus());
        return selected;
    }

    // ---- 描画の証拠と layout の検査 ----
    void shot(const std::string& name) {
        pump(150);
        const QImage image = window->grabWindow();
        const auto file = evidence / (name + ".png");
        const bool saved = !image.isNull() && image.save(QString::fromStdWString(file.wstring()));
        QRect panel;
        if (auto* flick = scroll())
            panel = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()))
                        .toAlignedRect()
                        .adjusted(-8, -40, 8, 8)
                        .intersected(image.rect());
        const auto crop = evidence / (name + "-panel.png");
        const bool cropped =
            !panel.isEmpty() && image.copy(panel).save(QString::fromStdWString(crop.wstring()));
        check(saved && cropped, "描画の証拠を保存: " + name);
        shots.append(QJsonObject{
            {QStringLiteral("name"), QString::fromStdString(name)},
            {QStringLiteral("window"), QString::fromStdWString(file.filename().wstring())},
            {QStringLiteral("panel"), QString::fromStdWString(crop.filename().wstring())},
            {QStringLiteral("width"), image.width()},
            {QStringLiteral("height"), image.height()}});
    }

    // inspector の見えている項目が親の幅の中に描かれ、文字が欄からはみ出さないこと。
    // ListView・ScrollView の中は親が切り取るので、その親の矩形を検査する。
    void layout(const std::string& name) {
        auto* root = inspector();
        auto* flick = scroll();
        if (!root || !flick) {
            check(false, "layout: inspector がある: " + name);
            return;
        }
        const QRectF bound = flick->mapRectToScene(QRectF(0, 0, flick->width(), flick->height()));
        QStringList problems;
        int inspected = 0;
        std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            if (!item->isVisible() || item->width() <= 0 || item->height() <= 0)
                return;
            ++inspected;
            const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            if (rect.left() < bound.left() - 1 || rect.right() > bound.right() + 1)
                problems << QStringLiteral("%1(%2) x=[%3,%4] 幅の外")
                                .arg(QString::fromLatin1(item->metaObject()->className()),
                                     item->objectName())
                                .arg(rect.left())
                                .arg(rect.right());
            const auto className = QString::fromLatin1(item->metaObject()->className());
            if (className.startsWith(QStringLiteral("QQuickText")) &&
                !className.contains(QStringLiteral("Edit")) &&
                !className.contains(QStringLiteral("Input"))) {
                const int elide = item->property("elide").toInt();
                const int wrap = item->property("wrapMode").toInt();
                if (elide == 0 && wrap == 0 &&
                    item->property("contentWidth").toReal() > item->width() + 1)
                    problems << QStringLiteral("文字がはみ出す: %1")
                                    .arg(item->property("text").toString().left(40));
            }
            if (item->clip())
                return; // 中は親が切り取る
            for (auto* child : item->childItems())
                walk(child);
        };
        walk(root);
        check(root->width() <= flick->width() + 0.5,
              "layout: inspector の幅は panel の幅以下: " + name);
        check(problems.isEmpty(),
              "layout: 項目が幅の外へ描かれず文字がはみ出さない: " + name +
                  (problems.isEmpty()
                       ? std::string()
                       : " (" + problems.join(QStringLiteral("; ")).toStdString() + ")"));
        check(inspected > 20,
              "layout: 検査した項目がある (" + std::to_string(inspected) + "): " + name);
    }

    // 主な操作に縦スクロールで到達できること。
    void reachable(const std::string& name, const QStringList& names) {
        for (const auto& item : names) {
            auto* found = find(item);
            if (!found)
                continue; // この場面では出ない操作
            check(reach(found), "到達できる: " + item.toStdString() + " (" + name + ")");
        }
    }
};

const QStringList kControls = {
    QStringLiteral("equationStatusPanel"),     QStringLiteral("equationStateList"),
    QStringLiteral("equationInsertAfter"),     QStringLiteral("equationDeleteState"),
    QStringLiteral("equationSourceScroll"),    QStringLiteral("equationHoldField"),
    QStringLiteral("equationAddPart"),         QStringLiteral("equationRebindPart"),
    QStringLiteral("equationAddAction"),       QStringLiteral("equationUpdateAction"),
    QStringLiteral("equationDeleteAction"),    QStringLiteral("equationAddPair_incoming"),
    QStringLiteral("equationAddPair_outgoing")};

std::unique_ptr<Session> open(const std::filesystem::path& path, const project::Project& initial,
                              bool backendAvailable = true, bool expectCleanFixture = true) {
    auto s = std::make_unique<Session>();
    const auto realManim = qEnvironmentVariable("MVM_P36_REAL_MANIM");
    s->controller = std::make_unique<MvmController>(
        path, std::filesystem::path(realManim.toStdWString()), initial, nullptr,
        [observation = s->observation](const project::Project& project,
                                       const mvm::app::TimelineExportRequest& request) {
            auto observed = request;
            if (observation->graphExport) {
                observed.timeoutMs = 30000;
                observed.graphFrameObserver = [observation, project,
                                               request](const std::string& id, std::int64_t frame,
                                                        const mvm::graph::Raster& raster) {
                    std::lock_guard lock(observation->mutex);
                    const auto clip =
                        std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                                     [&](const auto& candidate) { return candidate.id == id; });
                    if (clip == project.timelineClips.end())
                        return;
                    auto compiled = mvm::app::compileGraphRender(
                        clip->graph, clip->sourceFrameCount, request.width, request.height);
                    const auto* spec = std::get_if<mvm::graph::GraphRenderSpec>(&compiled);
                    if (!spec)
                        return;
                    // loader の台帳を期待値に使わない。元の clip の有理時刻から直接選ぶ。
                    const auto numerator =
                        clip->sourceFpsDen * project.timelineFpsNum * clip->speedDen;
                    const auto denominator =
                        clip->sourceFpsNum * project.timelineFpsDen * clip->speedNum;
                    const auto origin =
                        (clip->sourceInFrame * numerator + denominator - 1) / denominator;
                    const auto source = std::min(clip->sourceFrameCount - 1,
                                                 (origin + frame - clip->timelineStartFrame) *
                                                     denominator / numerator);
                    const auto base =
                        mvm::graph::staticKey(*spec, request.graphEnvironment.toolchain);
                    const auto key =
                        spec->drawFrames ? mvm::graph::drawKey(base, spec->drawFrames) : base;
                    const auto name = source < spec->drawFrames
                                          ? "frame-" + std::to_string(source) + ".png"
                                          : "static.png";
                    const auto decoded = mvm::graph::readRgba(
                        request.graphEnvironment.cache / key / name, request.width, request.height);
                    if (const auto* expected = std::get_if<mvm::graph::Raster>(&decoded)) {
                        observation->exportMismatches += raster.rgba != expected->rgba;
                        auto composed = expected->rgba;
                        for (std::size_t at = 0; at < composed.size(); at += 4) {
                            const auto pixel =
                                mvm::test::mltSourceOver(observation->graphBackground,
                                                         {composed[at], composed[at + 1],
                                                          composed[at + 2], composed[at + 3]},
                                                         clip->effects.opacityPercent / 100.0);
                            std::copy(pixel.begin(), pixel.end(),
                                      composed.begin() + static_cast<std::ptrdiff_t>(at));
                        }
                        if (request.burnSubtitles && project.subtitles &&
                            project.subtitles->visible)
                            for (const auto& cue : project.subtitles->cues) {
                                if (frame < cue.startFrame || frame >= cue.endFrame)
                                    continue;
                                QString error;
                                const auto subtitle = mvm::app::renderSubtitleRaster(
                                                          cue, project.subtitles->style,
                                                          request.width, request.height, error)
                                                          .convertToFormat(QImage::Format_RGBA8888);
                                if (subtitle.isNull())
                                    return;
                                for (int y = 0; y < request.height; ++y)
                                    for (int x = 0; x < request.width; ++x) {
                                        const auto at =
                                            static_cast<std::size_t>(y * request.width + x) * 4;
                                        const auto* pixel = subtitle.constScanLine(y) + x * 4;
                                        const auto result = mvm::test::mltSourceOver(
                                            {composed[at], composed[at + 1], composed[at + 2],
                                             composed[at + 3]},
                                            {pixel[0], pixel[1], pixel[2], pixel[3]});
                                        std::copy(result.begin(), result.end(),
                                                  composed.begin() +
                                                      static_cast<std::ptrdiff_t>(at));
                                    }
                            }
                        observation->exportExpected[frame] = std::move(composed);
                    }
                };
                observed.encoderFrameValidator = [observation,
                                                  cancel = request.graphEnvironment.cancel](
                                                     std::int64_t frame, const std::uint8_t* actual,
                                                     int width, int height) {
                    std::unique_lock lock(observation->mutex);
                    const auto found = observation->exportExpected.find(frame);
                    if (found == observation->exportExpected.end() ||
                        found->second.size() != static_cast<std::size_t>(width * height * 4))
                        return false;
                    // 最初の encoder frame を独立 oracle で検証した後、lock を外して encoder を
                    // 保持する。解放条件は、製品の取消 flag を encoder の完了待ち loop が
                    // progress で受け取ったことだけで、待機時間を合否に使わない。期限は変異で
                    // 取消が来ない場合に試験を終わらせるためで、consumer の timeout より短い。
                    const auto holdAtBarrier = [&](bool valid) {
                        if (!observation->holdFirstEncoderFrame ||
                            observation->barrierReached.load(std::memory_order_acquire))
                            return valid;
                        observation->barrierFrame = frame;
                        observation->barrierFrameValid = valid;
                        observation->barrierThread = std::this_thread::get_id();
                        lock.unlock();
                        observation->barrierReached.store(true, std::memory_order_release);
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        while (
                            !observation->cancelPolledWhileHeld.load(std::memory_order_acquire) &&
                            std::chrono::steady_clock::now() < deadline)
                            std::this_thread::yield();
                        const bool cancelObserved =
                            cancel && cancel->load(std::memory_order_acquire) &&
                            observation->cancelPolledWhileHeld.load(std::memory_order_acquire);
                        lock.lock();
                        observation->barrierCancelObserved = cancelObserved;
                        observation->barrierReleased.store(true, std::memory_order_release);
                        return valid;
                    };
                    ++observation->exportCompared;
                    observation->graphComparedFrames.insert(frame);
                    for (std::size_t at = 0; at < found->second.size(); at += 4) {
                        const auto& rgba = found->second;
                        const mvm::test::Pixel expected{rgba[at], rgba[at + 1], rgba[at + 2],
                                                        rgba[at + 3]};
                        for (std::size_t channel = 0; channel < 4; ++channel) {
                            if (actual[at + channel] != expected[channel] &&
                                observation->exportMismatches == 0)
                                std::fprintf(stderr,
                                             "P4-5: 最初の不一致 frame=%lld pixel=%zu channel=%zu "
                                             "実値=%u 期待=%u\n",
                                             static_cast<long long>(frame), at / 4, channel,
                                             unsigned(actual[at + channel]),
                                             unsigned(expected[channel]));
                            observation->exportMismatches +=
                                actual[at + channel] != expected[channel];
                        }
                    }
                    return holdAtBarrier(observation->exportMismatches == 0);
                };
            }
            observed.equationFrameObserver = [observation](const std::string&, std::int64_t frame,
                                                           const std::vector<std::uint8_t>& rgba) {
                std::lock_guard lock(observation->mutex);
                const auto found = observation->exportExpected.find(frame);
                if (found != observation->exportExpected.end()) {
                    ++observation->exportCompared;
                    observation->exportMismatches += found->second != rgba ? 1 : 0;
                }
            };
            {
                std::lock_guard lock(observation->mutex);
                observation->preflightConfigured =
                    static_cast<bool>(request.graphEnvironment.preflight);
                if (observation->holdFirstEncoderFrame && request.progress)
                    observed.progress = [observation, progress = request.progress](
                                            long long completed, long long total) {
                        const bool cancelled = progress(completed, total);
                        if (cancelled &&
                            observation->barrierReached.load(std::memory_order_acquire) &&
                            !observation->barrierReleased.load(std::memory_order_acquire))
                            observation->cancelPolledWhileHeld.store(true,
                                                                     std::memory_order_release);
                        return cancelled;
                    };
            }
            auto exported = mvm::app::exportTimeline(project, observed);
            {
                std::lock_guard lock(observation->mutex);
                observation->runnerSuccess = exported.success;
                observation->runnerCancelled = exported.cancelled;
                observation->runnerGraphCancelled =
                    exported.graphReadiness.failure == mvm::app::GraphExportFailure::Cancelled;
                observation->runnerError = exported.error;
            }
            observation->runnerReturned.store(true, std::memory_order_release);
            return exported;
        },
        MvmController::ExportThreadFactory{},
        [](const std::filesystem::path&, QString&) { return true; });
    const bool cleanFixture = !s->controller->recoveryAvailable() &&
                              !s->controller->recoveryCorrupt() &&
                              !s->controller->recoveryForeign();
    check(cleanFixture == expectCleanFixture, "製品 UI: fixture の復旧状態が期待どおり");
    if (!cleanFixture)
        return nullptr;
    s->engine = std::make_unique<QQmlApplicationEngine>();
    auto properties = mvm::app::testFixedWindowInitialProperties();
    properties.insert(QStringLiteral("visible"), false);
    properties.insert(QStringLiteral("mvmController"), QVariant::fromValue(s->controller.get()));
    properties.insert(QStringLiteral("waveformCache"), QVariant::fromValue(&s->waveforms));
    if (!mvm::app::testFixedWindowRequested())
        properties.insert(QStringLiteral("flags"), mvm::test::backgroundWindowFlags());
    s->engine->setInitialProperties(properties);
    s->engine->load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
    s->window = s->engine->rootObjects().isEmpty()
                    ? nullptr
                    : qobject_cast<QQuickWindow*>(s->engine->rootObjects().first());
    if (!s->window)
        return nullptr;
    s->window->installEventFilter(new mvm::app::FocusReleaseFilter(s->window));
    auto* surface =
        s->window->findChild<mvm::app::PreviewEngineRhiItem*>(QStringLiteral("previewSurface"));
    if (!surface) {
        check(false, "製品 UI: preview surface がある");
        return nullptr;
    }
    s->controller->attachPreview(surface);
    s->window->setVisible(true);
    QString reason;
    if (!QTest::qWaitForWindowExposed(s->window) || !mvm::test::focusWithoutForeground(s->window) ||
        !mvm::test::isolatedFromUserInput(s->window, reason)) {
        std::fprintf(stderr, "PROTOCOL_INVALID: 試験を操作から隔離できません: %s\n",
                     qUtf8Printable(reason));
        return nullptr;
    }
    s->window->setProperty("leftPanelTab", 0);
    s->window->setProperty("leftPanelWidth", 520);
    const bool ready = pumpUntil([&] { return s->controller->previewReady(); }, 30000);
    check(ready, "製品 UI: preview の初期化: " + s->controller->statusText().toStdString());
    if (!ready) {
        const auto native = s->controller->previewEngineForTest()->status();
        std::fprintf(stderr, "初期化診断: engine state=%d accepted=%d presented=%d\n",
                     static_cast<int>(native.state),
                     native.latestAcceptedDesiredComposition.has_value(),
                     native.lastPresentedComposition.has_value());
        return nullptr;
    }
    s->controller->setEquationPreviewObserverForTest(
        [observation = s->observation](const std::string&, std::int64_t frame,
                                       const std::optional<mvm::app::EquationPreviewTime>&,
                                       const mvm::app::EquationPreviewShown& shown) {
            std::lock_guard lock(observation->mutex);
            observation->frames.emplace_back(frame, shown);
        });
    if (realManim.isEmpty() || !backendAvailable)
        s->controller->setMathPreflightForTest(
            backendAvailable ? s->backend.preflight()
                             : FakeMathBackend::unavailable("試験: Manim が無い"));
    return s;
}

QString partId(Session& s, const QString& label) {
    for (const auto& part : s.view().value("parts").toList())
        if (part.toMap().value("label").toString() == label)
            return part.toMap().value("id").toString();
    return {};
}

// UI で部分式を足す: 範囲をキーボードで選び、名前を入れて「選択範囲を部分式に追加」。
bool addPart(Session& s, const std::string& text, const QString& label, bool last = false) {
    const auto source = s.view().value("state").toMap().value("source").toString();
    const auto needle = QString::fromStdString(text);
    const auto at = last ? source.lastIndexOf(needle) : source.indexOf(needle);
    if (at < 0 || !s.selectSource(static_cast<int>(at), static_cast<int>(at + needle.size())))
        return false;
    auto* field = s.find(QStringLiteral("equationPartLabelField"));
    if (!field || !s.reach(field))
        return false;
    QTest::mouseClick(s.window, Qt::LeftButton, {}, s.center(field));
    pump(30);
    s.key(Qt::Key_A, Qt::ControlModifier);
    s.type(label.toStdString()); // ASCII の名前だけ (IME は扱わない)
    return s.click(QStringLiteral("equationAddPart")) && !partId(s, label).isEmpty();
}

bool addPair(Session& s, const QString& role, const QString& from, const QString& to) {
    const auto ok = s.chooseCombo(QStringLiteral("equationPairFrom_") + role, from) &&
                    s.chooseCombo(QStringLiteral("equationPairTo_") + role, to);
    return ok && s.click(QStringLiteral("equationAddPair_") + role);
}

std::int64_t timelineFrameOfSource(const project::TimelineClip& clip, std::int64_t source) {
    // 60fps の Project と同じ FPS の sequence (output = source)。
    return clip.timelineStartFrame + source - clip.sourceInFrame;
}

// ---- A. 二次方程式の導出 ----
int recoveryFixtureGuard(const std::filesystem::path& directory) {
    const auto path = directory / L"recovery-guard.mvm";
    auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "復旧前提: fixture を保存");
    {
        MvmController controller(path, {}, initial);
        FakeMathBackend backend;
        controller.setMathPreflightForTest(backend.preflight());
        check(controller.createEquationSequenceClip(QStringLiteral("x")),
              "復旧前提: 未保存の変更を作る");
        controller.shutdown();
    }
    initial.outputWidth = 1280;
    initial.outputHeight = 720;
    check(project::saveProjectJson(initial, path).success, "復旧前提: Project を外部から変更");
    auto rejected = open(path, initial, true, false);
    check(!rejected, "復旧前提: 復旧ダイアログを伴う fixture を受け入れの前に拒否する");
    return failures == 0 ? 0 : 1;
}

int quadraticWorkflow(const std::filesystem::path& path) {
    auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "A: 空の Project を保存");
    auto s = open(path, initial);
    if (!s)
        return 4;
    // 作成: ファイルメニューの「数式 sequence を追加」。
    auto* entry = s->window->findChild<QObject*>(QStringLiteral("addEquationSequenceMenuItem"));
    const auto depth0 = s->controller->undoDepthForTest();
    check(entry && QMetaObject::invokeMethod(entry, "triggered"), "A: メニューの作成を実行");
    check(pumpUntil([&] { return s->sourceEditor() && s->sourceEditor()->hasActiveFocus(); }),
          "A: 作成後に式の欄へ focus が移る");
    check(s->sequenceCount() == 1 && s->data().states.size() == 1 &&
              s->data().transitions.empty() && s->controller->undoDepthForTest() == depth0 + 1,
          "A: 1 状態の sequence を Undo 1 回分で作る");
    check(s->find(QStringLiteral("equationNoTransitions")) &&
              s->find(QStringLiteral("equationNoParts")) &&
              s->find(QStringLiteral("equationNoActions")) &&
              s->find(QStringLiteral("equationLastStateNote")),
          "A: 変形・部分式・強調の空の案内と、最後の状態を消せない案内");
    s->shot("01-fresh-one-state");
    s->layout("fresh");

    // 8 状態: 先頭を書き換え、「後に挿入」+ 式の入力を 7 回。
    const auto initialDepth = s->controller->undoDepthForTest();
    check(s->replaceSource(kQuadratic[0]) && s->data().states[0].equation.source == kQuadratic[0] &&
              s->controller->undoDepthForTest() == initialDepth + 1,
          "A: 先頭の式をキー入力で確定 (Undo 1 回)");
    for (int i = 1; i < kStates; ++i) {
        const auto d = s->controller->undoDepthForTest();
        check(s->click(QStringLiteral("equationInsertAfter")), "A: 後に挿入");
        check(s->replaceSource(kQuadratic[i]), "A: 式を入力");
        check(s->data().states.size() == static_cast<std::size_t>(i + 1) &&
                  s->data().states[static_cast<std::size_t>(i)].equation.source == kQuadratic[i] &&
                  s->controller->undoDepthForTest() == d + 2,
              "A: 状態 " + std::to_string(i) + " を挿入して式を確定 (挿入 1 + 確定 1)");
    }
    // hold / 変形の長さを数値欄で。
    check(s->clickStateRow(0) && s->enterNumber(QStringLiteral("equationHoldField"), 90) &&
              s->data().states[0].holdFrames == 90,
          "A: 状態 0 の hold を 90f");
    check(s->clickStateRow(7) && s->enterNumber(QStringLiteral("equationHoldField"), 120) &&
              s->data().states[7].holdFrames == 120,
          "A: 状態 7 の hold を 120f");
    check(s->clickStateRow(6) &&
              s->enterNumber(QStringLiteral("equationTransitionFrames_outgoing"), 24) &&
              s->data().transitions[6].frames == 24,
          "A: 変形 6 の長さを 24f");

    // 部分式: 判別式 b^2-4ac を状態 4〜7 に、状態 6・7 に分母 2a、状態 7 に -b。
    for (int i = 4; i < kStates; ++i) {
        check(s->clickStateRow(i) && addPart(*s, "b^2-4ac", QStringLiteral("disc")),
              "A: 状態 " + std::to_string(i) + " に判別式の部分式");
    }
    check(s->clickStateRow(6) && addPart(*s, "2a", QStringLiteral("denom"), true),
          "A: 状態 6 の分母 (同じ文字 2a の後ろの方)");
    check(s->clickStateRow(7) && addPart(*s, "2a", QStringLiteral("denom")) &&
              addPart(*s, "-b", QStringLiteral("minus-b")),
          "A: 状態 7 の分母と -b");
    const auto& last = s->data().states[7];
    const auto disc = std::find_if(last.parts.begin(), last.parts.end(),
                                   [](const auto& p) { return p.label == "disc"; });
    check(disc != last.parts.end() && disc->binding.expectedText == "b^2-4ac" &&
              disc->binding.status == project::BindingStatus::Bound,
          "A: 最終状態の判別式が b^2-4ac に bound");
    s->shot("02-state-with-parts");
    s->layout("parts");

    // 対応: 判別式 4→5→6→7、分母 6→7。
    check(s->clickStateRow(5) &&
              addPair(*s, QStringLiteral("incoming"), QStringLiteral("disc「b^2-4ac」"),
                      QStringLiteral("disc「b^2-4ac」")) &&
              addPair(*s, QStringLiteral("outgoing"), QStringLiteral("disc「b^2-4ac」"),
                      QStringLiteral("disc「b^2-4ac」")),
          "A: 判別式の対応 4→5 と 5→6");
    check(s->clickStateRow(7) &&
              addPair(*s, QStringLiteral("incoming"), QStringLiteral("disc「b^2-4ac」"),
                      QStringLiteral("disc「b^2-4ac」")) &&
              addPair(*s, QStringLiteral("incoming"), QStringLiteral("denom「2a」"),
                      QStringLiteral("denom「2a」")),
          "A: 判別式と分母の対応 6→7");
    check(s->data().transitions[4].correspondence.size() == 1 &&
              s->data().transitions[5].correspondence.size() == 1 &&
              s->data().transitions[6].correspondence.size() == 2,
          "A: 明示の対応が 3 辺に保存される");
    check(s->clickStateRow(6), "A: 対応の場面");
    s->shot("03-transition-correspondence");
    s->layout("correspondence");

    // 強調: 状態 7 の判別式に outline [10,40)、pulse [60,90)。
    check(s->clickStateRow(7) && s->clickPartRow(QStringLiteral("disc")), "A: 判別式を選ぶ");
    check(
        s->chooseCombo(QStringLiteral("equationActionTarget"), QStringLiteral("disc「b^2-4ac」")) &&
            s->chooseCombo(QStringLiteral("equationActionOperation"),
                           QStringLiteral("outline (囲み線)")) &&
            s->enterNumber(QStringLiteral("equationActionStart"), 10) &&
            s->enterNumber(QStringLiteral("equationActionDuration"), 30) &&
            s->click(QStringLiteral("equationAddAction")),
        "A: outline を追加");
    check(s->click(QStringLiteral("equationNewAction")) &&
              s->chooseCombo(QStringLiteral("equationActionOperation"),
                             QStringLiteral("pulse (拡大と強調色)")) &&
              s->enterNumber(QStringLiteral("equationActionStart"), 60) &&
              s->enterNumber(QStringLiteral("equationActionDuration"), 30) &&
              s->click(QStringLiteral("equationAddAction")),
          "A: pulse を追加");
    const auto& actions = s->data().actions;
    check(actions.size() == 2 && actions[0].operation == project::EquationOperation::Outline &&
              actions[0].start == 10 && actions[0].duration == 30 &&
              actions[1].operation == project::EquationOperation::Pulse && actions[1].start == 60 &&
              actions[1].duration == 30 && actions[0].target == disc->id &&
              actions[1].target == disc->id && actions[0].state == last.id,
          "A: 判別式への outline と pulse");
    // 区間が hold を超える変更は拒否 (UI の案内と domain の両方)。
    {
        const auto before = s->project();
        const auto d = s->controller->undoDepthForTest();
        check(s->enterNumber(QStringLiteral("equationActionDuration"), 200), "A: 長さを打つ");
        auto* hint = s->find(QStringLiteral("equationActionHint"));
        check(hint && hint->property("text").toString().contains(QStringLiteral("超えています")),
              "A: hold を超える区間を確定前に示す");
        s->click(QStringLiteral("equationUpdateAction"));
        check(s->project() == before && s->controller->undoDepthForTest() == d &&
                  s->view().value("messageError").toBool(),
              "A: hold を超える action の変更は Project を変えず理由を示す");
    }
    // action の行を選んで編集の場面を撮る。
    auto selectActionRow = [&](int index) {
        return s->click(QStringLiteral("equationActionRow_%1").arg(index));
    };
    check(selectActionRow(0), "A: outline を選ぶ");
    s->shot("04-outline-action-editor");
    s->layout("outline");
    check(selectActionRow(1), "A: pulse を選ぶ");
    s->shot("05-pulse-action-editor");
    s->layout("pulse");
    s->reachable("quadratic", kControls);
    s->shot("06-quadratic-8-states");

    // ---- キーボード・focus ----
    {
        const auto clips = s->project().timelineClips.size();
        const auto d = s->controller->undoDepthForTest();
        check(s->clickStateRow(3), "鍵: 状態の一覧に focus");
        s->key(Qt::Key_Delete);
        check(s->project().timelineClips.size() == clips && s->data().states.size() == kStates &&
                  s->controller->undoDepthForTest() == d,
              "鍵: 状態の一覧の Delete は clip も状態も消さない");
        s->key(Qt::Key_Down);
        check(s->view().value("state").toMap().value("index").toInt() == 4 &&
                  s->data().states[3].equation.source == kQuadratic[3],
              "鍵: 一覧の下は選択の移動で、並べ替えない");
        check(s->clickStateRow(7) && s->clickPartRow(QStringLiteral("disc")),
              "鍵: 部分式の一覧に focus");
        s->key(Qt::Key_Delete);
        check(s->project().timelineClips.size() == clips && s->data().states[7].parts.size() == 3 &&
                  s->controller->undoDepthForTest() == d,
              "鍵: 部分式の一覧の Delete は clip も部分式も消さない");
        // 編集欄: 矢印・Space・Ctrl+Z は文字の編集で、状態の順・再生・Project の履歴に効かない。
        check(s->clickStateRow(0) && s->focusSource(), "鍵: 式の欄に focus");
        s->key(Qt::Key_End);
        s->key(Qt::Key_Up);
        s->key(Qt::Key_Down);
        s->key(Qt::Key_Space);
        check(!s->controller->playing() && s->data().states[0].equation.source == kQuadratic[0],
              "鍵: 編集欄の Space は再生せず、矢印は並べ替えない");
        s->key(Qt::Key_Z, Qt::ControlModifier);
        check(s->controller->undoDepthForTest() == d &&
                  s->sourceEditor()->property("text").toString() ==
                      QString::fromLatin1(kQuadratic[0]),
              "鍵: 編集欄の Ctrl+Z は入力を戻し、Project の Undo を使わない");
        s->key(Qt::Key_Delete);
        check(s->project().timelineClips.size() == clips,
              "鍵: 編集欄の Delete は文字を消し clip を消さない");
        s->key(Qt::Key_Escape);
        check(s->data().states[0].equation.source == kQuadratic[0] &&
                  s->controller->undoDepthForTest() == d,
              "鍵: Esc で取り消すと Project は変わらない");
        // Tab は tab 文字を入れず focus を移す (離れると確定。文字は変わっていない)。
        check(s->focusSource(), "鍵: 式の欄に focus (Tab)");
        s->key(Qt::Key_Tab);
        check(!s->sourceEditor()->hasActiveFocus() &&
                  s->data().states[0].equation.source == kQuadratic[0],
              "鍵: Tab は focus を次の操作へ移し、tab 文字を入れない");
        // 確定の後に focus を移してから Ctrl+Z / Ctrl+Shift+Z: Project の編集を 1
        // 回ずつ戻す・やり直す。
        const auto beforeEdit = s->project();
        check(s->focusSource(), "鍵: 式の欄に focus (Undo)");
        s->key(Qt::Key_End);
        s->type(" ");
        check(s->clickStateRow(1), "鍵: 別の行をクリックして確定");
        const auto afterEdit = s->project();
        check(s->data().states[0].equation.source == std::string(kQuadratic[0]) + " " &&
                  s->controller->undoDepthForTest() == d + 1,
              "鍵: focus を離すと 1 回だけ確定");
        s->key(Qt::Key_Z, Qt::ControlModifier);
        check(s->project() == beforeEdit && s->controller->undoDepthForTest() == d,
              "鍵: Ctrl+Z は確定 1 回分だけ戻す");
        s->key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        check(s->project() == afterEdit, "鍵: Ctrl+Shift+Z で同じ Project (ID を含む) に戻る");
        s->key(Qt::Key_Z, Qt::ControlModifier);
        check(s->project() == beforeEdit, "鍵: 非破壊的な編集の Undo");
    }

    // 拒否された空の下書きから状態を変える操作を実際の製品 UI で試す。
    check(s->clickStateRow(0), "拒否: 最初の状態を選ぶ");
    const auto rejectedProject = s->project();
    const auto rejectedState = s->view().value("state").toMap().value("id").toString();
    const auto rejectedDepth = s->controller->undoDepthForTest();
    check(s->focusSource(), "拒否: 編集欄に focus");
    s->key(Qt::Key_A, Qt::ControlModifier);
    s->key(Qt::Key_Backspace);
    s->key(Qt::Key_Return, Qt::ControlModifier);
    const auto verifyRejected = [&](const char* operation) {
        check(s->sourceEditor()->property("text").toString().isEmpty(),
              std::string(operation) + ": 下書きは正確に空のまま");
        check(s->view().value("state").toMap().value("id").toString() == rejectedState,
              std::string(operation) + ": 選択状態は不変");
        check(s->project() == rejectedProject, std::string(operation) + ": Project は不変");
        check(s->controller->undoDepthForTest() == rejectedDepth,
              std::string(operation) + ": Undo depth は不変");
        check(s->sourceEditor()->hasActiveFocus(), std::string(operation) + ": 編集を継続できる");
    };
    check(s->clickStateRow(1), "拒否: 別の状態をクリック");
    verifyRejected("状態選択");
    check(s->click(QStringLiteral("equationInsertBefore")), "拒否: 前に挿入をクリック");
    verifyRejected("前に挿入");
    check(s->click(QStringLiteral("equationInsertAfter")), "拒否: 後に挿入をクリック");
    verifyRejected("後に挿入");
    check(s->click(QStringLiteral("equationDeleteState")), "拒否: 状態削除をクリック");
    verifyRejected("状態削除");
    s->shot("rejected-draft-navigation");
    s->key(Qt::Key_Escape);

    // ---- 保存・閉じる・開き直す ----
    s->window->contentItem()->forceActiveFocus();
    s->key(Qt::Key_S, Qt::ControlModifier);
    check(pumpUntil([&] { return !s->controller->dirty(); }), "保存: Ctrl+S で保存");
    const auto saved = s->project();
    const auto savedClip = *s->sequence();
    s->close();
    s.reset();
    const auto loaded = project::loadProjectJson(path);
    check(loaded.success && loaded.project.schemaVersion == project::kProjectSchemaVersion,
          "再読込: schema 21: " + loaded.error);
    if (!loaded.success)
        return 1;
    check(loaded.project.timelineClips == saved.timelineClips,
          "再読込: clip (状態の順・尺・ID・binding・対応・action) が保存前と同じ");
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(file)), {});
        const auto again = project::serializeProjectJson(loaded.project, path);
        check(again.success && again.json == bytes, "再読込: JSON の往復で同じ byte 列");
    }
    auto reopened = open(path, loaded.project);
    if (!reopened)
        return 4;
    s = std::move(reopened);
    int clipIndex = -1;
    for (std::size_t i = 0; i < s->project().timelineClips.size(); ++i)
        if (s->project().timelineClips[i].kind == project::TimelineClipKind::EquationSequence)
            clipIndex = static_cast<int>(i);
    check(clipIndex >= 0 && s->controller->selectClip(clipIndex), "再読込: clip を選ぶ");
    const auto& data = s->data();
    bool order = data.states.size() == kStates;
    for (int i = 0; order && i < kStates; ++i)
        order = data.states[static_cast<std::size_t>(i)].equation.source == kQuadratic[i] &&
                data.states[static_cast<std::size_t>(i)].id ==
                    savedClip.equationSequence.states[static_cast<std::size_t>(i)].id;
    check(order && data == savedClip.equationSequence &&
              s->view().value("stateCount").toInt() == kStates,
          "再読込: 状態の順と全 ID が同じで、UI に 8 状態");

    // ---- 直接 seek の提示 (P3-4 の経路) ----
    const auto& clip = *s->sequence();
    if (!qEnvironmentVariable("MVM_P36_REAL_MANIM").isEmpty()) {
        const auto compiled = mvm::app::compileEquationSequence(clip.equationSequence);
        std::string detail;
        const auto spec = mvm::app::equationSequenceRenderSpecFor(*compiled.value, detail);
        check(pumpUntil(
                  [&] {
                      return s->controller->mathRastersForTest()
                                 .equationSequenceEntryOf(*spec)
                                 .state == MathRasterCache::State::Ready;
                  },
                  300000),
              "P3-6: 再読込後の実 Manim artifact を待つ");
    }
    std::vector<project::EquationInterval> intervals;
    std::int64_t length = 0;
    std::string error;
    project::equationIntervals(clip.equationSequence, intervals, length, error);

    struct Probe {
        const char* name;
        std::int64_t source;
        const char* preview;
    };

    const auto holdBegin = [&](int state) {
        for (const auto& i : intervals)
            if (!i.transition && i.index == static_cast<std::size_t>(state))
                return i.begin;
        return std::int64_t{-1};
    };
    const auto transitionBegin = [&](int t) {
        for (const auto& i : intervals)
            if (i.transition && i.index == static_cast<std::size_t>(t))
                return i.begin;
        return std::int64_t{-1};
    };
    const Probe probes[] = {{"hold 0 の frame 5", holdBegin(0) + 5, "static"},
                            {"変形 6 の中央", transitionBegin(6) + 12, "transition"},
                            {"outline の中央", holdBegin(7) + 25, "action"},
                            {"pulse の中央", holdBegin(7) + 75, "action"},
                            {"状態 7 の hold (action の間)", holdBegin(7) + 50, "static"}};
    for (const auto& probe : probes) {
        const auto frame = timelineFrameOfSource(clip, probe.source);
        s->seek(frame);
        const bool shown = pumpUntil(
            [&] {
                s->editor().refreshStatus();
                return s->editor().status().value("preview").toString() ==
                       QString::fromLatin1(probe.preview);
            },
            20000);
        check(shown && s->controller->playheadFrame() == frame &&
                  pumpUntil([&] { return s->observed(frame, probe.preview); }),
              std::string("直接 seek: ") + probe.name + " で " + probe.preview + " を提示 (" +
                  s->editor().status().value("previewText").toString().toStdString() + ")");
        if (!shown) {
            const auto diagnostic = s->controller->equationSequencePreviewStatus(
                QString::fromStdString(clip.id), frame);
            std::fprintf(stderr, "描画の診断: %s / 実行 %d 回\n",
                         qUtf8Printable(diagnostic.diskMessage),
                         s->backend.equationRenders->load());
        }
    }
    s->shot("07-reopened-seek-action");

    // ---- P3-6: 再読込した Project から oracle を作り、製品の書き出し UI を実行 ----
    {
        namespace app = mvm::app;
        const auto compiled = app::compileEquationSequence(clip.equationSequence);
        const auto renderSpec = app::equationSequenceRenderSpecFor(*compiled.value, error);
        auto& cache = s->controller->mathRastersForTest();
        const auto artifact = cache.readyEquationSequence(*renderSpec);
        check(artifact.has_value(), "P3-6: 再読込後の現在 artifact を検証");
        if (!artifact)
            return 1;
        app::EquationPreviewInputs inputs;
        inputs.clip = clip;
        inputs.spec = *compiled.value;
        inputs.timelineFpsNum = s->project().timelineFpsNum;
        inputs.timelineFpsDen = s->project().timelineFpsDen;
        inputs.outputWidth = s->project().outputWidth;
        inputs.outputHeight = s->project().outputHeight;
        inputs.artifactReady = true;
        for (const auto& state : renderSpec->states) {
            const auto png = cache.readyArtifact(state.still);
            mvm::math::MathCoverage mask;
            check(png && app::loadMathCoverage(*png, mask, error), "oracle: 現在の静止を取得");
            inputs.statics.push_back(app::EquationPreviewStatic{
                mask.width, mask.height,
                std::make_shared<const std::vector<std::uint8_t>>(mask.alpha),
                state.foregroundArgb});
        }
        for (std::size_t t = 0; t < artifact->transitions.size(); ++t) {
            const auto& item = artifact->transitions[t];
            app::EquationPreviewTransitionArtifact metadata{
                item.width,   item.height, item.sourceX, item.sourceY, item.targetX,
                item.targetY, {}};
            for (std::size_t i = 0; i < item.frames.size(); ++i) {
                std::vector<std::uint8_t> bytes;
                check(app::loadEquationArtifactFrame(item.frames[i], item.width, item.height, bytes,
                                                     error),
                      "oracle: 変形の SHA を検証");
                inputs.transitionFrames[{t, static_cast<std::int64_t>(i)}] =
                    std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
                metadata.colors.push_back(item.frames[i].colorArgb);
            }
            inputs.transitions.push_back(std::move(metadata));
        }
        for (std::size_t a = 0; a < artifact->actions.size(); ++a) {
            const auto& item = artifact->actions[a];
            app::EquationPreviewActionArtifact metadata{
                item.width, item.height, item.staticX, item.staticY, item.base.colorArgb, {}};
            std::vector<std::uint8_t> base;
            check(app::loadEquationArtifactFrame(item.base, item.width, item.height, base, error),
                  "oracle: base の SHA を検証");
            inputs.actionBases[a] =
                std::make_shared<const std::vector<std::uint8_t>>(std::move(base));
            for (std::size_t i = 0; i < item.accent.size(); ++i) {
                std::vector<std::uint8_t> bytes;
                check(app::loadEquationArtifactFrame(item.accent[i], item.width, item.height, bytes,
                                                     error),
                      "oracle: accent の SHA を検証");
                inputs.actionAccents[{a, static_cast<std::int64_t>(i)}] =
                    std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
                metadata.accentColors.push_back(item.accent[i].colorArgb);
            }
            inputs.actions.push_back(std::move(metadata));
        }
        app::EquationPreviewModel model(inputs);
        const auto rect = model.patchRect();
        const std::vector<std::int64_t> sources{0,
                                                45,
                                                transitionBegin(6),
                                                transitionBegin(6) + 12,
                                                transitionBegin(6) + 23,
                                                holdBegin(7),
                                                holdBegin(7) + 10,
                                                holdBegin(7) + 25,
                                                holdBegin(7) + 39,
                                                holdBegin(7) + 40,
                                                holdBegin(7) + 60,
                                                holdBegin(7) + 75,
                                                holdBegin(7) + 89,
                                                holdBegin(7) + 90,
                                                length - 1};
        for (const auto source : sources) {
            const auto frame = timelineFrameOfSource(clip, source);
            std::vector<std::uint8_t> patch(static_cast<std::size_t>(rect.width * rect.height * 4));
            model.fill(model.stateCode(model.shownAt(frame)), patch.data());
            std::vector<std::uint8_t> rgba(
                static_cast<std::size_t>(inputs.outputWidth * inputs.outputHeight * 4));
            for (int y = 0; y < rect.height; ++y)
                std::copy_n(patch.data() + y * rect.width * 4, rect.width * 4,
                            rgba.data() + ((rect.y + y) * inputs.outputWidth + rect.x) * 4);
            s->observation->exportExpected.emplace(frame, std::move(rgba));
        }
        const auto output = evidence / L"quadratic-reopened.mp4";
        auto* fileDialog = s->window->findChild<QObject*>(QStringLiteral("exportFileDialog"));
        check(fileDialog &&
                  fileDialog->setProperty(
                      "selectedFile",
                      QUrl::fromLocalFile(QString::fromStdWString(output.wstring()))) &&
                  QMetaObject::invokeMethod(fileDialog, "accepted"),
              "P3-6: 書き出し先の UI signal から設定ダイアログを開く");
        pump(100);
        auto* exportButton = s->find(QStringLiteral("exportAcceptButton"));
        if (exportButton)
            QTest::mouseClick(s->window, Qt::LeftButton, {}, s->center(exportButton));
        check(exportButton && s->controller->exporting(), "P3-6: 製品の書き出すボタンをクリック");
        check(pumpUntil([&] { return !s->controller->exporting(); }, 120000) &&
                  std::filesystem::exists(output),
              "P3-6: 保存再読込後の timeline を製品の H.264 profile で出力: " +
                  s->controller->statusText().toStdString());
        {
            std::lock_guard lock(s->observation->mutex);
            check(s->observation->exportCompared == static_cast<int>(sources.size()) &&
                      s->observation->exportMismatches == 0,
                  "P3-6: 全 15 probe が CPU oracle と RGBA byte 単位で完全一致");
        }
        QProcess decoder;
        decoder.start(
            QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
            {"-v", "error", "-i", QString::fromStdWString(output.wstring()), "-f", "null", "-"});
        check(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
              "P3-6: 製品出力を全 frame 復号できる");
    }

    // ---- 破壊的な編集の Undo/Redo (UI) ----
    {
        const auto before = s->project();
        check(s->clickStateRow(3) && s->click(QStringLiteral("equationDeleteState")),
              "破壊: 削除の確認を開く");
        auto* text = s->find(QStringLiteral("equationDeleteStateText"));
        check(text &&
                  text->property("text").toString().contains(QStringLiteral("隣接する変形 2 件")),
              "破壊: 削除で消える action・変形・対応を示す");
        s->shot("08-delete-state-confirm");
        check(s->click(QStringLiteral("equationDeleteStateConfirmButton")) &&
                  s->data().states.size() == kStates - 1,
              "破壊: 状態を削除");
        const auto after = s->project();
        s->window->contentItem()->forceActiveFocus();
        s->key(Qt::Key_Z, Qt::ControlModifier);
        check(s->project() == before, "破壊: Ctrl+Z で元の ID の状態・辺・対応が戻る");
        s->key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        check(s->project() == after, "破壊: Ctrl+Shift+Z で削除後と同じ Project");
        s->key(Qt::Key_Z, Qt::ControlModifier);
    }
    s->close();
    return 0;
}

// ---- B. 修復・状態・layout ----
int repairAndLayouts(const std::filesystem::path& directory) {
    const auto path = directory / L"repair.mvm";
    auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "B: Project を保存");
    auto s = open(path, initial);
    if (!s)
        return 4;
    auto* entry = s->window->findChild<QObject*>(QStringLiteral("addEquationSequenceMenuItem"));
    check(entry && QMetaObject::invokeMethod(entry, "triggered"), "B: 作成");
    pumpUntil([&] { return s->sourceEditor() && s->sourceEditor()->hasActiveFocus(); });
    check(s->replaceSource("a+bc+d") && addPart(*s, "bc", QStringLiteral("bc")), "B: 部分式 bc");
    check(s->click(QStringLiteral("equationInsertAfter")) && s->replaceSource("a+bc+d") &&
              addPart(*s, "bc", QStringLiteral("bc2")),
          "B: 2 つ目の状態");
    s->shot("09-empty-correspondence");
    check(s->clickStateRow(0) && s->clickPartRow(QStringLiteral("bc")) &&
              s->enterNumber(QStringLiteral("equationActionStart"), 5) &&
              s->enterNumber(QStringLiteral("equationActionDuration"), 20) &&
              s->click(QStringLiteral("equationAddAction")),
          "B: bc に outline");
    const auto bcId = partId(*s, QStringLiteral("bc"));
    // Invalid: 部分式の中に文字を打つ (信頼済み編集で Invalid)。
    check(s->focusSource(), "B: 編集欄");
    s->key(Qt::Key_Home, Qt::ControlModifier);
    s->key(Qt::Key_Right);
    s->key(Qt::Key_Right);
    s->key(Qt::Key_Right);
    s->type("Q");
    s->key(Qt::Key_Return, Qt::ControlModifier);
    check(s->data().states[0].equation.source == "a+bQc+d" &&
              s->data().states[0].parts[0].binding.status == project::BindingStatus::Invalid,
          "B: 部分式の中の入力で Invalid");
    s->editor().refreshStatus();
    check(s->clickPartRow(QStringLiteral("bc")) &&
              s->view().value("selectedPart").toMap().value("status") == "invalid_binding",
          "B: Invalid を修復の対象として示す");
    s->shot("10-invalid-binding");
    s->layout("invalid");
    check(s->selectSource(2, 5) && s->click(QStringLiteral("equationRebindPart")) &&
              s->data().states[0].parts[0].binding.status == project::BindingStatus::Bound &&
              s->data().states[0].parts[0].id.value == bcId.toStdString(),
          "B: 選び直した範囲で同じ PartId を修復");
    check(
        s->view().value("message").toString().contains(QStringLiteral("対応は自動では戻りません")),
        "B: 修復した後、対応は戻らないことを示す");
    s->shot("11-invalid-repaired");
    // 欠落: 部分式を消す (確認付き) → action は missing。
    check(s->clickPartRow(QStringLiteral("bc")) && s->click(QStringLiteral("equationDeletePart")) &&
              s->click(QStringLiteral("equationDeletePartConfirmButton")),
          "B: 部分式を削除");
    check(s->data().actions[0].targetStatus == project::EquationTargetStatus::Missing &&
              s->view().value("selectedPart").toMap().value("status") == "missing_target",
          "B: action の対象が欠落し、作り直せる状態として選ばれている");
    s->shot("12-missing-action-target");
    s->layout("missing");
    check(s->selectSource(2, 5) && s->click(QStringLiteral("equationRebindPart")) &&
              s->data().actions[0].targetStatus == project::EquationTargetStatus::Present &&
              s->data().actions[0].target.value == bcId.toStdString(),
          "B: 欠落した部分式を同じ ID で作り直し、action を有効に戻す");

    // backend 不在 (環境の問題として示し、内容の修復を出さない)。
    s->controller->setMathPreflightForTest(FakeMathBackend::unavailable("試験: Manim が無い"));
    check(pumpUntil([&] {
              s->editor().refreshStatus();
              return s->editor().status().value("renderer") == "unavailable";
          }),
          "B: backend 不在の状態");
    check(s->find(QStringLiteral("equationDependencyGuidance")) &&
              s->find(QStringLiteral("equationRetryButton")) &&
              s->editor().status().value("category") == "environment",
          "B: 導入の案内と再試行、分類は環境");
    s->reach(s->find(QStringLiteral("equationStatusPanel")));
    s->shot("13-backend-unavailable");
    s->controller->setMathPreflightForTest(s->backend.preflight());

    // OverBudget: action の frame で memory の上限を 1 byte に。
    const auto& clip = *s->sequence();
    s->seek(clip.timelineStartFrame + 10);
    check(pumpUntil(
              [&] {
                  s->editor().refreshStatus();
                  return s->editor().status().value("preview") == "action";
              },
              20000),
          "B: action の frame を提示");
    auto& cache = s->controller->mathRastersForTest();
    cache.setResidentMemoryBudget(1);
    s->seek(clip.timelineStartFrame + 12);
    check(pumpUntil(
              [&] {
                  s->editor().refreshStatus();
                  return s->editor().status().value("residency") == "over_budget";
              },
              20000),
          "B: OverBudget の状態");
    check(s->editor().status().value("category") == "memory" &&
              s->editor().status().value("compile") == "ready" &&
              !s->find(QStringLiteral("equationRetryButton")),
          "B: OverBudget は memory の問題で、Project の破損や修復として示さない");
    s->reach(s->find(QStringLiteral("equationStatusPanel")));
    s->shot("14-over-budget");
    cache.setResidentMemoryBudget(MathRasterCache::kDefaultResidentMemoryBudget);

    // 読むだけの polling: 再生位置の外の sequence を選んでも描画を始めない。
    {
        auto distantProject = s->project();
        auto data = s->data();
        std::string error;
        check(project::remapEquationSequenceIds(
                  data,
                  [] {
                      static int n = 0;
                      return "far-" + std::to_string(n++);
                  },
                  error),
              "B: 外の sequence の ID");
        data.states[0].equation.source = "q+r";
        data.states[1].equation.source = "q+r";
        for (auto& st : data.states)
            st.parts.clear();
        data.actions.clear();
        for (auto& t : data.transitions)
            t.correspondence.clear();
        check(project::addEquationSequence(distantProject, data, "far", "外",
                                           {project::TrackKind::Video, 0}, 9000)
                  .success,
              "B: 再生位置の外に 2 本目");
        s->close();
        s.reset();
        // autosave の残る修復用 Project を外部から上書きせず、別 fixture で検査する。
        const auto pollingPath = directory / L"polling.mvm";
        check(project::saveProjectJson(distantProject, pollingPath).success,
              "B: 2 本目を含めて保存");
        s = open(pollingPath, distantProject);
        if (!s)
            return 4;
        // 再生位置の sequence の描画が終わってから数え始める (見えている clip の要求と混ぜない)。
        const auto nearId = QString::fromStdString(s->project().timelineClips[0].id);
        check(pumpUntil(
                  [&] {
                      return s->controller->equationSequencePreviewStatus(nearId, 0).disk ==
                             MathRasterCache::State::Ready;
                  },
                  20000),
              "B: 再生位置の sequence の描画が終わる");
        pump(500);
        auto& c = s->controller->mathRastersForTest();
        int index = -1;
        for (std::size_t i = 0; i < s->project().timelineClips.size(); ++i)
            if (s->project().timelineClips[i].id == "far")
                index = static_cast<int>(i);
        check(index >= 0 && s->controller->selectClip(index) &&
                  s->view().value("clipId").toString() == "far",
              "B: 外の sequence を UI で選ぶ");
        pump(300);
        s->shot("polling-outside-clip");
        check(s->inspector() != nullptr, "B: 外の sequence の inspector が実描画される");
        const auto records = c.recordCount();
        const auto sequences = c.equationSequenceRecordCount();
        const auto renders = s->backend.renders->load();
        const auto equationRenders = s->backend.equationRenders->load();
        const auto polls = s->editor().statusRefreshCountForTest();
        const bool polled = pumpUntil(
            [&] {
                // 状態が実際に問い合わせられることを確認する。animation の頻度は契約ではない。
                s->window->update();
                return s->editor().statusRefreshCountForTest() > polls;
            },
            10000);
        check(polled, "B: QML の polling が状態を問い合わせている (" +
                          std::to_string(s->editor().statusRefreshCountForTest() - polls) + " 回)");
        auto* poller = s->inspector()->findChild<QObject*>(QStringLiteral("equationStatusPoll"));
        check(poller && poller->property("running").toBool() && poller->property("repeat").toBool(),
              "B: 状態の polling が繰り返し有効");
        // 四回の handler 実行と読み取り専用性は、背面の animation cadence と独立に検査する。
        const auto handlerPolls = s->editor().statusRefreshCountForTest();
        bool invoked = poller != nullptr;
        for (int i = 0; invoked && i < 4; ++i)
            invoked = QMetaObject::invokeMethod(poller, "triggered", Qt::DirectConnection);
        check(invoked && s->editor().statusRefreshCountForTest() >= handlerPolls + 4,
              "B: 実 QML handler の四回の問い合わせを検査");
        check(c.recordCount() == records && c.equationSequenceRecordCount() == sequences &&
                  s->backend.renders->load() == renders &&
                  s->backend.equationRenders->load() == equationRenders,
              "B: polling は描画を要求せず cache の record も作らない");
        check(s->editor().status().value("preview") == "outside", "B: 再生位置の外と示す");
    }

    // 狭い panel と低い window。
    s->window->setProperty("leftPanelWidth", 240);
    pump(200);
    s->layout("narrow");
    s->reachable("narrow", kControls);
    s->shot("15-narrow-panel");
    s->window->setProperty("leftPanelWidth", 520);
    const QSize size = s->window->size();
    s->window->resize(size.width(), 560);
    pump(300);
    s->layout("low-height");
    s->reachable("low-height", kControls);
    if (auto* flick = s->scroll())
        flick->setProperty("contentY", 0);
    s->shot("16-low-height-top");
    if (auto* flick = s->scroll())
        flick->setProperty(
            "contentY",
            std::max<qreal>(0, flick->property("contentHeight").toReal() - flick->height()));
    s->shot("17-low-height-bottom");
    s->window->resize(size);
    pump(200);
    s->close();
    return 0;
}

// 長い内容: 10 状態・600 文字の式・12 部分式・長い名前・長い理由。
int longContent(const std::filesystem::path& directory) {
    const auto path = directory / L"long.mvm";
    auto initial = project::createDefaultProject();
    project::EquationSequenceClipData data;
    std::string longSource;
    for (int i = 0; i < 60; ++i)
        longSource += "x_" + std::to_string(i % 10) + "+";
    longSource += "y=0";
    for (int i = 0; i < 10; ++i) {
        project::EquationState st;
        st.id = {"long-s" + std::to_string(i)};
        st.revision = "long-r" + std::to_string(i);
        st.equation.source = i == 0 ? longSource : "a_" + std::to_string(i) + "+b=c";
        st.holdFrames = 30;
        data.states.push_back(st);
        if (i > 0)
            data.transitions.push_back({{"long-t" + std::to_string(i)},
                                        data.states[static_cast<std::size_t>(i - 1)].id,
                                        st.id,
                                        10,
                                        {}});
    }
    auto& first = data.states[0];
    for (int k = 0; k < 12; ++k) {
        const auto at = static_cast<std::int64_t>(k * 4);
        first.parts.push_back(
            {{"long-p" + std::to_string(k)},
             "とても長い部分式の名前で狭いパネルでも省略されるべきもの " + std::to_string(k),
             {first.revision, at, at + 3,
              first.equation.source.substr(static_cast<std::size_t>(at), 3),
              project::BindingStatus::Bound}});
    }
    check(project::addEquationSequence(initial, data, "long", "長い",
                                       {project::TrackKind::Video, 0}, 0)
              .success,
          "長い: fixture");
    check(project::saveProjectJson(initial, path).success, "長い: 保存");
    auto s = open(path, initial);
    if (!s)
        return 4;
    check(s->controller->selectClip(0) && s->view().value("stateCount").toInt() == 10,
          "長い: 選ぶ");
    // 長い理由: 重なる範囲の追加を拒否させる。
    check(s->selectSource(1, 6), "長い: 重なる範囲を選ぶ");
    s->click(QStringLiteral("equationAddPart"));
    check(s->view().value("messageError").toBool(), "長い: 拒否の理由");
    s->window->setProperty("leftPanelWidth", 300);
    pump(200);
    s->layout("long");
    s->reachable("long", kControls);
    if (auto* flick = s->scroll())
        flick->setProperty("contentY", 0);
    s->shot("18-long-content");
    s->close();
    return 0;
}
} // namespace

int runGraphAuthoringUi(const std::filesystem::path& directory, const std::filesystem::path& manim,
                        bool exportAcceptance) {
    if (std::filesystem::exists(directory))
        return 2;
    std::filesystem::create_directories(directory);
    evidence = directory;
    const auto path = directory / L"graph.mvm";
    bool observedReady = true;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [&] { observedReady = false; }, Qt::QueuedConnection);
    check(pumpUntil([&] { return observedReady; }),
          "Graph: 成立した待機条件を次の更新で失敗へ反転させない");
    QCoreApplication::processEvents();
    check(!observedReady, "Graph: 待機の負例が次の更新を実際に配送した");
    auto initial = project::createDefaultProject();
    initial.outputWidth = 320;
    initial.outputHeight = 180;
    check(project::saveProjectJson(initial, path).success, "Graph: 空の Project の準備");
    qputenv("MVM_P36_REAL_MANIM", QByteArray::fromStdString(manim.string()));
    auto s = open(path, initial);
    if (!s)
        return 4;
    auto* entry = s->window->findChild<QObject*>(QStringLiteral("addGraphClipMenuItem"));
    const auto depth = s->controller->undoDepthForTest();
    check(entry && QMetaObject::invokeMethod(entry, "triggered"), "Graph: 実際の作成メニュー");
    pump(100);
    check(s->controller->selectedGraphClip().contains("clipId") &&
              s->controller->undoDepthForTest() == depth + 1,
          "Graph: 一回の作成と選択");
    const auto input = [&](const QString& key, const std::string& text) {
        auto* field = s->find(QStringLiteral("graphField_") + key);
        if (!field || !s->reach(field)) {
            check(false, "Graph: 編集欄への到達 " + key.toStdString());
            return false;
        }
        QTest::mouseClick(s->window, Qt::LeftButton, {}, s->center(field));
        s->key(Qt::Key_A, Qt::ControlModifier);
        s->type(text);
        const bool typed = field->property("text").toString() == QString::fromStdString(text);
        check(typed, "Graph: キー入力 " + key.toStdString());
        if (!typed)
            return false;
        s->key(Qt::Key_Return);
        pump(50);
        return true;
    };
    check(input("yMax", "25"), "Graph: viewport の入力");
    check(input("label", "f_1"), "Graph: 表示ラベル");
    check(input("color", "#80FF6655"), "Graph: 半透明色");
    check(s->click("graphAddFunction"), "Graph: 関数を追加");
    pump(50);
    auto functions = s->controller->selectedGraphClip().value("functions").toList();
    if (functions.size() != 2) {
        s->close();
        return 1;
    }
    const auto second = functions[1].toMap().value("id").toString();
    check(s->click("graphFunction_" + second) && input("expression", "2*x+1") &&
              input("color", "#FF55FF66"),
          "Graph: 二番目を ID で選び編集");
    check(s->click("graphAddFunction"), "Graph: 三番目の追加");
    pump(50);
    functions = s->controller->selectedGraphClip().value("functions").toList();
    if (functions.size() != 3) {
        s->close();
        return 1;
    }
    const auto third = functions[2].toMap().value("id").toString();
    check(s->click("graphFunction_" + third) && input("expression", "sin(x)") &&
              input("color", "#FF5577FF"),
          "Graph: 三番目の式と色");
    const int drawFrames = exportAcceptance ? 10 : 3;
    check(s->click("graphDraw") && input("frames", std::to_string(drawFrames)),
          "Graph: Draw frame の確定");
    const auto clipId = s->controller->selectedGraphClip().value("clipId").toString();
    check(
        pumpUntil([&] { return s->controller->graphStatusFromUi(clipId).value("ready").toBool(); },
                  180000),
        "Graph: 実 Manim の現在 artifact と resident frame が Ready");
    if (exportAcceptance)
        check(s->controller->trimClip(clipId, QStringLiteral("right"), -276, false),
              "P4-5: 製品 timeline の trim で受け入れ区間を 24 frame に確定する");
    s->shot("graph-ready");
    check(s->controller->saveProject(), "Graph: 製品保存");
    const auto saved = s->project();
    s->close();
    // UI で確定した同じ Project を原寸の native surface で描く。期待値は検証済み PNG の
    // straight RGBA に独立の整数式で黒背景を合成し、全画素を比較する。
    {
        MvmController nativeController(path, manim, saved);
        QQuickWindow nativeWindow;
        nativeWindow.setFlags(mvm::app::testBackgroundWindowFlags());
        mvm::app::applyTestFixedWindow(nativeWindow);
        nativeWindow.resize(320, 180);
        auto* surface = new mvm::app::PreviewEngineRhiItem(nativeWindow.contentItem());
        surface->setWidth(320);
        surface->setHeight(180);
        nativeController.attachPreview(surface);
        nativeWindow.show();
        check(pumpUntil([&] { return nativeController.previewReady(); }, 30000),
              "Graph: UI で確定した Project の native 初期化");
        QString isolation;
        check(mvm::test::isolatedFromUserInput(&nativeWindow, isolation),
              "Graph: 原寸比較も利用者の操作から隔離する");
        for (int frame : {2, 0, 1, 3, 10}) {
            bool accepted = false;
            check(pumpUntil([&] {
                      if (!accepted)
                          accepted = nativeController.seekTimelineFrame(frame);
                      return accepted;
                  }),
                  "Graph: Draw の非単調 seek");
            check(pumpUntil(
                      [&] {
                          return nativeController.graphPreviewStatus(clipId.toStdString(), frame)
                                     .frameAvailable &&
                                 nativeController.previewPresentedLatest();
                      },
                      180000),
                  "Graph: 現在 frame の native 提示");
            const auto status = nativeController.graphPreviewStatus(clipId.toStdString(), frame);
            const auto png =
                directory / L"cache" / L"graph" / L"graph.mvm" / status.key.toStdWString() /
                (frame < drawFrames ? "frame-" + std::to_string(frame) + ".png" : "static.png");
            const auto decoded = mvm::graph::readRgba(png, 320, 180);
            check(std::holds_alternative<mvm::graph::Raster>(decoded), "Graph: 期待値 PNG の読込");
            if (const auto* raster = std::get_if<mvm::graph::Raster>(&decoded)) {
                const auto image =
                    nativeWindow.grabWindow().convertToFormat(QImage::Format_RGBA8888);
                int mismatches = 0;
                if (image.size() != QSize(320, 180))
                    mismatches = 320 * 180;
                else
                    for (int y = 0; y < 180; ++y) {
                        const auto* actual = image.constScanLine(y);
                        for (int x = 0; x < 320; ++x) {
                            const auto at = static_cast<std::size_t>(y * 320 + x) * 4;
                            bool mismatch = actual[x * 4 + 3] != 255;
                            for (int c = 0; c < 3; ++c) {
                                const auto expected =
                                    (raster->rgba[at + static_cast<std::size_t>(c)] *
                                         raster->rgba[at + 3] +
                                     127) /
                                    255;
                                mismatch = mismatch || actual[x * 4 + c] != expected;
                            }
                            mismatches += mismatch;
                        }
                    }
                check(mismatches == 0,
                      "Graph: native 全画素の独立 RGBA 比較 frame=" + std::to_string(frame) +
                          " 不一致=" + std::to_string(mismatches));
                image.save(QString::fromStdWString(
                    (directory / ("native-" + std::to_string(frame) + ".png")).wstring()));
            }
        }
        nativeController.shutdown();
    }
    const auto loaded = project::loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips == saved.timelineClips,
          "Graph: 保存した全データと ID の一致");
    s = open(path, loaded.project);
    if (!s)
        return 4;
    check(s->controller->selectClip(0), "Graph: 再起動後の選択");
    pump(100);
    const auto exportProduct = [&](const std::filesystem::path& output, bool expectedSuccess = true,
                                   bool oracle = true, int stopMode = 0) {
        const auto snapshot = s->project();
        const auto duration = project::timelineEndFrame(snapshot);
        {
            std::lock_guard lock(s->observation->mutex);
            s->observation->graphExport = oracle;
            s->observation->exportExpected.clear();
            s->observation->graphComparedFrames.clear();
            s->observation->exportCompared = s->observation->exportMismatches = 0;
            s->observation->holdFirstEncoderFrame = stopMode == 3;
            s->observation->barrierReached.store(false);
            s->observation->barrierReleased.store(false);
            s->observation->runnerReturned.store(false);
            s->observation->cancelPolledWhileHeld.store(false);
            s->observation->barrierFrame = -1;
            s->observation->barrierFrameValid = s->observation->barrierCancelObserved = false;
            s->observation->barrierThread = {};
        }
        auto* dialog = s->window->findChild<QObject*>(QStringLiteral("exportFileDialog"));
        check(dialog &&
                  dialog->setProperty("selectedFile", QUrl::fromLocalFile(QString::fromStdWString(
                                                          output.wstring()))) &&
                  QMetaObject::invokeMethod(dialog, "accepted"),
              "P4-5: 保存再読込した Graph の製品書き出し設定を開く");
        pump(100);
        auto* button = s->find(QStringLiteral("exportAcceptButton"));
        // 失敗を期待するだけでは、開始前の no-op 取消や別理由の失敗が通る。
        // 取消と終了は、worker 開始後の status 遷移を同期的に記録して区別する。
        std::vector<QString> observedStatuses;
        QMetaObject::Connection statusWatch;
        if (stopMode != 0)
            statusWatch = QObject::connect(s->controller.get(), &MvmController::stateChanged, [&] {
                observedStatuses.push_back(s->controller->statusText());
            });
        if (button)
            QTest::mouseClick(s->window, Qt::LeftButton, {}, s->center(button));
        bool exportStarted = s->controller->exporting();
        if (stopMode != 0 && !exportStarted)
            exportStarted = pumpUntil([&] { return s->controller->exporting(); }, 15000);
        check(button && (stopMode == 0 ? exportStarted || !expectedSuccess : exportStarted),
              "P4-5: 製品の書き出すボタン");
        if (stopMode == 1 && exportStarted) {
            s->controller->cancelTimelineExport();
            observedStatuses.push_back(s->controller->statusText());
        }
        if (stopMode == 2 && exportStarted)
            s->controller->shutdown();
        // stopMode 3: 最初の encoder frame で保持した worker に対して、controller を所有する
        // thread から製品の shutdown を呼ぶ。
        bool activeShutdownProven = false;
        QJsonObject activeShutdown;
        if (stopMode == 3) {
            const bool reached =
                exportStarted &&
                pumpUntil([&] { return s->observation->barrierReached.load(); }, 60000);
            bool frameValid = false;
            std::int64_t frame = -1;
            bool workerThread = false;
            bool preflight = false;
            {
                std::lock_guard lock(s->observation->mutex);
                frameValid = s->observation->barrierFrameValid;
                frame = s->observation->barrierFrame;
                workerThread = s->observation->barrierThread != std::thread::id{} &&
                               s->observation->barrierThread != std::this_thread::get_id();
                preflight = s->observation->preflightConfigured;
            }
            check(reached && frame == 0 && frameValid && preflight && workerThread,
                  "P4-5 補遺: preflight と compile を通り、独立 oracle で一致した最初の encoder "
                  "frame で worker を保持する");
            const bool ownerThread = QThread::currentThread() == s->controller->thread();
            const bool heldBeforeShutdown = reached && !s->observation->barrierReleased.load() &&
                                            !s->observation->runnerReturned.load() &&
                                            s->controller->exporting();
            check(ownerThread && heldBeforeShutdown,
                  "P4-5 補遺: 保持中の worker に controller の所有 thread から shutdown する");
            int failureNotifications = 0;
            const auto failureWatch =
                QObject::connect(s->controller.get(), &MvmController::exportFailed,
                                 [&](const QString&) { ++failureNotifications; });
            if (reached)
                s->controller->shutdown();
            const auto statusesAtShutdown = observedStatuses.size();
            const auto statusAtShutdown = s->controller->statusText();
            bool cancelObserved = false;
            bool runnerCancelled = false;
            bool runnerSuccess = true;
            bool graphCancelled = false;
            QString runnerError;
            {
                std::lock_guard lock(s->observation->mutex);
                cancelObserved = s->observation->barrierCancelObserved;
                runnerCancelled = s->observation->runnerCancelled;
                runnerSuccess = s->observation->runnerSuccess;
                graphCancelled = s->observation->runnerGraphCancelled;
                runnerError = QString::fromStdString(s->observation->runnerError);
            }
            // shutdown の join が戻った時点で worker は runner を返し、完了の queued 呼出を
            // 投函し終えている。後から投函した marker が届けば、その完了呼出も配送済みである。
            const bool lifetimeResolved = reached && s->observation->barrierReleased.load() &&
                                          s->observation->runnerReturned.load() &&
                                          !s->controller->exporting() && !s->controller->busy();
            check(reached && cancelObserved && runnerCancelled && !runnerSuccess &&
                      graphCancelled && lifetimeResolved,
                  "P4-5 補遺: encoder の保持中に shutdown の取消を完了待ち loop が観測し、join "
                  "で寿命を解決する");
            bool markerDelivered = false;
            QMetaObject::invokeMethod(
                s->controller.get(), [&] { markerDelivered = true; }, Qt::QueuedConnection);
            const bool drained = pumpUntil([&] { return markerDelivered; });
            QObject::disconnect(failureWatch);
            const bool noStaleCompletion =
                drained && observedStatuses.size() == statusesAtShutdown &&
                s->controller->statusText() == statusAtShutdown && failureNotifications == 0;
            check(reached && noStaleCompletion,
                  "P4-5 補遺: shutdown 後に stale な完了通知を出さない");
            activeShutdownProven = reached && frame == 0 && frameValid && preflight &&
                                   workerThread && ownerThread && heldBeforeShutdown &&
                                   cancelObserved && runnerCancelled && !runnerSuccess &&
                                   graphCancelled && lifetimeResolved && noStaleCompletion;
            QJsonArray afterShutdown;
            for (auto i = statusesAtShutdown; i < observedStatuses.size(); ++i)
                afterShutdown.append(observedStatuses[i]);
            activeShutdown =
                QJsonObject{{"barrierReached", reached},
                            {"barrierFrame", static_cast<qint64>(frame)},
                            {"barrierFrameOracleMatched", frameValid},
                            {"preflightConfigured", preflight},
                            {"barrierOffOwnerThread", workerThread},
                            {"shutdownOnOwnerThread", ownerThread},
                            {"heldBeforeShutdown", heldBeforeShutdown},
                            {"cancelObservedAtBarrier", cancelObserved},
                            {"runnerCancelled", runnerCancelled},
                            {"runnerSuccess", runnerSuccess},
                            {"graphReadinessCancelled", graphCancelled},
                            {"runnerError", runnerError},
                            {"lifetimeResolved", lifetimeResolved},
                            {"queuedCompletionDrained", drained},
                            {"statusesAfterShutdown", afterShutdown},
                            {"failureNotificationsAfterShutdown", failureNotifications}};
        }
        const bool completed = (stopMode == 0 || exportStarted) &&
                               pumpUntil([&] { return !s->controller->exporting(); }, 60000);
        if (statusWatch)
            QObject::disconnect(statusWatch);
        const auto sawStatus = [&](const QString& text) {
            return std::find(observedStatuses.cbegin(), observedStatuses.cend(), text) !=
                   observedStatuses.cend();
        };
        const bool stopProven =
            stopMode == 0 ||
            (stopMode == 1 && sawStatus(QStringLiteral("書き出しをキャンセルしました"))) ||
            (stopMode == 2 && sawStatus(QStringLiteral("書き出しています…")) &&
             !s->controller->exporting()) ||
            (stopMode == 3 && activeShutdownProven);
        if (stopMode != 0 && !stopProven) {
            std::fprintf(stderr, "P4-5: 停止の観測が不足:");
            for (const auto& text : observedStatuses)
                std::fprintf(stderr, " [%s]", text.toUtf8().constData());
            std::fprintf(stderr, "\n");
        }
        check(completed && std::filesystem::exists(output) == expectedSuccess && stopProven,
              "P4-5: 実 Manim の Graph を製品 H.264 へ公開: " +
                  s->controller->statusText().toStdString());
        QJsonObject exportResult{{"successExpected", expectedSuccess},
                                 {"completed", completed},
                                 {"outputExists", std::filesystem::exists(output)},
                                 {"stopProven", stopProven},
                                 {"status", s->controller->statusText()}};
        if (stopMode != 0) {
            QJsonArray observed;
            for (const auto& text : observedStatuses)
                observed.append(text);
            exportResult.insert(QStringLiteral("observedStatuses"), observed);
        }
        if (stopMode == 3)
            exportResult.insert(QStringLiteral("activeShutdown"), activeShutdown);
        if (!expectedSuccess) {
            check(!std::filesystem::exists(output.wstring() + L".mvmtmp"),
                  "P4-5: 製品の失敗・取消・終了で一時出力を残さない");
            QFile resultFile(QString::fromStdWString((output.wstring() + L".result.json")));
            if (resultFile.open(QIODevice::WriteOnly))
                resultFile.write(QJsonDocument(exportResult).toJson());
            if (stopMode == 0) {
                pump(100);
                bool closed = false;
                for (auto* failureDialog : s->window->findChildren<QObject*>()) {
                    if (failureDialog->property("title").toString() !=
                            QStringLiteral("書き出しに失敗しました") ||
                        !failureDialog->property("visible").toBool())
                        continue;
                    for (auto* closeButton : failureDialog->findChildren<QQuickItem*>()) {
                        if (closeButton->property("text").toString() != QStringLiteral("OK"))
                            continue;
                        QTest::mouseClick(s->window, Qt::LeftButton, {}, s->center(closeButton));
                        closed =
                            pumpUntil([&] { return !failureDialog->property("visible").toBool(); });
                        break;
                    }
                    break;
                }
                check(closed, "P4-5: 製品の失敗ダイアログの OK を押して修復へ戻る");
            }
            return;
        }
        if (oracle) {
            std::lock_guard lock(s->observation->mutex);
            check(s->observation->exportCompared > 3 && s->observation->exportMismatches == 0 &&
                      s->observation->graphComparedFrames.size() ==
                          s->observation->exportExpected.size(),
                  "P4-5: 実 artifact の全出力 frame を独立 MLT 7.36.1 oracle と完全比較");
            std::fprintf(stderr, "P4-5: 比較 frame=%d staged=%zu 不一致 channel=%d\n",
                         s->observation->exportCompared, s->observation->exportExpected.size(),
                         s->observation->exportMismatches);
            exportResult.insert("comparedCalls", s->observation->exportCompared);
            exportResult.insert("mismatches", s->observation->exportMismatches);
            QJsonArray identities;
            for (const auto frame : s->observation->graphComparedFrames)
                identities.append(static_cast<qint64>(frame));
            exportResult.insert("comparedFrames", identities);
        }
        QFile resultFile(QString::fromStdWString((output.wstring() + L".result.json")));
        if (resultFile.open(QIODevice::WriteOnly))
            resultFile.write(QJsonDocument(exportResult).toJson());
        QProcess decoder;
        decoder.start(
            QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
            {"-v", "error", "-i", QString::fromStdWString(output.wstring()), "-f", "null", "-"});
        check(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
              "P4-5: 製品 H.264 の全 frame を復号");
        QProcess probe;
        probe.start(QStringLiteral("C:/msys64/ucrt64/bin/ffprobe.exe"),
                    {"-v", "error", "-select_streams", "v:0", "-count_frames", "-show_frames",
                     "-show_entries",
                     "stream=codec_name,width,height,r_frame_rate,nb_read_frames,time_base:frame="
                     "best_effort_timestamp",
                     "-of", "json", QString::fromStdWString(output.wstring())});
        check(probe.waitForFinished(30000) && probe.exitCode() == 0,
              "P4-5: 製品出力の frame 数と有理 timestamp を取得する");
        const auto probeBytes = probe.readAllStandardOutput();
        QFile probeEvidence(QString::fromStdWString((output.wstring() + L".probe.json")));
        if (probeEvidence.open(QIODevice::WriteOnly))
            probeEvidence.write(probeBytes);
        const auto info = QJsonDocument::fromJson(probeBytes).object();
        const auto streams = info.value("streams").toArray();
        const auto frames = info.value("frames").toArray();
        check(duration.success && streams.size() == 1 && frames.size() == duration.frame &&
                  streams[0].toObject().value("codec_name").toString() == QStringLiteral("h264") &&
                  streams[0].toObject().value("r_frame_rate").toString() ==
                      QString::number(snapshot.timelineFpsNum) + "/" +
                          QString::number(snapshot.timelineFpsDen),
              "P4-5: 実 MP4 の H.264・FPS・frame 数が一致する");
        if (streams.size() == 1) {
            const auto timeBase = streams[0].toObject().value("time_base").toString().split('/');
            bool timestamps =
                timeBase.size() == 2 && duration.success && frames.size() == duration.frame;
            if (timeBase.size() == 2)
                for (qsizetype frame = 0; frame < frames.size(); ++frame)
                    timestamps =
                        timestamps &&
                        frames[frame].toObject().value("best_effort_timestamp").toInteger(-1) *
                                timeBase[0].toLongLong() * snapshot.timelineFpsNum ==
                            frame * timeBase[1].toLongLong() * snapshot.timelineFpsDen;
            check(timestamps, "P4-5: 全出力 timestamp を整数の有理式で検査する");
        }
    };
    if (exportAcceptance)
        exportProduct(directory / L"graph-reopened.mp4");
    check(input("expression", "sin("), "Graph: 不正原文を UI から保存");
    check(s->controller->saveProject(), "Graph: 不正原文の製品保存");
    const auto invalid = project::loadProjectJson(path);
    check(invalid.success &&
              invalid.project.timelineClips[0].graph.functions[0].expression == "sin(",
          "Graph: 不正原文の再読込");
    s->close();
    s = open(path, invalid.project);
    if (!s)
        return 4;
    check(s->controller->selectClip(0), "Graph: 不正原文の Project を再起動して選ぶ");
    pump(100);
    const auto invalidState = s->project();
    if (exportAcceptance)
        exportProduct(directory / L"visible-invalid.mp4", false);
    check(input("expression", "x^2"), "Graph: 式の修復");
    const auto repaired = s->project();
    const auto firstId = repaired.timelineClips[0].graph.functions[0].id.value;
    check(s->click("graphFunction_" + QString::fromStdString(firstId)),
          "Graph: 確定欄から関数選択へ移る");
    s->key(Qt::Key_Z, Qt::ControlModifier);
    check(s->project() == invalidState, "Graph: 製品 Undo shortcut で原文と ID を戻す");
    s->key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(s->project() == repaired, "Graph: 製品 Redo shortcut で修復と ID を戻す");
    if (exportAcceptance) {
        check(s->controller->saveProject(), "P4-5: 修復した原文を製品保存する");
        s->close();
        const auto reopened = project::loadProjectJson(path);
        check(reopened.success && reopened.project == repaired,
              "P4-5: 修復を schema22 から完全に再読込する");
        s = open(path, reopened.project);
        if (!s)
            return 4;
        check(s->controller->selectClip(0), "P4-5: 修復再読込後の Graph を選ぶ");
        pump(100);
        exportProduct(directory / L"repaired-reopened.mp4");
        const auto status = s->controller->graphPreviewStatus(clipId.toStdString(), 0);
        const auto framePath = directory / L"cache" / L"graph" / L"graph.mvm" /
                               status.key.toStdWString() / L"frame-4.png";
        const auto originalPath = directory / L"original-draw-frame-4.png";
        check(!status.key.isEmpty() && std::filesystem::exists(framePath),
              "P4-5: 現在 key の実 Draw frame を検査する");
        if (!status.key.isEmpty() && std::filesystem::exists(framePath)) {
            std::filesystem::copy_file(framePath, originalPath);
            auto decoded = mvm::graph::readRgba(framePath, 320, 180);
            if (auto* raster = std::get_if<mvm::graph::Raster>(&decoded)) {
                for (std::size_t at = 0; at < raster->rgba.size(); at += 4)
                    if (raster->rgba[at + 3] != 0) {
                        raster->rgba[at] ^= 1;
                        break;
                    }
                check(mvm::graph::writeRgba(framePath, *raster),
                      "P4-5: decoder が読める実 artifact の SHA 破損を作る");
                exportProduct(directory / L"corrupt-artifact.mp4", false);
                std::filesystem::copy_file(originalPath, framePath,
                                           std::filesystem::copy_options::overwrite_existing);
                check(std::filesystem::remove(framePath), "P4-5: 必要な実 Draw frame の欠損を作る");
                exportProduct(directory / L"missing-artifact.mp4", false);
                std::filesystem::copy_file(originalPath, framePath);
            } else
                check(false, "P4-5: 破損対照を作る前の実 PNG が読める");
        }
    }
    for (const auto& size : {QSize(520, 900), QSize(300, 900), QSize(520, 480), QSize(300, 480)}) {
        s->window->setProperty("leftPanelWidth", size.width());
        s->window->resize(1280, size.height());
        pump(100);
        for (const auto& name : {"graphAddFunction", "graphDeleteFunction", "graphMoveFunctionUp",
                                 "graphField_expression", "graphField_color", "graphField_frames"})
            check(s->reach(s->find(QString::fromLatin1(name))),
                  "Graph: 低い/狭い panel の操作到達");
        s->shot("graph-layout-" + std::to_string(size.width()) + "-" +
                std::to_string(size.height()));
    }
    bool seekAccepted = false;
    check(pumpUntil([&] {
              if (!seekAccepted)
                  seekAccepted = s->controller->seekTimelineFrame(1);
              return seekAccepted;
          }),
          "Graph: Draw 内の split 位置へ seek");
    check(s->click("graphFunction_" + QString::fromStdString(firstId)),
          "Graph: split 前の focus を編集欄から外す");
    const auto beforeSplit = s->project();
    s->key(Qt::Key_K, Qt::ControlModifier);
    const auto split = s->project();
    check(split.timelineClips.size() == 2, "Graph: 製品 split shortcut で二つに分割");
    const auto right = std::find_if(split.timelineClips.begin(), split.timelineClips.end(),
                                    [](const auto& clip) { return clip.sourceInFrame == 1; });
    if (right != split.timelineClips.end()) {
        std::string error;
        const auto frame = project::evaluateGraphClip(*right, {60, 1}, 0, error);
        check(frame && frame->sourceFrame == 1 && frame->progressNumerator == 1 &&
                  frame->progressDenominator == drawFrames,
              "Graph: UI split の右側は Draw を最初から始めない");
        check(right->graph.functions[0].id.value != firstId,
              "Graph: UI split の右側で所有 ID を remap");
    } else
        check(false, "Graph: split の右側が存在する");
    s->key(Qt::Key_Z, Qt::ControlModifier);
    check(s->project() == beforeSplit, "Graph: UI split の Undo 完全一致");
    s->key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    check(s->project() == split, "Graph: UI split の Redo 完全一致");
    if (exportAcceptance) {
        exportProduct(directory / L"graph-split.mp4");
        s->window->contentItem()->forceActiveFocus();
        s->key(Qt::Key_Z, Qt::ControlModifier);
        check(s->controller->trimClip(clipId, QStringLiteral("left"), 4, false) &&
                  s->controller->moveTimelineClip(clipId, QStringLiteral("video"), 0, 0, false),
              "P4-5: 製品 trim と移動で元の source frame 4 を先頭にする");
        exportProduct(directory / L"graph-trim.mp4");
        check(s->controller->setTimelineFrameRate(24000, 1001),
              "P4-5: 製品で fractional FPS を確定する");
        exportProduct(directory / L"graph-fractional.mp4");
        check(s->controller->setTimelineFrameRate(60, 1) &&
                  s->controller->moveTimelineClip(clipId, QStringLiteral("video"), 1, 0, false),
              "P4-5: 実映像の上へ Graph を置く");
        const auto backgroundPath = directory / L"background.png";
        QImage background(320, 180, QImage::Format_RGBA8888);
        background.fill(QColor(20, 40, 60));
        check(background.save(QString::fromStdWString(backgroundPath.wstring())),
              "P4-5: 独立した既知 RGB の動画入力を作る");
        const auto video = directory / L"background.mkv";
        const auto audio = directory / L"tone.wav";
        const auto ffmpeg = [&](const QStringList& arguments) {
            QProcess process;
            process.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"), arguments);
            return process.waitForFinished(30000) && process.exitCode() == 0;
        };
        check(ffmpeg({"-v", "error", "-loop", "1", "-framerate", "60", "-i",
                      QString::fromStdWString(backgroundPath.wstring()), "-frames:v", "20", "-c:v",
                      "ffv1", "-pix_fmt", "bgr0", QString::fromStdWString(video.wstring())}) &&
                  ffmpeg({"-v", "error", "-f", "lavfi", "-i",
                          "sine=frequency=440:sample_rate=48000", "-t", "0.333333333333", "-c:a",
                          "pcm_s16le", QString::fromStdWString(audio.wstring())}),
              "P4-5: 実 FFV1 映像と PCM 音声を生成する");
        check(
            s->controller->addMediaFilesToTimelineAt(
                {QUrl::fromLocalFile(QString::fromStdWString(video.wstring()))}, "video", 0, 0) &&
                s->controller->addMediaFilesToTimelineAt(
                    {QUrl::fromLocalFile(QString::fromStdWString(audio.wstring()))}, "audio", 0, 0),
            "P4-5: 製品の素材配置経路で映像と音声を載せる");
        check(s->controller->addSubtitle(QStringLiteral("共存"), 0, 20) &&
                  s->controller->setClipEffectValues(clipId, {{"opacity", 50}}, true),
              "P4-5: 字幕と Graph の通常 ClipEffects を確定する");
        s->observation->graphBackground = {20, 40, 60, 255};
        const auto coexist = directory / L"graph-coexist.mp4";
        exportProduct(coexist);
        check(s->controller->toggleTimelineClipEnabled(clipId),
              "P4-5: 音声対照で Graph を非出力にする");
        int graphIndex = -1;
        for (std::size_t i = 0; i < s->project().timelineClips.size(); ++i)
            if (s->project().timelineClips[i].id == clipId.toStdString())
                graphIndex = static_cast<int>(i);
        if (graphIndex >= 0)
            s->controller->selectClip(graphIndex);
        pump(100);
        check(graphIndex >= 0 &&
                  s->controller->selectedGraphClip().value("clipId").toString() == clipId &&
                  input("expression", "sin("),
              "P4-5: 非出力の Graph に不正原文を残す");
        const auto baseline = directory / L"audio-baseline.mp4";
        exportProduct(baseline, true, false);
        const auto pcmGraph = directory / L"graph-audio.pcm";
        const auto pcmBaseline = directory / L"baseline-audio.pcm";
        check(ffmpeg({"-v", "error", "-i", QString::fromStdWString(coexist.wstring()), "-map",
                      "0:a:0", "-f", "f32le", QString::fromStdWString(pcmGraph.wstring())}) &&
                  ffmpeg({"-v", "error", "-i", QString::fromStdWString(baseline.wstring()), "-map",
                          "0:a:0", "-f", "f32le", QString::fromStdWString(pcmBaseline.wstring())}),
              "P4-5: 共存出力と Graph なし対照の実 AAC を PCM に復号する");
        QFile graphAudio(QString::fromStdWString(pcmGraph.wstring()));
        QFile baselineAudio(QString::fromStdWString(pcmBaseline.wstring()));
        check(graphAudio.open(QIODevice::ReadOnly) && baselineAudio.open(QIODevice::ReadOnly) &&
                  graphAudio.size() > 48000 && graphAudio.readAll() == baselineAudio.readAll(),
              "P4-5: Graph が実音声の PCM を一 byte も変えない");
        check(s->controller->toggleTimelineClipEnabled(clipId), "P4-5: Graph の出力を戻す");
        int enabledIndex = -1;
        const auto enabledProject = s->project();
        for (std::size_t i = 0; i < enabledProject.timelineClips.size(); ++i)
            if (enabledProject.timelineClips[i].id == clipId.toStdString())
                enabledIndex = static_cast<int>(i);
        if (enabledIndex >= 0)
            s->controller->selectClip(enabledIndex);
        check(enabledIndex >= 0 &&
                  s->controller->selectedGraphClip().value("clipId").toString() == clipId,
              "P4-5: 出力へ戻した Graph を選ぶ");
        pump(100);
        QString invalidFunction;
        for (const auto& row : s->controller->selectedGraphClip().value("functions").toList()) {
            const auto function = row.toMap();
            if (function.value("expression").toString() == QStringLiteral("sin("))
                invalidFunction = function.value("id").toString();
        }
        check(!invalidFunction.isEmpty() &&
                  s->click(QStringLiteral("graphFunction_") + invalidFunction),
              "P4-5: 不正原文の関数を選び直す");
        if (auto* expression = s->find(QStringLiteral("graphField_expression"))) {
            QTest::mouseClick(s->window, Qt::LeftButton, {}, s->center(expression));
            s->key(Qt::Key_Escape);
            pump(50);
        }
        check(input("expression", "x^2"), "P4-5: 非可視負例の原文を修復する");
        bool restored = false;
        bool stillInvalid = false;
        for (const auto& row : s->controller->selectedGraphClip().value("functions").toList()) {
            const auto expression = row.toMap().value("expression").toString();
            restored = restored || expression == QStringLiteral("x^2");
            stillInvalid = stillInvalid || expression == QStringLiteral("sin(");
        }
        check(restored && !stillInvalid, "P4-5: 修復した原文が製品の Project に残る");
        if (!restored || stillInvalid) {
            std::fprintf(stderr, "P4-5: 修復後 status=%s\n",
                         s->controller->statusText().toUtf8().constData());
            for (const auto& row : s->controller->selectedGraphClip().value("functions").toList())
                std::fprintf(stderr, "P4-5: 式 [%s]\n",
                             row.toMap().value("expression").toString().toUtf8().constData());
        }
        check(s->controller->saveProject(), "P4-5: 共存した schema22 を製品保存する");
        if (restored && !stillInvalid) {
            exportProduct(directory / L"graph-cancelled.mp4", false, true, 1);
            exportProduct(directory / L"graph-shutdown.mp4", false, true, 2);
            // P4-5 補遺: shutdown 済みの session を閉じ、保存した同じ Project を再起動して、
            // encoder が実際に frame を処理している最中の shutdown を検査する。
            const auto beforeRestart = s->project().timelineClips;
            const auto graphBackground = s->observation->graphBackground;
            s->close();
            const auto reopened = project::loadProjectJson(path);
            check(reopened.success && reopened.project.timelineClips == beforeRestart,
                  "P4-5 補遺: 保存した Project を再起動して開く");
            s = open(path, reopened.project);
            if (!s)
                return 4;
            // 同じ Project の既知 RGB 実動画を背景にした独立 oracle を引き継ぐ。artifact は
            // 製品の書き出しが disk から検証し、最初の encoder frame を全画素で比較する。
            s->observation->graphBackground = graphBackground;
            exportProduct(directory / L"graph-active-shutdown.mp4", false, true, 3);
        }
    }
    s->close();
    QJsonObject summary{
        {"checks", checks}, {"failures", failures}, {"results", results}, {"screenshots", shots}};
    QFile output(QString::fromStdWString((directory / L"results.json").wstring()));
    if (output.open(QIODevice::WriteOnly))
        output.write(QJsonDocument(summary).toJson());
    return failures == 0 && checks > 0 ? 0 : 1;
}

int runEquationSequenceUi(const std::filesystem::path& requestedDirectory, bool scratch) {
    const auto directory =
        scratch
            ? requestedDirectory / QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString()
            : requestedDirectory;
    if (std::filesystem::exists(directory)) {
        std::fprintf(stderr, "証拠の directory が既にあります (上書きしません): %s\n",
                     directory.string().c_str());
        return 2;
    }
    std::filesystem::create_directories(directory);
    evidence = directory;
    int status = recoveryFixtureGuard(directory);
    if (status == 0)
        status = quadraticWorkflow(directory / L"quadratic.mvm");
    if (status == 0 && failures == 0 && qEnvironmentVariable("MVM_P36_REAL_MANIM").isEmpty())
        status = repairAndLayouts(directory);
    if (status == 0 && failures == 0 && qEnvironmentVariable("MVM_P36_REAL_MANIM").isEmpty())
        status = longContent(directory);
    QJsonObject summary{{QStringLiteral("checks"), checks},
                        {QStringLiteral("failures"), failures},
                        {QStringLiteral("protocolInvalid"), status == 4},
                        {QStringLiteral("screenshots"), shots},
                        {QStringLiteral("results"), results}};
    QFile out(QString::fromStdWString((directory / L"results.json").wstring()));
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(summary).toJson());
    std::fprintf(stderr, "%d 検査中 %d 件失敗 (画像 %d 枚: %s)\n", checks, failures,
                 static_cast<int>(shots.size()), directory.string().c_str());
    if (status != 0)
        return status;
    return failures == 0 && checks > 0 ? 0 : 1;
}
