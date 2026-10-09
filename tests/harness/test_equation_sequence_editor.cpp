// P3-5: EquationSequence の authoring controller (EquationSequenceEditor) を実 MvmController と
// 偽の数式 backend で検査する (GPU なし)。
//
// 一操作 = Project の確定一回 = Undo 一回、Undo/Redo が元の操作の ID をそのまま戻すこと、
// 式の編集 session (信頼済み編集 / 全体置換 / 取消 / 拒否 / 対象の削除)、部分式の追加・修復、
// 対応・action、古い選択、読むだけの状態の問い合わせ、編集後の今の key の preview を確かめる。
// 期待値は試験の側で手で書く。

#include "app/equation_sequence_authoring.h"
#include "equation_sequence_editor.h"
#include "math_fake_backend.h"
#include "mvm_controller.h"
#include "project/equation_sequence.h"
#include "project/equation_sequence_edit.h"
#include "project/project_json.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using mvm::app::EquationSequenceEditor;
using mvm::app::MathRasterCache;
using mvm::app::MvmController;
using mvm::test::FakeMathBackend;
namespace project = mvm::project;
namespace app = mvm::app;

bool pump(const std::function<bool()>& done, int milliseconds = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds) {
        QGuiApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

void settle(int milliseconds) {
    pump([] { return false; }, milliseconds);
}

std::unique_ptr<MvmController> makeController(const std::filesystem::path& path,
                                              const project::Project& initial) {
    return std::make_unique<MvmController>(
        path, std::filesystem::path{}, initial, nullptr,
        [](const project::Project&, const app::TimelineExportRequest& request) {
            app::TimelineExportResult result;
            result.success = true;
            result.outputPath = request.outputPath;
            return result;
        },
        MvmController::ExportThreadFactory{},
        MvmController::FileRevealer{[](const std::filesystem::path&, QString&) { return true; }});
}

struct Fixture {
    QTemporaryDir temp;
    FakeMathBackend backend;
    std::filesystem::path path;
    std::unique_ptr<MvmController> controller;

    EquationSequenceEditor& editor() { return controller->equationEditorRef(); }

    const project::Project& project() const { return controller->projectForTest(); }

    const project::TimelineClip* sequence() const {
        for (const auto& clip : project().timelineClips)
            if (clip.kind == project::TimelineClipKind::EquationSequence)
                return &clip;
        return nullptr;
    }

    const project::EquationSequenceClipData& data() const { return sequence()->equationSequence; }
};

std::unique_ptr<Fixture> openFixture(const std::string& name, project::Project initial,
                                     bool backend = true) {
    auto f = std::make_unique<Fixture>();
    check(f->temp.isValid(), name + ": 作業 directory");
    f->path = std::filesystem::path(f->temp.filePath("p35.mvm").toStdWString());
    check(project::saveProjectJson(initial, f->path).success, name + ": Project の保存");
    f->controller = makeController(f->path, initial);
    f->controller->setMathPreflightForTest(backend ? f->backend.preflight()
                                                   : FakeMathBackend::unavailable("fake: 無し"));
    check(pump([&] {
              return f->controller->mathRastersForTest().backendState() !=
                     MathRasterCache::BackendState::Checking;
          }),
          name + ": backend の確認が終わる");
    return f;
}

QVariantMap viewOf(EquationSequenceEditor& editor) {
    return editor.view();
}

QVariantList listOf(EquationSequenceEditor& editor, const char* key) {
    return editor.view().value(QString::fromLatin1(key)).toList();
}

QString selectedStateId(EquationSequenceEditor& editor) {
    return editor.view().value("state").toMap().value("id").toString();
}

int partIndexByLabel(EquationSequenceEditor& editor, const QString& label) {
    const auto parts = listOf(editor, "parts");
    for (int i = 0; i < parts.size(); ++i)
        if (parts[i].toMap().value("label").toString() == label)
            return i;
    return -1;
}

QString partIdByLabel(EquationSequenceEditor& editor, const QString& label) {
    const auto i = partIndexByLabel(editor, label);
    return i < 0 ? QString() : listOf(editor, "parts")[i].toMap().value("id").toString();
}

// 一つの論理操作が Undo 1 回分であり、Undo/Redo が元の操作の結果 (ID を含む) を正確に戻すこと。
void expectOneUndo(Fixture& f, const std::string& name, const std::function<bool()>& operation) {
    const auto before = f.project();
    const auto depth = f.controller->undoDepthForTest();
    check(operation(), name + ": 操作が成功する");
    const auto after = f.project();
    check(f.controller->undoDepthForTest() == depth + 1,
          name + ": Undo は 1 回分 (" + std::to_string(f.controller->undoDepthForTest() - depth) +
              ")");
    check(f.controller->undoLastEdit() && f.project() == before, name + ": Undo で元に戻る");
    check(f.controller->redoLastEdit() && f.project() == after,
          name + ": Redo は元の操作の ID と値を正確に戻す (ID を発行し直さない)");
}

void expectRejected(Fixture& f, const std::string& name, const std::function<bool()>& operation) {
    const auto before = f.project();
    const auto depth = f.controller->undoDepthForTest();
    check(!operation(), name + ": 拒否される");
    check(f.project() == before && f.controller->undoDepthForTest() == depth,
          name + ": 拒否は Project と Undo を変えない");
    check(viewOf(f.editor()).value("messageError").toBool() &&
              !viewOf(f.editor()).value("message").toString().isEmpty(),
          name + ": 拒否の理由を示す");
}

// 文字を document の位置 pos に入れる (編集欄の 1 回の入力)。
void type(QTextDocument& document, int pos, const QString& text) {
    QTextCursor cursor(&document);
    cursor.setPosition(pos);
    cursor.insertText(text);
}

void erase(QTextDocument& document, int begin, int end) {
    QTextCursor cursor(&document);
    cursor.setPosition(begin);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
}

std::unique_ptr<Fixture> created(const std::string& name) {
    auto f = openFixture(name, project::createDefaultProject());
    check(f->controller->createEquationSequenceClip(QStringLiteral("a+bc+d")),
          name + ": sequence を作る");
    return f;
}

void testCreate() {
    auto f = openFixture("作成", project::createDefaultProject());
    const auto depth = f->controller->undoDepthForTest();
    check(f->controller->createEquationSequenceClip(QStringLiteral("ax^2+bx+c=0")),
          "作成: 製品の作成操作");
    const auto* clip = f->sequence();
    check(clip != nullptr, "作成: EquationSequence clip ができる");
    if (!clip)
        return;
    const auto& d = clip->equationSequence;
    std::string error;
    check(d.states.size() == 1 && d.transitions.empty() && d.actions.empty() &&
              d.states[0].parts.empty() && d.states[0].holdFrames >= 1 &&
              d.states[0].equation.backgroundColor == "#00000000" &&
              clip->sourceFpsNum == f->project().timelineFpsNum &&
              clip->sourceFpsDen == f->project().timelineFpsDen &&
              clip->sourceFrameCount == d.states[0].holdFrames &&
              clip->sourceOutFrame == clip->sourceFrameCount &&
              project::validateEquationSequence(d, f->project().outputHeight, error),
          "作成: 1 状態・透明背景・Project の FPS・辺/部分式/action なしで有効");
    check(f->controller->undoDepthForTest() == depth + 1, "作成: Undo は 1 回分");
    check(f->controller->currentClipIndex() >= 0 &&
              viewOf(f->editor()).value("clipId").toString() == QString::fromStdString(clip->id) &&
              viewOf(f->editor()).value("stateCount").toInt() == 1,
          "作成: 作った clip を選び editor が表示する");
    const auto after = f->project();
    check(f->controller->undoLastEdit() && !f->sequence() && viewOf(f->editor()).isEmpty(),
          "作成: Undo で clip が消え、editor は空になる");
    check(f->controller->redoLastEdit() && f->project() == after,
          "作成: Redo は同じ clip ID と内部 ID を戻す");
    // timeline のトランジションは作れない (P3-4 の制限を UI からも破らない)。
    f->controller->shutdown();
}

void testStates() {
    auto f = created("状態");
    auto& editor = f->editor();
    // 挿入 (後ろ・前)。新しい辺は対応なし。新しい状態を選ぶ。
    expectOneUndo(*f, "状態の後ろへ挿入", [&] { return editor.insertState(true); });
    check(f->data().states.size() == 2 && f->data().transitions.size() == 1 &&
              f->data().transitions[0].correspondence.empty() &&
              selectedStateId(editor) == QString::fromStdString(f->data().states[1].id.value),
          "挿入: 2 状態・新しい辺・新しい状態を選ぶ");
    expectOneUndo(*f, "状態の前へ挿入", [&] { return editor.insertState(false); });
    check(f->data().states.size() == 3 &&
              selectedStateId(editor) == QString::fromStdString(f->data().states[1].id.value),
          "前へ挿入: 選んだ位置の前に入る");

    // 部分式・対応・action を持つ中央の状態を消す: action と隣接の辺 (対応ごと) が一緒に消え、
    // 前後は対応なしの新しい辺でつながる。
    auto& d0 = f->data();
    const auto first = d0.states[0].id, middle = d0.states[1].id, last = d0.states[2].id;
    check(f->controller->editEquationSequenceData(
              f->sequence()->id,
              [&](project::EquationSequenceClipData& d, std::string& error) {
                  for (auto* s : {&d.states[0], &d.states[1], &d.states[2]})
                      s->parts.push_back({{"part-" + s->id.value},
                                          "b",
                                          {s->revision, 2, 3, "b", project::BindingStatus::Bound}});
                  d.transitions[0].correspondence = {
                      {{"part-" + first.value}, {"part-" + middle.value}}};
                  d.transitions[1].correspondence = {
                      {{"part-" + middle.value}, {"part-" + last.value}}};
                  d.actions.push_back({{"owned"},
                                       middle,
                                       {"part-" + middle.value},
                                       project::EquationTargetStatus::Present,
                                       0,
                                       3,
                                       project::EquationOperation::Outline});
                  return project::validateEquationSequence(d, 1080, error);
              }),
          "状態: 部分式・対応・action の準備");
    editor.selectState(QString::fromStdString(middle.value));
    const auto confirm = viewOf(editor).value("state").toMap();
    check(confirm.value("deleteActions").toInt() == 1 &&
              confirm.value("deleteTransitions").toInt() == 2 &&
              confirm.value("deleteCorrespondence").toInt() == 2 &&
              confirm.value("deleteJoinsNeighbors").toBool(),
          "状態の削除の確認: action 1・変形 2・対応 2・前後を新しい辺でつなぐ");
    expectOneUndo(*f, "中央の状態の削除", [&] { return editor.deleteSelectedState(); });
    const auto& d = f->data();
    check(d.states.size() == 2 && d.actions.empty() && d.transitions.size() == 1 &&
              d.transitions[0].from == first && d.transitions[0].to == last &&
              d.transitions[0].correspondence.empty() && d.transitions[0].id.value != "t0",
          "状態の削除: 所有 action を残さず、前後は対応なしの新しい辺");
    check(selectedStateId(editor) == QString::fromStdString(last.value),
          "状態の削除: 選択は近い生存 (同じ位置) へ移る");
    // Undo で消えた状態が戻れば、その ID は再び選べる (元の ID)。
    check(f->controller->undoLastEdit(), "状態の削除の Undo");
    editor.selectState(QString::fromStdString(middle.value));
    check(selectedStateId(editor) == QString::fromStdString(middle.value),
          "Undo で戻った元の StateId を選べる");
    check(f->controller->redoLastEdit(), "状態の削除の Redo");
    check(selectedStateId(editor) == QString::fromStdString(last.value),
          "Redo で選択中の状態が消えると近い生存へ移る (古い index を使わない)");

    // 並べ替え: (first,last) を逆順にすると旧い辺の組は隣接しない。
    check(f->controller->editEquationSequenceData(
              f->sequence()->id,
              [&](project::EquationSequenceClipData& data, std::string& error) {
                  data.transitions[0].correspondence = {
                      {{"part-" + first.value}, {"part-" + last.value}}};
                  return project::validateEquationSequence(data, 1080, error);
              }),
          "並べ替え: 対応の準備");
    editor.selectState(QString::fromStdString(last.value));
    expectOneUndo(*f, "状態を上へ移動", [&] { return editor.moveSelectedState(-1); });
    check(f->data().states[0].id == last && f->data().states[1].id == first &&
              f->data().transitions[0].from == last &&
              f->data().transitions[0].correspondence.empty(),
          "並べ替え: 隣接しなくなった組の対応を残さない");
    check(selectedStateId(editor) == QString::fromStdString(last.value),
          "並べ替え: 動かした状態を選んだまま");
    expectRejected(*f, "先頭からさらに上へ", [&] { return editor.moveSelectedState(-1); });

    // hold / 変形の長さ。
    expectOneUndo(*f, "hold の変更", [&] { return editor.setHoldFrames(45); });
    check(f->data().states[0].holdFrames == 45 &&
              f->sequence()->sourceFrameCount == f->sequence()->sourceOutFrame,
          "hold: 全体の末尾が新しい長さへ追従");
    expectRejected(*f, "hold 0", [&] { return editor.setHoldFrames(0); });
    expectRejected(*f, "hold 上限超え", [&] { return editor.setHoldFrames(4'000'000); });
    const auto transitionId = QString::fromStdString(f->data().transitions[0].id.value);
    expectOneUndo(*f, "変形の長さの変更",
                  [&] { return editor.setTransitionFrames(transitionId, 12); });
    expectRejected(*f, "変形 0", [&] { return editor.setTransitionFrames(transitionId, 0); });
    // 同じ値は Project を変えない (Undo を積まない)。
    const auto depth = f->controller->undoDepthForTest();
    check(editor.setHoldFrames(45) && editor.setTransitionFrames(transitionId, 12) &&
              f->controller->undoDepthForTest() == depth,
          "同じ値の確定は Undo を積まない");

    // 最後の 1 状態は消せない。
    editor.selectState(QString::fromStdString(f->data().states[1].id.value));
    expectOneUndo(*f, "2 状態から 1 状態へ", [&] { return editor.deleteSelectedState(); });
    check(viewOf(editor).value("state").toMap().value("canDelete").toBool() == false &&
              listOf(editor, "transitions").isEmpty(),
          "1 状態: 削除できず変形の一覧は空");
    expectRejected(*f, "最後の状態の削除", [&] { return editor.deleteSelectedState(); });
    f->controller->shutdown();
}

void testRightTrimRejectsShortHold() {
    auto f = created("右 trim");
    auto& editor = f->editor();
    // 外側の可視範囲を 30 frame にする (sourceOut < sourceFrameCount)。
    auto initial = f->project();
    for (auto& clip : initial.timelineClips)
        if (clip.kind == project::TimelineClipKind::EquationSequence)
            clip.sourceOutFrame = 30;
    f->controller->shutdown();
    auto g = openFixture("右 trim の Project", initial);
    g->controller->selectClip(0);
    check(!viewOf(g->editor()).isEmpty(), "右 trim: editor が clip を表示");
    expectRejected(*g, "右 trim の外へ短くする hold",
                   [&] { return g->editor().setHoldFrames(20); });
    expectOneUndo(*g, "右 trim の内側の hold", [&] { return g->editor().setHoldFrames(40); });
    (void)editor;
    g->controller->shutdown();
}

void testSourceEditing() {
    auto f = created("式の編集");
    auto& editor = f->editor();
    // 部分式 "bc" (UTF-16 [2,4)) を選択から作る。
    const auto source = QStringLiteral("a+bc+d");
    expectOneUndo(*f, "部分式の追加",
                  [&] { return editor.addPart(2, 4, source, QStringLiteral("bc")); });
    const auto partId = partIdByLabel(editor, "bc");
    check(!partId.isEmpty() && viewOf(editor).value("selectedPart").toMap().value("id") == partId,
          "部分式の追加: 追加した部分式を選ぶ");

    QTextDocument document;
    document.setPlainText(source);
    // 部分式より前へ 1 文字ずつ 5 回入力 → 確定は Project の操作 1 回、範囲は移る。
    const auto depth = f->controller->undoDepthForTest();
    editor.beginSourceEdit(&document);
    for (int i = 0; i < 5; ++i)
        type(document, 0, QStringLiteral("x"));
    check(f->controller->undoDepthForTest() == depth &&
              f->data().states[0].equation.source == "a+bc+d",
          "入力中は Project を変えない");
    auto result = editor.commitSourceEdit(document.toPlainText());
    const auto& part = f->data().states[0].parts[0];
    check(result.value("ok").toBool() && !result.value("fullReplacement").toBool() &&
              f->controller->undoDepthForTest() == depth + 1 &&
              f->data().states[0].equation.source == "xxxxxa+bc+d" &&
              part.binding.status == project::BindingStatus::Bound && part.binding.begin == 7 &&
              part.id.value == partId.toStdString(),
          "前への入力 5 回は確定 1 回 (Undo 1 回) で、範囲は信頼済み編集で移る");
    // 部分式の両側の編集を一つの session で: 無傷。
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("("));
    type(document, static_cast<int>(document.toPlainText().size()), QStringLiteral(")"));
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("ok").toBool() &&
              f->data().states[0].parts[0].binding.status == project::BindingStatus::Bound,
          "部分式の両側の編集は部分式を無効にしない");
    // 部分式の中の編集 → Invalid、確定の結果に名前が出る。
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    const auto inside = static_cast<int>(f->data().states[0].parts[0].binding.begin) + 1;
    type(document, inside, QStringLiteral("Q"));
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("ok").toBool() &&
              f->data().states[0].parts[0].binding.status == project::BindingStatus::Invalid &&
              result.value("invalidated").toStringList().size() == 1,
          "部分式の中の編集は Invalid にし、壊れた部分式を知らせる");
    // Esc (取消) は Project を変えない。
    const auto beforeCancel = f->project();
    const auto cancelDepth = f->controller->undoDepthForTest();
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("zzz"));
    editor.cancelSourceEdit();
    check(f->project() == beforeCancel && f->controller->undoDepthForTest() == cancelDepth &&
              !editor.sourceEditActive(),
          "取消は Project と Undo を変えない");
    // 打って消した入力 (文字が同じ) は確定しても Project を変えない。
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("k"));
    erase(document, 0, 1);
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("ok").toBool() && !result.value("changed").toBool() &&
              f->project() == beforeCancel,
          "文字が変わらない確定は Project を変えない");
    // 拒否 (空の式) は Project を変えず、session を保つ (修正して確定できる)。
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    erase(document, 0, static_cast<int>(document.toPlainText().size()));
    result = editor.commitSourceEdit(document.toPlainText());
    check(!result.value("ok").toBool() && !result.value("discarded").toBool() &&
              f->project() == beforeCancel && editor.sourceEditActive(),
          "拒否された確定は Project を変えず入力を保持する");
    type(document, 0, QStringLiteral("y=1"));
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("ok").toBool() && f->data().states[0].equation.source == "y=1",
          "拒否の後に修正して確定できる");

    // 記録の無い (編集欄の外の) 変更は全体置換: 全 binding を無効にし、同じ文字でも付け直さない。
    f->controller->undoLastEdit(); // y=1 を戻す
    expectOneUndo(*f, "Invalid の部分式の修復", [&] {
        const auto now = QString::fromStdString(f->data().states[0].equation.source);
        const auto at = static_cast<int>(now.indexOf(QStringLiteral("bQc")));
        return editor.rebindSelectedPart(at, at + 3, now);
    });
    check(f->data().states[0].parts[0].id.value == partId.toStdString() &&
              f->data().states[0].parts[0].binding.status == project::BindingStatus::Bound,
          "修復は同じ PartId のまま Bound に戻す");
    const auto bound = f->data().states[0].equation.source;
    editor.beginSourceEdit(nullptr);
    result = editor.commitSourceEdit(QString::fromStdString(bound) + QStringLiteral(" "));
    check(result.value("ok").toBool() && result.value("fullReplacement").toBool() &&
              f->data().states[0].parts[0].binding.status == project::BindingStatus::Invalid,
          "記録の無い変更は全体置換で全 binding を無効にする");
    // document の文字が式と違う状態で始めた記録も信頼しない (古い入力の上の位置)。
    f->controller->undoLastEdit();
    QTextDocument stale;
    stale.setPlainText(QStringLiteral("other text"));
    editor.beginSourceEdit(&stale);
    type(stale, 0, QStringLiteral("q"));
    result = editor.commitSourceEdit(stale.toPlainText());
    check(result.value("ok").toBool() && result.value("fullReplacement").toBool(),
          "式と違う文字から始めた記録は全体置換にする");
    f->controller->undoLastEdit();

    // 編集中に別の状態を選んでも、確定は session を始めた状態へ書く。
    check(editor.insertState(true), "対象: 2 つ目の状態");
    editor.selectState(QString::fromStdString(f->data().states[0].id.value));
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("A"));
    editor.selectState(QString::fromStdString(f->data().states[1].id.value));
    const auto secondSource = f->data().states[1].equation.source;
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("ok").toBool() && f->data().states[0].equation.source.front() == 'A' &&
              f->data().states[1].equation.source == secondSource,
          "選択を変えた後の確定は元の状態へ書く (別の状態を変えない)");
    // 編集中に対象の状態が消えたら、確定は入力を破棄する (Project はその削除だけ)。
    editor.selectState(QString::fromStdString(f->data().states[1].id.value));
    document.setPlainText(QString::fromStdString(f->data().states[1].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("B"));
    check(editor.deleteSelectedState(), "対象: 編集中の状態を消す");
    const auto afterDelete = f->project();
    result = editor.commitSourceEdit(document.toPlainText());
    check(!result.value("ok").toBool() && result.value("discarded").toBool() &&
              f->project() == afterDelete && !editor.sourceEditActive(),
          "対象が消えた編集は破棄し Project を変えない");
    // clip ごと消えても同じ。
    document.setPlainText(QString::fromStdString(f->data().states[0].equation.source));
    editor.beginSourceEdit(&document);
    type(document, 0, QStringLiteral("C"));
    check(f->controller->deleteTimelineClip(QString::fromStdString(f->sequence()->id)),
          "対象: clip を消す");
    const auto afterClip = f->project();
    result = editor.commitSourceEdit(document.toPlainText());
    check(result.value("discarded").toBool() && f->project() == afterClip &&
              viewOf(editor).isEmpty(),
          "clip が消えた編集は破棄し、editor は空になる");
    f->controller->shutdown();
}

