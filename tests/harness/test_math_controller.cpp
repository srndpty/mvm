#include "app/preview/test_window_mode.h"
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
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "math_fake_backend.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/equation_sequence_edit.h"
#include "project/graph_edit.h"
#include "project/project_json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QProcess>
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
    std::optional<mvm::app::TimelineExportRequest> transformRequest;
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

// 数式の変形 (P2-1): controller を通した編集の不変条件・Undo / Redo・保存と開き直し・
// コピー / 複製・書き出しの拒否。描画と preview の変形はまだ無い (Project / timeline だけ)。
void testMathTransformEditing(const QTemporaryDir& temp,
                              const std::shared_ptr<CapturedExport>& captured) {
    const auto math = [](const std::string& id, const std::string& source, qint64 start) {
        project::TimelineClip clip;
        clip.kind = project::TimelineClipKind::Math;
        clip.id = id;
        clip.name = id;
        clip.sourceFpsNum = 60;
        clip.sourceFpsDen = 1;
        clip.sourceFrameCount = clip.sourceOutFrame = 300;
        clip.timelineStartFrame = start;
        clip.math.source = source;
        return clip;
    };
    auto initial = project::createDefaultProject();
    initial.timelineClips = {math("A", "x^2 + \\frac{b}{a}x = -\\frac{c}{a}", 0),
                             math("B", "\\left(x + \\frac{b}{2a}\\right)^2 = c", 300)};
    initial.timelineTransitions = {
        {"t1", "A", "B", 60, 120, project::TransitionKind::MathTransform}};
    const auto path = std::filesystem::path(temp.filePath("transform.mvm").toStdWString());
    check(project::saveProjectJson(initial, path).success, "変形: project の保存");
    const auto transitions = [](const MvmController& controller) {
        return controller.projectForTest().timelineTransitions;
    };
    const auto only = [&](const MvmController& controller, const std::string& outgoing,
                          qint64 before, qint64 after) {
        const auto list = transitions(controller);
        return list.size() == 1 && list[0].kind == project::TransitionKind::MathTransform &&
               list[0].outgoingClipId == outgoing && list[0].incomingClipId == "B" &&
               list[0].framesBeforeCut == before && list[0].framesAfterCut == after;
    };
    {
        const auto loaded = project::loadProjectJson(path);
        check(loaded.success && loaded.project.timelineTransitions == initial.timelineTransitions,
              "変形: 保存した変形を読める");
        auto controller = makeController(path, initial, captured);
        check(only(*controller, "A", 60, 120), "変形: controller が変形を持つ");

        // 条件を壊す確定は拒否し、Project と Undo を変えない。
        const auto before = controller->projectForTest();
        const auto depth = controller->undoDepthForTest();
        check(!controller->updateMathClip(QStringLiteral("B"),
                                          {{QStringLiteral("intro"), QStringLiteral("write")}}) &&
                  controller->projectForTest() == before && controller->undoDepthForTest() == depth,
              "変形の後ろの clip への Write を拒否する");
        check(!controller->updateMathClip(QStringLiteral("A"), {{QStringLiteral("backgroundColor"),
                                                                 QStringLiteral("#80000000")}}) &&
                  controller->projectForTest() == before && controller->undoDepthForTest() == depth,
              "変形の clip への背景を拒否する");
        check(controller->updateMathClip(QStringLiteral("A"),
                                         {{QStringLiteral("source"), QStringLiteral("x^2 = y")}}) &&
                  only(*controller, "A", 60, 120),
              "式を変えても変形は残る (端点は clip を参照する)");
        check(controller->undoLastEdit(), "変形: 式の変更を Undo");

        // trim と Undo / Redo。
        check(controller->trimClip(QStringLiteral("B"), QStringLiteral("right"), -200, false) &&
                  only(*controller, "A", 60, 100),
              "B を 100 frame にすると後ろは 100");
        check(controller->undoLastEdit() && only(*controller, "A", 60, 120), "trim の Undo");
        check(controller->redoLastEdit() && only(*controller, "A", 60, 100), "trim の Redo");
        check(controller->undoLastEdit() && only(*controller, "A", 60, 120),
              "trim をもう一度 Undo");

        // split の Undo。
        check(controller->splitClipAt(QStringLiteral("A"), 100, false, false) &&
                  transitions(*controller).size() == 1 &&
                  transitions(*controller)[0].outgoingClipId != "A",
              "A を分けると右側が前の clip");
        check(controller->undoLastEdit() && only(*controller, "A", 60, 120), "split の Undo");

        // 削除と Undo。
        check(controller->deleteTimelineClip(QStringLiteral("B")) &&
                  transitions(*controller).empty(),
              "後ろの clip を削除すると変形は消える");
        check(controller->undoLastEdit() && transitions(*controller) == initial.timelineTransitions,
              "削除の Undo で変形 (種類を含む) が戻る");

        // コピー / 貼り付け / 複製は clip だけを写す (Blend と同じ)。
        check(controller->selectTimelineClips({QStringLiteral("A"), QStringLiteral("B")}) &&
                  controller->copySelectedClips(),
              "変形の両端をコピーする");
        const auto clipCount = controller->projectForTest().timelineClips.size();
        check(controller->pasteClips() &&
                  controller->projectForTest().timelineClips.size() == clipCount + 2 &&
                  only(*controller, "A", 60, 120),
              "貼り付けは clip だけを写し、変形を写さない");
        check(controller->selectTimelineClips({QStringLiteral("A"), QStringLiteral("B")}) &&
                  controller->duplicateSelectedClips() &&
                  controller->projectForTest().timelineClips.size() == clipCount + 4 &&
                  only(*controller, "A", 60, 120),
              "複製は clip だけを写し、変形を写さない");
        check(controller->undoLastEdit() && controller->undoLastEdit() &&
                  controller->projectForTest().timelineClips.size() == clipCount,
              "貼り付けと複製の Undo");

        check(controller->saveProject(), "変形: 保存する");
        controller->shutdown();
    }
    {
        const auto loaded = project::loadProjectJson(path);
        check(loaded.success && loaded.project.timelineTransitions == initial.timelineTransitions,
              "変形: 開き直しても変形 (種類・長さ) は同じ");
        // 書き出しの描画はまだ無い。出力する変形を cut で黙って置き換えず拒否する。
        mvm::app::TimelineExportRequest request;
        request.outputPath = temp.filePath("transform.mp4").toStdWString();
        request.width = loaded.project.outputWidth;
        request.height = loaded.project.outputHeight;
        request.fpsNum = static_cast<int>(loaded.project.timelineFpsNum);
        request.fpsDen = static_cast<int>(loaded.project.timelineFpsDen);
        const auto plan = mvm::app::mapTimelineExportPlan(loaded.project, request);
        check(!plan.error.empty() && plan.error.find("数式の変形") != std::string::npos,
              "出力する変形のある書き出しは拒否する: " + plan.error);
        auto disabled = loaded.project;
        disabled.timelineClips[0].enabled = false;
        const auto disabledPlan = mvm::app::mapTimelineExportPlan(disabled, request);
        check(disabledPlan.error.find("数式の変形") == std::string::npos,
              "対照: 片方を無効にした変形は書き出しを止めない: " + disabledPlan.error);
    }
}

// ---- 数式の変形の preview (P2-5) ----
//
// A (timeline 0..300) と B (300..600) の間の変形 t1 (cut の前 10・後 20、30 枚)。区間は
// timeline frame 290..319 で、frame f は変形の frame f - 290。偽の backend の変形は、frame 0 が
// A の静止、frame 1 以降が B の静止と canvas の (2, 3) の 1 画素 (alpha 200)。canvas は大きい方の
// 端点 + 各辺 6 画素。期待値はここで手で数えた式で作る (実装の配置関数を呼ばない)。

constexpr std::int64_t kTransformCut = 300;
constexpr std::int64_t kTransformBefore = 10;
constexpr std::int64_t kTransformAfter = 20;
constexpr std::int64_t kTransformFrames = kTransformBefore + kTransformAfter;
constexpr std::int64_t kTransformStart = kTransformCut - kTransformBefore;
constexpr int kOutputWidth = 1920;
constexpr int kOutputHeight = 1080;

project::TimelineClip transformEndpoint(const std::string& id, const std::string& source,
                                        qint64 start, const std::string& color) {
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Math;
    clip.id = id;
    clip.name = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = clip.sourceOutFrame = 300;
    clip.timelineStartFrame = start;
    clip.math.source = source;
    clip.math.color = color;
    return clip;
}

project::Project transformProject(const std::string& sourceA, const std::string& sourceB,
                                  const std::string& colorA = "#FFFFFFFF",
                                  const std::string& colorB = "#FFFFFFFF",
                                  std::int64_t before = kTransformBefore,
                                  std::int64_t after = kTransformAfter) {
    auto initial = project::createDefaultProject();
    initial.timelineClips = {transformEndpoint("A", sourceA, 0, colorA),
                             transformEndpoint("B", sourceB, kTransformCut, colorB)};
    initial.timelineTransitions = {
        {"t1", "A", "B", before, after, project::TransitionKind::MathTransform}};
    return initial;
}

QString transitionValue(const MvmController& controller, const char* key) {
    return controller.selectedTransition().value(QString::fromLatin1(key)).toString();
}

std::unique_ptr<MvmController> openTransformProject(const QTemporaryDir& temp, const QString& name,
                                                    const project::Project& initial,
                                                    const std::shared_ptr<CapturedExport>& captured,
                                                    const FakeMathBackend& backend) {
    const auto path = std::filesystem::path(temp.filePath(name).toStdWString());
    check(project::saveProjectJson(initial, path).success, "変形 preview: project の保存");
    auto controller = makeController(path, initial, captured);
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "変形 preview: 偽の backend が使える");
    check(controller->selectTransition(QStringLiteral("t1")), "変形 preview: 変形を選ぶ");
    return controller;
}

bool waitTransformState(MvmController& controller, const QString& wanted) {
    return pump([&] {
        controller.selectTransition(QStringLiteral("t1"));
        return transitionValue(controller, "transformState") == wanted;
    });
}

// frame の数式の layer に、変形の animation が付くまで合成を作り直す (preview 用の mask は
// 合成が要求してから別の worker で読む)。
std::optional<mvm::preview::PreviewCompositionLayer>
waitForAnimatedLayer(const MvmController& controller, qint64 frame) {
    std::optional<mvm::preview::PreviewCompositionLayer> layer;
    pump([&] {
        layer = mathLayer(controller, frame);
        return layer && layer->stillAnimation;
    });
    return layer;
}

// still に、animation が frame で見せる patch を重ねた画素 (engine が texture の矩形を
// 置き換えるのと同じ)。state が負なら still のまま。
std::vector<std::uint8_t> presentedPixels(const mvm::preview::PreviewStillImage& still,
                                          const mvm::preview::PreviewStillAnimation* animation,
                                          std::int64_t frame) {
    auto rgba = still.rgba;
    if (!animation)
        return rgba;
    const auto state = animation->stateAt(frame);
    if (state < 0)
        return rgba;
    const auto rect = animation->patchRect();
    // engine は patch の作業領域を state をまたいで使い回す。前の state の画素が残っていても
    // fillPatch が矩形の全画素を書くことを確かめるため、無関係な値で埋めてから呼ぶ。
    std::vector<std::uint8_t> patch(
        static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 4U, 0xCD);
    animation->fillPatch(state, patch.data());
    const std::size_t row = static_cast<std::size_t>(rect.width) * 4U;
    for (int y = 0; y < rect.height; ++y)
        std::memcpy(rgba.data() + (static_cast<std::size_t>(rect.y + y) *
                                       static_cast<std::size_t>(still.width) +
                                   static_cast<std::size_t>(rect.x)) *
                                      4U,
                    patch.data() + static_cast<std::size_t>(y) * row, row);
    return rgba;
}

std::vector<std::uint8_t> presentedPixels(const mvm::preview::PreviewCompositionLayer& layer,
                                          std::int64_t frame) {
    return presentedPixels(*layer.stillImage, layer.stillAnimation.get(), frame);
}

std::vector<std::uint8_t> transparentOutput() {
    return std::vector<std::uint8_t>(
        static_cast<std::size_t>(kOutputWidth) * static_cast<std::size_t>(kOutputHeight) * 4U, 0);
}

void paintPixel(std::vector<std::uint8_t>& rgba, int x, int y, std::uint32_t rgb,
                std::uint8_t alpha) {
    const auto at = (static_cast<std::size_t>(y) * kOutputWidth + static_cast<std::size_t>(x)) * 4U;
    rgba[at] = static_cast<std::uint8_t>(rgb >> 16);
    rgba[at + 1] = static_cast<std::uint8_t>(rgb >> 8);
    rgba[at + 2] = static_cast<std::uint8_t>(rgb);
    rgba[at + 3] = alpha;
}

