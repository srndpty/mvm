// 数式 clip を MvmController 経由で検査する (偽の backend、GPU なし)。
//
// - 作成・確定は Undo 1 回分。描けない式も確定でき、Project に残る (巻き戻さない)
// - 描き直し中・失敗中の preview は最後に描けた画素 (last-good) を出す
// - 色の変更は描き直さない (key に入らない)。複製・貼り付けは同じ描画を共有する
// - 書き出しは現在の式の描画が済んだ clip だけを受け付け、描画済みの PNG を渡す
// - 保存して開き直すと、disk の描画を描かずに使う
// - 入力中の preview は Project と Undo を変えない
// - backend が使えなければ「利用不可」で、書き出しを拒否する
// - 描画中の shutdown は描画を止めて速やかに返る

#include "math_fake_backend.h"
#include "mvm_controller.h"
#include "project/project_json.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

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

using mvm::app::MvmController;
using mvm::test::FakeMathBackend;
namespace project = mvm::project;

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

struct CapturedExport {
    std::mutex mutex;
    int calls = 0;
    std::map<std::string, std::filesystem::path> artifacts;
};

std::unique_ptr<MvmController> makeController(const std::filesystem::path& path,
                                              const project::Project& initial,
                                              const std::shared_ptr<CapturedExport>& captured) {
    auto controller = std::make_unique<MvmController>(
        path, std::filesystem::path{}, initial, nullptr,
        [captured](const project::Project&, const mvm::app::TimelineExportRequest& request) {
            std::lock_guard lock(captured->mutex);
            ++captured->calls;
            captured->artifacts = request.mathArtifacts;
            mvm::app::TimelineExportResult result;
            result.success = true;
            result.outputPath = request.outputPath;
            return result;
        },
        MvmController::ExportThreadFactory{},
        MvmController::FileRevealer{[](const std::filesystem::path&, QString&) { return true; }});
    return controller;
}

QString state(const MvmController& controller, const QString& clipId) {
    return controller.mathClipData(clipId).value(QStringLiteral("state")).toString();
}

std::vector<const project::TimelineClip*> mathClips(const MvmController& controller) {
    std::vector<const project::TimelineClip*> clips;
    for (const auto& clip : controller.projectForTest().timelineClips)
        if (clip.kind == project::TimelineClipKind::Math)
            clips.push_back(&clip);
    return clips;
}

// frame の合成で、数式 clip の層の (x, y) の画素。層が無ければ空。
std::vector<std::uint8_t> mathPixel(const MvmController& controller, qint64 frame, int x, int y) {
    QString error;
    const auto composition = controller.subtitleCompositionForTest(frame, error);
    if (!composition)
        return {};
    for (const auto& layer : composition->layers) {
        if (!layer.stillImage)
            continue;
        const auto& image = *layer.stillImage;
        const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x)) *
                        4U;
        return {image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]};
    }
    return {};
}