void testPartsAndRepair() {
    auto f = openFixture("部分式", project::createDefaultProject());
    auto& editor = f->editor();
    // α (UTF-16 1 単位 = UTF-8 2 byte) の後ろの b を選ぶ。byte offset は controller が変換する。
    const auto source = QString::fromUtf8("\xCE\xB1+b+\\frac{c}{d}");
    check(f->controller->createEquationSequenceClip(source), "部分式: 作成");
    expectOneUndo(*f, "非 ASCII の後ろの部分式",
                  [&] { return editor.addPart(2, 3, source, QStringLiteral("b")); });
    const auto& b = f->data().states[0].parts[0].binding;
    check(b.begin == 3 && b.end == 4 && b.expectedText == "b",
          "UTF-16 の選択 [2,3) を UTF-8 の [3,4) へ変換する");
    check(viewOf(editor).value("selectedPart").toMap().value("rangeStart").toInt() == 2,
          "表示の範囲は UTF-16 (編集欄の位置) で返す");
    expectRejected(*f, "空の選択", [&] { return editor.addPart(3, 3, source, QString()); });
    expectRejected(*f, "重なる範囲", [&] { return editor.addPart(1, 3, source, QString()); });
    expectRejected(*f, "TeX の構造の途中",
                   [&] { return editor.addPart(4, 11, source, QString()); });
    expectRejected(*f, "空白・括弧だけ", [&] { return editor.addPart(9, 10, source, QString()); });
    expectRejected(*f, "未確定の入力がある",
                   [&] { return editor.addPart(0, 1, source + QStringLiteral("x"), QString()); });
    expectRejected(*f, "終端の外", [&] { return editor.addPart(0, 99, source, QString()); });
    const auto denominator = static_cast<int>(source.indexOf(QStringLiteral("d")));
    expectOneUndo(*f, "分母の部分式", [&] {
        return editor.addPart(denominator, denominator + 1, source, QStringLiteral("分母"));
    });
    // サロゲートペアの内側は区切れない。
    const auto emoji = QString::fromUtf8("x+\xF0\x9F\x98\x80");
    check(editor.insertState(true), "部分式: 2 つ目の状態");
    QTextDocument document;
    document.setPlainText(source);
    editor.beginSourceEdit(&document);
    document.setPlainText(emoji); // 全体の差し替え (記録と一致すれば信頼済み、違えば全体置換)
    check(editor.commitSourceEdit(emoji).value("ok").toBool(), "部分式: 絵文字の式");
    expectRejected(*f, "サロゲートペアの内側",
                   [&] { return editor.addPart(2, 3, emoji, QString()); });

    // 改名・削除・欠落の修復 (状態 0)。
    editor.selectState(QString::fromStdString(f->data().states[0].id.value));
    const auto bId = partIdByLabel(editor, "b");
    editor.selectPart(bId);
    expectOneUndo(*f, "部分式の改名",
                  [&] { return editor.renameSelectedPart(QStringLiteral("項 b")); });
    check(f->data().states[0].parts[0].label == "項 b" &&
              f->data().states[0].parts[0].id.value == bId.toStdString(),
          "改名は ID を変えない");
    // action を付けてから部分式を消す → action は「見つからない部分式」を参照する。
    expectOneUndo(*f, "action の追加",
                  [&] { return editor.addAction(bId, QStringLiteral("pulse"), 0, 10); });
    editor.selectPart(bId);
    expectOneUndo(*f, "部分式の削除", [&] { return editor.deleteSelectedPart(); });
    const auto& action = f->data().actions[0];
    check(action.targetStatus == project::EquationTargetStatus::Missing &&
              action.target.value == bId.toStdString() &&
              std::none_of(f->data().states[0].parts.begin(), f->data().states[0].parts.end(),
                           [&](const auto& p) { return p.id.value == bId.toStdString(); }),
          "部分式の削除: present のまま実在しない ID を残さず、action は missing");
    const auto selected = viewOf(editor).value("selectedPart").toMap();
    check(selected.value("id").toString() == bId && selected.value("status") == "missing_target" &&
              selected.value("exists").toBool() == false,
          "削除後も action の参照する欠落 ID を選び、修復できる状態として示す");
    expectRejected(*f, "欠落した部分式の削除", [&] { return editor.deleteSelectedPart(); });
    expectOneUndo(*f, "欠落した部分式を同じ ID で作り直す",
                  [&] { return editor.rebindSelectedPart(2, 3, source); });
    check(f->data().actions[0].targetStatus == project::EquationTargetStatus::Present &&
              f->data().actions[0].target.value == bId.toStdString(),
          "作り直しで action が有効に戻る (同じ PartId)");
    check(viewOf(editor).value("message").toString().contains(
              QStringLiteral("対応は自動では戻りません")),
          "修復の後、対応は戻っていないことを示す");
    // 消えた選択: 存在しない ID の選択は無視する。
    const auto before = viewOf(editor);
    editor.selectPart(QStringLiteral("no-such-part"));
    editor.selectState(QStringLiteral("no-such-state"));
    editor.selectAction(QStringLiteral("no-such-action"));
    check(viewOf(editor) == before, "古い・存在しない ID の選択は無視する");
    f->controller->shutdown();
}