void paintBlock(std::vector<std::uint8_t>& rgba, int left, int top, int width, int height,
                std::uint32_t rgb) {
    for (int y = top; y < top + height; ++y)
        for (int x = left; x < left + width; ++x)
            paintPixel(rgba, x, y, rgb, 255);
}

std::vector<std::uint8_t> pixelAt(const std::vector<std::uint8_t>& rgba, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * kOutputWidth + static_cast<std::size_t>(x)) * 4U;
    return {rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
}

std::size_t differingPixels(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                            int* lastX = nullptr, int* lastY = nullptr) {
    if (a.size() != b.size())
        return std::numeric_limits<std::size_t>::max();
    std::size_t count = 0;
    for (std::size_t at = 0; at < a.size(); at += 4)
        if (std::memcmp(a.data() + at, b.data() + at, 4) != 0) {
            ++count;
            if (lastX)
                *lastX = static_cast<int>((at / 4) % kOutputWidth);
            if (lastY)
                *lastY = static_cast<int>((at / 4) / kOutputWidth);
        }
    return count;
}

// 偽の backend の変形を、手で数えた配置で出力 raster に置いた期待値。
struct TransformGeometry {
    int sw, sh, tw, th;
    // 静止の配置 (出力の中央、余りは左上)。
    int staticAX, staticAY, staticBX, staticBY;
    // canvas の中の端点 (canvas の中央、余りは左上)。canvas は大きい方 + 各辺 6。
    int canvasAX, canvasAY, canvasBX, canvasBY;
    // 出力での artifact の左上。artifact の左上は canvas の (2, 3) (追加の 1 画素) なので、
    // 端点の静止が静止の配置に重なる位置は 静止の配置 - canvas の中の端点 + (2, 3)。
    int originAX, originAY, originBX, originBY;

    TransformGeometry(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight)
        : sw(sourceWidth), sh(sourceHeight), tw(targetWidth), th(targetHeight) {
        staticAX = (kOutputWidth - sw) / 2;
        staticAY = (kOutputHeight - sh) / 2;
        staticBX = (kOutputWidth - tw) / 2;
        staticBY = (kOutputHeight - th) / 2;
        const int canvasWidth = std::max(sw, tw) + 12;
        const int canvasHeight = std::max(sh, th) + 12;
        canvasAX = (canvasWidth - sw) / 2;
        canvasAY = (canvasHeight - sh) / 2;
        canvasBX = (canvasWidth - tw) / 2;
        canvasBY = (canvasHeight - th) / 2;
        originAX = staticAX - canvasAX + 2;
        originAY = staticAY - canvasAY + 3;
        originBX = staticBX - canvasBX + 2;
        originBY = staticBY - canvasBY + 3;
    }

    // 端点の差は 1 画素以内。進み具合 t/N が 1/2 以上で target の位置に移る。
    static int originAt(int from, int to, std::int64_t t) {
        return from != to && 2 * t >= kTransformFrames ? to : from;
    }

    // frame t (0 <= t < N) の出力。
    std::vector<std::uint8_t> frame(std::int64_t t, std::uint32_t rgb) const {
        auto rgba = transparentOutput();
        if (t == 0) {
            paintBlock(rgba, staticAX, staticAY, sw, sh, rgb);
            return rgba;
        }
        const int left = originAt(originAX, originBX, t);
        const int top = originAt(originAY, originBY, t);
        paintBlock(rgba, left + canvasBX - 2, top + canvasBY - 3, tw, th, rgb);
        paintPixel(rgba, left, top, rgb, 200);
        return rgba;
    }

    std::vector<std::uint8_t> staticA(std::uint32_t rgb) const {
        auto rgba = transparentOutput();
        paintBlock(rgba, staticAX, staticAY, sw, sh, rgb);
        return rgba;
    }

    std::vector<std::uint8_t> staticB(std::uint32_t rgb) const {
        auto rgba = transparentOutput();
        paintBlock(rgba, staticBX, staticBY, tw, th, rgb);
        return rgba;
    }
};

std::string sizeSource(int width, int height, const std::string& suffix) {
    return "SIZE" + std::to_string(width) + "x" + std::to_string(height) + " " + suffix;
}

// 端点の偶奇の組 1 つ: frame 0 は A の静止と画素で一致し、最後の frame の次の B の静止へ
// 段差なく続き、途中の frame は手で数えた位置に置かれる。前 (A) と後ろ (B) の layer の
// animation は区間のどの frame でも同じ画素を見せる。
void checkTransformParity(const QTemporaryDir& temp,
                          const std::shared_ptr<CapturedExport>& captured, int sw, int sh, int tw,
                          int th) {
    const std::string what = "偶奇 " + std::to_string(sw) + "x" + std::to_string(sh) + "→" +
                             std::to_string(tw) + "x" + std::to_string(th);
    const TransformGeometry geometry(sw, sh, tw, th);
    std::fprintf(stderr, "%s: 端点の左上 A (%d,%d) B (%d,%d)\n", what.c_str(), geometry.originAX,
                 geometry.originAY, geometry.originBX, geometry.originBY);
    FakeMathBackend backend;
    auto controller = openTransformProject(
        temp,
        QString::fromStdString("parity " + std::to_string(sw) + "x" + std::to_string(sh) + "-" +
                               std::to_string(tw) + "x" + std::to_string(th) + ".mvm"),
        transformProject(sizeSource(sw, sh, "a"), sizeSource(tw, th, "b")), captured, backend);
    check(waitTransformState(*controller, QStringLiteral("ready")), what + ": 変形が disk に揃う");
    const auto a = waitForAnimatedLayer(*controller, kTransformStart + 5);
    const auto b = waitForAnimatedLayer(*controller, kTransformCut + 10);
    if (!a || !a->stillAnimation || !b || !b->stillAnimation) {
        check(false, what + ": 前・後ろの layer に変形の animation が付く");
        controller->shutdown();
        return;
    }
    // timeline の frame から変形の frame を選ぶ (cut の前後とも、区間の外は静止)。
    for (const auto* animation : {a->stillAnimation.get(), b->stillAnimation.get()})
        check(animation->stateAt(kTransformStart - 1) == -1 &&
                  animation->stateAt(kTransformStart) == 0 &&
                  animation->stateAt(kTransformCut - 1) == kTransformBefore - 1 &&
                  animation->stateAt(kTransformCut) == kTransformBefore &&
                  animation->stateAt(kTransformStart + kTransformFrames - 1) ==
                      kTransformFrames - 1 &&
                  animation->stateAt(kTransformStart + kTransformFrames) == -1,
              what + ": 区間の i 番目の timeline frame は変形の frame i");

    const auto plainA = mathLayer(*controller, 100);
    const auto plainB = mathLayer(*controller, 500);
    check(plainA && plainA->stillImage == a->stillImage && plainB &&
              plainB->stillImage == b->stillImage,
          what + ": 変形の layer の下地は両端の普通の静止の画素");
    check(plainA && presentedPixels(*plainA, 100) == geometry.staticA(0xFFFFFF),
          what + ": 対照: 普通の A の静止は手で数えた位置");
    check(plainB && presentedPixels(*plainB, 500) == geometry.staticB(0xFFFFFF),
          what + ": 対照: 普通の B の静止は手で数えた位置");
    check(plainA && presentedPixels(*a, kTransformStart) == presentedPixels(*plainA, 100),
          what + ": 変形の frame 0 は普通の A の静止の preview と画素で一致");
    for (const std::int64_t t :
         {std::int64_t{0}, std::int64_t{1}, kTransformFrames / 2 - 1, kTransformFrames / 2,
          kTransformFrames / 2 + 1, kTransformFrames - 1}) {
        const auto expected = geometry.frame(t, 0xFFFFFF);
        const auto fromA = presentedPixels(*a, kTransformStart + t);
        const auto fromB = presentedPixels(*b, kTransformStart + t);
        check(fromA == expected, what + ": 前の layer の frame " + std::to_string(t) +
                                     " は手で数えた位置 (違う画素 " +
                                     std::to_string(differingPixels(fromA, expected)) + ")");
        check(fromB == expected, what + ": 後ろの layer の frame " + std::to_string(t) +
                                     " は手で数えた位置 (違う画素 " +
                                     std::to_string(differingPixels(fromB, expected)) + ")");
    }
    // 最後の frame の次は普通の B の静止。違うのは偽の変形が足した 1 画素だけ (glyph は動かない)。
    int x = -1;
    int y = -1;
    const auto last = presentedPixels(*b, kTransformStart + kTransformFrames - 1);
    const auto after = presentedPixels(*b, kTransformStart + kTransformFrames);
    check(after == presentedPixels(*plainB, 500) && differingPixels(last, after, &x, &y) == 1 &&
              x == geometry.originBX && y == geometry.originBY,
          what + ": 最後の frame から B の静止へ glyph が跳ばない (違うのは追加の 1 画素だけ: " +
              std::to_string(x) + "," + std::to_string(y) + ")");
    controller->shutdown();
}

void testMathTransformPreviewParity(const QTemporaryDir& temp,
                                    const std::shared_ptr<CapturedExport>& captured) {
    const auto initial = project::createDefaultProject();
    check(initial.timelineFpsNum == 60 && initial.timelineFpsDen == 1 &&
              initial.outputWidth == kOutputWidth && initial.outputHeight == kOutputHeight,
          "変形 preview: 既定の project は 60 fps・1920x1080 (期待値の前提)");
    // 幅は奇 → 偶・高さは偶 → 奇 (端点の左上は横 +1、縦 -1 ずれる)。
    checkTransformParity(temp, captured, 7, 2, 4, 5);
    // 逆向き (横 -1、縦 +1)。
    checkTransformParity(temp, captured, 4, 5, 7, 2);
    // 偶奇が同じ (偶数どうし・奇数どうし)。
    checkTransformParity(temp, captured, 6, 4, 4, 2);
    checkTransformParity(temp, captured, 5, 3, 9, 7);
    // 偶奇は違うが、大きい方が偶数 (canvas と出力の偶奇が同じ) なのでずれない。
    checkTransformParity(temp, captured, 8, 3, 5, 6);
    const TransformGeometry shifted(7, 2, 4, 5);
    check(shifted.originBX - shifted.originAX == 1 && shifted.originBY - shifted.originAY == -1,
          "対照: 7x2→4x5 は端点の左上が横・縦とも 1 画素ずれる組である");
}

// 色の補間: A #FF102030 → B #FF5021F0 (30 枚)。期待値は手で数えた値。
void testMathTransformPreviewColors(const QTemporaryDir& temp,
                                    const std::shared_ptr<CapturedExport>& captured) {
    FakeMathBackend backend;
    auto controller = openTransformProject(
        temp, QStringLiteral("transform colors.mvm"),
        transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "b"), "#FF102030", "#FF5021F0"),
        captured, backend);
    check(waitTransformState(*controller, QStringLiteral("ready")), "色: 変形が disk に揃う");
    const auto b = waitForAnimatedLayer(*controller, kTransformCut + 10);
    if (!b || !b->stillAnimation) {
        check(false, "色: 変形の animation が付く");
        controller->shutdown();
        return;
    }
    const TransformGeometry geometry(7, 2, 4, 5);
    // frame 0: A の色。
    check(pixelAt(presentedPixels(*b, kTransformStart), geometry.staticAX, geometry.staticAY) ==
              std::vector<std::uint8_t>{0x10, 0x20, 0x30, 255},
          "色: frame 0 は A の文字色");
    // frame 14: R 16 + 64·14/30 = 45.9→46、G 32 + 14/30 = 32.47→32、B 48 + 192·14/30
    // = 137.6→138。B の glyph の左上は A の位置 (952,535) + (5,3) = (957,538)。
    const auto at14 = presentedPixels(*b, kTransformStart + 14);
    check(pixelAt(at14, 957, 538) == std::vector<std::uint8_t>{0x2E, 0x20, 0x8A, 255},
          "色: frame 14 の文字色");
    check(pixelAt(at14, 961, 541)[3] == 0,
          "色: frame 14 の glyph はまだ A の位置 (右下の外は透明)");
    // frame 15 (進み具合 1/2): G 32.5 は切り上げて 33、R 48、B 144。glyph は B の位置 (958,537)。
    const auto at15 = presentedPixels(*b, kTransformStart + 15);
    check(pixelAt(at15, 958, 537) == std::vector<std::uint8_t>{0x30, 0x21, 0x90, 255} &&
              pixelAt(at15, 961, 541) == std::vector<std::uint8_t>{0x30, 0x21, 0x90, 255},
          "色: frame 15 の文字色 (0.5 は切り上げ) と位置");
    // 追加の 1 画素 (被覆 200) は同じ色で alpha 200。位置は artifact の左上 (953,534)。
    check(pixelAt(at15, 953, 534) == std::vector<std::uint8_t>{0x30, 0x21, 0x90, 200},
          "色: 被覆 200 の画素は alpha 200 の同じ色 (背景は透明)");
    // frame 29: R 16 + 64·29/30 = 77.9→78、G 32.97→33、B 48 + 192·29/30 = 233.6→234。
    check(pixelAt(presentedPixels(*b, kTransformStart + 29), 958, 537) ==
              std::vector<std::uint8_t>{0x4E, 0x21, 0xEA, 255},
          "色: 最後の frame の文字色");
    check(pixelAt(presentedPixels(*b, kTransformStart + kTransformFrames), 958, 537) ==
              std::vector<std::uint8_t>{0x50, 0x21, 0xF0, 255},
          "色: 区間の後は B の静止の文字色");
    controller->shutdown();
}

