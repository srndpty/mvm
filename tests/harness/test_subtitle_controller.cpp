#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "app/text_raster.h"
#include "mvm_controller.h"
#include "preview_engine/preview_engine_internal.h"
#include "project/graph_edit.h"
#include "project/project_json.h"
#include "project/subtitles.h"
#include "timeline_wheel_filter.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickView>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QThread>
using mvm::app::MvmController;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}

bool pump(const std::function<bool()>& done) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 10000) {
        QGuiApplication::processEvents();
        QThread::msleep(2);
    }
    return done();
}

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? argv[1] : "";
    const bool native = mode.starts_with("--native");
    if (!native)
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(native ? QSGRendererInterface::Direct3D11
                                        : QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    QTemporaryDir temp;
    require(temp.isValid(), "作業フォルダー");
    if (native) {
        auto project = mvm::project::createDefaultProject();
        project.subtitles.emplace();
        project.subtitles->cues = {{"first", 5, 15, "最初の字幕", {}},
                                   {"next", 30, 50, "次の字幕", {}}};
        if (mode != "--native") {
            require(mvm::project::addGraph(project, "unrelated", {"f"}, "寄与しない Graph",
                                           {mvm::project::TrackKind::Video, 0}, 100)
                        .success,
                    "寄与しない Graph の追加");
            auto& graph = project.timelineClips.back();
            if (mode == "--native-disabled") {
                graph.timelineStartFrame = 0;
                graph.enabled = false;
            } else if (mode == "--native-hidden") {
                graph.timelineStartFrame = 0;
                project.videoTracks[0].muted = true;
            } else if (mode == "--native-absent") {
                graph.timelineStartFrame = 0;
                graph.graph.functions[0].expression = "x+";
            }
        }
        const auto path = std::filesystem::path(temp.filePath("native.mvm").toStdWString());
        require(mvm::project::saveProjectJson(project, path).success, "字幕だけのProject保存");
        MvmController controller(path, {}, project);
        QQuickWindow window;
        window.setFlags(mvm::app::testBackgroundWindowFlags());
        window.resize(640, 360);
        auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
        surface->setWidth(640);
        surface->setHeight(360);
        controller.attachPreview(surface);
        window.show();
        require(
            pump([&] { return controller.previewReady() && controller.previewPresentedLatest(); }),
            "字幕だけのnativeプレビュー初期化");
        const auto before = controller.previewEngineForTest()->telemetry().presentedFrameCount;
        const auto initialSeeks = mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(
                                      *controller.previewEngineForTest())
                                      .seekRequestCount;
        require(initialSeeks == 1, "字幕 native preview の初期 seek は厳密に一回");
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 300) {
            QGuiApplication::processEvents();
            QThread::msleep(2);
        }
        require(controller.previewEngineForTest()->telemetry().presentedFrameCount == before,
                "字幕だけの初期seekを繰り返さない");
        // 現在 frame に寄与しない cache の通知を初期化後にも明示的に届ける。
        Q_EMIT controller.graphRastersForTest().changed();
        timer.restart();
        while (timer.elapsed() < 300) {
            QGuiApplication::processEvents();
            QThread::msleep(2);
        }
        require(controller.previewEngineForTest()->telemetry().presentedFrameCount == before,
                "mapping に Graph が無い cache 通知は seek を増やさない");
        require(mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(
                    *controller.previewEngineForTest())
                        .seekRequestCount == initialSeeks,
                "寄与しない cache 通知は seek request 自体も増やさない");
        for (const auto [frame, layers] :
             {std::pair{4, 0}, {5, 1}, {14, 1}, {15, 0}, {29, 0}, {30, 1}, {49, 1}}) {
            require(controller.seekTimelineFrame(frame) &&
                        pump([&] { return controller.previewPresentedLatest(); }),
                    "字幕境界へnative seek");
            const auto presented = controller.lastPresentedFrameForTest();
            require(presented.outputFrame == frame &&
                        presented.layerCount == static_cast<std::uint32_t>(layers),
                    "実提示フレームと字幕レイヤーを比較");
        }
        require(controller.seekTimelineFrame(0) &&
                    pump([&] { return controller.previewPresentedLatest(); }) &&
                    controller.canPlay() && controller.playTimeline(),
                "字幕だけのタイムラインを再生");
        require(pump([&] { return controller.playheadFrame() >= 40; }),
                "字幕と空白区間を通して再生が進む");
        require(controller.pauseTimeline(), "字幕再生を停止");
        controller.shutdown();
        std::puts("字幕だけのnative表示境界と再生の検査に合格しました");
        return 0;
    }
    const auto path = std::filesystem::path(temp.filePath("subtitles.mvm").toStdWString());
    auto project = mvm::project::createDefaultProject();
    mvm::project::MediaItem media;
    media.id = "audio";
    media.kind = mvm::project::MediaKind::Audio;
    media.mediaPath = std::filesystem::path(MVM_SUBTITLE_TEST_AUDIO);
    media.name = "音声";
    media.sampleRate = 48000;
    media.durationSamples = 240000;
    project.mediaItems.push_back(media);
    require(mvm::project::saveProjectJson(project, path).success, "初期プロジェクト保存");
    MvmController controller(path, {}, project);
    require(controller.addSubtitle("日本語の字幕", 0, 60), "字幕の追加");
    const auto first = controller.selectedSubtitleId();
    QString compositionError;
    auto composition = controller.subtitleCompositionForTest(0, compositionError);
    require(composition && composition->layers.size() == 1 && composition->layers.back().stillImage,
            "字幕のプレビュー合成レイヤー");
    require(controller.subtitleCompositionForTest(59, compositionError)->layers.size() == 1 &&
                controller.subtitleCompositionForTest(60, compositionError)->layers.empty(),
            "プレビューの終了前後フレーム");
    auto expectedRaster =
        mvm::app::renderSubtitleRaster({first.toStdString(), 0, 60, "日本語の字幕", {}}, {},
                                       project.outputWidth, project.outputHeight, compositionError)
            .convertToFormat(QImage::Format_RGBA8888);
    const auto& still = *composition->layers.back().stillImage;
    require(composition->layers.back().motion->evaluate(-1).opacity == 0 &&
                composition->layers.back().motion->evaluate(0).opacity == 1 &&
                composition->layers.back().motion->evaluate(59).opacity == 1 &&
                composition->layers.back().motion->evaluate(60).opacity == 0,
            "GUI通知の前でもrender側の区間評価は開始・終了境界を守る");
    require(!expectedRaster.isNull() &&
                still.rgba.size() == static_cast<std::size_t>(expectedRaster.sizeInBytes()) &&
                std::memcmp(still.rgba.data(), expectedRaster.constBits(), still.rgba.size()) == 0,
            "製品プレビューと共通ラスタの全画素を比較");
    require(controller.subtitleModel()->rowCount() == 1 && controller.totalTimelineFrames() == 60,
            "一覧とタイムライン尺");
    require(controller.updateSubtitle(first, "本文を修正", 0, 90), "本文・時刻の更新");
    require(controller.undoLastEdit() &&
                controller.selectedSubtitle().value("content").toString() == "日本語の字幕",
            "更新のUndo");
    require(controller.redoLastEdit() &&
                controller.selectedSubtitle().value("endFrame").toLongLong() == 90,
            "更新のRedo");
    require(controller.addSubtitle("次の字幕", 90, 150), "隣接字幕の追加");
    const auto second = controller.selectedSubtitleId();
    require(!controller.updateSubtitle(second, "重複", 50, 150) &&
                controller.selectedSubtitle().value("startFrame").toLongLong() == 90,
            "重複編集は不変");
    require(controller.setSubtitleStyle({{"fontSize", 42}, {"outlineWidth", 4}}) &&
                controller.subtitleStyle().value("fontSize").toInt() == 42,
            "共通書式変更");
    require(!controller.setSubtitleStyle({{"fontFamily", "mvm-missing-font"}}) &&
                controller.subtitleStyle().value("fontFamily").toString() == "Meiryo",
            "欠落フォントは部分適用しない");
    auto srtUrl = QUrl::fromLocalFile(temp.filePath("subtitles.srt"));
    require(controller.exportSubtitles(srtUrl), "SRT出力");
    const auto historyBeforeFailure = controller.undoDepthForTest();
    require(!controller.exportSubtitles(QUrl::fromLocalFile(temp.path())) &&
                controller.subtitleModel()->rowCount() == 2 &&
                controller.undoDepthForTest() == historyBeforeFailure,
            "保存先がディレクトリのSRT出力失敗では字幕と履歴を変更しない");
    require(controller.deleteSelectedSubtitle() && controller.subtitleModel()->rowCount() == 1,
            "字幕削除");
    require(controller.undoLastEdit() && controller.subtitleModel()->rowCount() == 2 &&
                controller.selectedSubtitleId() == second,
            "削除のUndoと選択復元");
    require(!controller.importSubtitles(srtUrl, false) &&
                controller.subtitleModel()->rowCount() == 2,
            "追加SRTの重複を拒否");
    require(controller.importSubtitles(srtUrl, true) && controller.subtitleModel()->rowCount() == 2,
            "SRT置換");
    // 共通書式の preview は Project と履歴を変えずに画素だけを変え、取り消すと元へ戻る。
    const auto subtitleBytes = [&] {
        QString error;
        auto layers = controller.subtitleCompositionForTest(0, error);
        require(layers && layers->layers.size() == 1, "書式 preview の対照群の字幕レイヤー");
        return layers->layers.back().stillImage->rgba;
    };
    const auto committedBytes = subtitleBytes();
    const auto historyBeforePreview = controller.undoDepthForTest();
    require(controller.previewSubtitleStyle({{"fontSize", 80}, {"color", "#FFFFD000"}}) &&
                controller.subtitleStyle().value("fontSize").toInt() == 42 &&
                controller.undoDepthForTest() == historyBeforePreview,
            "書式 preview は Project を変えない");
    require(subtitleBytes() != committedBytes, "書式 preview の画素が実際に変わる");
    controller.cancelSubtitleStylePreview();
    require(subtitleBytes() == committedBytes, "preview の取り消しで元の画素へ戻る");
    require(!controller.previewSubtitleStyle({{"fontSize", 0}}), "不正な書式は preview しない");
    require(controller.previewSubtitleStyle({{"fontSize", 80}}) &&
                controller.setSubtitleStyle({{"fontSize", 80}}) &&
                controller.subtitleStyle().value("fontSize").toInt() == 80 &&
                controller.undoDepthForTest() == historyBeforePreview + 1 &&
                controller.undoLastEdit() &&
                controller.subtitleStyle().value("fontSize").toInt() == 42,
            "preview 後の確定は 1 回の Undo");
    require(subtitleBytes() == committedBytes, "確定の Undo 後は preview を残さない");
    // timeline で選んだ字幕は clip と同じ操作 (Delete・Ctrl+C/X/V) の対象になる。
    const auto cueIdAt = [&](int row) {
        return controller.subtitleModel()
            ->data(controller.subtitleModel()->index(row, 0), mvm::app::SubtitleListModel::CueId)
            .toString();
    };
    const auto cueStartAt = [&](int row) {
        return controller.subtitleModel()
            ->data(controller.subtitleModel()->index(row, 0),
                   mvm::app::SubtitleListModel::StartFrame)
            .toLongLong();
    };
    require(controller.playheadFrame() == 0 && cueStartAt(0) == 0, "ペースト位置の前提");
    require(!controller.canDeleteSelection() &&
                controller.selectTimelineSubtitle(cueIdAt(0), false) &&
                controller.canDeleteSelection() && controller.selectedSubtitleIds().size() == 1,
            "timeline の字幕選択で Delete の対象になる");
    const auto historyBeforePaste = controller.undoDepthForTest();
    require(controller.copySelectedClips() && !controller.pasteClips() &&
                controller.subtitleModel()->rowCount() == 2 &&
                controller.undoDepthForTest() == historyBeforePaste,
            "既存の字幕と重なるペーストは拒否し、履歴を変えない");
    require(controller.cutSelectedClips() && controller.subtitleModel()->rowCount() == 1,
            "字幕のカット");
    require(controller.pasteClips() && controller.subtitleModel()->rowCount() == 2 &&
                cueStartAt(0) == 0 && cueIdAt(0) != controller.selectedSubtitleIds().value(1) &&
                controller.selectedSubtitleIds().size() == 1,
            "再生位置へのペーストと貼った字幕の選択");
    require(controller.selectTimelineSubtitle(cueIdAt(1), true) &&
                controller.selectedSubtitleIds().size() == 2 && controller.deleteSelection() &&
                controller.subtitleModel()->rowCount() == 0,
            "複数選択した字幕の削除");
    require(controller.undoLastEdit() && controller.subtitleModel()->rowCount() == 2,
            "字幕の削除は 1 回の Undo");
    require(controller.selectedSubtitleIds().size() == 2 && controller.canDeleteSelection() &&
                controller.copySelectedClips() &&
                controller.statusText().startsWith(QStringLiteral("2件")),
            "Undo で字幕の複数選択も戻り、コピーの対象も 2 件になる");
    require(controller.redoLastEdit() && controller.subtitleModel()->rowCount() == 0 &&
                controller.selectedSubtitleIds().isEmpty() && controller.undoLastEdit(),
            "Redo では削除した字幕を選択に残さない");
    require(controller.selectTimelineSubtitle(cueIdAt(0), false) &&
                controller.selectTimelineClips({}) && controller.selectedSubtitleIds().isEmpty() &&
                !controller.canDeleteSelection(),
            "clip 側の選択操作で字幕の選択を外す");
    // 選択が空になったら、パネルの編集対象 (主選択) も外す。timeline とパネルで別の字幕を
    // 指さない。
    const auto selectionCleared = [&] {
        return controller.selectedSubtitleIds().isEmpty() &&
               controller.selectedSubtitleId().isEmpty() && controller.selectedSubtitle().isEmpty();
    };
    require(controller.selectTimelineSubtitle(cueIdAt(0), false) &&
                controller.selectedSubtitleId() == cueIdAt(0) &&
                controller.selectTimelineSubtitlesInRange(100000, 100010) && selectionCleared(),
            "空白の矩形選択で主選択も外す");
    require(controller.selectTimelineSubtitle(cueIdAt(0), false) &&
                controller.selectTimelineSubtitle(cueIdAt(0), true) && selectionCleared(),
            "Ctrl+クリックで最後の字幕の選択を外すと主選択も外す");
    require(controller.selectTimelineSubtitle(cueIdAt(0), false) &&
                controller.selectTimelineSubtitle(cueIdAt(1), true) &&
                controller.selectedSubtitleId() == cueIdAt(1) &&
                controller.selectTimelineSubtitle(cueIdAt(1), true) &&
                controller.selectedSubtitleId() == cueIdAt(0),
            "主選択を選択から外すと、残った字幕が主選択になる");
    std::atomic<bool> release{false};
    controller.setTranscriptionRunnerForTest([&](const auto&, const std::atomic<bool>* cancel) {
        while (!release.load() && !cancel->load())
            QThread::msleep(2);
        mvm::transcribe::Result result;
        result.success = !cancel->load();
        result.cancelled = cancel->load();
        result.segments = {{0, 1000, "認識した本文"}};
        return result;
    });
    const auto modelUrl = QUrl::fromLocalFile(temp.filePath("model.bin"));
    require(controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 180),
            "バックグラウンド認識の開始");
    require(!controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 180),
            "同時ジョブを拒否");
    require(controller.setSubtitlesVisible(false), "認識中の編集");
    release.store(true);
    require(pump([&] { return !controller.transcribing(); }) && !controller.canApplyTranscription(),
            "revision変更後の適用を拒否");
    require(!controller.applyTranscription(true), "古い認識結果は保存しない");
    require(controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 180),
            "認識の再実行");
    require(pump([&] { return !controller.transcribing(); }) && controller.canApplyTranscription(),
            "候補を作成");
    require(controller.subtitleModel()->rowCount() == 2 &&
                controller.transcriptionModel()->rowCount() == 1,
            "適用前に字幕を変更しない");
    const auto cue =
        controller.transcriptionModel()
            ->data(controller.transcriptionModel()->index(0, 0), mvm::app::SubtitleListModel::CueId)
            .toString();
    require(controller.updateTranscriptionCue(cue, "候補を修正", 180, 240), "認識候補の編集");
    require(controller.applyTranscription(true) && controller.subtitleModel()->rowCount() == 1,
            "認識候補の置換適用");
    require(controller.undoLastEdit() && controller.subtitleModel()->rowCount() == 2,
            "認識適用は1回のUndo");
    release.store(false);
    require(controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 0),
            "キャンセル試験の開始");
    controller.cancelTranscription();
    require(pump([&] { return !controller.transcribing(); }) &&
                !controller.canApplyTranscription() && controller.subtitleModel()->rowCount() == 2,
            "キャンセル時は字幕不変");
    require(controller.saveProject(), "同じProjectを開き直す試験の保存");
    release.store(false);
    require(controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 0),
            "開き直す前の認識開始");
    require(controller.openProject(QUrl::fromLocalFile(QString::fromStdWString(path.wstring()))),
            "同じProjectを開き直す");
    release.store(true);
    require(pump([&] { return !controller.transcribing(); }) && !controller.canApplyTranscription(),
            "同じパスのProject切替でも古い結果を拒否");
    // 実際のQMLをロードし、一覧と書式のUIが生成されることを確認する。
    QQuickView view;
    view.setFlags(mvm::app::testBackgroundWindowFlags());
    view.setInitialProperties({{"mvmController", QVariant::fromValue(&controller)}});
    view.setSource(QUrl::fromLocalFile(MVM_SUBTITLE_PANEL_QML));
    for (const auto& error : view.errors())
        std::fprintf(stderr, "%s\n", error.toString().toUtf8().constData());
    require(view.status() == QQuickView::Ready, "字幕パネルのQML生成");
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(600, 650);
    view.show();
    QGuiApplication::processEvents();
    QThread::msleep(50);
    QGuiApplication::processEvents();
    const auto image = view.grabWindow();
    require(!image.isNull(), "字幕パネルの描画");
    image.save("subtitle-panel-test.png");
    // 実際の低いパネルでも、先頭の認識ボタンと最下部の編集ボタンへ内部スクロールで到達できる。
    for (const auto& dimensions : {QSize(560, 335), QSize(320, 300)}) {
        view.resize(dimensions);
        QGuiApplication::processEvents();
        auto* panel = view.rootObject();
        auto* flickable = qvariant_cast<QQuickItem*>(panel->property("contentItem"));
        auto* action = panel->findChild<QQuickItem*>(QStringLiteral("transcribeOpenButton"));
        auto* styleEditor = panel->findChild<QQuickItem*>(QStringLiteral("subtitleStyleEditor"));
        require(panel->clip() && flickable && action && styleEditor,
                "字幕パネルのクリップと内部スクロール");
        auto* list = panel->findChild<QQuickItem*>(QStringLiteral("subtitleList"));
        require(styleEditor->isVisible() && styleEditor->height() > 0 && list &&
                    styleEditor->mapToItem(panel, QPointF()).y() <
                        list->mapToItem(panel, QPointF()).y(),
                "共通書式はパネル内に展開し、字幕一覧より上に表示する");
        // 一覧の左右にはパネル全体をスクロールできる余白を残す。
        const auto listLeft = list->mapToItem(panel, QPointF()).x();
        const auto listRight = panel->width() - (listLeft + list->width());
        require(listLeft >= 16 && listRight >= 16,
                "字幕一覧の左右にパネルをスクロールできる余白がある");
        require(flickable->property("boundsBehavior").toInt() == 0 &&
                    flickable->property("boundsMovement").toInt() == 0,
                "字幕パネルは端で止まり、跳ね返らない");
        const auto inside = [&](QQuickItem* item) {
            const auto position = item->mapToItem(panel, QPointF());
            return position.y() >= 0 && position.y() + item->height() <= panel->height() &&
                   position.x() >= 0 && position.x() + item->width() <= panel->width();
        };
        flickable->setProperty("contentY", 0);
        QGuiApplication::processEvents();
        require(inside(action), "先頭の認識ボタンは親領域内で表示される");
        const auto maximum = flickable->property("contentHeight").toReal() - flickable->height();
        require(maximum > 0, "低いパネルでは縦スクロールが必要");
        flickable->setProperty("contentY", maximum);
        QGuiApplication::processEvents();
        // 最下部の編集ボタンの行が親領域の内側に来る。
        auto* editButtons = panel->findChild<QQuickItem*>(QStringLiteral("subtitleEditButtons"));
        require(editButtons && inside(editButtons), "最下部の編集ボタンは親領域内で表示される");
        const auto compactImage = view.grabWindow();
        require(!compactImage.isNull(), "狭い字幕パネルの描画");
        compactImage.save(QStringLiteral("subtitle-panel-%1x%2.png")
                              .arg(dimensions.width())
                              .arg(dimensions.height()));
        for (const auto* name : {"subtitleTranscribeDialog"}) {
            auto* dialog = panel->findChild<QObject*>(QString::fromLatin1(name));
            require(dialog && QMetaObject::invokeMethod(dialog, "open"), "字幕ダイアログを開く");
            QGuiApplication::processEvents();
            require(dialog->property("visible").toBool() &&
                        dialog->property("width").toReal() <= view.width() &&
                        dialog->property("height").toReal() <= view.height() &&
                        dialog->property("x").toReal() >= 0 && dialog->property("y").toReal() >= 0,
                    "字幕ダイアログは実ウィンドウの範囲に収まる");
            auto* scroll = qvariant_cast<QQuickItem*>(dialog->property("contentItem"));
            auto* internal = qvariant_cast<QQuickItem*>(scroll->property("contentItem"));
            require(internal && internal->property("boundsBehavior").toInt() == 0 &&
                        internal->property("boundsMovement").toInt() == 0,
                    "ダイアログの内部スクロールも端で止まる");
            if (std::string_view(name) == "subtitleTranscribeDialog") {
                auto* applyMode =
                    panel->findChild<QQuickItem*>(QStringLiteral("transcriptionApplyMode"));
                auto* footer = qvariant_cast<QQuickItem*>(dialog->property("footer"));
                require(applyMode && footer, "候補の適用方法をフッターに表示する");
                const auto modePosition = applyMode->mapToItem(footer, QPointF());
                require(modePosition.x() >= 0 && modePosition.y() >= 0 &&
                            modePosition.x() + applyMode->width() <= footer->width() &&
                            modePosition.y() + applyMode->height() <= footer->height(),
                        "適用方法の選択欄がフッターの内側に収まる");
            }
            view.grabWindow().save(QStringLiteral("%1-%2x%3.png")
                                       .arg(QString::fromLatin1(name))
                                       .arg(dimensions.width())
                                       .arg(dimensions.height()));
            require(QMetaObject::invokeMethod(dialog, "close"), "字幕ダイアログを閉じる");
        }
    }
    controller.setTranscriptionRunnerForTest([](const auto&, const auto*) {
        mvm::transcribe::Result result;
        result.success = true;
        for (int index = 0; index < 20; ++index)
            result.segments.push_back({index * 1000, (index + 1) * 1000, "スクロール検査の候補"});
        return result;
    });
    require(controller.startTranscription("audio", false, modelUrl, "cpu", "ja", 0) &&
                pump([&] { return !controller.transcribing(); }),
            "スクロール検査の長い候補一覧を作る");
    view.resize(650, 760);
    QQmlEngine probeEngine;
    QQmlComponent probeComponent(&probeEngine);
    probeComponent.setData("import QtQuick\nItem { property int wheelCalls: 0; "
                           "function handleNativePlainWheel(delta) { wheelCalls++; } }",
                           QUrl());
    std::unique_ptr<QQuickItem> timelineProbe(qobject_cast<QQuickItem*>(probeComponent.create()));
    require(static_cast<bool>(timelineProbe), "背面タイムラインの入力検査を作る");
    timelineProbe->setWidth(view.width());
    timelineProbe->setHeight(view.height());
    TimelineWheelEventFilter wheelFilter(&view, timelineProbe.get());
    view.installEventFilter(&wheelFilter);
    const auto sendWheel = [&](QPointF point) {
        QWheelEvent event(point, view.mapToGlobal(point.toPoint()), {}, QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &event);
        QGuiApplication::processEvents();
    };
    view.setProperty("timelineWheelBlocked", false);
    sendWheel(QPointF(100, 100));
    require(timelineProbe->property("wheelCalls").toInt() == 1,
            "ダイアログが閉じている対照群ではタイムラインへホイールを渡す");
    auto* recognitionDialog =
        view.rootObject()->findChild<QObject*>(QStringLiteral("subtitleTranscribeDialog"));
    require(QMetaObject::invokeMethod(recognitionDialog, "open"), "候補一覧のダイアログを開く");
    QGuiApplication::processEvents();
    view.setProperty("timelineWheelBlocked", true);
    auto* candidates =
        view.rootObject()->findChild<QQuickItem*>(QStringLiteral("transcriptionCandidates"));
    // 描画 (polish) の前は一覧が余った高さへ広がる前の寸法なので、描画してから測る。
    view.grabWindow();
    require(candidates && candidates->property("contentHeight").toReal() > candidates->height(),
            "候補一覧は実際にスクロールできる尺を持つ");
    // 利用者は描画された画面の上でホイールを回す。描画 (polish) の前は候補一覧が余った高さへ
    // 広がる前の位置にあり、一覧の中心がフッターに重なる (実測 y=552 → 描画後 472)。
    view.grabWindow().save(QStringLiteral("subtitleTranscribeDialog-candidates.png"));
    const auto initialPosition = candidates->property("contentY").toReal();
    sendWheel(candidates->mapToScene(QPointF(candidates->width() / 2, candidates->height() / 2)));
    require(pump([&] { return candidates->property("contentY").toReal() > initialPosition; }) &&
                timelineProbe->property("wheelCalls").toInt() == 1,
            "実ホイールで前面の候補一覧だけが動き、背面タイムラインは動かない");
    require(QMetaObject::invokeMethod(recognitionDialog, "close"), "候補一覧を閉じる");
    // フォント一覧は modal でない popup で、背面の timeline に重なって開く。その上のホイールを
    // timeline へ渡さない (以前は一覧の下側で timeline がスクロールした)。
    view.setProperty("timelineWheelBlocked", false);
    auto* fontBox = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("subtitleFontBox"));
    auto* fontPopup = fontBox ? qvariant_cast<QObject*>(fontBox->property("popup")) : nullptr;
    require(fontPopup && QMetaObject::invokeMethod(fontPopup, "open"), "フォント一覧を開く");
    view.grabWindow();
    auto* fontList = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("subtitleFontList"));
    auto* fontSearch =
        view.rootObject()->findChild<QQuickItem*>(QStringLiteral("subtitleFontSearch"));
    require(fontList && fontSearch && fontList->isVisible() &&
                fontList->property("contentHeight").toReal() > fontList->height(),
            "フォント一覧は実際にスクロールできる尺を持つ");
    const auto wheelCallsBeforeFont = timelineProbe->property("wheelCalls").toInt();
    // 一覧自身が動くことは実 window の text_ui_direct_input で確かめる (offscreen の
    // QQuickView では modal でない popup の一覧へ合成ホイールが届かない)。ここでは
    // timeline 側が横取りしないことを確かめる。
    sendWheel(fontList->mapToScene(QPointF(fontList->width() / 2, fontList->height() - 10)));
    QGuiApplication::processEvents();
    require(timelineProbe->property("wheelCalls").toInt() == wheelCallsBeforeFont,
            "フォント一覧の下側のホイールを背面タイムラインへ渡さない");
    fontSearch->setProperty("text", QStringLiteral("GOTHIC"));
    QGuiApplication::processEvents();
    const auto matches = fontList->property("count").toInt();
    bool allMatch = matches > 0;
    for (const auto& family : fontList->property("model").toStringList())
        allMatch = allMatch && family.contains(QStringLiteral("gothic"), Qt::CaseInsensitive);
    require(allMatch, "検索欄の部分一致 (大文字小文字を区別しない) で一覧を絞り込む");
    fontSearch->setProperty("text", QStringLiteral("mvm-存在しない書体"));
    QGuiApplication::processEvents();
    require(fontList->property("count").toInt() == 0, "一致しない検索では一覧が空になる");
    require(QMetaObject::invokeMethod(fontPopup, "close"), "フォント一覧を閉じる");
    QGuiApplication::processEvents();
    sendWheel(QPointF(5, 5));
    require(timelineProbe->property("wheelCalls").toInt() == wheelCallsBeforeFont + 1,
            "一覧を閉じた後の対照群ではタイムラインへホイールを渡す");
    view.removeEventFilter(&wheelFilter);
    controller.shutdown();
    {
        // 認識した素材の出どころ。外部で同じ path の素材を差し替えたら、古い候補を適用しない。
        const auto copyPath = temp.filePath(QStringLiteral("provenance.wav"));
        require(QFile::copy(QString::fromUtf8(MVM_SUBTITLE_TEST_AUDIO), copyPath),
                "出どころ試験の素材を複製");
        auto provenanceProject = project;
        provenanceProject.mediaItems.front().mediaPath = copyPath.toStdWString();
        provenanceProject.subtitles.reset();
        const auto provenancePath =
            std::filesystem::path(temp.filePath("provenance.mvm").toStdWString());
        require(mvm::project::saveProjectJson(provenanceProject, provenancePath).success,
                "出どころ試験のProject保存");
        // 内容の hash を何回にも分けて読む大きさ (1 MiB ごと) にする。
        {
            QFile file(copyPath);
            require(file.open(QIODevice::Append) &&
                        file.write(QByteArray(8 * 1024 * 1024, '\0')) == 8 * 1024 * 1024,
                    "出どころ試験の素材を大きくする");
        }
        MvmController provenance(provenancePath, {}, provenanceProject);
        {
            // 素材の hash の途中でキャンセルすると、残りを読まずに終わり、認識もしない。
            std::atomic<int> chunks{0};
            std::atomic<int> chunksAfterCancel{0};
            std::atomic<bool> ran{false};
            std::atomic<bool> cancelRequested{false};
            provenance.setTranscriptionHashObserverForTest([&] {
                ++chunks;
                if (cancelRequested.load())
                    ++chunksAfterCancel;
                if (!cancelRequested.exchange(true))
                    provenance.cancelTranscription();
            });
            provenance.setTranscriptionRunnerForTest([&](const auto&, const auto*) {
                ran = true;
                return mvm::transcribe::Result{};
            });
            require(provenance.startTranscription("audio", false, modelUrl, "cpu", "ja", 0) &&
                        pump([&] { return !provenance.transcribing(); }),
                    "hash の途中のキャンセル試験");
            require(!ran.load() && chunksAfterCancel.load() <= 1 &&
                        !provenance.canApplyTranscription() &&
                        provenance.transcriptionError().contains(QStringLiteral("キャンセル")),
                    "素材の hash の途中でキャンセルすると、残りを読まずに終わる");
            // 対照群: キャンセルしなければ hash は何回にも分けて全部読む。
            chunks = 0;
            provenance.setTranscriptionHashObserverForTest([&] { ++chunks; });
            provenance.setTranscriptionRunnerForTest([](const auto&, const auto*) {
                mvm::transcribe::Result result;
                result.success = true;
                result.segments = {{0, 1000, "本文"}};
                return result;
            });
            require(provenance.startTranscription("audio", false, modelUrl, "cpu", "ja", 0) &&
                        pump([&] { return !provenance.transcribing(); }) &&
                        provenance.canApplyTranscription() && chunks.load() >= 2 * 9,
                    "対照群: 認識の前後で素材の hash を全部読む");
            provenance.setTranscriptionHashObserverForTest({});
        }
        std::atomic<bool> hold{true};
        provenance.setTranscriptionRunnerForTest([&](const auto&, const std::atomic<bool>* stop) {
            while (hold.load() && !stop->load())
                QThread::msleep(2);
            mvm::transcribe::Result result;
            result.success = true;
            result.segments = {{0, 1000, "差し替え前の本文"}};
            return result;
        });
        // 認識の途中で、size と更新時刻を保ったまま中央の 1 byte だけを書き換える
        // (内容全体の hash でしか見つからない差し替え)。
        require(provenance.startTranscription("audio", false, modelUrl, "cpu", "ja", 0),
                "出どころ試験の認識開始");
        QThread::msleep(100);
        {
            // 更新時刻は 100ns 単位 (FILETIME) で比べるので、Win32 で丸めずに戻す。
            const std::wstring widePath = copyPath.toStdWString();
            HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            require(handle != INVALID_HANDLE_VALUE, "素材の書き換えの準備");
            FILETIME created{}, accessed{}, written{};
            LARGE_INTEGER size{};
            require(GetFileTime(handle, &created, &accessed, &written) &&
                        GetFileSizeEx(handle, &size),
                    "素材の時刻と大きさを控える");
            LARGE_INTEGER middle{};
            middle.QuadPart = size.QuadPart / 2;
            char middleByte = 0;
            DWORD done = 0;
            require(SetFilePointerEx(handle, middle, nullptr, FILE_BEGIN) &&
                        ReadFile(handle, &middleByte, 1, &done, nullptr) && done == 1,
                    "中央の 1 byte を読む");
            middleByte = static_cast<char>(middleByte ^ 0x5a);
            require(SetFilePointerEx(handle, middle, nullptr, FILE_BEGIN) &&
                        WriteFile(handle, &middleByte, 1, &done, nullptr) && done == 1 &&
                        SetFileTime(handle, &created, &accessed, &written),
                    "中央の 1 byte を書き換えて更新時刻を元へ戻す");
            CloseHandle(handle);
        }
        hold.store(false);
        require(pump([&] { return !provenance.transcribing(); }) &&
                    !provenance.canApplyTranscription() &&
                    provenance.transcriptionError().contains(QStringLiteral("素材が変更")),
                "認識中の素材の差し替え (size・更新時刻は同じ) を検出して候補を作らない");
        // 対照群: 差し替えなければ候補を作る。その後、確認中に素材を差し替えると適用しない。
        require(provenance.startTranscription("audio", false, modelUrl, "cpu", "ja", 0) &&
                    pump([&] { return !provenance.transcribing(); }) &&
                    provenance.canApplyTranscription(),
                "対照群: 差し替えなければ候補を作る");
        {
            QFile file(copyPath);
            require(file.open(QIODevice::Append) && file.write("x", 1) == 1, "素材へ追記");
        }
        require(!provenance.applyTranscription(true) &&
                    provenance.subtitleModel()->rowCount() == 0 &&
                    provenance.transcriptionError().contains(QStringLiteral("認識した後に")),
                "候補の確認中に差し替えた素材へは適用しない");
        provenance.shutdown();
    }
    {
        // 文字起こしした音声 clip を Alt+ドラッグで複製する (利用者の環境でクラッシュした操作)。
        auto linkedProject = project;
        mvm::project::TimelineClip audioClip;
        audioClip.kind = mvm::project::TimelineClipKind::Audio;
        audioClip.mediaPath = media.mediaPath;
        audioClip.mediaItemId = media.id;
        audioClip.name = "音声";
        audioClip.id = "speech";
        audioClip.sourceFpsNum = 60;
        audioClip.sourceFrameCount = audioClip.sourceOutFrame = 300;
        audioClip.track = {mvm::project::TrackKind::Audio, 0};
        linkedProject.timelineClips = {audioClip};
        linkedProject.subtitles.emplace();
        linkedProject.subtitles->cues = {{"cue-a", 0, 60, "リンクした字幕", "speech"},
                                         {"cue-b", 90, 150, "二つ目", "speech"}};
        const auto linkedPath = std::filesystem::path(temp.filePath("linked.mvm").toStdWString());
        require(mvm::project::saveProjectJson(linkedProject, linkedPath).success,
                "リンク字幕のプロジェクト保存");
        MvmController linked(linkedPath, {}, linkedProject);
        require(linked.selectTimelineClip("speech", true) &&
                    linked.duplicateTimelineClipsAt("speech", "audio", 0, 600) &&
                    linked.timelineModel()->rowCount() == 2,
                "リンク字幕を持つ clip の Alt+ドラッグ複製");
        linked.shutdown();
    }
    std::puts("字幕controller・履歴・認識候補・キャンセル・QMLの検査に合格しました");
}