void testCorrespondenceAndActions() {
    auto f = created("対応と action");
    auto& editor = f->editor();
    const auto source = QStringLiteral("a+bc+d");
    check(editor.addPart(2, 4, source, QStringLiteral("前")), "対応: 前の部分式");
    check(editor.insertState(true), "対応: 後の状態");
    check(editor.addPart(2, 4, source, QStringLiteral("後")), "対応: 後の部分式");
    const auto transition = listOf(editor, "transitions")[0].toMap();
    const auto from = transition.value("fromCandidates").toList()[0].toMap().value("id").toString();
    const auto to = transition.value("toCandidates").toList()[0].toMap().value("id").toString();
    const auto tid = transition.value("id").toString();
    expectOneUndo(*f, "対応の追加", [&] { return editor.addCorrespondence(tid, from, to); });
    const auto updated = listOf(editor, "transitions")[0].toMap();
    check(updated.value("correspondence").toList().size() == 1 &&
              updated.value("fromCandidates").toList().isEmpty() &&
              updated.value("toCandidates").toList().isEmpty(),
          "対応の追加: 使った部分式は候補から外れる (重複を確定前に防ぐ)");
    expectRejected(*f, "同じ対応の重複", [&] { return editor.addCorrespondence(tid, from, to); });
    expectRejected(*f, "片側が空", [&] { return editor.addCorrespondence(tid, from, QString()); });
    expectOneUndo(*f, "対応の削除", [&] { return editor.removeCorrespondence(tid, from, to); });

    // action: hold は既定 1 秒 (60f)。
    editor.selectState(QString::fromStdString(f->data().states[0].id.value));
    const auto hold = f->data().states[0].holdFrames;
    expectOneUndo(*f, "outline の追加",
                  [&] { return editor.addAction(from, QStringLiteral("outline"), 0, 20); });
    const auto outlineId = viewOf(editor).value("selectedAction").toMap().value("id").toString();
    expectRejected(*f, "hold を超える区間",
                   [&] { return editor.addAction(from, QStringLiteral("pulse"), hold - 5, 10); });
    expectRejected(*f, "同時の action",
                   [&] { return editor.addAction(from, QStringLiteral("pulse"), 10, 20); });
    expectRejected(*f, "未知の operation (set_color)",
                   [&] { return editor.addAction(from, QStringLiteral("set_color"), 30, 5); });
    expectRejected(*f, "別の状態の部分式",
                   [&] { return editor.addAction(to, QStringLiteral("pulse"), 30, 5); });
    expectOneUndo(*f, "pulse の追加",
                  [&] { return editor.addAction(from, QStringLiteral("pulse"), 30, 10); });
    editor.selectAction(outlineId);
    const auto playhead = f->controller->playheadFrame();
    expectOneUndo(*f, "action の変更",
                  [&] { return editor.updateSelectedAction(from, QStringLiteral("pulse"), 2, 8); });
    check(f->controller->playheadFrame() == playhead,
          "action の変更で再生位置を動かさない (利用者の操作だけが seek する)");
    expectRejected(*f, "変更で hold を超える", [&] {
        return editor.updateSelectedAction(from, QStringLiteral("pulse"), 2, hold);
    });
    // 明示の seek: action の先頭の timeline frame (60fps・clip 先頭 = 再生位置 0、状態 0 の hold
    // 先頭)。 (preview engine の無い試験では seek の戻り値は失敗だが、再生位置は動く)
    const auto start = f->sequence()->timelineStartFrame;
    editor.seekToSelectedAction();
    check(f->controller->playheadFrame() == start + 2,
          "「再生位置を移動」で action の先頭へ seek する");
    expectOneUndo(*f, "action の削除", [&] { return editor.deleteSelectedAction(); });
    check(viewOf(editor).value("selectedAction").toMap().isEmpty(),
          "削除した action の選択は外れる");
    f->controller->shutdown();
}