// 両端に同じ一定の ClipEffects (位置 X 10%・不透明度 50%) を付ける。ClipEffects は普通の合成の
// layer に 1 回だけ掛かり、変形の画素 (patch) には入らない。
void testMathTransformPreviewEffects(const QTemporaryDir& temp,
                                     const std::shared_ptr<CapturedExport>& captured) {
    auto initial = transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "b"));
    for (auto& clip : initial.timelineClips) {
        clip.effects.positionXPercent = 10;
        clip.effects.opacityPercent = 50;
    }
    check(project::validateTimeline(initial).success,
          "効果: 両端で等しく一定の ClipEffects は変形の条件を満たす");
    FakeMathBackend backend;
    auto controller = openTransformProject(temp, QStringLiteral("transform effects.mvm"), initial,
                                           captured, backend);
    check(waitTransformState(*controller, QStringLiteral("ready")), "効果: 変形が disk に揃う");
    const auto a = waitForAnimatedLayer(*controller, kTransformStart + 5);
    const auto b = waitForAnimatedLayer(*controller, kTransformCut + 10);
    const auto plainA = mathLayer(*controller, 100);
    const auto plainB = mathLayer(*controller, 500);
    if (!a || !b || !plainA || !plainB || !a->stillAnimation || !b->stillAnimation) {
        check(false, "効果: 4 つの frame の数式の layer");
        controller->shutdown();
        return;
    }
    const auto sameEffects = [](const mvm::preview::PreviewCompositionLayer& left,
                                const mvm::preview::PreviewCompositionLayer& right) {
        return left.destination == right.destination && left.sourceRect == right.sourceRect &&
               left.opacity == right.opacity && left.effectsEnabled == right.effectsEnabled &&
               left.rotationDegrees == right.rotationDegrees && !left.motion && !right.motion;
    };
    check(a->opacity == 0.5F && a->destination.x != 0.0F,
          "効果: 変形の layer に ClipEffects が掛かる (対照: 既定値ではない)");
    check(sameEffects(*a, *plainA) && sameEffects(*b, *plainB) && sameEffects(*a, *b),
          "効果: 変形の layer の ClipEffects は普通の静止の layer と同じ");
    for (const qint64 frame : {kTransformStart + 5, kTransformCut + 10}) {
        QString error;
        const auto composition = controller->subtitleCompositionForTest(frame, error);
        check(composition && composition->layers.size() == 1,
              "効果: 区間の frame の合成は数式の layer 1 枚だけ (二重に重ねない)");
    }
    // 不透明度は合成で掛かる。patch の glyph は被覆 255 のまま (artifact に焼き込まない)。
    const TransformGeometry geometry(7, 2, 4, 5);
    check(pixelAt(presentedPixels(*a, kTransformStart + 5), geometry.originAX + 5,
                  geometry.originAY + 3) == std::vector<std::uint8_t>{255, 255, 255, 255},
          "効果: 変形の画素に不透明度・位置を焼き込まない");
    controller->shutdown();
}

// 変形が使えない間は今の cut のまま (静止を見せる)。古い変形・前に描けた変形は使わない。
void testMathTransformPreviewFallbacks(const QTemporaryDir& temp,
                                       const std::shared_ptr<CapturedExport>& captured) {
    const TransformGeometry geometry(7, 2, 4, 5);
    const auto hardCut = [&](const MvmController& controller, const std::string& what,
                             std::uint32_t colorA) {
        const auto a = mathLayer(controller, kTransformStart + 5);
        const auto b = mathLayer(controller, kTransformCut + 10);
        check(a && !a->stillAnimation && b && !b->stillAnimation &&
                  presentedPixels(*a, kTransformStart + 5) == geometry.staticA(colorA) &&
                  presentedPixels(*b, kTransformCut + 10) == geometry.staticB(0xFFFFFF),
              what + ": cut の前は A、後は B の静止 (hard cut)");
    };
    FakeMathBackend backend;
    backend.transformGate->store(true);
    auto controller = openTransformProject(
        temp, QStringLiteral("transform fallback.mvm"),
        transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "b")), captured, backend);
    auto& cache = controller->mathRastersForTest();
    check(pump([&] {
              return state(*controller, QStringLiteral("A")) == QStringLiteral("ready") &&
                     state(*controller, QStringLiteral("B")) == QStringLiteral("ready") &&
                     backend.transformHeld->load();
          }),
          "Pending: 両端の静止は描け、変形は描画中");
    controller->selectTransition(QStringLiteral("t1"));
    check(transitionValue(*controller, "transformState") == QStringLiteral("rendering") &&
              transitionValue(*controller, "kind") == QStringLiteral("math_transform"),
          "Pending: inspector は変形を描画中と示す");
    hardCut(*controller, "Pending", 0xFFFFFF);
    backend.transformGate->store(false);
    check(waitTransformState(*controller, QStringLiteral("ready")), "Pending の後に変形が揃う");
    const auto spec =
        mvm::app::mathTransformSpecFor(controller->projectForTest().timelineTransitions[0],
                                       controller->projectForTest().timelineClips[0],
                                       controller->projectForTest().timelineClips[1]);
    if (!spec) {
        check(false, "fallback: 変形の描画要求");
        controller->shutdown();
        return;
    }

    // disk は Ready でも、memory に読んでいる間 (Loading) は cut のまま。
    cache.holdResidentLoadsForTest(true);
    hardCut(*controller, "Loading", 0xFFFFFF);
    check(pump([&] { return cache.heldResidentLoadCountForTest() == 1; }),
          "Loading: 合成が mask の読み込みを始める (読み終えても届けない)");
    hardCut(*controller, "Loading (読み終えて保留中)", 0xFFFFFF);
    controller->selectTransition(QStringLiteral("t1"));
    check(transitionValue(*controller, "transformState") == QStringLiteral("ready") &&
              transitionValue(*controller, "transformPreview") == QStringLiteral("loading"),
          "Loading: disk は ready、preview は loading");
    cache.holdResidentLoadsForTest(false);
    const auto resident = waitForAnimatedLayer(*controller, kTransformStart + 5);
    check(resident && resident->stillAnimation, "Loading の後は変形を見せる");
    controller->selectTransition(QStringLiteral("t1"));
    check(transitionValue(*controller, "transformPreview") == QStringLiteral("ready"),
          "inspector は preview の mask が使えることを示す");
    const int rendersBefore = *backend.transformRenders;
    check(rendersBefore == 1, "変形を 1 回描く");

    // memory の上限に収まらない (OverBudget): disk は Ready のまま、preview は cut。
    // 変形の mask は 11 x 8 x 30 = 2640 byte (artifact は (2,3)-(13,11))。
    cache.setResidentMemoryBudget(2000);
    hardCut(*controller, "OverBudget", 0xFFFFFF);
    controller->selectTransition(QStringLiteral("t1"));
    check(transitionValue(*controller, "transformState") == QStringLiteral("ready") &&
              transitionValue(*controller, "transformPreview") == QStringLiteral("memory") &&
              transitionValue(*controller, "transformPreviewMessage")
                  .contains(QStringLiteral("memory")) &&
              transitionValue(*controller, "transformMessage").isEmpty(),
          "OverBudget: disk の変形は ready のまま、memory の理由を描画の error と分けて示す");
    check(cache.readyTransform(*spec).has_value(), "OverBudget: disk の変形は使える (書き出し用)");

    // 上限を戻すと disk から読み直す (Manim を起動しない)。
    const int loadsBefore = cache.residentLoadCount();
    cache.setResidentMemoryBudget(mvm::app::MathRasterCache::kDefaultResidentMemoryBudget);
    const auto reloaded = waitForAnimatedLayer(*controller, kTransformStart + 5);
    check(reloaded && reloaded->stillAnimation && cache.residentLoadCount() == loadsBefore + 1 &&
              *backend.transformRenders == rendersBefore,
          "memory に置き直した変形は disk から読み、描き直さない");

    // retainOnly: 変形に関係しない編集 (色) で描画を要求し直しても、今の変形は取り下げない。
    check(controller->updateMathClip(QStringLiteral("A"),
                                     {{QStringLiteral("color"), QStringLiteral("#FFFF0000")}}),
          "A の色を変える");
    settle(200);
    check(cache.readyTransform(*spec).has_value() &&
              cache.transformResidencyOf(*spec).state ==
                  mvm::app::MathRasterCache::Residency::Resident &&
              *backend.transformRenders == rendersBefore,
          "retainOnly は今の変形 (disk と memory) を残し、描き直さない");
    const auto recolored = waitForAnimatedLayer(*controller, kTransformStart + 5);
    check(recolored && pixelAt(presentedPixels(*recolored, kTransformStart), geometry.staticAX,
                               geometry.staticAY) == std::vector<std::uint8_t>{255, 0, 0, 255},
          "色を変えると同じ mask で新しい色の変形を見せる");

    // 式の変更: 変形の key が変わる。古い変形は捨て、新しい変形が描けるまで cut で見せる。
    backend.transformGate->store(true);
    backend.transformHeld->store(false);
    check(controller->updateMathClip(QStringLiteral("A"),
                                     {{QStringLiteral("source"), QStringLiteral("SIZE7x2 a2")}}),
          "A の式を変える");
    check(pump([&] {
              return state(*controller, QStringLiteral("A")) == QStringLiteral("ready") &&
                     backend.transformHeld->load();
          }),
          "新しい変形を描き始める");
    check(!cache.readyTransform(*spec).has_value() && cache.transformRecordCount() == 1,
          "古い式の変形は取り下げる (残るのは新しい変形だけ)");
    hardCut(*controller, "式の変更の後 (古い変形を使わない)", 0xFF0000);
    const auto specFor = [&] {
        const auto& p = controller->projectForTest();
        return mvm::app::mathTransformSpecFor(p.timelineTransitions[0], p.timelineClips[0],
                                              p.timelineClips[1]);
    };
    const auto spec2 = specFor();
    // 描画中にもう一度式を変える: 描きかけの変形の結果は残さない。
    check(controller->updateMathClip(QStringLiteral("A"),
                                     {{QStringLiteral("source"), QStringLiteral("SIZE7x2 a3")}}),
          "描画中に A の式をもう一度変える");
    const auto spec3 = specFor();
    backend.transformGate->store(false);
    check(waitTransformState(*controller, QStringLiteral("ready")), "最後の式の変形が揃う");
    settle(200);
    check(spec2 && spec3 && !cache.readyTransform(*spec2).has_value() &&
              cache.readyTransform(*spec3).has_value() && cache.transformRecordCount() == 1,
          "取り下げた変形の描きかけの結果は確定しない");

    // 長さの変更: frame 数が key に入るので描き直す。
    const int rendersBeforeSpan = *backend.transformRenders;
    check(controller->selectTransition(QStringLiteral("t1")) &&
              controller->setTransitionSpan(10, 10, false),
          "変形の長さを変える (20 frame)");
    const auto spec4 = specFor();
    check(spec4 && spec4->frames == 20 &&
              waitTransformState(*controller, QStringLiteral("ready")) &&
              *backend.transformRenders == rendersBeforeSpan + 1 &&
              !cache.readyTransform(*spec3).has_value(),
          "長さを変えると前の長さの変形を捨てて描き直す");
    const auto shorter = waitForAnimatedLayer(*controller, kTransformStart + 5);
    check(shorter && shorter->stillAnimation &&
              shorter->stillAnimation->stateAt(kTransformStart + 19) == 19 &&
              shorter->stillAnimation->stateAt(kTransformStart + 20) == -1,
          "新しい長さの区間で変形を見せる");
    controller->shutdown();

    // 描画の失敗 (Failed) と backend の不在 (Unavailable) は cut のまま、理由を分けて示す。
    for (const auto& [target, wanted] :
         {std::pair{std::string("BADT"), QStringLiteral("error")},
          std::pair{std::string("GONET"), QStringLiteral("unavailable")}}) {
        FakeMathBackend failing;
        auto failed = openTransformProject(
            temp, QString::fromStdString("transform " + target + ".mvm"),
            transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, target)), captured, failing);
        check(waitTransformState(*failed, wanted),
              target + ": 変形の状態は " + wanted.toStdString());
        check(!transitionValue(*failed, "transformMessage").isEmpty() &&
                  transitionValue(*failed, "transformPreview").isEmpty(),
              target + ": 描画の理由を示し、memory の状態は出さない");
        if (wanted == QStringLiteral("unavailable"))
            check(transitionValue(*failed, "transformUnavailableReason") ==
                      QStringLiteral("backend"),
                  target + ": 依存の不足として示す (導入の案内の対象)");
        hardCut(*failed, target, 0xFFFFFF);
        failed->shutdown();
    }
}

