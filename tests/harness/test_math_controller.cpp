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
// - Write: 確定した式の連番を描いて preview に付け、書き出しへ渡す。入力中・連番の描画中は
//   静止を見せ、連番が描けていなければ書き出さない

#include "app/preview/preview_engine_rhi_item.h"
#include "math_fake_backend.h"
#include "mvm_controller.h"
#include "project/project_json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    std::map<std::string, std::vector<std::filesystem::path>> writeFrames;
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
            captured->writeFrames = request.mathWriteFrames;
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

// frame の合成の、数式 clip の静止画 layer (1 本だけ置く試験で使う)。
std::optional<mvm::preview::PreviewCompositionLayer> mathLayer(const MvmController& controller,
                                                               qint64 frame) {
    QString error;
    const auto composition = controller.subtitleCompositionForTest(frame, error);
    if (!composition)
        return std::nullopt;
    for (const auto& layer : composition->layers)
        if (layer.stillImage)
            return layer;
    return std::nullopt;
}

QString writeState(const MvmController& controller, const QString& clipId) {
    return controller.mathClipData(clipId).value(QStringLiteral("writeState")).toString();
}

// Write の preview 用の mask は合成が要求してから別の worker で読む。読めるまで (Write の
// animation が付くまで) 合成を作り直す。
std::optional<mvm::preview::PreviewCompositionLayer>
waitForWriteLayer(const MvmController& controller, qint64 frame) {
    std::optional<mvm::preview::PreviewCompositionLayer> layer;
    pump([&] {
        layer = mathLayer(controller, frame);
        return layer && layer->stillAnimation;
    });
    return layer;
}