// 読むだけの状態の問い合わせ: 再生位置の外の sequence を何度問い合わせても描画を始めない。
void testReadOnlyStatus() {
    auto initial = project::createDefaultProject();
    auto data = app::newEquationSequenceData("x+y", 60, [] {
        static int n = 0;
        return "far-" + std::to_string(n++);
    });
    check(project::addEquationSequence(initial, data, "far", "外", {project::TrackKind::Video, 0},
                                       5000)
              .success,
          "問い合わせ: 再生位置の外の sequence");
    auto f = openFixture("問い合わせ", initial);
    auto& editor = f->editor();
    check(f->controller->selectClip(0), "問い合わせ: 外の clip を選ぶ");
    settle(200);
    auto& cache = f->controller->mathRastersForTest();
    const auto records = cache.recordCount();
    const auto sequences = cache.equationSequenceRecordCount();
    const auto renders = f->backend.renders->load();
    const auto equationRenders = f->backend.equationRenders->load();
    const auto before = editor.statusRefreshCountForTest();
    for (int i = 0; i < 50; ++i)
        editor.refreshStatus();
    settle(300);
    check(editor.statusRefreshCountForTest() == before + 50, "問い合わせ: 50 回問い合わせた");
    check(cache.recordCount() == records && cache.equationSequenceRecordCount() == sequences &&
              f->backend.renders->load() == renders &&
              f->backend.equationRenders->load() == equationRenders,
          "問い合わせ: 静止・sequence の描画を要求せず cache の record も作らない (静止 " +
              std::to_string(cache.recordCount()) + "/" + std::to_string(records) + ")");
    const auto status = editor.status();
    check(status.value("preview") == "outside" && status.value("artifact") == "pending",
          "問い合わせ: 再生位置の外と未要求を示す");
    f->controller->shutdown();
}