// 前の clip の Write と変形は 1 つの animation にまとまる。Write と区間が重なる確定は拒否する
// (P2-1 の Project の不変条件)。
void testMathTransformWithWrite(const QTemporaryDir& temp,
                                const std::shared_ptr<CapturedExport>& captured) {
    FakeMathBackend backend;
    auto controller = openTransformProject(
        temp, QStringLiteral("transform write.mvm"),
        transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "b")), captured, backend);
    const auto before = controller->projectForTest();
    check(!controller->updateMathClip(QStringLiteral("A"),
                                      {{QStringLiteral("intro"), QStringLiteral("write")},
                                       {QStringLiteral("introSeconds"), 4.9}}) &&
              controller->projectForTest() == before,
          "変形の区間 (290..) に掛かる A の Write (294 frame) は拒否する");
    check(!controller->updateMathClip(QStringLiteral("B"),
                                      {{QStringLiteral("intro"), QStringLiteral("write")}}) &&
              controller->projectForTest() == before,
          "変形の後ろの clip の Write は拒否する");
    check(controller->updateMathClip(QStringLiteral("A"),
                                     {{QStringLiteral("intro"), QStringLiteral("write")},
                                      {QStringLiteral("introSeconds"), 2.0}}),
          "区間の前で終わる A の Write (120 frame) は確定できる");
    check(waitTransformState(*controller, QStringLiteral("ready")) && pump([&] {
              return writeState(*controller, QStringLiteral("A")) == QStringLiteral("ready");
          }),
          "Write と変形が disk に揃う");
    std::optional<mvm::preview::PreviewCompositionLayer> layer;
    pump([&] {
        layer = mathLayer(*controller, 10);
        return layer && layer->stillAnimation &&
               layer->stillAnimation->stateAt(kTransformStart) >= 0 &&
               layer->stillAnimation->stateAt(60) >= 0;
    });
    if (!layer || !layer->stillAnimation) {
        check(false, "Write と変形の animation が付く");
        controller->shutdown();
        return;
    }
    const auto& animation = *layer->stillAnimation;
    check(animation.stateAt(60) == 60 && animation.stateAt(120) == -1 &&
              animation.stateAt(kTransformStart - 1) == -1 &&
              animation.stateAt(kTransformStart) == 120 &&
              animation.stateAt(kTransformCut - 1) == 120 + kTransformBefore - 1,
          "1 つの animation が Write (0..119) と変形 (120 から) の frame を見せる");
    const TransformGeometry geometry(7, 2, 4, 5);
    // Write の frame 60 は左から 7·60/120 = 3 列だけが不透明。
    auto expectedWrite = transparentOutput();
    paintBlock(expectedWrite, geometry.staticAX, geometry.staticAY, 3, 2, 0xFFFFFF);
    check(presentedPixels(*layer, 60) == expectedWrite,
          "Write の frame は静止の位置で、広い矩形の外は静止のまま");
    check(presentedPixels(*layer, 200) == geometry.staticA(0xFFFFFF) &&
              presentedPixels(*layer, kTransformStart) == geometry.staticA(0xFFFFFF) &&
              presentedPixels(*layer, kTransformStart + 20) == geometry.frame(20, 0xFFFFFF),
          "Write の後は静止、区間では変形の frame");
    controller->shutdown();
}

// 入力中の式の静止が、裏で disk に揃えている変形より先に描かれる (P2-5.1)。worker は 1 本なので、
// 描画中・待ちの変形を止めて静止を先に描かせ、止めた変形は後で要求し直して Ready にする。
void testMathTransformYieldsToEditing(const QTemporaryDir& temp,
                                      const std::shared_ptr<CapturedExport>& captured) {
    auto initial = transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "held"));
    // 変形と無関係な数式 clip (同じ track の離れた位置)。
    initial.timelineClips.push_back(
        transformEndpoint("C", sizeSource(3, 3, "c"), 1000, "#FFFFFFFF"));
    FakeMathBackend backend;
    backend.transformCancellableGate->store(true);
    auto controller = openTransformProject(temp, QStringLiteral("transform yields.mvm"), initial,
                                           captured, backend);
    check(pump([&] {
              return state(*controller, QStringLiteral("C")) == QStringLiteral("ready") &&
                     backend.transformHeld->load();
          }),
          "優先: 静止は描け、無関係な変形が描画中で止まっている");
    const auto events = [&] {
        std::lock_guard lock(backend.transformLog->mutex);
        return backend.transformLog->events;
    };
    const auto indexOf = [](const std::vector<std::string>& list, const std::string& event) {
        const auto found = std::find(list.begin(), list.end(), event);
        return found == list.end() ? -1 : static_cast<int>(found - list.begin());
    };
    check(indexOf(events(), "static:SIZE5x5 typed") < 0, "優先: 入力する式の静止はまだ無い (対照)");

    // C の式を入力する: その静止は描けておらず、変形が worker を使っている。
    check(controller->previewMathClip(
              QStringLiteral("C"), {{QStringLiteral("source"), QStringLiteral("SIZE5x5 typed")}}),
          "優先: C の式を入力する");
    check(pump([&] { return state(*controller, QStringLiteral("C")) == QStringLiteral("ready"); },
               5000),
          "優先: 入力中の式の静止が描ける (変形の描画を待たない)");
    const auto afterTyping = events();
    const int cancelled = indexOf(afterTyping, "transform-cancelled:SIZE4x5 held");
    const int typed = indexOf(afterTyping, "static:SIZE5x5 typed");
    check(backend.transformSawCancel->load() && cancelled >= 0 && typed > cancelled &&
              indexOf(afterTyping, "transform:SIZE4x5 held") < 0,
          "優先: 描画中の変形は取消を受け取って止まり、入力中の静止が先に描き終わる");
    check(controller->projectForTest().timelineClips[2].math.source == "SIZE3x3 c",
          "優先: 入力中の preview は Project を変えない");

    // 止めた変形は要求し直されていて、worker が空けば Ready になる。
    check(transitionValue(*controller, "transformState") == QStringLiteral("rendering"),
          "優先: 止めた変形は要求し直されて描画待ち");
    backend.transformCancellableGate->store(false);
    check(waitTransformState(*controller, QStringLiteral("ready")), "優先: 変形が Ready になる");
    const auto finished = events();
    check(indexOf(finished, "transform:SIZE4x5 held") > indexOf(finished, "static:SIZE5x5 typed"),
          "優先: 変形は入力中の静止の後に描き終わる");
    const auto animated = waitForAnimatedLayer(*controller, kTransformStart + 5);
    check(animated && animated->stillAnimation, "優先: 描き直した変形を preview で見せる");
    controller->cancelMathPreview();
    controller->shutdown();
}

// artifact が出力 raster に収まらない変形 (P2-5.1): disk の変形は Ready のまま、preview で使えない
// 理由を disk の状態と分けて示し、preview は cut で見せる。
// A は 1916x2 (出力 1920 に収まる)。偽の変形の artifact は canvas の (2, 3) の 1 画素を含むので、
// 横は canvas の 2..1922 (幅 1920)、A の静止は artifact の中の x = 6 - 2 = 4。出力の中の A の
// 静止は x = (1920 - 1916) / 2 = 2 なので、artifact の左は 2 - 4 = -2 で出力からはみ出す。
void testMathTransformPlacementDoesNotFit(const QTemporaryDir& temp,
                                          const std::shared_ptr<CapturedExport>& captured) {
    FakeMathBackend backend;
    auto controller = openTransformProject(
        temp, QStringLiteral("transform wide.mvm"),
        transformProject(sizeSource(1916, 2, "a"), sizeSource(4, 5, "b")), captured, backend);
    check(waitTransformState(*controller, QStringLiteral("ready")),
          "収まらない: disk の変形は ready");
    const auto& p = controller->projectForTest();
    const auto spec = mvm::app::mathTransformSpecFor(p.timelineTransitions[0], p.timelineClips[0],
                                                     p.timelineClips[1]);
    const auto artifact =
        spec ? controller->mathRastersForTest().readyTransform(*spec) : std::nullopt;
    check(artifact && artifact->width == 1920 && artifact->sourceX == 4,
          "収まらない: artifact は 1920 幅で、A の静止は artifact の x = 4 (手計算の前提)");
    // 合成で preview を使えるか判定させる。
    const auto a = mathLayer(*controller, kTransformStart + 5);
    const auto b = mathLayer(*controller, kTransformCut + 5);
    controller->selectTransition(QStringLiteral("t1"));
    check(transitionValue(*controller, "transformState") == QStringLiteral("ready") &&
              transitionValue(*controller, "transformMessage").isEmpty(),
          "収まらない: disk の状態は ready のまま (描画の error にしない)");
    check(transitionValue(*controller, "transformPreview") == QStringLiteral("placement") &&
              transitionValue(*controller, "transformPreviewMessage")
                  .contains(QStringLiteral("出力")),
          "収まらない: preview で使えない理由を transformPreview に分けて示す");
    check(controller->mathRastersForTest().transformResidencyOf(*spec).state ==
              mvm::app::MathRasterCache::Residency::NotReady,
          "収まらない: preview 用の mask を memory に読まない");
    const TransformGeometry geometry(1916, 2, 4, 5);
    check(a && !a->stillAnimation && b && !b->stillAnimation &&
              presentedPixels(*a, kTransformStart + 5) == geometry.staticA(0xFFFFFF) &&
              presentedPixels(*b, kTransformCut + 5) == geometry.staticB(0xFFFFFF),
          "収まらない: preview は cut の前は A、後は B の静止 (hard cut)");
    controller->shutdown();
}

// reader は GUI thread なら待たずに失敗させるので、旧実装でも試験自体はハングしない。
// worker の reader は、start が返った後に試験が明示解放するまで待機する。
void testMathTransformExportWorker(const QTemporaryDir& temp) {
    const auto initial = transformProject("a", "b");
    const auto path =
        std::filesystem::path(temp.filePath("transform export worker.mvm").toStdWString());
    const auto output = std::filesystem::path(temp.filePath("worker.mp4").toStdWString());
    check(project::saveProjectJson(initial, path).success, "worker: project の保存");
    const auto guiThread = std::this_thread::get_id();
    std::atomic<int> guiLoads{0}, workerLoads{0}, progressCalls{0};
    std::atomic<bool> wrongProgressThread{false}, entered{false}, timedOut{false};
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    auto controller = std::make_unique<MvmController>(
        path, std::filesystem::path{}, initial, nullptr,
        [&](const project::Project& snapshot, const mvm::app::TimelineExportRequest& request) {
            auto observed = request;
            const auto progress = request.progress;
            observed.progress = [&, progress](long long completed, long long total) {
                ++progressCalls;
                if (std::this_thread::get_id() == guiThread)
                    wrongProgressThread = true;
                return progress && progress(completed, total);
            };
            return mvm::app::exportTimeline(snapshot, observed);
        });
    FakeMathBackend backend;
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "worker: 偽の backend が使える");
    controller->selectTransition(QStringLiteral("t1"));
    check(waitTransformState(*controller, QStringLiteral("ready")), "worker: 現在の disk は Ready");
    controller->setMathTransformExportFrameLoaderForTest(
        [&](std::size_t, std::vector<std::uint8_t>&, std::string& error) {
            error = "試験で停止した変形 frame";
            if (std::this_thread::get_id() == guiThread) {
                ++guiLoads;
                return false;
            }
            ++workerLoads;
            std::unique_lock lock(mutex);
            entered = true;
            if (!released.wait_for(lock, std::chrono::seconds(10), [&] { return release; }))
                timedOut = true;
            return false;
        });
    const bool started =
        controller->exportTimeline(QUrl::fromLocalFile(QString::fromStdWString(output.wstring())));
    check(started && guiLoads == 0, "worker: start は frame reader を GUI thread で呼ばずに返る");
    check(started && pump([&] { return entered.load(); }), "worker: reader が worker で待機に入る");
    bool heartbeat = false;
    QMetaObject::invokeMethod(controller.get(), [&] { heartbeat = true; }, Qt::QueuedConnection);
    check(pump([&] { return heartbeat; }) && controller->exporting() && !timedOut,
          "worker: reader の待機中も GUI event を処理し、書き出しは未完了");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    released.notify_all();
    check(pump([&] { return !controller->exporting(); }), "worker: 解放後に検査失敗を通知する");
    check(workerLoads == 1 && guiLoads == 0 && !timedOut && progressCalls > 0 &&
              !wrongProgressThread,
          "worker: frame 検査と progress は worker のみで実行する");
    check(!std::filesystem::exists(output), "worker: frame 検査失敗では出力を作らない");
    controller->shutdown();
}