// Write (clip の先頭で式を書く) の controller の契約。偽の backend の "WIDE" は 64x8 の帯で、
// Write の frame i は左から 64 * i / frames 列が不透明 (期待値はここで独立に計算する)。
void testWrite(const QTemporaryDir& temp, const project::Project& initial,
               const std::shared_ptr<CapturedExport>& captured) {
    const auto path = std::filesystem::path(temp.filePath("write project.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, "Write の project の保存");
    FakeMathBackend backend;
    QString clipId;
    qint64 start = 0;
    {
        auto controller = makeController(path, initial, captured);
        controller->setMathPreflightForTest(backend.preflight());
        check(pump([&] {
                  return controller->mathRastersForTest().backendState() ==
                         mvm::app::MathRasterCache::BackendState::Available;
              }),
              "Write: 偽の backend が使える");
        check(controller->createMathClip(QStringLiteral("WIDE w")), "Write: 数式 clip を作る");
        const auto clips = mathClips(*controller);
        if (clips.size() != 1) {
            check(false, "Write: 数式 clip が 1 本");
            return;
        }
        clipId = QString::fromStdString(clips[0]->id);
        start = clips[0]->timelineStartFrame;
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
              "Write: 静止の描画が終わる");
        const auto initialData = controller->mathClipData(clipId);
        check(initialData.value(QStringLiteral("intro")).toString() == QStringLiteral("none") &&
                  writeState(*controller, clipId) == QStringLiteral("none"),
              "作った数式 clip は Write を持たない");
        const auto plain = mathLayer(*controller, start);
        check(plain && !plain->stillAnimation && *backend.sequenceRenders == 0,
              "Write の無い clip は連番を描かず、animation を付けない");

        const auto undoBefore = controller->undoDepthForTest();
        check(
            controller->updateMathClip(clipId, {{QStringLiteral("intro"), QStringLiteral("write")},
                                                {QStringLiteral("introSeconds"), 0.5}}),
            "Write を付ける (0.5 秒)");
        check(controller->undoDepthForTest() == undoBefore + 1, "Write の確定は Undo 1 回分");
        const auto data = controller->mathClipData(clipId);
        check(data.value(QStringLiteral("intro")).toString() == QStringLiteral("write") &&
                  data.value(QStringLiteral("introFrames")).toLongLong() == 30 &&
                  data.value(QStringLiteral("introSeconds")).toDouble() == 0.5 &&
                  data.value(QStringLiteral("introMaxSeconds")).toDouble() == 5.0,
              "60 fps の 0.5 秒は 30 frame、上限は clip の尺 (5 秒)");
        check(pump([&] { return writeState(*controller, clipId) == QStringLiteral("ready"); }),
              "連番の描画が終わると writeState は ready");
        check(*backend.sequenceRenders == 1, "連番を 1 回描く");

        const auto first = mathLayer(*controller, start);
        check(first && !first->stillAnimation,
              "preview 用の mask を読み終えるまでは書き終えた式 (静止) を見せる");
        const auto layer = waitForWriteLayer(*controller, start);
        check(layer && layer->stillAnimation,
              "mask を読み終えると Write のある clip の still layer に animation を付ける");
        check(controller->mathClipData(clipId).value(QStringLiteral("writePreview")).toString() ==
                  QStringLiteral("ready"),
              "inspector に preview の mask が使えることを返す");
        if (layer && layer->stillAnimation) {
            const auto& animation = *layer->stillAnimation;
            // 64x8 の mask は 1920x1080 の中央 (左上 (1920-64)/2 = 928, (1080-8)/2 = 536)。
            check(animation.patchRect() == mvm::preview::PreviewPixelRect{928, 536, 64, 8},
                  "書き換える矩形は静止の mask を置いた矩形");
            check(animation.stateAt(start) == 0 && animation.stateAt(start + 15) == 15 &&
                      animation.stateAt(start + 29) == 29 && animation.stateAt(start + 30) == -1 &&
                      animation.stateAt(start + 200) == -1,
                  "clip の先頭から 30 frame が Write の frame 0..29、その後は静止");
            std::vector<std::uint8_t> patch(64U * 8U * 4U, 0xEE);
            animation.fillPatch(15, patch.data());
            // frame 15 は左から 64 * 15 / 30 = 32 列が不透明 (白)。
            check(patch[31 * 4 + 3] == 255 && patch[31 * 4] == 255 && patch[32 * 4 + 3] == 0,
                  "frame 15 の patch は左 32 列だけが白い");
            const auto again = mathLayer(*controller, start + 10);
            check(again && again->stillAnimation == layer->stillAnimation,
                  "同じ見た目の間は同じ animation instance を渡す (engine の texture を保つ)");
        }

        // 色を変えると animation も作り直し、patch に色が付く。連番は描き直さない。
        check(controller->updateMathClip(clipId,
                                         {{QStringLiteral("color"), QStringLiteral("#FFFF0000")}}),
              "Write の clip の色を変える");
        const auto red = mathLayer(*controller, start);
        check(red && red->stillAnimation && layer && red->stillAnimation != layer->stillAnimation,
              "色が変われば別の animation instance (memory の mask はそのまま使う)");
        if (red && red->stillAnimation) {
            std::vector<std::uint8_t> patch(64U * 8U * 4U, 0);
            red->stillAnimation->fillPatch(29, patch.data());
            check(patch[0] == 255 && patch[1] == 0 && patch[3] == 255, "patch は数式の色で塗る");
        }
        check(*backend.sequenceRenders == 1, "色の変更で連番を描き直さない");

        // 入力中は静止だけを見せる。
        check(controller->previewMathClip(
                  clipId, {{QStringLiteral("source"), QStringLiteral("WIDE typed")}}),
              "Write の clip の式を入力する");
        pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); });
        const auto typing = mathLayer(*controller, start);
        check(typing && !typing->stillAnimation,
              "入力中の clip は Write を付けない (静止を見せる)");
        controller->cancelMathPreview();

        // 尺は clip の尺までに収め、不正な種類は拒否する。
        const auto beforeInvalid = controller->projectForTest();
        check(!controller->updateMathClip(clipId,
                                          {{QStringLiteral("intro"), QStringLiteral("unwrite")}}) &&
                  controller->projectForTest() == beforeInvalid,
              "未知の intro は拒否し、Project を変えない");
        check(controller->updateMathClip(clipId, {{QStringLiteral("introSeconds"), 100.0}}) &&
                  mathClips(*controller)[0]->mathAnimation.introFrames == 300,
              "長すぎる尺は clip の尺 (300 frame) に収める");
        check(controller->updateMathClip(clipId, {{QStringLiteral("introSeconds"), 0.5}}),
              "尺を 0.5 秒に戻す");
        check(pump([&] { return writeState(*controller, clipId) == QStringLiteral("ready"); }),
              "戻した尺の連番は描画済み");

        const auto output = std::filesystem::path(temp.filePath("write.mp4").toStdWString());
        check(exportAndWait(*controller, output), "Write が描けていれば書き出す");
        {
            std::lock_guard lock(captured->mutex);
            const auto found = captured->writeFrames.find(clipId.toStdString());
            check(found != captured->writeFrames.end() && found->second.size() == 30 &&
                      std::filesystem::is_regular_file(found->second.front()) &&
                      std::filesystem::is_regular_file(found->second.back()),
                  "書き出しへ Write の連番 (30 枚の PNG) を渡す");
        }

        // 連番を描いている間は書き出さない (静止で代用しない)。
        check(controller->updateMathClip(
                  clipId, {{QStringLiteral("source"), QStringLiteral("SLOW_WRITE")}}),
              "連番が終わらない式を確定する");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }) &&
                  pump([&] { return backend.slowStarted->load(); }),
              "静止は描け、連番は描き始める");
        check(writeState(*controller, clipId) == QStringLiteral("rendering"),
              "連番の描画中は writeState が rendering");
        const auto callsBefore = captured->calls;
        check(
            !controller->exportTimeline(QUrl::fromLocalFile(temp.filePath("pending-write.mp4"))) &&
                captured->calls == callsBefore,
            "Write の描画が未完了なら runner を呼ばず書き出さない");
        const auto pending = mathLayer(*controller, start);
        check(pending && !pending->stillAnimation,
              "連番の描画中は Write を付けず、書き終えた式 (静止) を見せる");
        check(controller->undoLastEdit() && mathClips(*controller)[0]->math.source == "WIDE w",
              "Undo で描けた式に戻る");
        check(pump([&] { return backend.slowSawCancel->load(); }),
              "要求されなくなった連番の描画は止まる");

        check(controller->updateMathClip(clipId,
                                         {{QStringLiteral("intro"), QStringLiteral("none")}}) &&
                  mathClips(*controller)[0]->mathAnimation == project::MathClipAnimation{},
              "Write を外す");
        const auto removed = mathLayer(*controller, start);
        check(removed && !removed->stillAnimation &&
                  writeState(*controller, clipId) == QStringLiteral("none"),
              "Write を外すと animation を付けない");
        check(controller->undoLastEdit() &&
                  mathClips(*controller)[0]->mathAnimation ==
                      project::MathClipAnimation{project::MathIntroKind::Write, 30},
              "Undo で Write が戻る");
        check(controller->saveProject(), "Write の project を保存する");
        controller->shutdown();
    }
    {
        FakeMathBackend reopened;
        const auto loaded = project::loadProjectJson(path);
        check(loaded.success && loaded.project.timelineClips.size() == 1 &&
                  loaded.project.timelineClips[0].mathAnimation ==
                      project::MathClipAnimation{project::MathIntroKind::Write, 30},
              "保存した Write を読める");
        auto controller = makeController(path, loaded.project, captured);
        controller->setMathPreflightForTest(reopened.preflight());
        check(pump([&] { return writeState(*controller, clipId) == QStringLiteral("ready"); }),
              "開き直すと Write は ready");
        check(*reopened.sequenceRenders == 0, "開き直したときは disk の連番を使い、描かない");
        const auto layer = waitForWriteLayer(*controller, start);
        check(layer && layer->stillAnimation && layer->stillAnimation->stateAt(start + 1) == 1,
              "開き直した Write も preview に付く");
        controller->shutdown();
    }
}