// 状態の表示の型: 内容の修復 / 構造 / 環境 / 描画結果 / memory を分ける。
void testStatusClassification() {
    {
        auto f = openFixture("環境", project::createDefaultProject(), false);
        check(f->controller->createEquationSequenceClip(QStringLiteral("x")), "環境: 作成");
        f->editor().refreshStatus();
        const auto status = f->editor().status();
        check(status.value("renderer") == "unavailable" &&
                  status.value("category") == "environment" && status.value("compile") == "ready",
              "backend 不在は環境の問題 (内容の修復ではない)");
        f->controller->shutdown();
    }
    {
        auto f = created("内容");
        auto& editor = f->editor();
        check(editor.addPart(2, 4, QStringLiteral("a+bc+d"), QString()), "内容: 部分式");
        check(f->controller->editEquationSequenceData(
                  f->sequence()->id,
                  [](project::EquationSequenceClipData& d, std::string& error) {
                      return project::replaceEquationSource(d, d.states[0].id, "a+bc+d", "r2", 1080,
                                                            error);
                  }),
              "内容: 全体置換で Invalid");
        QString error;
        pump([&] {
            f->controller->subtitleCompositionForTest(0, error);
            editor.refreshStatus();
            return editor.status().value("preview") == "current_state_only";
        });
        auto status = editor.status();
        check(status.value("compile") == "failed" && status.value("category") == "content" &&
                  status.value("compileRepairable").toBool() &&
                  status.value("preview") == "current_state_only",
              "Invalid binding は内容の修復、preview は状態の静止だけ");
        // 同じ文字列のまま付け直さない (全体置換の後も Invalid)。
        check(f->data().states[0].parts[0].binding.status == project::BindingStatus::Invalid,
              "全体置換は同じ文字でも付け直さない");
        f->controller->shutdown();
    }
    {
        // backend の構造検証の失敗 (偽の backend の EQFAIL)。
        auto f = openFixture("描画の検証", project::createDefaultProject());
        check(f->controller->createEquationSequenceClip(QStringLiteral("EQFAIL x")), "検証: 作成");
        QString error;
        const auto ready = pump([&] {
            f->controller->subtitleCompositionForTest(0, error);
            f->editor().refreshStatus();
            return f->editor().status().value("artifact") == "failed";
        });
        const auto status = f->editor().status();
        check(ready && status.value("category") == "content" &&
                  status.value("artifactFailure") == "empty_action_target",
              "backend の検証の失敗は型付きで、内容の問題として示す");
        f->controller->shutdown();
    }
}

