// 製品の QML 編集入口の構造拒否・原文保存・所有 ID と履歴を独立の期待値で検査する。
#include "mvm_controller.h"
#include "project/graph_edit.h"
#include "project/project_json.h"

#include <algorithm>
#include <cstdio>

#include <QGuiApplication>
#include <QQuickStyle>
#include <QTemporaryDir>

namespace {
int checks = 0, failures = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message);
    }
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickStyle::setStyle("Basic");
    QGuiApplication application(argc, argv);
    QTemporaryDir directory;
    const auto path = std::filesystem::path(directory.filePath("graph.mvm").toStdWString());
    auto initial = mvm::project::createDefaultProject();
    check(mvm::project::saveProjectJson(initial, path).success, "初期 Project の保存");
    mvm::app::MvmController controller(path, {}, initial, nullptr);
    check(controller.createGraphClipFromUi(), "製品の作成入口");
    auto view = controller.selectedGraphClip();
    check(!view.isEmpty(), "作成後に Graph を選択");
    if (view.isEmpty()) {
        controller.shutdown();
        return 1;
    }
    auto id = view.value("functions").toList().front().toMap().value("id").toString();
    const auto edit = [&](const QString& operation, const QVariantMap& values,
                          const QString& function = QString()) {
        return controller
            .editGraphFromUi(controller.selectedGraphClip(), function.isEmpty() ? id : function,
                             operation, values)
            .value("ok")
            .toBool();
    };
    const auto rejected = [&](const QString& operation, const QVariantMap& values,
                              const QString& function = QString()) {
        const auto before = controller.projectForTest();
        const auto depth = controller.undoDepthForTest();
        check(!edit(operation, values, function), "不正な操作の拒否");
        check(controller.projectForTest() == before && controller.undoDepthForTest() == depth,
              "拒否で Project と Undo を変えない");
        // 変異で拒否が壊れても後続の検査は正常終了させる。失敗を crash で検出しない。
        if (controller.projectForTest() != before)
            controller.undoLastEdit();
    };
    rejected("delete", {});
    rejected("field", {{"xMin", "1e"}});
    rejected("field", {{"xMin", "5"}});
    rejected("field", {{"domainMin", "100"}});
    rejected("field", {{"strokeWidth", "0"}});
    rejected("field", {{"expression", "x"}}, "存在しない関数");
    for (const auto& expression : {"x^2", "sin(x)", "1/x", "sin(", "unknown(x)", ""}) {
        const auto before = controller.projectForTest();
        const auto depth = controller.undoDepthForTest();
        check(edit("field", {{"expression", expression}}), "不正を含む式原文の確定");
        const auto after = controller.projectForTest();
        check(after.timelineClips.front().graph.functions.front().expression == expression,
              "式の完全一致");
        const bool changed = after != before;
        check(controller.undoDepthForTest() == depth + (changed ? 1 : 0),
              "変更した確定だけ Undo 一回");
        if (changed) {
            check(controller.undoLastEdit() && controller.projectForTest() == before,
                  "Undo 完全一致");
            check(controller.redoLastEdit() && controller.projectForTest() == after,
                  "Redo 完全一致");
        }
        check(controller.saveProject(), "製品の保存");
        const auto reopened = mvm::project::loadProjectJson(path);
        check(reopened.success && reopened.project.timelineClips == after.timelineClips,
              "不正原文と ID の保存再読込");
    }
    check(edit("add", {}) && edit("add", {}), "三関数へ追加");
    rejected("add", {});
    auto functions = controller.selectedGraphClip().value("functions").toList();
    const auto secondId = functions[1].toMap().value("id").toString();
    check(edit("move", {{"index", 2}}), "ID 指定の並べ替え");
    functions = controller.selectedGraphClip().value("functions").toList();
    check(functions[2].toMap().value("id").toString() == id &&
              functions[0].toMap().value("id").toString() == secondId,
          "並べ替えで所有 ID を保つ");
    check(edit("field", {{"color", "#8000ff55"}}), "半透明色を確定");
    check(controller.projectForTest().timelineClips.front().graph.functions[2].color == "#8000FF55",
          "ARGB の alpha と正準表現");
    check(edit("field", {{"domainMin", "0"}}), "ゼロの定義域");
    check(edit("field", {{"domainMin", ""}}), "定義域の解除");
    check(edit("field", {{"draw", true}}), "Draw を有効化");
    for (auto count : {1, 3, 10})
        check(edit("field", {{"frames", QString::number(count)}}), "整数 Draw frame 数");
    rejected("field", {{"frames", "10001"}});
    for (const auto& values :
         {QVariantMap{{"xMin", "-10"}, {"xMax", "10"}, {"yMin", "-10"}, {"yMax", "25"}},
          QVariantMap{{"showAxes", false}, {"showGrid", false}, {"xLabel", ""}, {"yLabel", "y_1"}},
          QVariantMap{{"label", "\\frac{"}, {"strokeWidth", "64"}, {"color", "#00000000"}},
          QVariantMap{{"domainMin", "0"}, {"domainMax", "5"}},
          QVariantMap{{"domainMin", ""}, {"domainMax", "5"}},
          QVariantMap{{"domainMin", "0"}, {"domainMax", ""}},
          QVariantMap{{"domainMin", ""}, {"domainMax", ""}}}) {
        const auto previous = controller.projectForTest();
        const auto previousDepth = controller.undoDepthForTest();
        check(edit("field", values), "表示範囲・軸・ラベル・色・線幅・任意定義域の確定");
        const auto accepted = controller.projectForTest();
        check(controller.undoDepthForTest() == previousDepth + 1, "複数欄の論理確定は Undo 一回");
        check(controller.undoLastEdit() && controller.projectForTest() == previous,
              "複数欄の Undo 完全一致");
        check(controller.redoLastEdit() && controller.projectForTest() == accepted,
              "複数欄の Redo 完全一致");
    }
    rejected("field", {{"domainMin", "5"}, {"domainMax", "0"}});
    const auto stale = controller.selectedGraphClip();
    check(edit("field", {{"expression", "x^2"}}), "不正な式を修復");
    check(
        !controller.editGraphFromUi(stale, id, "field", {{"expression", "x"}}).value("ok").toBool(),
        "古い revision を拒否");
    const auto before = controller.projectForTest();
    const auto depth = controller.undoDepthForTest();
    const auto count = controller.graphRastersForTest().renderCount();
    for (int i = 0; i < 10; ++i)
        controller.graphStatusFromUi(stale.value("clipId").toString());
    check(controller.projectForTest() == before && controller.undoDepthForTest() == depth &&
              controller.graphRastersForTest().renderCount() == count,
          "状態問い合わせは読むだけ");
    const auto originalClip = controller.projectForTest().timelineClips.front();
    check(controller.splitClipAt(QString::fromStdString(originalClip.id), 2, false, false),
          "Draw の途中で既存の split を使う");
    const auto splitProject = controller.projectForTest();
    const auto right =
        std::find_if(splitProject.timelineClips.begin(), splitProject.timelineClips.end(),
                     [](const auto& clip) { return clip.sourceInFrame == 2; });
    check(right != splitProject.timelineClips.end(), "split の右側が存在する");
    if (right != splitProject.timelineClips.end()) {
        std::string error;
        const auto frame = mvm::project::evaluateGraphClip(*right, {60, 1}, 0, error);
        check(frame && frame->sourceFrame == 2 && frame->progressNumerator == 2 &&
                  frame->progressDenominator == 10,
              "split の右側は元の Draw 位相から続く");
        check(right->graph.functions[0].id != originalClip.graph.functions[0].id,
              "split の右側に新しい所有 ID");
    }
    check(controller.undoLastEdit() && controller.projectForTest() == before,
          "split の Undo は元の所有 ID と Project を戻す");
    check(controller.redoLastEdit() && controller.projectForTest() == splitProject,
          "split の Redo は確定済みの所有 ID を戻す");
    check(controller.selectClip(0) && controller.duplicateSelectedClips(), "既存の複製入口");
    check(controller.projectForTest().timelineClips.size() == 3, "複製で一つだけ増える");
    const auto deletedAuthority = controller.selectedGraphClip();
    check(controller.deleteCurrentClip(), "選択した Graph の削除");
    check(controller.selectedGraphClip().value("clipId") != deletedAuthority.value("clipId"),
          "削除した Graph を編集対象から外す");
    check(!controller.editGraphFromUi(deletedAuthority, id, "field", {{"expression", "x"}})
               .value("ok")
               .toBool(),
          "削除前の欄から別の Graph を編集しない");
    check(controller.undoLastEdit() && !controller.selectedGraphClip().isEmpty(),
          "削除 Undo で選択と Graph を戻す");
    // 選択中の clip が再生ヘッドに掛かっていても、作成メニューはその track へ重ねず、
    // 文字・数式と同じく空いている映像 track へ置く。
    {
        const auto project = controller.projectForTest();
        const auto selectedId = controller.selectedGraphClip().value("clipId").toString();
        const auto selected =
            std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                         [&](const auto& clip) { return clip.id == selectedId.toStdString(); });
        const auto active = mvm::project::activeClipsAt(project, mvm::project::TrackKind::Video,
                                                        controller.playheadFrame());
        const bool overlapping =
            selected != project.timelineClips.end() &&
            selected->track.index < static_cast<int>(active.size()) &&
            active[static_cast<std::size_t>(selected->track.index)] == &*selected;
        check(overlapping, "前提: 選択中の Graph が再生ヘッドに掛かっている");
        const auto undoBefore = controller.undoDepthForTest();
        check(controller.createGraphClipFromUi(),
              "選択中の clip と重なる位置でも Graph を作成する");
        const auto created = controller.projectForTest();
        const auto createdId = controller.selectedGraphClip().value("clipId").toString();
        const auto placed =
            std::find_if(created.timelineClips.begin(), created.timelineClips.end(),
                         [&](const auto& clip) { return clip.id == createdId.toStdString(); });
        check(controller.undoDepthForTest() == undoBefore + 1 &&
                  created.timelineClips.size() == project.timelineClips.size() + 1 &&
                  placed != created.timelineClips.end() && createdId != selectedId &&
                  placed->track.kind == mvm::project::TrackKind::Video && overlapping &&
                  placed->track.index > selected->track.index &&
                  placed->timelineStartFrame == controller.playheadFrame(),
              "作成した Graph は再生ヘッドの位置で、使用中より上の空き映像 track に置く");
    }
    controller.shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return checks > 0 && failures == 0 ? 0 : 1;
}