bool submittedHasWrite(const MvmController& controller) {
    const auto composition = controller.submittedCompositionForTest();
    if (!composition)
        return false;
    return std::any_of(composition->layers.begin(), composition->layers.end(),
                       [](const auto& layer) { return layer.stillImage && layer.stillAnimation; });
}

// engine が前の組み直し (cache の結果による preview の更新) を seek している間は seek を受け付け
// ないので、受け付けるまで繰り返す。
bool seekWhenReady(MvmController& controller, qint64 frame) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (controller.seekTimelineFrame(frame))
            return true;
        settle(50);
    }
    std::fprintf(stderr, "seek できません: %s\n", qUtf8Printable(controller.statusText()));
    return false;
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
    window.setFlags(mvm::app::testBackgroundWindowFlags());
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
    check(seekWhenReady(*controller, 0) && pump([&] {
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
    check(seekWhenReady(*controller, 0) && pump([&] {
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

// ---- 再生中に変形の mask が memory に届く (実 D3D11 preview、--native-transform) ----
//
// A (0..300) → B (300..600)、変形の区間は 240..419 (cut の前 60・後 120、180 枚)。engine の render
// thread が出力 frame ごとに評価した変形の frame を observer で記録し、timeline の frame と比べる
// (P1.2 と同じ方法)。
//   1. 再生前に mask が memory にある: 区間の先頭から frame 0 を見せ、cut の後も後ろの layer で続く
//   2. 再生前は memory に無く、区間の途中 (cut の前) で mask を届ける: 一時停止・seek をせずに、
//      届いた後の frame から変形を見せ、frame 0 からやり直さない (timeline の時刻が authority)
struct TransformObservation {
    std::mutex mutex;
    // (評価した layer の clip、出力 frame、変形の frame)
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> frames;

    void clear() {
        std::lock_guard lock(mutex);
        frames.clear();
    }

    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> snapshot() {
        std::lock_guard lock(mutex);
        return frames;
    }
};

// 記録のうち、変形を見せた (frame >= 0) もの。
std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>
shownTransformFrames(const std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>& all) {
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> shown;
    for (const auto& item : all)
        if (std::get<2>(item) >= 0)
            shown.push_back(item);
    return shown;
}

// すべての記録で「変形の frame = 出力 frame - 区間の先頭」(区間の外は -1)。
bool transformTimelineAuthoritative(
    const std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>& all, std::int64_t start,
    std::int64_t frames) {
    for (const auto& [clip, frame, shown] : all) {
        const std::int64_t want = frame >= start && frame < start + frames ? frame - start : -1;
        if (shown != want)
            return false;
    }
    return true;
}

struct PreviewWindowHarness {
    QQuickWindow window;
    mvm::app::PreviewEngineRhiItem* surface = nullptr;

    void attach(MvmController& controller) {
        // 入力を送らない試験だが、利用者の作業を止めないよう前面とフォーカスを奪わず、OS の
        // マウス入力も透過させる (tests/harness/test_window_focus.h と同じ flags)。
        window.setFlags(mvm::app::testBackgroundWindowFlags());
        window.resize(640, 360);
        surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
        surface->setWidth(640);
        surface->setHeight(360);
        controller.attachPreview(surface);
        window.show();
    }
};

int nativeTransformPlayback() {
    QTemporaryDir temp;
    check(temp.isValid(), "native 変形: 作業フォルダー");
    auto captured = std::make_shared<CapturedExport>();
    FakeMathBackend backend;
    const auto path = std::filesystem::path(temp.filePath("native transform.mvm").toStdWString());
    constexpr std::int64_t kStart = kTransformCut - 60;
    constexpr std::int64_t kFrames = 180;
    const auto initial = transformProject(sizeSource(7, 2, "a"), sizeSource(4, 5, "b"), "#FFFFFFFF",
                                          "#FFFFFFFF", 60, 120);
    check(project::saveProjectJson(initial, path).success, "native 変形: project の保存");
    auto controller = makeController(path, initial, captured);
    PreviewWindowHarness harness;
    harness.attach(*controller);
    check(pump([&] { return controller->previewReady(); }), "native 変形: preview の初期化");
    controller->setMathPreflightForTest(backend.preflight());
    check(pump([&] {
              return controller->mathRastersForTest().backendState() ==
                     mvm::app::MathRasterCache::BackendState::Available;
          }),
          "native 変形: 偽の backend が使える");
    // 選択は 1 回だけ (選び直すたびに preview を組み直させない)。状態は cache の結果で更新される。
    check(controller->selectTransition(QStringLiteral("t1")) && pump([&] {
              return transitionValue(*controller, "transformState") == QStringLiteral("ready");
          }),
          "native 変形: 変形が disk に揃う");
    auto observation = std::make_shared<TransformObservation>();
    controller->setMathTransformObserverForTest(
        [observation](const std::string& transitionId, const std::string& clipId,
                      std::int64_t frame, std::int64_t shown) {
            if (transitionId != "t1")
                return;
            std::lock_guard lock(observation->mutex);
            observation->frames.emplace_back(clipId, frame, shown);
        });
    const auto clipsSeen = [](const auto& shown) {
        std::set<std::string> clips;
        for (const auto& item : shown)
            clips.insert(std::get<0>(item));
        return clips;
    };

    // 1. 再生前に mask が memory にある。
    check(seekWhenReady(*controller, 200) && pump([&] {
              return submittedHasWrite(*controller) && controller->previewPresentedLatest();
          }),
          "1: 一時停止中に mask を読み、合成に変形を付ける");
    observation->clear();
    const auto rebuildsBefore = controller->playbackRebuildCount();
    check(controller->playTimeline(), "1: frame 200 から再生する");
    check(pump([&] { return controller->playheadFrame() >= 400; }), "1: 再生が frame 400 まで進む");
    check(controller->pauseTimeline(), "1: 再生を止める");
    const auto first = observation->snapshot();
    const auto firstShown = shownTransformFrames(first);
    std::int64_t firstMin = std::numeric_limits<std::int64_t>::max();
    for (const auto& item : firstShown)
        firstMin = std::min(firstMin, std::get<1>(item));
    std::fprintf(stderr, "1: 記録 %zu 件 (変形 %zu 件)、変形の最小の frame %lld\n", first.size(),
                 firstShown.size(), static_cast<long long>(firstMin));
    check(!firstShown.empty() && firstMin <= kStart + 2, "1: 区間の先頭から変形を見せる");
    check(transformTimelineAuthoritative(first, kStart, kFrames),
          "1: 変形の frame = 出力 frame - 区間の先頭 (区間の外は静止)");
    check(clipsSeen(firstShown) == std::set<std::string>{"A", "B"},
          "1: cut の前は前の layer、後は後ろの layer が同じ変形を見せる");

    // 2. 再生前は memory に無い。区間の途中 (cut の前) で mask を届ける。
    controller->mathRastersForTest().setResidentMemoryBudget(
        mvm::app::MathRasterCache::kDefaultResidentMemoryBudget);
    controller->mathRastersForTest().holdResidentLoadsForTest(true);
    check(seekWhenReady(*controller, 200) && pump([&] {
              return controller->previewPresentedLatest() && !submittedHasWrite(*controller) &&
                     controller->mathRastersForTest().heldResidentLoadCountForTest() == 1;
          }),
          "2: frame 200 へ戻すと mask は memory に無く (読み終えても届けない)、静止を見せる");
    observation->clear();
    check(controller->playTimeline(), "2: frame 200 から再生する");
    check(pump([&] { return controller->playheadFrame() >= kStart + 30; }),
          "2: 再生が区間の途中 (frame 270) まで進む");
    check(!submittedHasWrite(*controller) && observation->snapshot().empty(),
          "2: mask が届く前は変形を付けない (cut の前は A の静止)");
    const auto released = controller->playheadFrame();
    controller->mathRastersForTest().holdResidentLoadsForTest(false);
    check(pump([&] { return controller->playheadFrame() >= released + 60; }),
          "2: 一時停止せずに再生が cut を越えて進む");
    const bool stillPlaying = controller->playing();
    check(controller->pauseTimeline(), "2: 再生を止める");
    const auto second = observation->snapshot();
    const auto secondShown = shownTransformFrames(second);
    std::int64_t secondMin = std::numeric_limits<std::int64_t>::max();
    std::int64_t secondMax = -1;
    for (const auto& item : secondShown) {
        secondMin = std::min(secondMin, std::get<1>(item));
        secondMax = std::max(secondMax, std::get<1>(item));
    }
    std::fprintf(stderr, "2: 届けた時の再生位置 %lld、変形の記録 %zu 件 (frame %lld..%lld)\n",
                 static_cast<long long>(released), secondShown.size(),
                 static_cast<long long>(secondMin), static_cast<long long>(secondMax));
    check(stillPlaying && controller->playbackRebuildCount() == rebuildsBefore,
          "2: mask が届いても再生を止めず、組み直し (seek) もしない");
    check(!secondShown.empty() && secondMin >= released - 10 && secondMin < kStart + kFrames,
          "2: 届いた後の再生中の frame から変形を見せる (区間の中で始まる)");
    check(transformTimelineAuthoritative(second, kStart, kFrames),
          "2: 変形の frame は timeline の frame から決まり、0 からやり直さない");
    check(std::none_of(secondShown.begin(), secondShown.end(),
                       [](const auto& item) { return std::get<2>(item) == 0; }),
          "2: 途中から見せ始めた変形に frame 0 を出さない");
    check(clipsSeen(secondShown).contains("B"), "2: cut の後も後ろの layer で変形が続く");

    // 3. memory の上限に収まらない変形の区間へ seek する: preview は cut の静止を提示し、
    //    選択中の変形の inspector は (選び直さなくても) memory の理由を示す。
    controller->mathRastersForTest().setResidentMemoryBudget(16);
    check(seekWhenReady(*controller, kStart + 30), "3: 区間の途中へ seek する");
    const bool presented = pump([&] {
        return controller->previewPresentedLatest() && !submittedHasWrite(*controller) &&
               controller->selectedTransition().value("transformPreview").toString() ==
                   QStringLiteral("memory");
    });
    if (!presented)
        std::fprintf(
            stderr, "3: 提示 %d、変形 %d、preview の状態 '%s'\n",
            int(controller->previewPresentedLatest()), int(submittedHasWrite(*controller)),
            qUtf8Printable(controller->selectedTransition().value("transformPreview").toString()));
    check(presented, "3: 上限に収まらない変形は cut で提示し、inspector に memory の理由を出す");
    check(controller->selectedTransition().value("transformState").toString() ==
              QStringLiteral("ready"),
          "3: disk の変形は ready のまま");
    controller->shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}

// ---- 実 Manim の変形の受け入れ (--real-manim-transform、CTest に登録しない) ----
//
//   mvm_test_math_controller --real-manim-transform <manim.exe> <作業 directory (存在しないこと)>
//
// Manim の確認 → 両端の静止 → 変形の cache (.a8) → memory → 実 D3D11 の preview の再生、を製品の
// controller で通す。解の公式の変形 (E1 → E2 → E3 の連鎖) と小さい式の変形を置き、
//   - frame 0 は普通の A の静止の preview と画素で一致する
//   - 最後の frame の artifact の位置で、B の静止 (照合済みの終状態) が普通の B の静止に重なる
//   - 端点の左上がずれる (偶奇の違う) 変形が少なくとも 1 つある
//   - 再生中の engine の評価は「変形の frame = 出力 frame - 区間の先頭」
// を確かめる。画面を表示するが入力は送らない (操作可)。
struct RealTransformCase {
    std::string name;
    std::string source;
    int sourceFont;
    std::string target;
    int targetFont;
};

int realManimTransform(const std::filesystem::path& manim,
                       const std::filesystem::path& workDirectory) {
    std::error_code error;
    if (std::filesystem::exists(workDirectory, error)) {
        std::fprintf(stderr, "作業 directory が既にあります (上書きしません): %s\n",
                     workDirectory.string().c_str());
        return 2;
    }
    std::filesystem::create_directories(workDirectory, error);
    if (error) {
        std::fprintf(stderr, "作業 directory を作れません\n");
        return 2;
    }
    const std::string e1 = "x^2 + \\frac{b}{a}x = -\\frac{c}{a}";
    const std::string e2 = "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                           "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2";
    const std::string e3 = "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}";
    // 解の公式は E1 → E2 → E3 の連鎖 (E2 の clip は後ろの端と前の端の両方の変形を持つ)。
    // 小さい式は端点の大きさの偶奇が分かれやすい組 (P2-3 の smoke の small-*)。
    const std::vector<RealTransformCase> cases = {
        {"e1-e2", e1, 64, e2, 64},           {"e2-e3", e2, 64, e3, 64},
        {"small-y", "y", 48, "y^2 = 1", 72}, {"small-a", "a", 64, "a + b", 64},
        {"small-k", "k = 1", 72, "k", 50},
    };
    // timeline: 各 clip は 2 秒 (120 frame)。変形は cut の前 15・後 15 (30 枚)。
    // 連鎖は e1 (0..120) → e2 (120..240) → e3 (240..360)、小さい式は 600 frame ごとに置く。
    constexpr qint64 kClip = 120;
    constexpr std::int64_t kHalf = 15;
    auto initial = project::createDefaultProject();
    const auto endpoint = [&](const std::string& id, const std::string& source, int font,
                              qint64 start, const std::string& color) {
        auto clip = transformEndpoint(id, source, start, color);
        clip.sourceFrameCount = clip.sourceOutFrame = kClip;
        clip.math.fontSize = font;
        return clip;
    };

    struct Placed {
        RealTransformCase c;
        std::string transitionId;
        std::string outgoing;
        std::string incoming;
        qint64 cut = 0;
    };

    std::vector<Placed> placed;
    initial.timelineClips.push_back(endpoint("e1", e1, 64, 0, "#FFFFFFFF"));
    initial.timelineClips.push_back(endpoint("e2", e2, 64, kClip, "#FFFFD040"));
    initial.timelineClips.push_back(endpoint("e3", e3, 64, 2 * kClip, "#FF60C0FF"));
    placed.push_back({cases[0], "t-e1-e2", "e1", "e2", kClip});
    placed.push_back({cases[1], "t-e2-e3", "e2", "e3", 2 * kClip});
    qint64 at = 600;
    for (std::size_t index = 2; index < cases.size(); ++index) {
        const auto& c = cases[index];
        initial.timelineClips.push_back(
            endpoint(c.name + "-a", c.source, c.sourceFont, at, "#FFFFFFFF"));
        initial.timelineClips.push_back(
            endpoint(c.name + "-b", c.target, c.targetFont, at + kClip, "#FFFFFFFF"));
        placed.push_back({c, "t-" + c.name, c.name + "-a", c.name + "-b", at + kClip});
        at += 600;
    }
    for (const auto& p : placed)
        initial.timelineTransitions.push_back({p.transitionId, p.outgoing, p.incoming, kHalf, kHalf,
                                               project::TransitionKind::MathTransform});
    const auto valid = project::validateTimeline(initial);
    check(valid.success, "real: project が変形の条件を満たす: " + valid.error);
    const auto path = workDirectory / L"real transform.mvm";
    check(project::saveProjectJson(initial, path).success, "real: project の保存");

    auto captured = std::make_shared<CapturedExport>();
    auto controller = std::make_unique<MvmController>(
        path, manim, initial, nullptr,
        [captured](const project::Project&, const mvm::app::TimelineExportRequest& request) {
            std::lock_guard lock(captured->mutex);
            captured->transformRequest = request;
            mvm::app::TimelineExportResult result;
            result.success = true;
            result.outputPath = request.outputPath;
            return result;
        },
        MvmController::ExportThreadFactory{},
        MvmController::FileRevealer{[](const std::filesystem::path&, QString&) { return true; }});
    PreviewWindowHarness harness;
    harness.attach(*controller);
    check(pump([&] { return controller->previewReady(); }), "real: preview の初期化");
    auto& cache = controller->mathRastersForTest();
    QElapsedTimer timer;
    timer.start();
    check(pump(
              [&] {
                  return cache.backendState() != mvm::app::MathRasterCache::BackendState::Checking;
              },
              120000) &&
              cache.backendState() == mvm::app::MathRasterCache::BackendState::Available,
          "real: Manim の確認 (preflight) が通る: " + cache.backendMessage().toStdString());
    std::fprintf(stderr, "real: toolchain %s (%lld ms)\n", qUtf8Printable(cache.toolchainText()),
                 static_cast<long long>(timer.elapsed()));
    if (cache.backendState() != mvm::app::MathRasterCache::BackendState::Available) {
        controller->shutdown();
        return 1;
    }
    // 全変形が disk に揃うまで待つ (worker は 1 本で、静止 → 変形の順に描く)。
    const auto transformState = [&](const std::string& id) {
        controller->selectTransition(QString::fromStdString(id));
        return transitionValue(*controller, "transformState");
    };
    timer.restart();
    check(pump(
              [&] {
                  for (const auto& p : placed) {
                      const auto s = transformState(p.transitionId);
                      if (s != QStringLiteral("ready") && s != QStringLiteral("error") &&
                          s != QStringLiteral("unavailable"))
                          return false;
                  }
                  return true;
              },
              600000),
          "real: すべての変形の描画が終わる");
    std::fprintf(stderr, "real: 静止と変形の描画 %lld ms\n",
                 static_cast<long long>(timer.elapsed()));

    int shiftedCases = 0;
    const auto& projectNow = controller->projectForTest();
    for (const auto& p : placed) {
        const auto s = transformState(p.transitionId);
        check(s == QStringLiteral("ready"),
              p.c.name + ": 変形が ready (" + s.toStdString() + " " +
                  transitionValue(*controller, "transformMessage").toStdString() + ")");
        if (s != QStringLiteral("ready"))
            continue;
        const auto& transition = *std::find_if(
            projectNow.timelineTransitions.begin(), projectNow.timelineTransitions.end(),
            [&](const auto& t) { return t.id == p.transitionId; });
        const auto clipOf = [&](const std::string& id) {
            return *std::find_if(projectNow.timelineClips.begin(), projectNow.timelineClips.end(),
                                 [&](const auto& clip) { return clip.id == id; });
        };
        const auto spec =
            mvm::app::mathTransformSpecFor(transition, clipOf(p.outgoing), clipOf(p.incoming));
        const auto artifact = spec ? cache.readyTransform(*spec) : std::nullopt;
        const auto sourceStatic =
            spec ? cache.request(spec->source) : mvm::app::MathRasterCache::Entry{};
        const auto targetStatic =
            spec ? cache.request(spec->target) : mvm::app::MathRasterCache::Entry{};
        if (!artifact || !sourceStatic.mask || !targetStatic.mask) {
            check(false, p.c.name + ": artifact と両端の静止");
            continue;
        }
        // disk の .a8 (1 画素 1 byte の生の byte 列)。
        bool a8 = artifact->frames.size() == static_cast<std::size_t>(2 * kHalf);
        for (const auto& frame : artifact->frames)
            a8 = a8 && frame.extension() == L".a8" &&
                 std::filesystem::file_size(frame, error) ==
                     static_cast<std::uintmax_t>(artifact->width) *
                         static_cast<std::uintmax_t>(artifact->height);
        check(a8, p.c.name + ": 変形の frame は 30 枚の .a8 (幅 x 高さ byte)");
        // 端点の左上を手で数える: 静止の配置 (中央、余りは左上) - artifact の中の位置。
        const int sw = sourceStatic.mask->width;
        const int sh = sourceStatic.mask->height;
        const int tw = targetStatic.mask->width;
        const int th = targetStatic.mask->height;
        const int ax = (kOutputWidth - sw) / 2 - artifact->sourceX;
        const int ay = (kOutputHeight - sh) / 2 - artifact->sourceY;
        const int bx = (kOutputWidth - tw) / 2 - artifact->targetX;
        const int by = (kOutputHeight - th) / 2 - artifact->targetY;
        std::fprintf(
            stderr, "%s: 静止 %dx%d → %dx%d、artifact %dx%d、端点の左上 (%d,%d) → (%d,%d)\n",
            p.c.name.c_str(), sw, sh, tw, th, artifact->width, artifact->height, ax, ay, bx, by);
        if (ax != bx || ay != by)
            ++shiftedCases;
        // 最後の frame の位置は target の位置 (target の静止が普通の B の静止に重なる)。
        const int lastLeft = TransformGeometry::originAt(ax, bx, 2 * kHalf - 1);
        const int lastTop = TransformGeometry::originAt(ay, by, 2 * kHalf - 1);
        check(lastLeft == bx && lastTop == by,
              p.c.name + ": 最後の frame の artifact は target の静止が B の静止に重なる位置");

        // 合成 (preview の製品の経路) で frame 0 と最後の frame を確かめる。
        const qint64 start = p.cut - kHalf;
        std::optional<mvm::preview::PreviewCompositionLayer> a;
        std::optional<mvm::preview::PreviewCompositionLayer> b;
        pump([&] {
            a = mathLayer(*controller, start + 1);
            b = mathLayer(*controller, p.cut + 1);
            return a && b && a->stillAnimation && b->stillAnimation &&
                   a->stillAnimation->stateAt(start) >= 0 && b->stillAnimation->stateAt(start) >= 0;
        });
        const auto plainA = mathLayer(*controller, start - 1);
        const auto plainB = mathLayer(*controller, p.cut + kHalf);
        if (!a || !b || !plainA || !plainB || !a->stillAnimation || !b->stillAnimation) {
            check(false, p.c.name + ": 変形の animation が付く");
            continue;
        }
        check(presentedPixels(*a, start) == presentedPixels(*plainA, start - 1),
              p.c.name + ": frame 0 は普通の A の静止の preview と画素で一致");
        check(presentedPixels(*b, start) == presentedPixels(*a, start) &&
                  presentedPixels(*b, p.cut + kHalf - 1) == presentedPixels(*a, p.cut + kHalf - 1),
              p.c.name + ": 前・後ろの layer は同じ変形の画素を見せる");
        // 最後の frame (進み具合 29/30) と B の静止の違う画素の数 (参考)。1 画素ずらすと増える。
        const auto last = presentedPixels(*b, p.cut + kHalf - 1);
        const auto staticB = presentedPixels(*plainB, p.cut + kHalf);
        std::fprintf(stderr, "%s: 最後の frame と B の静止の違う画素 %zu\n", p.c.name.c_str(),
                     differingPixels(last, staticB));
    }
    check(shiftedCases >= 1,
          "real: 端点の左上がずれる (偶奇の違う) 変形を少なくとも 1 つ確かめた (" +
              std::to_string(shiftedCases) + " 件)");

    // 実 D3D11 の preview で解の公式の連鎖を再生し、engine の評価を記録する。
    auto observation = std::make_shared<TransformObservation>();
    controller->setMathTransformObserverForTest(
        [observation](const std::string& transitionId, const std::string& clipId,
                      std::int64_t frame, std::int64_t shown) {
            std::lock_guard lock(observation->mutex);
            observation->frames.emplace_back(transitionId + "/" + clipId, frame, shown);
        });
    check(seekWhenReady(*controller, 60) && pump([&] {
              return submittedHasWrite(*controller) && controller->previewPresentedLatest();
          }),
          "real: 再生の前に変形の mask を memory に読む");
    observation->clear();
    check(controller->playTimeline(), "real: 解の公式の連鎖を再生する");
    check(pump([&] { return controller->playheadFrame() >= 2 * kClip + 2 * kHalf + 10; }),
          "real: 2 つの変形の区間を再生し終える");
    controller->pauseTimeline();
    const auto records = observation->snapshot();
    std::map<std::string, std::int64_t> start;
    start["t-e1-e2"] = kClip - kHalf;
    start["t-e2-e3"] = 2 * kClip - kHalf;
    std::size_t shownCount = 0;
    bool authoritative = true;
    std::set<std::string> seen;
    for (const auto& [key, frame, shown] : records) {
        const auto transitionId = key.substr(0, key.find('/'));
        const auto found = start.find(transitionId);
        if (found == start.end())
            continue;
        const std::int64_t want = frame >= found->second && frame < found->second + 2 * kHalf
                                      ? frame - found->second
                                      : -1;
        authoritative = authoritative && shown == want;
        if (shown >= 0) {
            ++shownCount;
            seen.insert(key);
        }
    }
    std::fprintf(stderr, "real: 再生中の変形の評価 %zu 件 (layer %zu 種)\n", shownCount,
                 seen.size());
    check(shownCount > 0 && authoritative, "real: 再生中の変形の frame = 出力 frame - 区間の先頭");
    check(seen.contains("t-e1-e2/e1") && seen.contains("t-e1-e2/e2") &&
              seen.contains("t-e2-e3/e2") && seen.contains("t-e2-e3/e3"),
          "real: 連鎖の両方の変形を、cut の前後の layer で見せる");
    // P2-6: 既存の映像のみの受け入れ経路で、実 Manim の二次方程式の連鎖を書き出す。
    // preview の正の patch と復号画素を照合してから、常駐予算を外して同じ disk を書き出す。
    std::map<qint64, std::vector<std::uint8_t>> expectedExport;
    std::map<qint64, std::vector<std::uint8_t>> hardCutControl;
    for (qint64 frame : {qint64{0}, qint64{105}, qint64{119}, qint64{120}, qint64{134}, qint64{135},
                         qint64{225}, qint64{239}, qint64{240}, qint64{254}, qint64{255}}) {
        const auto layer = mathLayer(*controller, frame);
        check(layer.has_value(), "real export: 比較対象の製品 preview の layer がある");
        if (layer)
            expectedExport.emplace(frame, presentedPixels(*layer, frame));
        const auto& clip = projectNow.timelineClips[frame < 120 ? 0 : frame < 240 ? 1 : 2];
        const auto staticArtifact = cache.readyArtifact(mvm::app::mathRenderSpecFor(clip.math));
        if (staticArtifact)
            hardCutControl.emplace(frame,
                                   mvm::app::composeMathClipFromPng(*staticArtifact, clip.math,
                                                                    kOutputWidth, kOutputHeight)
                                       .rgba);
    }
    cache.setResidentMemoryBudget(1);
    check(exportAndWait(*controller, workDirectory / "captured.mp4"),
          "real export: OverBudget でも controller が disk の成果物を受理する");
    std::optional<mvm::app::TimelineExportRequest> exportRequest;
    {
        std::lock_guard lock(captured->mutex);
        exportRequest = captured->transformRequest;
    }
    check(exportRequest.has_value(), "real export: 現在の disk artifact の要求を取得する");
    if (exportRequest) {
        auto sequence = projectNow;
        sequence.timelineClips.resize(3);
        sequence.timelineTransitions.resize(2);
        check(
            project::saveProjectJson(sequence, workDirectory / "quadratic-video-only.mvm").success,
            "real export: 二次方程式の映像のみの Project を保存する");
        exportRequest->outputPath = workDirectory / "quadratic-video-only.mp4";
        exportRequest->progress = {};
        exportRequest->videoCrf = 0;
        // 製品と同じ明示した module / data の path で起動し、映像だけを検証する。
        check(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
              "real export: 映像のみの書き出し用 MLT を初期化する");
        const auto exported = mvm::app::exportTimeline(sequence, *exportRequest);
        check(exported.success && exported.frameCount == 360,
              "real export: 実 Manim の連鎖を 360 frame 書き出す: " + exported.error);
        QByteArray ordinaryA, ordinaryB;
        for (const auto& [frame, expected] : expectedExport) {
            if (!exported.success)
                break;
            QProcess decoder;
            decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                          {"-v", "error", "-i",
                           QString::fromStdWString(exportRequest->outputPath.wstring()), "-vf",
                           QStringLiteral("select=eq(n\\,%1)").arg(frame), "-frames:v", "1", "-f",
                           "rawvideo", "-pix_fmt", "rgba", "-"});
            const bool decoded = decoder.waitForFinished(60000) && decoder.exitCode() == 0;
            const auto bytes = decoder.readAllStandardOutput();
            check(decoded && bytes.size() == static_cast<qsizetype>(expected.size()),
                  "real export: 選択 frame を復号できる: " + std::to_string(frame));
            if (!decoded || bytes.size() != static_cast<qsizetype>(expected.size()))
                continue;
            if (frame == 0)
                ordinaryA = bytes;
            if (frame == 135)
                ordinaryB = bytes;
            if (frame == 105)
                check(!ordinaryA.isEmpty() && ordinaryA == bytes,
                      "real export: 変形 frame 0 と通常静止 A の復号画素が完全一致する");
            if (frame == 225)
                check(!ordinaryB.isEmpty() && ordinaryB == bytes,
                      "real export: 連鎖の次の変形 frame 0 と通常静止 B の復号画素が完全一致する");
            const auto* actual = reinterpret_cast<const unsigned char*>(bytes.constData());
            std::size_t compared = 0, bad = 0, visible = 0, support = 0;
            long long totalError = 0, hardCutError = 0;
            for (std::size_t pixelAt = 0; pixelAt < expected.size(); pixelAt += 4) {
                visible += expected[pixelAt + 3] > 128;
                support += expected[pixelAt + 3] > 0;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int wanted =
                        (expected[pixelAt + channel] * expected[pixelAt + 3] + 127) / 255;
                    const int difference =
                        std::abs(static_cast<int>(actual[pixelAt + channel]) - wanted);
                    if (hardCutControl.contains(frame)) {
                        const auto& control = hardCutControl.at(frame);
                        const int cutWanted =
                            (control[pixelAt + channel] * control[pixelAt + 3] + 127) / 255;
                        hardCutError +=
                            std::abs(static_cast<int>(actual[pixelAt + channel]) - cutWanted);
                    }
                    totalError += difference;
                    bad += difference > 60;
                    ++compared;
                }
            }
            std::fprintf(
                stderr,
                "real export: frame=%lld 比較=%zu 被覆=%zu 非零被覆=%zu 誤差和=%lld 大差=%zu\n",
                static_cast<long long>(frame), compared, visible, support, totalError, bad);
            // 4:2:0 の色差は周囲の透明画素にも広がる。誤差和は全画素で数え、分母には
            // alpha > 128 の濃い部分だけでなく、変形中の薄い部分も含む全被覆を使う。
            check(visible > 100 && compared > 0 &&
                      totalError < static_cast<long long>(support) * 50 && bad < visible,
                  "real export: frame 0・cut の両側・変形直後が製品 preview と一致する");
            if (frame == 119 || frame == 120 || frame == 239 || frame == 240)
                check(hardCutControl.contains(frame) && totalError * 2 < hardCutError,
                      "real export: hard cut の対照より変形の製品画素に近い");
        }
        mvm_mlt_runtime_shutdown();
    }
    controller->shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}

void testGraphHistory() {
    QTemporaryDir temp;
    check(temp.isValid(), "Graph 履歴試験の作業 directory");
    const auto path = std::filesystem::path(temp.path().toStdWString()) / L"graph.mvm";
    const auto initial = project::createDefaultProject();
    check(project::saveProjectJson(initial, path).success, "Graph 初期保存");
    auto captured = std::make_shared<CapturedExport>();
    auto controller = makeController(path, initial, captured);
    const auto depth = controller->undoDepthForTest();
    check(controller->createGraphClip(0, project::TrackRef{project::TrackKind::Video, 0}),
          "Graph 作成");
    const auto created = controller->projectForTest();
    check(created.timelineClips.size() == 1 && controller->undoDepthForTest() == depth + 1,
          "Graph 作成は Undo 一回");
    if (created.timelineClips.empty()) {
        controller->shutdown();
        return;
    }
    const auto id = created.timelineClips[0].id;
    check(controller->undoLastEdit() && controller->projectForTest() == initial &&
              controller->redoLastEdit() && controller->projectForTest() == created,
          "作成の exact Undo/Redo");
    check(controller->setGraphDuration(id, 400), "Graph source 尺の確定");
    const auto extended = controller->projectForTest();
    check(extended.timelineClips[0].sourceFrameCount == 400 && controller->undoLastEdit() &&
              controller->projectForTest() == created && controller->redoLastEdit() &&
              controller->projectForTest() == extended && controller->undoLastEdit() &&
              controller->projectForTest() == created,
          "source 尺も exact Undo/Redo");
    const auto beforeDepth = controller->undoDepthForTest();
    check(!controller->editGraphData(id,
                                     [](auto& g, auto&) {
                                         g.functions.clear();
                                         return true;
                                     }) &&
              controller->undoDepthForTest() == beforeDepth &&
              controller->projectForTest() == created,
          "拒否編集の Project と Undo 不変");
    check(controller->editGraphData(id,
                                    [](auto& g, auto&) {
                                        g.functions[0].expression = "sin(";
                                        g.intro = {project::GraphIntroKind::Draw, 3};
                                        return true;
                                    }),
          "不正式と Draw の確定");
    const auto edited = controller->projectForTest();
    check(edited.timelineClips[0].graph.functions[0].id ==
                  created.timelineClips[0].graph.functions[0].id &&
              controller->undoDepthForTest() == beforeDepth + 1,
          "普通の編集は ID 保持と Undo 一回");
    check(controller->undoLastEdit() && controller->projectForTest() == created &&
              controller->redoLastEdit() && controller->projectForTest() == edited,
          "編集 exact Undo/Redo");
    check(controller->selectClip(0) && controller->duplicateSelectedClips(), "Graph 複製");
    const auto duplicated = controller->projectForTest();
    check(duplicated.timelineClips.size() == 2 && duplicated.timelineClips[1].id != id &&
              duplicated.timelineClips[1].graph.functions[0].id !=
                  edited.timelineClips[0].graph.functions[0].id,
          "複製の新しい所有 ID");
    check(controller->undoLastEdit() && controller->projectForTest() == edited &&
              controller->redoLastEdit() && controller->projectForTest() == duplicated &&
              controller->undoLastEdit(),
          "複製 ID の exact Undo/Redo");
    check(controller->splitClipAt(QString::fromStdString(id), 1, false, false), "Draw 内 split");
    const auto split = controller->projectForTest();
    check(split.timelineClips.size() == 2 && split.timelineClips[1].graph.functions[0].id !=
                                                 split.timelineClips[0].graph.functions[0].id,
          "右片は新所有 ID");
    check(controller->undoLastEdit() && controller->projectForTest() == edited &&
              controller->redoLastEdit() && controller->projectForTest() == split,
          "split ID exact Undo/Redo");
    auto serialized = project::serializeProjectJson(split, path);
    auto reopened = project::parseProjectJsonText(serialized.json, path);
    check(serialized.success && reopened.success && reopened.project == split,
          "controller 状態往復");
    const auto mapping = mvm::app::mapTimelineExportPlan(split, {});
    check(!mapping.success && mapping.error.find("Graph") != std::string::npos,
          "Graph export の明示拒否");
    const auto preview = mvm::app::mapTimelinePreviewFrame(split, 0);
    check(preview.success && preview.stillLayers.size() == 1 &&
              preview.stillLayers.front().kind == project::TimelineClipKind::Graph,
          "Graph は preview の静止画合成境界へ渡す");
    const auto rendersBeforeStatus = controller->graphRastersForTest().renderCount();
    for (int i = 0; i < 20; ++i)
        (void)controller->graphPreviewStatus(id, 0);
    check(controller->graphRastersForTest().renderCount() == rendersBeforeStatus,
          "状態照会は描画を起動しない");
    check(controller->toggleTimelineClipEnabled(QString::fromStdString(id)), "Graph を無効化");
    const auto disabled = controller->projectForTest();
    const auto disabledPreview = mvm::app::mapTimelinePreviewFrame(disabled, 0);
    check(disabledPreview.success &&
              std::none_of(disabledPreview.stillLayers.begin(), disabledPreview.stillLayers.end(),
                           [&](const auto& layer) {
                               return layer.kind == project::TimelineClipKind::Graph &&
                                      layer.clipId == id;
                           }),
          "無効 Graph は preview に出ない");
    const auto disabledStatus = controller->graphPreviewStatus(id, 0);
    check(disabledStatus.reason == mvm::app::GraphPreviewCache::Reason::DisabledClip &&
              disabledStatus.transparentFallback,
          "無効 Graph は typed の透明 fallback");
    check(controller->toggleTimelineClipEnabled(QString::fromStdString(id)), "Graph を再有効化");
    check(controller->setTrackMuted(QStringLiteral("video"), 0, true), "video track をミュート");
    const auto hiddenPreview = mvm::app::mapTimelinePreviewFrame(controller->projectForTest(), 0);
    check(hiddenPreview.success && hiddenPreview.stillLayers.empty(),
          "非表示 track の Graph は preview に出ない");
    const auto hiddenStatus = controller->graphPreviewStatus(id, 0);
    check(hiddenStatus.reason == mvm::app::GraphPreviewCache::Reason::HiddenTrack &&
              hiddenStatus.transparentFallback,
          "非表示 track は typed reason");
    check(controller->setTrackMuted(QStringLiteral("video"), 0, false), "video track を戻す");
    auto transitionProject = controller->projectForTest();
    std::string incoming;
    for (const auto& clip : transitionProject.timelineClips)
        if (clip.id != id)
            incoming = clip.id;
    const auto placed = project::applyDefaultEditTransition(
        transitionProject, id, incoming, 2, project::LinkMode::Single,
        [] { return std::string("graph-transition"); });
    check(!placed.success && controller->projectForTest().timelineTransitions.empty(),
          "Graph の外側 TimelineTransition は hard-cut として受理しない");
    controller->shutdown();
}

void testEquationSequenceHistory() {
    QTemporaryDir temp;
    check(temp.isValid(), "sequence 履歴試験の作業 directory");
    if (!temp.isValid())
        return;
    project::EquationSequenceClipData data;
    project::EquationState state;
    state.id = {"state"};
    state.equation.source = "x+x";
    state.revision = "r1";
    state.holdFrames = 10;
    state.parts.push_back({{"part"}, "項", {"r1", 0, 1, "x", project::BindingStatus::Bound}});
    data.states.push_back(state);
    auto middle = state;
    middle.id = {"middle"};
    middle.parts[0].id = {"middle-part"};
    auto last = state;
    last.id = {"last"};
    last.parts[0].id = {"last-part"};
    data.states.push_back(middle);
    data.states.push_back(last);
    data.transitions = {
        {{"first-edge"}, {"state"}, {"middle"}, 2, {{{"part"}, {"middle-part"}}}},
        {{"last-edge"}, {"middle"}, {"last"}, 2, {{{"middle-part"}, {"last-part"}}}}};
    data.actions.push_back({{"action"},
                            {"state"},
                            {"part"},
                            project::EquationTargetStatus::Present,
                            2,
                            4,
                            project::EquationOperation::Pulse});
    data.actions.push_back({{"owned-middle"},
                            {"middle"},
                            {"middle-part"},
                            project::EquationTargetStatus::Present,
                            0,
                            1,
                            project::EquationOperation::Outline});
    auto initial = project::createDefaultProject();
    check(project::addEquationSequence(initial, data, "sequence", "数式",
                                       {project::TrackKind::Video, 0}, 0)
              .success,
          "sequence 履歴の対照 Project");
    const auto path = std::filesystem::path(temp.path().toStdWString()) / L"sequence.mvm";
    const auto initialSaved = project::saveProjectJson(initial, path);
    check(initialSaved.success, "sequence の初期 Project を保存: " + initialSaved.error);
    auto captured = std::make_shared<CapturedExport>();
    auto controller = makeController(path, initial, captured);
    check(controller->selectClip(0), "sequence の構造選択");
    auto rangeCase = [&](const std::string& name, std::int64_t length, const auto& edit) {
        const auto beforeRange = controller->projectForTest();
        const auto rangeDepth = controller->undoDepthForTest();
        check(controller->editEquationSequenceData("sequence", edit), name + "の確定");
        const auto afterRange = controller->projectForTest();
        const auto& clip = afterRange.timelineClips[0];
        check(clip.sourceFrameCount == length && clip.sourceInFrame == 0 &&
                  clip.sourceOutFrame == length && controller->undoDepthForTest() == rangeDepth + 1,
              name + "の外側範囲と Undo 一回");
        check(controller->undoLastEdit() && controller->projectForTest() == beforeRange &&
                  controller->redoLastEdit() && controller->projectForTest() == afterRange &&
                  controller->undoLastEdit() && controller->projectForTest() == beforeRange,
              name + "の範囲・全 ID・sequence の exact Undo/Redo");
    };
    for (const auto frames : {14, 7}) {
        rangeCase("hold 尺編集", 24 + frames, [=](auto& d, auto& error) {
            return project::changeEquationHold(d, {"last"}, frames, 1080, error);
        });
    }
    for (const auto frames : {5, 1}) {
        rangeCase("transition 尺編集", 32 + frames, [=](auto& d, auto& error) {
            return project::changeEquationTransition(d, {"first-edge"}, frames, 1080, error);
        });
    }
    rangeCase("状態挿入", 37, [state](auto& d, auto& error) {
        auto inserted = state;
        inserted.id = {"inserted"};
        inserted.parts[0].id = {"inserted-part"};
        inserted.holdFrames = 3;
        return project::insertEquationState(d, 1, inserted,
                                            {{{"insert-a"}, {"state"}, {"inserted"}, 1, {}},
                                             {{"insert-b"}, {"inserted"}, {"middle"}, 1, {}}},
                                            1080, error);
    });
    rangeCase("状態削除", 21, [](auto& d, auto& error) {
        return project::deleteEquationState(
            d, {"middle"}, project::EquationStepTransition{{"joined"}, {"state"}, {"last"}, 1, {}},
            1080, error);
    });
    for (const auto frames : {14, 8, 1}) {
        auto trimmed = initial;
        trimmed.timelineClips[0].sourceInFrame = 3;
        trimmed.timelineClips[0].sourceOutFrame = 29;
        const auto trimPath = std::filesystem::path(temp.path().toStdWString()) /
                              ("trim-" + std::to_string(frames) + ".mvm");
        check(project::saveProjectJson(trimmed, trimPath).success, "右 trim の対照を保存");
        auto trimController = makeController(trimPath, trimmed, captured);
        const auto trimDepth = trimController->undoDepthForTest();
        const bool accepted =
            trimController->editEquationSequenceData("sequence", [=](auto& d, auto& error) {
                return project::changeEquationHold(d, {"last"}, frames, 1080, error);
            });
        check(accepted == (frames != 1), "右 trim 尺編集の成否");
        if (frames == 1) {
            check(trimController->projectForTest() == trimmed &&
                      trimController->undoDepthForTest() == trimDepth,
                  "可視末尾より短縮は Project と履歴を原子的に保持");
        } else {
            const auto afterTrim = trimController->projectForTest();
            const auto& clip = afterTrim.timelineClips[0];
            check(clip.sourceFrameCount == 24 + frames && clip.sourceInFrame == 3 &&
                      clip.sourceOutFrame == 29 &&
                      trimController->undoDepthForTest() == trimDepth + 1,
                  "右 trim と左端を保持して Undo 一回");
            check(trimController->undoLastEdit() && trimController->projectForTest() == trimmed &&
                      trimController->redoLastEdit() &&
                      trimController->projectForTest() == afterTrim,
                  "右 trim 尺編集の範囲・全 ID・sequence の exact Undo/Redo");
        }
        trimController->shutdown();
    }
    const auto depth = controller->undoDepthForTest();
    const bool duplicatedOk = controller->duplicateSelectedClips();
    check(duplicatedOk,
          "sequence 複製は既存編集経路を使う: " + controller->statusText().toStdString());
    const auto duplicated = controller->projectForTest();
    check(duplicated.timelineClips.size() == 2 && controller->undoDepthForTest() == depth + 1,
          "複製は Undo 一回");
    if (duplicated.timelineClips.size() != 2) {
        controller->shutdown();
        return;
    }
    check(duplicated.timelineClips[1].equationSequence.states[0].id != state.id,
          "製品の複製経路も内部 ID を発行する");
    check(controller->undoLastEdit() && controller->projectForTest() == initial,
          "複製 Undo は元の ID を復元");
    check(controller->redoLastEdit() && controller->projectForTest() == duplicated,
          "複製 Redo は確定した全 ID を復元し再発行しない");
    check(controller->undoLastEdit(), "paste 前の Undo");
    check(controller->selectClip(0) && controller->copySelectedClips() && controller->pasteClips(),
          "sequence copy/paste");
    const auto pasted = controller->projectForTest();
    check(pasted.timelineClips.size() == 2 &&
              pasted.timelineClips[1].equationSequence.states[0].id != state.id,
          "paste も内部参照を remap");
    check(controller->undoLastEdit() && controller->redoLastEdit() &&
              controller->projectForTest() == pasted,
          "paste Undo/Redo は確定 ID を維持");
    check(controller->undoLastEdit(), "split 前の Undo");
    check(controller->splitClipAt(QStringLiteral("sequence"), 3, false, false),
          "pulse 内の controller 分割");
    const auto split = controller->projectForTest();
    check(controller->undoLastEdit() && controller->redoLastEdit() &&
              controller->projectForTest() == split,
          "split Undo/Redo は右側の全 ID を復元");
    const auto before = controller->projectForTest();
    const auto editDepth = controller->undoDepthForTest();
    check(controller->editEquationSequenceData("sequence",
                                               [](auto& d, auto& error) {
                                                   return project::replaceEquationSource(
                                                       d, {"state"}, "x+x", "r2", 1080, error);
                                               }),
          "sequence domain 編集の確定");
    const auto edited = controller->projectForTest();
    check(controller->undoDepthForTest() == editDepth + 1 &&
              edited.timelineClips[0].equationSequence.states[0].parts[0].binding.status ==
                  project::BindingStatus::Invalid,
          "domain 一操作が Undo 一回で source binding を invalid にする");
    check(controller->undoLastEdit() && controller->projectForTest() == before &&
              controller->redoLastEdit() && controller->projectForTest() == edited,
          "domain 編集の exact Undo/Redo");
    const auto deleteDepth = controller->undoDepthForTest();
    check(controller->editEquationSequenceData(
              "sequence",
              [](auto& d, auto& error) {
                  return project::deleteEquationState(
                      d, {"middle"},
                      project::EquationStepTransition{{"new-edge"}, {"state"}, {"last"}, 1, {}},
                      1080, error);
              }),
          "状態と所有 action を通常 transaction で削除");
    const auto deleted = controller->projectForTest();
    check(controller->undoDepthForTest() == deleteDepth + 1 &&
              deleted.timelineClips[0].equationSequence.actions.size() == 1 &&
              deleted.timelineClips[0].equationSequence.transitions[0].correspondence.empty(),
          "状態削除と action 削除と辺の更新は Undo 一回");
    check(controller->undoLastEdit() && controller->projectForTest() == edited &&
              controller->redoLastEdit() && controller->projectForTest() == deleted &&
              controller->undoLastEdit() && controller->projectForTest() == edited,
          "状態削除 Undo は所有 action と旧辺を完全に復元");
    check(controller->saveProject(), "sequence の実保存");
    const auto loaded = project::loadProjectJson(path);
    check(loaded.success && loaded.project == edited, "sequence の実 save/reopen");
    controller->shutdown();
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--native-write") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        return nativeWritePlayback();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--native-transform") {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        return nativeTransformPlayback();
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--real-manim-transform") {
        if (argc != 4) {
            std::fprintf(stderr, "使い方: mvm_test_math_controller --real-manim-transform "
                                 "<manim.exe> <作業 directory (存在しないこと)>\n");
            return 2;
        }
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QGuiApplication app(argc, argv);
        const auto arguments = QGuiApplication::arguments();
        return realManimTransform(std::filesystem::path(arguments[2].toStdWString()),
                                  std::filesystem::path(arguments[3].toStdWString()));
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    QTemporaryDir temp;
    check(temp.isValid(), "作業フォルダー");
    if (argc == 2 && std::string_view(argv[1]) == "--equation-domain") {
        testEquationSequenceHistory();
        std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
        return failures == 0 && checks > 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--graph-domain") {
        testGraphHistory();
        std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
        return failures == 0 && checks > 0 ? 0 : 1;
    }
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

    testEquationSequenceHistory();
    testWrite(temp, initial, captured);
    testWriteResidencyBudget(temp, initial, captured);
    testWriteBeyondBackendCapability(temp, initial, captured);
    testMathTransformEditing(temp, captured);
    testMathTransformPreviewParity(temp, captured);
    testMathTransformPreviewColors(temp, captured);
    testMathTransformPreviewEffects(temp, captured);
    testMathTransformPreviewFallbacks(temp, captured);
    testMathTransformWithWrite(temp, captured);
    testMathTransformYieldsToEditing(temp, captured);
    testMathTransformPlacementDoesNotFit(temp, captured);
    testMathTransformExportWorker(temp);

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