// 編集後は今の key の preview: 前の key の artifact を出さず、描き終わるまでは静止で代用する。
// 再生位置は動かさない。
void testCurrentKeyAfterEdit() {
    auto f = created("今の key");
    auto& editor = f->editor();
    check(editor.addPart(2, 4, QStringLiteral("a+bc+d"), QString()), "今の key: 部分式");
    const auto part = viewOf(editor).value("selectedPart").toMap().value("id").toString();
    check(editor.addAction(part, QStringLiteral("outline"), 10, 20), "今の key: action");
    QString error;
    const auto ready = [&] {
        f->controller->subtitleCompositionForTest(f->controller->playheadFrame(), error);
        return f->controller
                   ->equationSequencePreviewStatus(QString::fromStdString(f->sequence()->id),
                                                   f->controller->playheadFrame())
                   .disk == MathRasterCache::State::Ready;
    };
    f->controller->seekTimelineFrame(15); // engine の無い試験でも再生位置は動く
    check(f->controller->playheadFrame() == 15 && pump(ready),
          "今の key: 最初の artifact が Ready");
    const auto clipId = QString::fromStdString(f->sequence()->id);
    const auto oldKey = f->controller->equationSequencePreviewStatus(clipId, 15).sequenceKey;
    f->backend.equationGate->store(true); // 次の描画を止めて、描画中の提示を確かめる
    editor.selectAction(viewOf(editor).value("selectedAction").toMap().value("id").toString());
    check(editor.updateSelectedAction(part, QStringLiteral("pulse"), 10, 20),
          "今の key: pulse へ変更");
    check(f->controller->playheadFrame() == 15, "今の key: 編集で再生位置を動かさない");
    f->controller->subtitleCompositionForTest(15, error);
    settle(100);
    auto status = f->controller->equationSequencePreviewStatus(clipId, 15);
    check(status.sequenceKey != oldKey && status.disk == MathRasterCache::State::Pending &&
              status.shown.kind == app::EquationPreviewShownKind::Static,
          "今の key: 新しい key を描いている間は静止で代用し、前の key の強調を出さない");
    f->backend.equationGate->store(false);
    check(pump([&] {
              f->controller->subtitleCompositionForTest(15, error);
              const auto now = f->controller->equationSequencePreviewStatus(clipId, 15);
              return now.disk == MathRasterCache::State::Ready &&
                     now.shown.kind == app::EquationPreviewShownKind::Action;
          }),
          "今の key: 描き終わると今の frame の強調が出る");
    f->controller->shutdown();
}

