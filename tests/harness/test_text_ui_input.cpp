// 横書き文字ツールの直接入力を、実際の Main.qml へキーとマウスの event を送って検査する。
//
// 製品と同じ QML・controller・preview surface を D3D11 の window で起動し、
//   T -> モニターをクリック -> 入力 -> Ctrl+Enter で文字 clip ができる
//   Esc では何も作らない
//   選択ツールでドラッグすると位置が変わり、ドラッグ中だけ UI が文字を重ねる
//   Ctrl+Z で位置が戻る
//   入力欄の編集中は timeline の shortcut (Ctrl+C/V/D, M, I, O) が効かない
// を確かめる。IME の変換確定は OS の入力方式が要るのでここでは扱わない
// (docs/premiere-like-editing.md の手動確認手順を参照)。
#include "app/preview/preview_engine_rhi_item.h"
#include "app/text_raster.h"
#include "focus_release_filter.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/timeline_edit.h"
#include "test_media_fixture.h"
#include "waveform_cache.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

void pump(int milliseconds = 100) {
    pumpUntil([] { return false; }, milliseconds);
}

// QTest::keyClicks は QWidget 用なので、QWindow へは 1 文字ずつ送る。
void typeText(QWindow* window, const char* text) {
    for (const char* c = text; *c; ++c)
        QTest::keyClick(window, *c);
}

// popup の中身は overlay (window の root item の子) に置かれ、QObject の親子では
// 辿れないことがある。見た目の木を辿って探す。
void collectItems(QQuickItem* item, QList<QQuickItem*>& out) {
    out.push_back(item);
    for (QQuickItem* child : item->childItems())
        collectItems(child, out);
}

QList<QQuickItem*> visualItems(QQuickWindow* window) {
    QList<QQuickItem*> items;
    collectItems(window->contentItem(), items);
    return items;
}

QQuickItem* findVisualItem(QQuickWindow* window, const QString& name) {
    for (QQuickItem* item : visualItems(window))
        if (item->objectName() == name)
            return item;
    return nullptr;
}