// backend が描けない長さの Write: Project の値としては確定・保存でき、描画の状態が未対応の
// error になり、書き出しは拒否する (静止で代用しない)。
void testWriteBeyondBackendCapability(const QTemporaryDir& temp, const project::Project& initial,
                                      const std::shared_ptr<CapturedExport>& captured) {
    const auto path = std::filesystem::path(temp.filePath("write capability.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, "capability: project の保存");
    FakeMathBackend backend;
    backend.maximumSequenceFrames = 10;
    auto controller = makeController(path, initial, captured);
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "capability: 偽の backend が使える");
    check(controller->createMathClip(QStringLiteral("WIDE long")), "capability: 数式 clip を作る");
    const auto clipId = QString::fromStdString(mathClips(*controller)[0]->id);
    check(controller->updateMathClip(clipId, {{QStringLiteral("intro"), QStringLiteral("write")},
                                              {QStringLiteral("introSeconds"), 0.5}}),
          "backend の上限 (10 frame) を超える 30 frame の Write も確定できる");
    check(pump([&] { return writeState(*controller, clipId) == QStringLiteral("error"); }) &&
              controller->mathClipData(clipId)
                  .value(QStringLiteral("writeMessage"))
                  .toString()
                  .contains(QStringLiteral("10 frame")) &&
              *backend.sequenceRenders == 0,
          "描画環境が描けない長さは描かずに理由付きの error");
    check(pump([&] { return state(*controller, clipId) == QStringLiteral("ready"); }),
          "capability: 静止の描画は使える");
    const auto calls = captured->calls;
    check(!controller->exportTimeline(QUrl::fromLocalFile(temp.filePath("capability.mp4"))) &&
              captured->calls == calls,
          "描けない Write を含む書き出しは拒否する");
    check(controller->saveProject() && project::loadProjectJson(path).success,
          "描けない長さの Write も Project として保存して読み直せる");
    controller->shutdown();
}

// Write のある 2 本が同じ frame に重なり、preview の mask の上限が 1 本分しか無い。
// 1 本は Write を見せ、もう 1 本は静止で見せて理由を返す。memory の合計は上限を超えず、
// 書き出しは disk の連番で 2 本とも Write を渡す。
void testWriteResidencyBudget(const QTemporaryDir& temp, const project::Project& initial,
                              const std::shared_ptr<CapturedExport>& captured) {
    const auto path = std::filesystem::path(temp.filePath("write budget.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, "budget: project の保存");
    FakeMathBackend backend;
    auto controller = makeController(path, initial, captured);
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "budget: 偽の backend が使える");
    // 各連番は 64 x 8 x 30 = 15360 byte。上限 20000 byte は 1 本分。
    controller->mathRastersForTest().setResidentMemoryBudget(20000);
    check(controller->createMathClip(QStringLiteral("WIDE one")) &&
              controller->createMathClip(QStringLiteral("WIDE two")),
          "budget: 同じ位置に数式 clip を 2 本作る (V1 と V2)");
    const auto clips = mathClips(*controller);
    if (clips.size() != 2) {
        check(false, "budget: 数式 clip が 2 本");
        return;
    }
    const auto start = clips[0]->timelineStartFrame;
    std::vector<QString> ids;
    for (const auto* clip : clips)
        ids.push_back(QString::fromStdString(clip->id));
    for (const auto& id : ids)
        check(controller->updateMathClip(id, {{QStringLiteral("intro"), QStringLiteral("write")},
                                              {QStringLiteral("introSeconds"), 0.5}}),
              "budget: Write を付ける");
    check(pump([&] {
              return writeState(*controller, ids[0]) == QStringLiteral("ready") &&
                     writeState(*controller, ids[1]) == QStringLiteral("ready");
          }),
          "budget: 2 本の連番が disk に揃う");
    int animated = 0;
    pump([&] {
        QString error;
        const auto composition = controller->subtitleCompositionForTest(start, error);
        animated = 0;
        if (composition)
            for (const auto& layer : composition->layers)
                animated += layer.stillImage && layer.stillAnimation ? 1 : 0;
        return animated == 1 &&
               controller->mathClipData(ids[0]).value(QStringLiteral("writePreview")) !=
                   QStringLiteral("loading") &&
               controller->mathClipData(ids[1]).value(QStringLiteral("writePreview")) !=
                   QStringLiteral("loading");
    });
    check(animated == 1, "上限が 1 本分なら Write を見せるのは 1 本だけ");
    const auto& cache = controller->mathRastersForTest();
    check(cache.residentBytes() <= 20000 && cache.residentBytes() == 15360,
          "preview の mask の合計は上限以下 (15360 byte)");
    int refused = 0;
    for (const auto& id : ids) {
        const auto data = controller->mathClipData(id);
        if (data.value(QStringLiteral("writePreview")).toString() == QStringLiteral("memory")) {
            ++refused;
            check(data.value(QStringLiteral("writePreviewMessage"))
                          .toString()
                          .contains(QStringLiteral("memory")) &&
                      data.value(QStringLiteral("writeState")).toString() ==
                          QStringLiteral("ready"),
                  "収まらない clip は理由を返し、Write の状態 (書き出し) は ready のまま");
        }
    }
    check(refused == 1, "収まらなかった 1 本を inspector に示す");
    const auto output = std::filesystem::path(temp.filePath("budget.mp4").toStdWString());
    check(exportAndWait(*controller, output), "budget: preview に置けない Write も書き出せる");
    {
        std::lock_guard lock(captured->mutex);
        check(captured->writeFrames.size() == 2 &&
                  captured->writeFrames[ids[0].toStdString()].size() == 30 &&
                  captured->writeFrames[ids[1].toStdString()].size() == 30,
              "書き出しへ 2 本とも disk の連番を渡す (memory に置いたかと無関係)");
    }
    controller->shutdown();
}

} // namespace

// ---- 再生中に Write の mask が memory に届く (実 D3D11 preview、--native-write) ----
//
// 5 秒の数式 clip の先頭 2 秒 (120 frame) が Write。engine の render thread が出力 frame ごとに
// 評価した Write の frame を observer で記録し、timeline の frame と比べる。
//   1. 再生前に mask が memory にある: 再生の先頭から frame 0 の Write を見せる
//   2. 再生前は memory に無く、Write の途中で mask を届ける: 一時停止・seek をせずに、届いた後の
//      frame から Write を見せ、frame 0 からやり直さない (timeline の時刻が authority)
struct WriteObservation {
    std::mutex mutex;
    std::vector<std::pair<std::int64_t, std::int64_t>> frames; // (出力 frame, Write の frame)

    void clear() {
        std::lock_guard lock(mutex);
        frames.clear();
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> snapshot() {
        std::lock_guard lock(mutex);
        return frames;
    }
};

bool submittedHasWrite(const MvmController& controller) {
    const auto composition = controller.submittedCompositionForTest();
    if (!composition)
        return false;
    return std::any_of(composition->layers.begin(), composition->layers.end(),
                       [](const auto& layer) { return layer.stillImage && layer.stillAnimation; });
}

int nativeWritePlayback() {
    QTemporaryDir temp;
    check(temp.isValid(), "native: 作業フォルダー");
    const auto path = std::filesystem::path(temp.filePath("native write.mvm").toStdWString());
    const auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "native: project の保存");
    auto captured = std::make_shared<CapturedExport>();
    FakeMathBackend backend;
    auto controller = makeController(path, initial, captured);
    QQuickWindow window;
    // 入力を送らない試験だが、利用者の作業を止めないよう前面とフォーカスを奪わず、OS の
    // マウス入力も透過させる (tests/harness/test_window_focus.h と同じ flags)。
    window.setFlags(Qt::Window | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    window.resize(640, 360);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(640);
    surface->setHeight(360);
    controller->attachPreview(surface);
    window.show();
    // 空の timeline には提示する frame が無いので、ここでは engine の準備だけを待つ。
    check(pump([&] { return controller->previewReady(); }), "native: preview の初期化");
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "native: 偽の backend が使える");
    check(controller->createMathClip(QStringLiteral("WIDE native")), "native: 数式 clip を作る");
    const auto clips = mathClips(*controller);
    if (clips.size() != 1)
        return 1;
    const auto clipId = QString::fromStdString(clips[0]->id);
    const auto start = clips[0]->timelineStartFrame;
    constexpr std::int64_t kWrite = 120;
    check(start == 0 && controller->updateMathClip(
                            clipId, {{QStringLiteral("intro"), QStringLiteral("write")},
                                     {QStringLiteral("introSeconds"), 2.0}}),
          "native: 先頭 2 秒 (120 frame) の Write を付ける");
    check(pump([&] { return writeState(*controller, clipId) == QStringLiteral("ready"); }),
          "native: 連番が disk に揃う");
    auto observation = std::make_shared<WriteObservation>();
    controller->setMathWriteObserverForTest(
        [observation](const std::string&, std::int64_t frame, std::int64_t state) {
            std::lock_guard lock(observation->mutex);
            observation->frames.emplace_back(frame, state);
        });
    // Write の区間の記録は、すべて「Write の frame = timeline の frame - clip の先頭」であること。
    const auto timelineAuthoritative = [&](const auto& frames) {
        bool all = true;
        for (const auto& [frame, state] : frames)
            if (frame >= start && frame < start + kWrite)
                all = all && state == frame - start;
        return all;
    };

    // 1. 再生前に mask が memory にある。
    check(controller->seekTimelineFrame(0) && pump([&] {
              return submittedHasWrite(*controller) && controller->previewPresentedLatest();
          }),
          "1: 一時停止中に mask を読み、合成に Write を付ける");
    observation->clear();
    const auto rebuildsBefore = controller->playbackRebuildCount();
    check(controller->canPlay() && controller->playTimeline(), "1: 先頭から再生する");
    check(pump([&] { return controller->playheadFrame() >= 60; }), "1: 再生が 60 frame まで進む");
    check(controller->pauseTimeline(), "1: 再生を止める");
    const auto first = observation->snapshot();
    std::int64_t firstMin = std::numeric_limits<std::int64_t>::max();
    for (const auto& [frame, state] : first)
        firstMin = std::min(firstMin, frame);
    std::fprintf(stderr, "1: 記録 %zu 件、最小の frame %lld\n", first.size(),
                 static_cast<long long>(firstMin));
    check(!first.empty() && firstMin <= 2 && timelineAuthoritative(first),
          "1: 再生の先頭から、出力 frame と同じ Write の frame を見せる");

    // 2. 再生前は memory に無い。Write の途中で mask を届ける。
    controller->mathRastersForTest().setResidentMemoryBudget(
        mvm::app::MathRasterCache::kDefaultResidentMemoryBudget);
    controller->mathRastersForTest().holdResidentLoadsForTest(true);
    check(controller->seekTimelineFrame(0) && pump([&] {
              return controller->previewPresentedLatest() && !submittedHasWrite(*controller) &&
                     controller->mathRastersForTest().heldResidentLoadCountForTest() == 1;
          }),
          "2: 先頭へ戻すと mask は memory に無く (読み終えても届けない)、静止を見せる");
    observation->clear();
    check(controller->playTimeline(), "2: 先頭から再生する");
    check(pump([&] { return controller->playheadFrame() >= 30; }), "2: 再生が 30 frame まで進む");
    check(!submittedHasWrite(*controller) && observation->snapshot().empty(),
          "2: mask が届く前は Write を付けない (静止を見せる)");
    const auto released = controller->playheadFrame();
    controller->mathRastersForTest().holdResidentLoadsForTest(false);
    check(pump([&] { return controller->playheadFrame() >= released + 40; }),
          "2: 一時停止せずに再生が進む");
    const bool stillPlaying = controller->playing();
    check(controller->pauseTimeline(), "2: 再生を止める");
    const auto second = observation->snapshot();
    std::int64_t secondMin = std::numeric_limits<std::int64_t>::max();
    std::int64_t secondMax = -1;
    for (const auto& [frame, state] : second) {
        secondMin = std::min(secondMin, frame);
        secondMax = std::max(secondMax, frame);
    }
    std::fprintf(stderr, "2: 届けた時の再生位置 %lld、記録 %zu 件 (frame %lld..%lld)\n",
                 static_cast<long long>(released), second.size(), static_cast<long long>(secondMin),
                 static_cast<long long>(secondMax));
    check(stillPlaying && controller->playbackRebuildCount() == rebuildsBefore,
          "2: mask が届いても再生を止めず、組み直し (seek) もしない");
    check(!second.empty() && secondMin >= released - 10 && secondMin < start + kWrite,
          "2: 届いた後の再生中の frame から Write を見せる (Write の区間の中で始まる)");
    check(timelineAuthoritative(second),
          "2: Write の frame は timeline の frame から決まり、0 からやり直さない");
    check(std::none_of(second.begin(), second.end(),
                       [](const auto& item) { return item.first > 0 && item.second == 0; }),
          "2: 途中から見せ始めた Write に frame 0 を出さない");
    controller->shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--native-write") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        return nativeWritePlayback();
    }
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
        check(controller->selectedMathClip()
                      .value(QStringLiteral("unavailableReason"))
                      .toString()
                      .isEmpty() &&
                  controller->selectedMathClip().value(QStringLiteral("canRetry")).toBool(),
              "利用可能な描画には利用不可の理由が無く、再試行できる");
        const auto beforeInvalid = controller->projectForTest();
        const auto invalidUndo = controller->undoDepthForTest();
        const auto invalidKey = controller->mathRastersForTest().keyFor({"latex", "x^2", 96});
        for (const auto& source : {QString(), QStringLiteral(" \n\t")}) {
            check(!controller->updateMathClip(clipId, {{QStringLiteral("source"), source}}),
                  "空の構造入力の確定を拒否する");
            check(controller->projectForTest() == beforeInvalid &&
                      controller->undoDepthForTest() == invalidUndo &&
                      controller->selectedMathClip().value(QStringLiteral("clipId")).toString() ==
                          clipId,
                  "確定の拒否は Project・Undo・選択を変えない");
            check(!invalidKey.isEmpty() &&
                      controller->mathRastersForTest().keyFor({"latex", "x^2", 96}) == invalidKey &&
                      state(*controller, clipId) == QStringLiteral("ready") &&
                      *backend.renders == 1,
                  "確定の拒否は描画済みの要求と画素を無効化しない");
        }
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
        check(controller->mathRastersForTest().readyArtifact({"latex", "x^2", 96}).has_value() &&
                  controller->mathRastersForTest().keyFor({"latex", "BAD", 96}) != invalidKey,
              "Undo で現在の式の artifact が戻り、別の式の key と区別される");
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

        check(controller->selectClip(index(clipId)) && controller->cutSelectedClips(),
              "数式 clip をカットする");
        check(mathClips(*controller).size() == 2 && controller->pasteClips(),
              "カットした数式 clip を貼り付ける");
        const auto pastedId =
            controller->selectedMathClip().value(QStringLiteral("clipId")).toString();
        check(!pastedId.isEmpty() && pastedId != clipId &&
                  controller->selectedMathClip().value(QStringLiteral("source")) ==
                      QStringLiteral("x^2") &&
                  controller->selectedMathClip().value(QStringLiteral("color")) ==
                      QStringLiteral("#FFFF0000"),
              "カット・貼り付けは新しい ID と数式・書式を保つ");
        check(controller->undoLastEdit() && controller->undoLastEdit() &&
                  mathClips(*controller).size() == 3,
              "カットと貼り付けを Undo して元の clip 群へ戻す");

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
        check(controller->updateMathClip(clipId,
                                         {{QStringLiteral("source"), QStringLiteral("GONE")}}),
              "描画中に backend が消える式を確定する");
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("unavailable"); }),
              "描画時の backend 不在も利用不可になる");
        check(controller->mathClipData(clipId)
                          .value(QStringLiteral("unavailableReason"))
                          .toString() == QStringLiteral("backend") &&
                  controller->mathClipData(clipId).value(QStringLiteral("canRetry")).toBool(),
              "描画時の backend 不在は導入案内と再試行の対象になる");
        controller->setMathPreflightForTest(
            FakeMathBackend::unavailable("LaTeX (latex.exe) が PATH に見つかりません"));
        check(pump([&] { return state(*controller, clipId) == QStringLiteral("unavailable"); }),
              "backend が使えなければ unavailable");
        check(controller->mathClipData(clipId)
                          .value(QStringLiteral("unavailableReason"))
                          .toString() == QStringLiteral("backend") &&
                  controller->mathClipData(clipId).value(QStringLiteral("canRetry")).toBool(),
              "確認時の依存不足を権限不足と区別し、再試行できる");
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
        const auto exportCalls = captured->calls;
        check(!controller->exportTimeline(QUrl::fromLocalFile(temp.filePath("pending.mp4"))) &&
                  captured->calls == exportCalls,
              "描画が未完了の式を含む書き出しは runner を呼ばず拒否する");
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
            auto intruder = makeController(shared, owner->projectForTest(), captured);
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
            const auto blockedId = QString::fromStdString(mathClips(*owner)[0]->id);
            const auto blocked = intruder->mathClipData(blockedId);
            check(blocked.value(QStringLiteral("state")).toString() ==
                          QStringLiteral("unavailable") &&
                      blocked.value(QStringLiteral("unavailableReason")).toString() ==
                          QStringLiteral("authority") &&
                      !blocked.value(QStringLiteral("canRetry")).toBool(),
                  "2 つ目の instance は権限不足を返し、依存導入・再試行の対象にしない");
            check(!blocked.value(QStringLiteral("message")).toString().isEmpty(),
                  "権限不足でも理由の表示文を残す");
            intruder->retryMathRendering();
            settle(100);
            check(*preflights == 0 && ownerJobsAlive(),
                  "権限不足への再試行は外部 renderer を起動せず所有者の cache を変更しない");
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

    testWrite(temp, initial, captured);
    testWriteResidencyBudget(temp, initial, captured);
    testWriteBeyondBackendCapability(temp, initial, captured);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