// timeline のトランジションは EquationSequence に置けない (Blend でも数式の変形でも)。
void testTimelineTransitionRejected() {
    auto initial = project::createDefaultProject();
    auto ids = [] {
        static int n = 0;
        return "tt-" + std::to_string(n++);
    };
    check(project::addEquationSequence(initial, app::newEquationSequenceData("x", 120, ids), "eq",
                                       "数式 sequence", {project::TrackKind::Video, 0}, 0)
              .success,
          "トランジション: 1 本目");
    check(project::addEquationSequence(initial, app::newEquationSequenceData("y", 120, ids), "eq2",
                                       "数式 sequence 2", {project::TrackKind::Video, 0}, 120)
              .success,
          "トランジション: 2 本目");
    // 左の末尾を trim して余白を作る (Blend が素材の余白を使えるように)。
    initial.timelineClips[0].sourceOutFrame = 100;
    initial.timelineClips[1].timelineStartFrame = 100;
    initial.timelineClips[1].sourceInFrame = 20;
    std::string error;
    auto f = openFixture("トランジション", initial);
    const auto depth = f->controller->undoDepthForTest();
    check(f->controller->selectEditPoint(QStringLiteral("eq"), QStringLiteral("right")),
          "トランジション: 編集点を選ぶ");
    const auto before = f->project();
    check(!f->controller->applyDefaultTransition() && f->project() == before &&
              f->controller->undoDepthForTest() == depth &&
              f->controller->statusText().contains(
                  QStringLiteral("数式 sequence には timeline のトランジションを置けません")),
          "トランジション: Blend を置かず理由を示す");
    f->controller->shutdown();
}