int textClipCount(const mvm::app::MvmController& controller) {
    auto* model = controller.timelineModel();
    int count = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        const auto roles = model->roleNames();
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            if (role.value() == "clipKind" && model->data(index, role.key()).toString() == "text")
                ++count;
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "FAIL: MLT runtime を初期化できません\n");
        return 3;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::fprintf(stderr, "FAIL: 一時 directory を作れません\n");
        return 3;
    }

    // V1 に映像を置く。文字は映像の上 (V2) に置かれ、engine が合成する経路を通る。
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip video;
    video.id = "video";
    video.name = "video";
    video.mediaPath = MVM_TEXT_TEST_VIDEO;
    video.sourceFpsNum = 60;
    video.sourceFpsDen = 1;
    video.sourceFrameCount = 300;
    video.sourceOutFrame = 120;
    project.timelineClips.push_back(video);
    mvm::test::attachFixtureMedia(project);
    const std::filesystem::path projectPath =
        directory.filePath(QStringLiteral("ui.mvm")).toStdWString();

    int exitCode = 0;
    {
        mvm::app::MvmController controller(projectPath, {}, project);
        mvm::app::WaveformCache waveformCache;
        QQmlApplicationEngine engine;
        engine.setInitialProperties(
            {{QStringLiteral("mvmController"), QVariant::fromValue(&controller)},
             {QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}});
        engine.load(QUrl(QStringLiteral("qrc:/mvm/app/Main.qml")));
        auto* window = engine.rootObjects().isEmpty()
                           ? nullptr
                           : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        auto* surface = window ? window->findChild<mvm::app::PreviewEngineRhiItem*>(
                                     QStringLiteral("previewSurface"))
                               : nullptr;
        auto* host =
            window ? window->findChild<QQuickItem*>(QStringLiteral("previewHost")) : nullptr;
        if (!window || !surface || !host) {
            std::fprintf(stderr, "FAIL: window / preview surface / preview host がありません\n");
            controller.shutdown();
            return 3;
        }
        window->installEventFilter(new mvm::app::FocusReleaseFilter(window));
        controller.attachPreview(surface);
        // どの経路で抜けても controller.shutdown() を通す。通さずに破棄すると
        // preview engine の teardown が QML engine の破棄と競合する。
        // key / mouse event は active な window にしか届かない (非 active になると Qt Quick は
        // activeFocusItem を外す)。検査中に他の操作で前面を奪われた run は、製品の失敗と
        // 混ぜずに PROTOCOL_INVALID として終える (AGENTS.md の Interactive measurement protocol)。
        bool activationLost = false;
        QObject::connect(window, &QWindow::activeChanged, window, [&] {
            if (!window->isActive())
                activationLost = true;
        });
        const auto run = [&]() -> int {
            window->requestActivate();
            if (!QTest::qWaitForWindowExposed(window) || !QTest::qWaitForWindowActive(window)) {
                std::fprintf(stderr, "PROTOCOL_INVALID: window が前面になりません。検査中は"
                                     "他の window を操作しないでください\n");
                return 4;
            }
            activationLost = false;
            if (!pumpUntil([&] { return controller.previewReady(); }, 30000)) {
                std::fprintf(stderr, "FAIL: preview が準備できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            // 起動直後は初回 seek の完了待ちで Seeking のことがある。受理されるまで再試行する。
            // seek の要求は毎回 stateChanged を出すので、間隔を空けて再試行する。
            const auto seekAccepted = [&] {
                for (int attempt = 0; attempt < 60; ++attempt) {
                    if (controller.seekTimelineFrame(10))
                        return true;
                    pump(500);
                }
                return false;
            };
            if (!seekAccepted()) {
                std::fprintf(stderr, "FAIL: 映像のある frame へ seek できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            pump(300);
            check(controller.previewVideoAtPlayhead(), "前提: playhead に映像がありません");

            // 0. 枠のドラッグのように effect を続けて変えると、先の変更の seek が終わる前に次が
            //    来る。途中の変更は捨ててよいが、最後の値 (C) は必ず preview に出る。
            {
                const QString videoId = QStringLiteral("video");
                controller.selectTimelineClips({videoId});
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                const QVariantMap a{{QStringLiteral("scaleX"), 90.0},
                                    {QStringLiteral("scaleY"), 90.0}};
                const QVariantMap b{{QStringLiteral("scaleX"), 70.0},
                                    {QStringLiteral("scaleY"), 70.0}};
                const QVariantMap c{{QStringLiteral("scaleX"), 50.0},
                                    {QStringLiteral("scaleY"), 40.0}};
                // event loop を回さずに 3 回続けて渡す (A の seek の途中で B・C が来る)。
                const bool accepted = controller.setClipEffectValues(videoId, a, false) &&
                                      controller.setClipEffectValues(videoId, b, false) &&
                                      controller.setClipEffectValues(videoId, c, false);
                check(accepted && !controller.previewPresentedLatest(),
                      "前提: 連続した effect 変更が seek の途中に重なっていません");
                // C: 幅 50%・高さ 40% を中央に置く。
                const auto showsC = [&] {
                    const auto destination = controller.submittedLayerDestination(videoId);
                    return destination && std::abs(destination->x() - 0.25) < 1e-4 &&
                           std::abs(destination->y() - 0.3) < 1e-4 &&
                           std::abs(destination->width() - 0.5) < 1e-4 &&
                           std::abs(destination->height() - 0.4) < 1e-4;
                };
                pumpUntil([&] { return controller.previewPresentedLatest() && showsC(); }, 10000);
                check(controller.previewPresentedLatest() && showsC(),
                      "seek の途中に重ねた最後の effect が preview に出ません");
                controller.cancelEffectPreview();
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);

                // 再生位置 (10) から離れた frame 200 へ素材を置く。置いた clip が current になり、
                // 再生位置も current も再生位置の clip (V1) へ戻らない。
                const qint64 playheadBefore = controller.playheadFrame();
                const int placedIndex = controller.clipCount();
                controller.addMediaItemsToTimelineAt({QStringLiteral("fixture-media-0")},
                                                     QStringLiteral("video"), 1, 200);
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                check(controller.clipCount() > placedIndex &&
                          controller.playheadFrame() == playheadBefore &&
                          controller.currentClipIndex() == placedIndex,
                      "離れた位置へ置いたclipがcurrentにならない、または再生位置が動きました");
                controller.undoLastEdit();
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
            }

            const auto scenePoint = [host](double fx, double fy) {
                return host->mapToScene(QPointF(host->width() * fx, host->height() * fy)).toPoint();
            };
            const int outputWidth = controller.property("outputWidth").toInt();
            const int outputHeight = controller.property("outputHeight").toInt();

            // 1. T で文字ツール、クリック、入力、Ctrl+Enter で確定。
            QTest::keyClick(window, Qt::Key_T);
            pump();
            check(window->property("timelineTool").toString() == QStringLiteral("text"),
                  "T で文字ツールになりません");
            QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.25, 0.25));
            pump();
            check(window->property("textEditing").toBool(), "クリックで文字の入力を始めません");
            typeText(window, "ABC");
            QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
            pumpUntil([&] { return textClipCount(controller) == 1; });
            check(textClipCount(controller) == 1, "Ctrl+Enter で文字 clip ができません");
            const QVariantMap created = controller.selectedTextClip();
            const QString clipId = created.value(QStringLiteral("clipId")).toString();
            check(created.value(QStringLiteral("content")).toString() == QStringLiteral("ABC"),
                  "入力した本文が保存されていません");
            const int createdX = created.value(QStringLiteral("x")).toInt();
            const int createdY = created.value(QStringLiteral("y")).toInt();
            check(std::abs(createdX - outputWidth / 4) <= 2 &&
                      std::abs(createdY - outputHeight / 4) <= 2,
                  "クリックした位置が文字の左上になっていません");
            check(controller.textOverlayClip().isEmpty(),
                  "確定後も文字を engine の合成から外したままです");

            // 2. Esc は破棄する。
            QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.6, 0.7));
            pump();
            check(window->property("textEditing").toBool(), "2 回目のクリックで入力を始めません");
            typeText(window, "XYZ");
            QTest::keyClick(window, Qt::Key_Escape);
            pump(300);
            check(!window->property("textEditing").toBool(), "Esc で入力を終えません");
            check(textClipCount(controller) == 1, "Esc で文字 clip を作りました");

            // 3. 選択ツールで文字をドラッグする。文字の画素を探して掴む。
            //    再生位置は文字 clip の先頭 (10) から離しておき、掴んでも動かないことを見る。
            constexpr qint64 kDragFrame = 40;
            bool movedPlayhead = false;
            for (int attempt = 0; attempt < 60 && !movedPlayhead; ++attempt) {
                movedPlayhead = controller.seekTimelineFrame(kDragFrame);
                if (!movedPlayhead)
                    pump(500);
            }
            check(movedPlayhead, "前提: 再生位置を動かせません");
            pump(300);
            QTest::keyClick(window, Qt::Key_V);
            pump();
            check(window->property("timelineTool").toString() == QStringLiteral("select"),
                  "V で選択ツールになりません");
            // QML と同じ換算 (floor(local * output / host)) で、押す scene 座標と
            // その上下左右 1 画素がすべて文字に当たる点を探す。縁を掴むと丸めで外れる。
            const auto hitsText = [&](QPoint scene) {
                const QPointF local = host->mapFromScene(QPointF(scene));
                const int px =
                    static_cast<int>(std::floor(local.x() * outputWidth / host->width()));
                const int py =
                    static_cast<int>(std::floor(local.y() * outputHeight / host->height()));
                return controller.textClipAt(px, py) == clipId;
            };
            const QPoint origin = scenePoint(static_cast<double>(createdX) / outputWidth,
                                             static_cast<double>(createdY) / outputHeight);
            QPoint grab;
            bool found = false;
            for (int dy = 0; dy < 60 && !found; ++dy)
                for (int dx = 0; dx < 120 && !found; ++dx) {
                    const QPoint candidate = origin + QPoint(dx, dy);
                    if (hitsText(candidate) && hitsText(candidate + QPoint(1, 0)) &&
                        hitsText(candidate - QPoint(1, 0)) && hitsText(candidate + QPoint(0, 1)) &&
                        hitsText(candidate - QPoint(0, 1))) {
                        grab = candidate;
                        found = true;
                    }
                }
            check(found, "掴める文字の画素が見つかりません");
            if (found) {
                const QPoint delta(40, 20);
                QTest::mousePress(window, Qt::LeftButton, {}, grab);
                pump(50);
                check(controller.textOverlayClip() == clipId,
                      "ドラッグ中の文字を engine の合成から外していません");
                for (int step = 1; step <= 4; ++step) {
                    QTest::mouseMove(window, grab + delta * step / 4);
                    pump(30);
                }
                QTest::mouseRelease(window, Qt::LeftButton, {}, grab + delta);
                pump(300);
                check(controller.textOverlayClip().isEmpty(),
                      "ドラッグ後も文字を engine の合成から外したままです");
                check(controller.property("playheadFrame").toLongLong() == kDragFrame,
                      "文字を掴むと再生位置が clip の先頭へ動きました");
                // 離した直後は位置の保存で seek 中である。そこへ合成を出し直して拒否されると、
                // 文字を外したままの composition が残り、文字が消えていた。
                pump(500);
                if (controller.statusText().contains(QStringLiteral("失敗")))
                    std::fprintf(stderr, "status: %s\n",
                                 controller.statusText().toUtf8().constData());
                check(!controller.statusText().contains(QStringLiteral("失敗")),
                      "ドラッグ後の preview 更新が失敗しました (文字が消える)");
                const QVariantMap moved = controller.textClipData(clipId);
                const int expectedX =
                    createdX +
                    static_cast<int>(std::lround(delta.x() * outputWidth / host->width()));
                const int expectedY =
                    createdY +
                    static_cast<int>(std::lround(delta.y() * outputHeight / host->height()));
                const int movedX = moved.value(QStringLiteral("x")).toInt();
                const int movedY = moved.value(QStringLiteral("y")).toInt();
                if (std::abs(movedX - expectedX) > 2 || std::abs(movedY - expectedY) > 2)
                    std::fprintf(stderr, "移動後 %d,%d / 期待 %d,%d / 作成時 %d,%d\n", movedX,
                                 movedY, expectedX, expectedY, createdX, createdY);
                check(std::abs(movedX - expectedX) <= 2 && std::abs(movedY - expectedY) <= 2,
                      "ドラッグした分だけ文字が動いていません");

                // 4. Ctrl+Z で位置が戻る。
                QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
                pumpUntil([&] {
                    return controller.textClipData(clipId).value(QStringLiteral("x")).toInt() ==
                           createdX;
                });
                const QVariantMap undone = controller.textClipData(clipId);
                check(undone.value(QStringLiteral("x")).toInt() == createdX &&
                          undone.value(QStringLiteral("y")).toInt() == createdY,
                      "Ctrl+Z で文字の位置が戻りません");

                // 5. 字形の無い所 (字間や字の内側) でも、文字の範囲の中なら掴める。
                const QRect bounds = controller.textClipBounds(clipId);
                // 期待値は製品の cache を使わず、保存された書式から描き直した画像で決める。
                const QVariantMap data = controller.textClipData(clipId);
                mvm::project::TextClipData text;
                text.content = data.value(QStringLiteral("content")).toString().toStdString();
                text.fontFamily = data.value(QStringLiteral("fontFamily")).toString().toStdString();
                text.fontSize = data.value(QStringLiteral("fontSize")).toInt();
                text.x = data.value(QStringLiteral("x")).toInt();
                text.y = data.value(QStringLiteral("y")).toInt();
                text.color = data.value(QStringLiteral("color")).toString().toStdString();
                text.bold = data.value(QStringLiteral("bold")).toBool();
                text.alignment = data.value(QStringLiteral("alignment")).toString().toStdString();
                text.outlineColor =
                    data.value(QStringLiteral("outlineColor")).toString().toStdString();
                text.outlineWidth = data.value(QStringLiteral("outlineWidth")).toInt();
                text.backgroundColor =
                    data.value(QStringLiteral("backgroundColor")).toString().toStdString();
                QString rasterError;
                const QImage raster =
                    mvm::app::renderTextRaster(text, outputWidth, outputHeight, rasterError);
                check(!raster.isNull(), "前提: 文字画像を描けません");
                QPoint gap;
                bool gapFound = false;
                for (int dy = 0; dy < 60 && !gapFound; ++dy)
                    for (int dx = 0; dx < 200 && !gapFound; ++dx) {
                        const QPoint candidate = origin + QPoint(dx, dy);
                        const QPointF local = host->mapFromScene(QPointF(candidate));
                        const int px =
                            static_cast<int>(std::floor(local.x() * outputWidth / host->width()));
                        const int py =
                            static_cast<int>(std::floor(local.y() * outputHeight / host->height()));
                        const QRect inner = bounds.adjusted(4, 4, -4, -4);
                        if (inner.contains(px, py) && !raster.isNull() &&
                            qAlpha(raster.pixel(px, py)) == 0) {
                            gap = candidate;
                            gapFound = true;
                        }
                    }
                check(gapFound, "前提: 文字の範囲の中に透明な画素が見つかりません");
                if (gapFound) {
                    QTest::mousePress(window, Qt::LeftButton, {}, gap);
                    pump(50);
                    check(controller.textOverlayClip() == clipId,
                          "字形の無い所では文字を掴めません");
                    QTest::mouseRelease(window, Qt::LeftButton, {}, gap);
                    pump(100);
                }
            }

            // 6. inspector の X を左右ドラッグすると、離す前から preview の文字が動き、
            //    Project は離したときにだけ変わる。
            auto* xField = window->findChild<QQuickItem*>(QStringLiteral("textNumberField_x"));
            check(xField != nullptr, "前提: inspector の X 欄がありません");
            if (xField) {
                const QRect before = controller.textClipBounds(clipId);
                const int savedX =
                    controller.textClipData(clipId).value(QStringLiteral("x")).toInt();
                const QPoint start =
                    xField->mapToScene(QPointF(xField->width() * 0.7, xField->height() / 2))
                        .toPoint();
                QTest::mousePress(window, Qt::LeftButton, {}, start);
                for (int step = 1; step <= 5; ++step) {
                    QTest::mouseMove(window, start + QPoint(step * 10, 0));
                    pump(30);
                }
                pump(200);
                const QRect during = controller.textClipBounds(clipId);
                check(during.x() > before.x(), "X のドラッグ中に preview の文字が動きません");
                check(controller.textClipData(clipId).value(QStringLiteral("x")).toInt() == savedX,
                      "X のドラッグ中に Project を書き換えました");
                QTest::mouseRelease(window, Qt::LeftButton, {}, start + QPoint(50, 0));
                pump(300);
                check(controller.textClipData(clipId).value(QStringLiteral("x")).toInt() > savedX,
                      "X のドラッグを離しても Project に保存されません");
            }

            // 8. 定位置ボタン。押すと下 15% の中央へ置き、X / Y
            // で動かした後に押しても同じ所へ戻る。
            auto* centerButton =
                window->findChild<QQuickItem*>(QStringLiteral("textPlaceButton_center"));
            check(centerButton != nullptr, "前提: 定位置ボタンがありません");
            if (centerButton) {
                const QPoint buttonPoint =
                    centerButton
                        ->mapToScene(QPointF(centerButton->width() / 2, centerButton->height() / 2))
                        .toPoint();
                QVariantMap data = controller.textClipData(clipId);
                mvm::project::TextClipData text;
                text.content = data.value(QStringLiteral("content")).toString().toStdString();
                text.fontFamily = data.value(QStringLiteral("fontFamily")).toString().toStdString();
                text.fontSize = data.value(QStringLiteral("fontSize")).toInt();
                text.bold = data.value(QStringLiteral("bold")).toBool();
                text.outlineWidth = data.value(QStringLiteral("outlineWidth")).toInt();
                text.alignment = "center";
                const auto expected =
                    mvm::app::textPresetPlacement(text, outputWidth, outputHeight, "center");
                check(expected.success, "前提: 定位置を求められません");
                for (int round = 0; round < 2; ++round) {
                    QTest::mouseClick(window, Qt::LeftButton, {}, buttonPoint);
                    pump(300);
                    data = controller.textClipData(clipId);
                    check(data.value(QStringLiteral("x")).toInt() == expected.x &&
                              data.value(QStringLiteral("y")).toInt() == expected.y &&
                              data.value(QStringLiteral("alignment")).toString() ==
                                  QStringLiteral("center"),
                          "定位置ボタンで中央下へ置かれません");
                    // 2 回目の前に位置をずらす。押し直すと定位置へ戻ること。
                    controller.updateTextClip(clipId,
                                              {{QStringLiteral("x"), 5}, {QStringLiteral("y"), 7}});
                    pump(300);
                }
            }

            const auto clickItem = [&](QQuickItem* item) {
                QTest::mouseClick(
                    window, Qt::LeftButton, {},
                    item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
            };
            const auto textValue = [&](const char* key) {
                return controller.textClipData(clipId).value(QString::fromLatin1(key)).toString();
            };

            // 9. 色は見本の横に非 modal で開き、選んでいる途中の色を preview へ反映する。
            //    Esc では Project を変えずに戻し、OK で確定する。
            auto* swatch = window->findChild<QQuickItem*>(QStringLiteral("textColorSwatch_color"));
            auto* picker = window->findChild<QObject*>(QStringLiteral("textColorPicker"));
            check(swatch && picker, "前提: 文字色の見本または color picker がありません");
            if (swatch && picker) {
                const QString originalColor = textValue("color");
                clickItem(swatch);
                pump(300);
                check(picker->property("visible").toBool(),
                      "色の見本を押しても color picker が開きません");
                check(!picker->property("modal").toBool() && !picker->property("dim").toBool(),
                      "color picker が背景を暗くしています");
                check(picker->property("x").toReal() >= swatch->width(),
                      "color picker が見本の横に出ていません");
                auto* yellow = findVisualItem(window, QStringLiteral("colorPreset_#FFFFE600"));
                check(yellow != nullptr, "前提: 色の候補がありません");
                if (yellow) {
                    const int serialBefore = controller.textPreviewSerial();
                    clickItem(yellow);
                    pump(300);
                    check(controller.textPreviewSerial() > serialBefore,
                          "選んでいる途中の色を preview へ反映しません");
                    check(textValue("color") == originalColor, "確定前に文字色を保存しました");
                    // 止まらずにドラッグし続けている間も、一定間隔で描き直す (throttle)。
                    // 止まるのを待つ実装では、離すまで 1 回も描き直されない。
                    auto* colorField = findVisualItem(window, QStringLiteral("colorPickerField"));
                    check(colorField != nullptr, "前提: 色の面がありません");
                    if (colorField) {
                        const QPoint from = colorField
                                                ->mapToScene(QPointF(colorField->width() * 0.2,
                                                                     colorField->height() * 0.2))
                                                .toPoint();
                        QTest::mousePress(window, Qt::LeftButton, {}, from);
                        pump(30);
                        const int serialDragStart = controller.textPreviewSerial();
                        // 実際のマウスに近い 10ms 間隔で約 1.2 秒動かし続ける。
                        for (int step = 1; step <= 120; ++step) {
                            QTest::mouseMove(window, from + QPoint(step, step / 2));
                            pump(10);
                        }
                        const int redrawsDuringDrag =
                            controller.textPreviewSerial() - serialDragStart;
                        QTest::mouseRelease(window, Qt::LeftButton, {}, from + QPoint(120, 60));
                        pump(300);
                        if (redrawsDuringDrag < 3)
                            std::fprintf(stderr, "ドラッグ中の描き直し: %d 回\n",
                                         redrawsDuringDrag);
                        check(redrawsDuringDrag >= 3,
                              "色の面をドラッグしている間に preview が描き直されません");
                    }
                    const int serialPicked = controller.textPreviewSerial();
                    QTest::keyClick(window, Qt::Key_Escape);
                    pump(300);
                    check(!picker->property("visible").toBool(),
                          "Esc で color picker が閉じません");
                    check(textValue("color") == originalColor, "Esc で文字色を保存しました");
                    check(controller.textPreviewSerial() > serialPicked,
                          "Esc で preview を元の色へ戻していません");

                    clickItem(swatch);
                    pump(300);
                    clickItem(yellow);
                    pump(200);
                    auto* ok = findVisualItem(window, QStringLiteral("colorPickerOk"));
                    check(ok != nullptr, "前提: OK ボタンがありません");
                    if (ok)
                        clickItem(ok);
                    pump(300);
                    check(textValue("color") == QStringLiteral("#FFFFE600"),
                          "OK で文字色を保存しません");
                }
            }

            // 10. フォント一覧で hover した書体を preview へ仮に反映し、選ばずに閉じれば戻す。
            auto* fontBox = window->findChild<QQuickItem*>(QStringLiteral("textFontBox"));
            check(fontBox != nullptr, "前提: フォントの一覧がありません");
            if (fontBox) {
                const QString originalFont = textValue("fontFamily");
                clickItem(fontBox);
                pump(300);
                QQuickItem* candidate = nullptr;
                for (QQuickItem* item : visualItems(window)) {
                    const QVariant family = item->property("modelData");
                    if (!item->inherits("QQuickItemDelegate") || !item->isVisible() ||
                        !family.isValid() || family.toString() == originalFont ||
                        item->width() <= 0)
                        continue;
                    // ListView は表示範囲の外にも delegate を作る。見えている行だけを使う。
                    QQuickItem* list = item->parentItem();
                    while (list && !list->inherits("QQuickListView"))
                        list = list->parentItem();
                    const QPointF center =
                        item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                    if (list && list->mapRectToScene(QRectF(0, 0, list->width(), list->height()))
                                    .adjusted(0, 4, 0, -4)
                                    .contains(center)) {
                        candidate = item;
                        break;
                    }
                }
                check(candidate != nullptr, "前提: hover できる別の書体が一覧にありません");
                if (candidate) {
                    const int serialBefore = controller.textPreviewSerial();
                    const QPoint over =
                        candidate
                            ->mapToScene(QPointF(candidate->width() / 2, candidate->height() / 2))
                            .toPoint();
                    QTest::mouseMove(window, over - QPoint(0, 1));
                    QTest::mouseMove(window, over);
                    pump(400);
                    check(controller.textPreviewSerial() > serialBefore,
                          "hover した書体を preview へ反映しません");
                    check(textValue("fontFamily") == originalFont,
                          "hover だけで書体を保存しました");
                    const int serialHovered = controller.textPreviewSerial();
                    QTest::keyClick(window, Qt::Key_Escape);
                    pump(300);
                    auto* fontPopup = fontBox->property("popup").value<QObject*>();
                    check(fontPopup && !fontPopup->property("visible").toBool(),
                          "Esc でフォントの一覧が閉じません");
                    check(textValue("fontFamily") == originalFont,
                          "閉じただけで書体を保存しました");
                    check(controller.textPreviewSerial() > serialHovered,
                          "一覧を閉じても preview を元の書体へ戻していません");
                }
            }

            // 7. inspector の本文欄に focus を移しても、preview を押せば単キー操作が戻る。
            auto* contentEditor =
                window->findChild<QQuickItem*>(QStringLiteral("textContentEditor"));
            check(contentEditor && contentEditor->isVisible(),
                  "前提: inspector の本文欄がありません");
            if (contentEditor && contentEditor->isVisible()) {
                const QPoint editorPoint = contentEditor
                                               ->mapToScene(QPointF(contentEditor->width() / 2,
                                                                    contentEditor->height() / 2))
                                               .toPoint();
                QTest::mouseClick(window, Qt::LeftButton, {}, editorPoint);
                pump();
                check(window->property("keyboardFocusTakesKeys").toBool(),
                      "前提: 本文欄を押しても入力 focus になりません");
                QTest::mouseClick(window, Qt::LeftButton, {}, scenePoint(0.9, 0.9));
                pump();
                check(!window->property("keyboardFocusTakesKeys").toBool(),
                      "preview を押しても本文欄が focus を持ち続け、単キー操作が止まったままです");
                // 選択ツールで preview の映像を押すと、その映像 (V1) が選ばれて枠が出る。
                check(controller.transformClipId() == QStringLiteral("video"),
                      "preview で押した映像が選ばれません");
                QTest::keyClick(window, Qt::Key_T);
                pump();
                check(window->property("timelineTool").toString() == QStringLiteral("text"),
                      "本文欄の後で T が効きません");
                // 以降は文字の inspector を使うので、文字 clip を選び直す (再生位置は動かない)。
                controller.selectTimelineClips({clipId});
                pump();

                // 11. 本文欄の入力中は、timeline の shortcut (Ctrl+C/V/X/D, M, I, O) が
                //     Action へ流れず、文字入力側だけが受ける。
                const QString savedContent = textValue("content");
                const int clipsBefore = controller.clipCount();
                const qint64 inBefore = controller.inFrame();
                const qint64 outBefore = controller.outFrame();
                const int markersBefore = static_cast<int>(controller.timelineMarkers().size());
                QTest::mouseClick(window, Qt::LeftButton, {}, editorPoint);
                pump();
                check(window->property("keyboardFocusTakesKeys").toBool(),
                      "前提: 本文欄を押し直しても入力 focus になりません");
                typeText(window, "mio");
                pump();
                const QString typed = contentEditor->property("text").toString();
                check(typed.contains(QStringLiteral("mio")), "M / I / O が本文欄へ入力されません");
                check(static_cast<int>(controller.timelineMarkers().size()) == markersBefore &&
                          controller.inFrame() == inBefore && controller.outFrame() == outBefore,
                      "入力中の M / I / O で timeline のマーカー・イン・アウトが変わりました");
                QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
                QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
                pump();
                check(QGuiApplication::clipboard()->text() == typed,
                      "入力中の Ctrl+C が本文をコピーしません");
                QTest::keyClick(window, Qt::Key_D, Qt::ControlModifier);
                QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
                pump(300);
                check(contentEditor->property("text").toString() == typed,
                      "入力中の Ctrl+V が本文へ貼り付けられません");
                check(controller.clipCount() == clipsBefore,
                      "入力中の Ctrl+D / Ctrl+V で timeline の clip が増えました");
                // Esc で入力を取り消す。timeline の clipboard
                // は空のままなので貼り付けは拒否される。
                QTest::keyClick(window, Qt::Key_Escape);
                pump(300);
                check(!window->property("keyboardFocusTakesKeys").toBool() &&
                          textValue("content") == savedContent,
                      "Esc で本文の入力を取り消せません");
                check(!controller.pasteClips() && controller.clipCount() == clipsBefore,
                      "入力中の Ctrl+C が timeline の clip をコピーしました");
            }

            // 12. Delete の宛先は最後に押した場所で決まる。プロジェクトパネルに素材の選択が
            //     残っていても、timeline の clip を押した後の Delete は clip を消す。
            {
                QTest::keyClick(window, Qt::Key_Escape);
                QTest::keyClick(window, Qt::Key_V);
                window->setProperty("leftPanelTab", 1);
                pump(300);
                const QString mediaId = QStringLiteral("fixture-media-0");
                const auto center = [](QQuickItem* item) {
                    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2))
                        .toPoint();
                };
                const auto hasMedia = [&] {
                    return controller.mediaBinModel()->rowOfEntry(mediaId) >= 0;
                };
                auto* panel = findVisualItem(window, QStringLiteral("projectPanel"));
                auto* dialog = window->findChild<QObject*>(QStringLiteral("mediaBinRemoveDialog"));
                const auto dialogOpen = [&] {
                    return dialog && dialog->property("visible").toBool();
                };
                const auto mediaRow = [&] {
                    return findVisualItem(window, QStringLiteral("mediaBinRow_") + mediaId);
                };
                auto* clipItem = findVisualItem(window, QStringLiteral("timelineClip_video"));
                check(panel && mediaRow() && clipItem && dialog,
                      "前提: プロジェクトパネルの素材・timeline の clip・削除確認がありません");
                if (panel && mediaRow() && clipItem && dialog) {
                    // a. 素材 A を選び、timeline の clip B を押して Delete。B だけが消え、
                    //    素材の削除確認は出ない。
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(mediaRow()));
                    pump();
                    check(panel->property("selectedIds").toList().size() == 1,
                          "前提: プロジェクトパネルの素材を選べません");
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(clipItem));
                    pump();
                    const int clipsBefore = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(
                        controller.clipCount() < clipsBefore && !dialogOpen() && hasMedia(),
                        "素材を選んだ後に timeline の clip を押しても Delete が clip を消しません");
                    controller.undoLastEdit();
                    pump(300);

                    // b. プロジェクトパネルへ戻って素材 A を押し、Delete。素材の削除確認が出る
                    //    (使っている clip も消えるため)。閉じれば何も消えない。
                    QTest::mouseClick(window, Qt::LeftButton, {}, center(mediaRow()));
                    pump();
                    const int clipsBeforePanel = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(dialogOpen() && controller.clipCount() == clipsBeforePanel,
                          "プロジェクトパネルの素材を選んで Delete しても削除確認が出ません");
                    QMetaObject::invokeMethod(dialog, "close");
                    pump(300);
                    check(!dialogOpen() && hasMedia() && controller.clipCount() == clipsBeforePanel,
                          "削除確認を閉じても素材か clip が消えました");

                    // c. パネルの空白 (行のすぐ下。一覧の下端は横スクロールバー) を押して素材の
                    //    選択を外し (focus はパネルのまま)、Delete。選択中の timeline の clip
                    //    が消える。
                    controller.selectTimelineClips({QStringLiteral("video")});
                    pump();
                    auto* row = mediaRow();
                    QTest::mouseClick(window, Qt::LeftButton, {},
                                      row->mapToScene(QPointF(10, row->height() + 30)).toPoint());
                    pump();
                    check(panel->property("selectedIds").toList().isEmpty(),
                          "前提: パネルの空白を押しても素材の選択が外れません");
                    const int clipsBeforeEmpty = controller.clipCount();
                    QTest::keyClick(window, Qt::Key_Delete);
                    pump(300);
                    check(
                        controller.clipCount() < clipsBeforeEmpty && !dialogOpen() && hasMedia(),
                        "パネルで素材を選んでいないときの Delete が timeline の clip を消しません");
                    controller.undoLastEdit();
                    pump(300);
                }
            }
            return failures == 0 ? 0 : 1;
        };
        exitCode = run();
        if (activationLost) {
            // 途中で event が届かなかった可能性があるので、FAIL の有無にかかわらず判定しない。
            std::fprintf(stderr,
                         "PROTOCOL_INVALID: 検査中に window が非アクティブになりました "
                         "(FAIL %d 件は判定に使えません)。検査中は他の window を操作"
                         "しないでください\n",
                         failures);
            exitCode = 4;
        }
        controller.shutdown();
    }
    mvm_mlt_runtime_shutdown();
    if (exitCode == 0)
        std::puts("文字ツールの直接入力 (作成・Esc・ドラッグ・範囲で掴む・Undo・focus 解放) "
                  "・入力中の shortcut 抑止を確認しました");
    return exitCode;
}