bool exportAndWait(MvmController& controller, const std::filesystem::path& output) {
    const bool started =
        controller.exportTimeline(QUrl::fromLocalFile(QString::fromStdWString(output.wstring())));
    if (!started)
        return false;
    return pump([&] { return !controller.exporting(); });
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    QTemporaryDir temp;
    check(temp.isValid(), "作業フォルダー");
    const auto path = std::filesystem::path(temp.filePath("数式 project.mvm").toStdWString());
    const auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "初期プロジェクトの保存");
    FakeMathBackend backend;
    auto captured = std::make_shared<CapturedExport>();
    // mask (3x2) は 1920x1080 の中央 (左上 958, 539) に置かれる。左上の画素の alpha は 255。
    constexpr int kX = 958;
    constexpr int kY = 539;

    QString clipId;
    {
        auto controller = makeController(path, initial, captured);
        controller->setMathPreflightForTest(backend.preflight());
        check(pump([&] {
                  return controller->mathRastersForTest().backendState() ==
                         mvm::app::MathRasterCache::BackendState::Available;
              }),
              "偽の backend が使える");

        const auto undoBefore = controller->undoDepthForTest();
        check(controller->createMathClip(QStringLiteral("  x^2  ")), "数式 clip を作る");
        check(controller->undoDepthForTest() == undoBefore + 1, "作成は Undo 1 回分");
        const auto clips = mathClips(*controller);
        check(clips.size() == 1 && clips[0]->math.source == "x^2",
              "式は前後の空白を除いて保存する");
        if (clips.size() != 1)
            return 1;
        clipId = QString::fromStdString(clips[0]->id);
        const auto frame = clips[0]->timelineStartFrame;
        check(controller->selectedMathClip().value(QStringLiteral("clipId")).toString() == clipId,
              "作った数式 clip を選択する");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
              "描画が終わると ready");
        check(*backend.renders == 1, "1 回描く");
        check(mathPixel(*controller, frame, kX, kY) ==
                  std::vector<std::uint8_t>{255, 255, 255, 255},
              "preview の合成に白い数式が中央に入る");

        // 色だけを変える: 描き直さず、合成の色が変わる。
        check(controller->updateMathClip(clipId,
                                         {{QStringLiteral("color"), QStringLiteral("#FFFF0000")}}),
              "色を変える");
        check(controller->undoDepthForTest() == undoBefore + 2, "書式の確定は Undo 1 回分");
        check(mathPixel(*controller, frame, kX, kY) == std::vector<std::uint8_t>{255, 0, 0, 255},
              "色は描き直さずに合成で付ける");
        check(*backend.renders == 1, "色の変更で描き直さない");

        // 描けない式を確定する: Project には残し、preview は前の描画を出し続ける。
        check(
            controller->updateMathClip(clipId, {{QStringLiteral("source"), QStringLiteral("BAD")}}),
            "描けない式も確定できる");
        check(mathClips(*controller)[0]->math.source == "BAD",
              "描けない式は Project に残る (巻き戻さない)");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("error"); }),
              "描けない式は error");
        const auto failed = controller->mathClipData(clipId);
        check(failed.value(QStringLiteral("message")).toString() ==
                  QStringLiteral("Undefined control sequence."),
              "error の理由を返す");
        check(failed.value(QStringLiteral("log")).toString() == QStringLiteral("fake log"),
              "error の log を返す");
        check(failed.value(QStringLiteral("showingPrevious")).toBool(),
              "前に描けた画素を出していることを示す");
        check(mathPixel(*controller, frame, kX, kY) == std::vector<std::uint8_t>{255, 0, 0, 255},
              "失敗中の preview は last-good を出す");

        const auto output = std::filesystem::path(temp.filePath("out.mp4").toStdWString());
        check(!controller->exportTimeline(
                  QUrl::fromLocalFile(QString::fromStdWString(output.wstring()))),
              "描けていない数式 clip があれば書き出さない");
        check(captured->calls == 0, "拒否した書き出しは runner を呼ばない");

        check(controller->undoLastEdit(), "Undo");
        check(mathClips(*controller)[0]->math.source == "x^2", "Undo で前の式に戻る");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
              "Undo した式は描画済みなので ready");
        check(controller->redoLastEdit() && mathClips(*controller)[0]->math.source == "BAD",
              "Redo で描けない式に戻る");
        check(controller->undoLastEdit(), "もう一度 Undo");
        // Redo で要求し直した描けない式の描画 (失敗の記録は Undo で捨てられる) が終わるのを
        // 待ってから数える。
        settle(300);
        const int rendersBeforeCopy = *backend.renders;

        // 複製・貼り付け: 新しい ID で同じ数式を持ち、描画を共有する。
        const auto index = [&](const QString& id) {
            const auto& all = controller->projectForTest().timelineClips;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (QString::fromStdString(all[i].id) == id)
                    return static_cast<int>(i);
            return -1;
        };
        check(controller->selectClip(index(clipId)) && controller->copySelectedClips(),
              "数式 clip をコピーする");
        check(controller->pasteClips(), "貼り付ける");
        check(controller->selectClip(index(clipId)) && controller->duplicateSelectedClips(),
              "複製する");
        const auto copies = mathClips(*controller);
        check(copies.size() == 3, "貼り付けと複製で数式 clip が 3 本になる");
        bool sameMath = copies.size() == 3;
        for (const auto* clip : copies)
            sameMath = sameMath && clip->math == copies[0]->math;
        check(sameMath && copies.size() == 3 && copies[0]->id != copies[1]->id &&
                  copies[1]->id != copies[2]->id && copies[0]->id != copies[2]->id,
              "数式の値を保ち、ID は新しくする");
        settle(200);
        check(*backend.renders == rendersBeforeCopy, "同じ式の複製は描き直さない");

        check(exportAndWait(*controller, output), "描画済みなら書き出す");
        {
            std::lock_guard lock(captured->mutex);
            check(captured->calls == 1 && captured->artifacts.size() == 3,
                  "書き出しへ数式 clip ごとの描画済み PNG を渡す");
            bool exist = true;
            for (const auto& [id, png] : captured->artifacts)
                exist = exist && std::filesystem::is_regular_file(png);
            check(exist, "渡した PNG は cache に存在する");
        }

        // 入力中の preview: Project と Undo を変えない。
        const auto undoBeforePreview = controller->undoDepthForTest();
        const auto projectBeforePreview = controller->projectForTest();
        check(
            controller->previewMathClip(clipId, {{QStringLiteral("source"), QStringLiteral("y")}}),
            "入力中の式を preview する");
        check(controller->mathClipData(clipId).value(QStringLiteral("source")).toString() ==
                  QStringLiteral("y"),
              "inspector には入力中の式を出す");
        check(controller->projectForTest() == projectBeforePreview &&
                  controller->undoDepthForTest() == undoBeforePreview,
              "preview は Project と Undo を変えない");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
              "入力中の式も描く");
        controller->cancelMathPreview();
        check(controller->mathClipData(clipId).value(QStringLiteral("source")).toString() ==
                  QStringLiteral("x^2"),
              "取り消すと確定済みの式に戻る");

        check(controller->saveProject(), "保存する");
        controller->shutdown();
    }

    {
        // 開き直すと disk の描画を使い、描かない。
        FakeMathBackend reopened;
        reopened.canonical = backend.canonical;
        const auto loaded = project::loadProjectJson(path);
        check(loaded.success, "保存した project を読める");
        auto controller = makeController(path, loaded.project, captured);
        controller->setMathPreflightForTest(reopened.preflight());
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
              "開き直すと ready");
        check(*reopened.renders == 0, "開き直したときは disk の描画を使い、描かない");

        // 描けない式を保存して開き直す: last-good は無く、preview に出ない。
        check(
            controller->updateMathClip(clipId, {{QStringLiteral("source"), QStringLiteral("BAD")}}),
            "描けない式を確定する");
        check(controller->saveProject(), "描けない式のまま保存できる");
        controller->shutdown();
    }

    {
        FakeMathBackend reopened;
        const auto loaded = project::loadProjectJson(path);
        check(loaded.success && loaded.project.timelineClips.size() >= 1,
              "描けない式を含む project を読める");
        auto controller = makeController(path, loaded.project, captured);
        controller->setMathPreflightForTest(reopened.preflight());
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("error"); }),
              "開き直した描けない式は error");
        check(!controller->mathClipData(clipId).value(QStringLiteral("showingPrevious")).toBool(),
              "開き直した後は last-good を対応付けない");
        const auto& clip =
            *std::find_if(controller->projectForTest().timelineClips.begin(),
                          controller->projectForTest().timelineClips.end(),
                          [&](const auto& c) { return QString::fromStdString(c.id) == clipId; });
        // 同じ frame には貼り付け・複製した (描ける) 数式 clip も重なっている。合成の層の数が
        // 「描けない clip を除いた数式 clip の数」と一致すること。
        pump([&] {
            for (const auto* other : mathClips(*controller))
                if (QString::fromStdString(other->id) != clipId &&
                    state(*controller, QString::fromStdString(other->id)) !=
                        QStringLiteral("ready"))
                    return false;
            return true;
        });
        std::size_t others = 0;
        for (const auto* other : mathClips(*controller)) {
            const auto end =
                other->timelineStartFrame + (other->sourceOutFrame - other->sourceInFrame);
            if (QString::fromStdString(other->id) != clipId &&
                other->timelineStartFrame <= clip.timelineStartFrame &&
                clip.timelineStartFrame < end)
                ++others;
        }
        QString compositionError;
        const auto composition =
            controller->subtitleCompositionForTest(clip.timelineStartFrame, compositionError);
        check(composition && others > 0 && composition->layers.size() == others,
              "描けたことの無い式は preview に出さない (他の数式 clip だけが合成される)");

        // backend が使えない: 利用不可を示し、書き出さない。
        controller->setMathPreflightForTest(
            FakeMathBackend::unavailable("LaTeX (latex.exe) が PATH に見つかりません"));
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("unavailable"); }),
              "backend が使えなければ unavailable");
        check(controller->mathClipData(clipId)
                  .value(QStringLiteral("message"))
                  .toString()
                  .contains(QStringLiteral("latex.exe")),
              "利用不可の理由を返す");
        check(!controller->exportTimeline(QUrl::fromLocalFile(temp.filePath("out2.mp4"))),
              "backend が使えなければ書き出さない");

        // 描画中の shutdown は描画を止めて速やかに返る。
        FakeMathBackend slow;
        controller->setMathPreflightForTest(slow.preflight());
        pump([&] {
            return controller->mathRastersForTest().backendState() ==
                   mvm::app::MathRasterCache::BackendState::Available;
        });
        check(controller->updateMathClip(clipId,
                                         {{QStringLiteral("source"), QStringLiteral("SLOW")}}),
              "時間のかかる式を確定する");
        check(pump([&] { return slow.slowStarted->load(); }), "時間のかかる式の描画が始まる");
        const auto started = std::chrono::steady_clock::now();
        controller->shutdown();
        check(std::chrono::steady_clock::now() - started < std::chrono::seconds(3),
              "描画中の shutdown が速やかに返る");
        check(*slow.slowSawCancel, "shutdown は描画を cancel する");
    }

    {
        // 排他: 同じ Project の 2 つ目の instance と、同じ directory の別の Project が、
        // 作業中の数式の job を乱さない。
        const auto shared = std::filesystem::path(temp.filePath("共有 project.mvm").toStdWString());
        check(project::saveProjectJson(initial, shared).success, "共有 project の保存");
        FakeMathBackend ownerBackend;
        auto owner = makeController(shared, initial, captured);
        owner->setMathPreflightForTest(ownerBackend.preflight());
        check(pump([&] {
                  return owner->mathRastersForTest().backendState() ==
                         mvm::app::MathRasterCache::BackendState::Available;
              }),
              "所有者の backend が使える");
        check(owner->createMathClip(QStringLiteral("SLOW")), "所有者が時間のかかる式を作る");
        check(pump([&] { return ownerBackend.slowStarted->load(); }), "所有者の描画が始まる");
        const auto ownerJobs = owner->mathRastersForTest().jobsDirectory();
        const auto ownerJobsAlive = [&] {
            std::error_code error;
            return std::filesystem::is_directory(ownerJobs, error) &&
                   !std::filesystem::is_empty(ownerJobs, error) && !*ownerBackend.slowSawCancel;
        };
        check(ownerJobsAlive(), "所有者の作業 directory がある (検査の対照)");

        {
            auto preflights = std::make_shared<std::atomic<int>>(0);
            auto intruder = makeController(shared, initial, captured);
            check(!intruder->mathRastersForTest().authorized(),
                  "Project lock を取れない instance は数式の cache の権限を持たない");
            intruder->setMathPreflightForTest(
                [preflights](const std::filesystem::path&, const std::atomic<bool>*) {
                    ++*preflights;
                    return mvm::math::MathPreflightResult{};
                });
            settle(300);
            check(*preflights == 0,
                  "lock を持たない instance は数式の確認 (外部 renderer) を始めない");
            check(intruder->mathRastersForTest().backendState() ==
                          mvm::app::MathRasterCache::BackendState::Unavailable &&
                      intruder->mathRastersForTest().backendMessage().contains(
                          QStringLiteral("他のプロセス")),
                  "lock を持たない理由を Unavailable で示す");
            check(ownerJobsAlive(), "2 つ目の instance は所有者の作業 directory を消さない");
            intruder->shutdown();
        }
        check(ownerJobsAlive(), "2 つ目の instance の終了も所有者の描画を止めない");

        {
            const auto other =
                std::filesystem::path(temp.filePath("別 project.mvm").toStdWString());
            check(project::saveProjectJson(initial, other).success, "別の project の保存");
            FakeMathBackend otherBackend;
            auto neighbor = makeController(other, initial, captured);
            neighbor->setMathPreflightForTest(otherBackend.preflight());
            check(pump([&] {
                      return neighbor->mathRastersForTest().backendState() ==
                             mvm::app::MathRasterCache::BackendState::Available;
                  }),
                  "別の project の backend が使える (確認と掃除が終わった)");
            check(neighbor->mathRastersForTest().cacheDirectory() !=
                      owner->mathRastersForTest().cacheDirectory(),
                  "同じ directory の別の project とは cache を分ける");
            check(ownerJobsAlive(), "別の project の確認は所有者の作業 directory を消さない");
            neighbor->shutdown();
        }
        owner->shutdown();
        check(*ownerBackend.slowSawCancel, "所有者の終了は自分の描画を止める");
    }

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