// 保存・再読込で状態の順・尺・ID・binding・対応・action が同じ。
void testSaveReopen() {
    auto f = created("保存");
    auto& editor = f->editor();
    check(editor.addPart(2, 4, QStringLiteral("a+bc+d"), QStringLiteral("p")), "保存: 部分式");
    check(editor.insertState(true) &&
              editor.addPart(2, 4, QStringLiteral("a+bc+d"), QStringLiteral("q")),
          "保存: 2 つ目");
    const auto t = listOf(editor, "transitions")[0].toMap();
    check(editor.addCorrespondence(
              t.value("id").toString(),
              t.value("fromCandidates").toList()[0].toMap().value("id").toString(),
              t.value("toCandidates").toList()[0].toMap().value("id").toString()),
          "保存: 対応");
    check(editor.addAction(partIdByLabel(editor, "q"), QStringLiteral("outline"), 5, 10),
          "保存: action");
    check(f->controller->saveProject(), "保存: 製品の保存");
    const auto saved = f->project();
    const auto loaded = project::loadProjectJson(f->path);
    check(loaded.success && loaded.project.schemaVersion == project::kProjectSchemaVersion &&
              loaded.project.timelineClips == saved.timelineClips,
          "保存: 再読込で clip (状態の順・尺・ID・binding・対応・action) が同じ: " + loaded.error);
    f->controller->shutdown();
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    testCreate();
    testStates();
    testRightTrimRejectsShortHold();
    testSourceEditing();
    testPartsAndRepair();
    testCorrespondenceAndActions();
    testReadOnlyStatus();
    testStatusClassification();
    testCurrentKeyAfterEdit();
    testTimelineTransitionRejected();
    testSaveReopen();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
