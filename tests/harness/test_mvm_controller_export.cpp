#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "test_media_fixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QUrl>

namespace {
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QCoreApplication::processEvents();
    return predicate();
}

mvm::project::Project videoProject() {
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip video;
    video.id = "video";
    video.name = "video";
    video.mediaPath = L"C:/mvm-test-video.mp4";
    video.sourceFpsNum = 60;
    video.sourceFpsDen = 1;
    video.sourceFrameCount = 120;
    video.sourceOutFrame = 120;
    project.timelineClips.push_back(video);
    mvm::test::attachFixtureMedia(project);
    return project;
}

mvm::project::Project linkedProject() {
    auto project = videoProject();
    project.timelineClips[0].linkGroupId = "pair";
    auto audio = project.timelineClips[0];
    audio.id = "audio";
    audio.name = "audio";
    audio.kind = mvm::project::TimelineClipKind::Audio;
    audio.track = {mvm::project::TrackKind::Audio, 0};
    project.timelineClips.push_back(std::move(audio));
    return project;
}

// 素材を Project パネルへ登録済みにする。貼り付けは bin を照合するので、中身が偽の
// 素材を調べ直させない。media を指すように変えた clip は、この素材を指すよう付け替える。
void registerFixtureMedia(mvm::project::Project& project, const std::filesystem::path& media,
                          const std::string& folderId = {}) {
    mvm::project::MediaItem item;
    item.id = "media-" + std::to_string(project.mediaItems.size());
    item.mediaPath = media;
    item.name = "fixture.mp4";
    item.folderId = folderId;
    item.fpsNum = 60;
    item.frameCount = 120;
    item.width = 1920;
    item.height = 1080;
    for (auto& clip : project.timelineClips)
        if (clip.mediaPath == media)
            clip.mediaItemId = item.id;
    // 付け替えで使われなくなった素材は残さない (同じ試験の Project に余計な素材を増やさない)。
    const auto used = mvm::project::mediaItemsInUse(project);
    std::erase_if(project.mediaItems, [&](const mvm::project::MediaItem& existing) {
        return !used.contains(existing.id);
    });
    project.mediaItems.push_back(item);
}

// testで本物のExplorerを開かないよう、全export testは記録するだけのrevealerを渡す。
struct RevealRecorder {
    int calls = 0;
    std::filesystem::path lastPath;
    bool succeed = true;

    mvm::app::MvmController::FileRevealer revealer() {
        return [this](const std::filesystem::path& path, QString& error) {
            ++calls;
            lastPath = path;
            if (!succeed)
                error = QStringLiteral("fixture reveal failure");
            return succeed;
        };
    }
};

mvm::app::TimelineExportResult successResult(const mvm::app::TimelineExportRequest& request) {
    mvm::app::TimelineExportResult result;
    result.success = true;
    result.outputPath = request.outputPath;
    result.frameCount = 120;
    result.durationSec = 2;
    return result;
}

void testCompleteAndRestart(const std::filesystem::path& path) {
    std::atomic<int> runs{0};
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [&](const auto&, const auto& request) {
            ++runs;
            request.progress(60, 120);
            return successResult(request);
        },
        {}, reveals.revealer());
    const auto outputPath = path.parent_path() / L"out.mp4";
    const QUrl output = QUrl::fromLocalFile(QString::fromStdWString(outputPath.wstring()));
    check(controller.exportTimeline(output) && controller.exporting() && controller.busy(),
          "開始直後にexporting/busyが立ちません");
    check(pumpUntil([&] { return !controller.exporting(); }), "正常exportが完了しません");
    check(!controller.busy() && runs == 1 && controller.exportProgress() == 1,
          "正常完了後の状態が不正です");
    check(reveals.calls == 1 && reveals.lastPath == outputPath,
          "書き出し完了後に出力ファイルをExplorerで1回表示しません");
    check(controller.exportTimeline(output), "完了後に再exportできません");
    check(pumpUntil([&] { return !controller.exporting(); }) && runs == 2,
          "2回目のexportが完了しません");
    check(reveals.calls == 2, "2回目の書き出し完了でExplorer表示しません");
}

void testExportQualitySelection(const std::filesystem::path& path) {
    std::atomic<int> receivedCrf{-1};
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [&](const auto&, const auto& request) {
            receivedCrf.store(request.videoCrf);
            return successResult(request);
        },
        {}, reveals.revealer());
    const QUrl output = QUrl::fromLocalFile(
        QString::fromStdWString((path.parent_path() / L"quality.mp4").wstring()));

    const auto verify = [&](const QString& quality, int expectedCrf, const char* message) {
        receivedCrf.store(-1);
        check(controller.exportTimelineWithQuality(output, quality), message);
        check(pumpUntil([&] { return !controller.exporting(); }), "品質指定exportが完了しません");
        check(receivedCrf.load() == expectedCrf, message);
    };
    verify(QStringLiteral("high"), 18, "高品質をCRF 18へ変換できません");
    verify(QStringLiteral("standard"), 23, "標準品質をCRF 23へ変換できません");
    verify(QStringLiteral("compact"), 28, "容量優先をCRF 28へ変換できません");

    QString failure;
    QObject::connect(&controller, &mvm::app::MvmController::exportFailed,
                     [&](const QString& message) { failure = message; });
    check(!controller.exportTimelineWithQuality(output, QStringLiteral("unknown")),
          "未知の品質を受理しました");
    check(failure == QStringLiteral("未知の書き出し品質です: unknown"),
          "未知の品質を利用者へ通知しません");
}

void testRevealFailureKeepsSuccess(const std::filesystem::path& path) {
    RevealRecorder reveals;
    reveals.succeed = false;
    QString failure;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [](const auto&, const auto& request) { return successResult(request); }, {},
        reveals.revealer());
    QObject::connect(&controller, &mvm::app::MvmController::exportFailed,
                     [&](const QString& message) { failure = message; });
    const QUrl output = QUrl::fromLocalFile(
        QString::fromStdWString((path.parent_path() / L"reveal-failure.mp4").wstring()));
    check(controller.exportTimeline(output), "Explorer表示失敗試験を開始できません");
    check(pumpUntil([&] { return !controller.exporting(); }), "Explorer表示失敗試験が完了しません");
    check(reveals.calls == 1 && failure.isEmpty() && controller.exportProgress() == 1 &&
              controller.statusText().startsWith(QStringLiteral("書き出しました: ")) &&
              controller.statusText().contains(QStringLiteral("fixture reveal failure")),
          "Explorer表示の失敗が書き出し成功を失敗へ変えた、または理由を表示しません");
}

void testEtaProgressText(const std::filesystem::path& path) {
    std::atomic<bool> release{false};
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [&](const auto&, const auto& request) {
            request.progress(0, 120);
            // ETAは1秒以上の観測が必要。基準点から1秒強待ってから半分まで進める。
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            request.progress(60, 120);
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return successResult(request);
        },
        {}, reveals.revealer());
    const QUrl output =
        QUrl::fromLocalFile(QString::fromStdWString((path.parent_path() / L"eta.mp4").wstring()));
    check(controller.exportTimeline(output), "ETA試験を開始できません");
    check(pumpUntil([&] {
              return controller.exportProgressText().contains(QStringLiteral("残り時間を計算中"));
          }),
          "基準点だけの段階で残り時間を計算中と表示しません");
    check(pumpUntil([&] {
              const QString text = controller.exportProgressText();
              return text.startsWith(QStringLiteral("60 / 120 frame")) &&
                     text.contains(QStringLiteral("残り約 "));
          }),
          "進捗が進んだ後に残り時間を表示しません");
    release.store(true);
    check(pumpUntil([&] { return !controller.exporting(); }), "ETA試験のexportが完了しません");
}

void testQueuedProgressAfterCancel(const std::filesystem::path& path) {
    std::promise<void> queued;
    auto queuedFuture = queued.get_future();
    std::atomic<bool> release{false};
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [&](const auto&, const auto& request) {
            request.progress(60, 120);
            queued.set_value();
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            mvm::app::TimelineExportResult result;
            result.cancelled = request.progress(61, 120);
            return result;
        },
        {}, reveals.revealer());
    const QUrl output = QUrl::fromLocalFile(
        QString::fromStdWString((path.parent_path() / L"cancel.mp4").wstring()));
    check(controller.exportTimeline(output), "キャンセル試験を開始できません");
    check(queuedFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "queued progressが届きません");
    controller.cancelTimelineExport();
    QCoreApplication::processEvents();
    check(controller.exportCancelling() &&
              controller.exportProgressText() == QStringLiteral("キャンセルしています…"),
          "キャンセル後にqueued progressが表示を上書きしました");
    release.store(true);
    check(pumpUntil([&] { return !controller.exporting(); }), "キャンセル後にworkerが停止しません");
    check(!controller.busy() && !controller.exportCancelling(), "キャンセル完了後の状態が不正です");
    check(reveals.calls == 0, "キャンセルした書き出しをExplorerで表示しました");
}

void testFailedExportNotification(const std::filesystem::path& path) {
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [](const auto&, const auto&) {
            mvm::app::TimelineExportResult result;
            result.error = "fixture export failure";
            return result;
        },
        {}, reveals.revealer());
    QString notification;
    int notificationCount = 0;
    QObject::connect(&controller, &mvm::app::MvmController::exportFailed,
                     [&](const QString& message) {
                         notification = message;
                         ++notificationCount;
                     });
    const QUrl output = QUrl::fromLocalFile(
        QString::fromStdWString((path.parent_path() / L"failed.mp4").wstring()));
    check(controller.exportTimeline(output), "失敗通知試験を開始できません");
    check(pumpUntil([&] { return !controller.exporting(); }), "失敗するexportが完了しません");
    check(notificationCount == 1 &&
              notification == QStringLiteral("書き出しに失敗しました: fixture export failure"),
          "書き出し失敗を利用者通知へ1回だけ渡せません");
    check(reveals.calls == 0, "失敗した書き出しをExplorerで表示しました");
}

void testShutdown(const std::filesystem::path& path, bool finishBeforeShutdown) {
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<bool> release{false};
    std::atomic<bool> stopped{false};
    RevealRecorder reveals;
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr,
        [&](const auto&, const auto& request) {
            entered.set_value();
            while (!release.load() && !request.progress(0, 120))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stopped.store(true);
            return successResult(request);
        },
        {}, reveals.revealer());
    const QUrl output = QUrl::fromLocalFile(
        QString::fromStdWString((path.parent_path() / L"shutdown.mp4").wstring()));
    check(controller.exportTimeline(output), "shutdown試験を開始できません");
    check(enteredFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "shutdown試験のworkerが起動しません");
    if (finishBeforeShutdown) {
        release.store(true);
        check(pumpUntil([&] { return stopped.load(); }), "完了直前のworkerが戻りません");
    }
    controller.shutdown();
    QCoreApplication::processEvents();
    check(stopped.load() && !controller.exporting() && !controller.busy(),
          "shutdownがworkerをjoinして状態を解放しません");
}

void testThreadFailure(const std::filesystem::path& path) {
    mvm::app::MvmController controller(
        path, {}, videoProject(), nullptr, {}, [](std::function<void()>) -> std::thread {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again));
        });
    QString notification;
    QObject::connect(&controller, &mvm::app::MvmController::exportFailed,
                     [&](const QString& message) { notification = message; });
    const QUrl output =
        QUrl::fromLocalFile(QString::fromStdWString((path.parent_path() / L"fail.mp4").wstring()));
    check(!controller.exportTimeline(output), "worker起動失敗を受理しました");
    check(!controller.exporting() && !controller.busy() && !controller.exportCancelling(),
          "worker起動失敗後の状態が戻りません");
    check(notification.startsWith(QStringLiteral("書き出しworkerを開始できません: ")),
          "worker起動失敗を利用者通知へ渡せません");
}

void testUndo(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "Undo試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.addTrack("video") && controller.videoTrackCount() == 3,
          "編集Aが保存されません");
    check(controller.addTrack("audio") && controller.audioTrackCount() == 2,
          "編集Bが保存されません");
    check(controller.undoLastEdit() && controller.videoTrackCount() == 3 &&
              controller.audioTrackCount() == 1,
          "1回目のUndoが編集Bの直前へ戻りません");
    check(controller.undoLastEdit() && controller.videoTrackCount() == 2 &&
              controller.audioTrackCount() == 1 && !controller.canUndo(),
          "2回目のUndoが編集Aの直前へ戻りません");
    const auto persisted = mvm::project::loadProjectJson(path);
    check(persisted.success && persisted.project.videoTracks.size() == 2 &&
              persisted.project.audioTracks.size() == 1,
          "Undoまでの未保存編集がcanonical Projectを書き換えました");

    check(controller.canRedo() && controller.redoLastEdit() && controller.videoTrackCount() == 3 &&
              controller.audioTrackCount() == 1,
          "1回目のRedoが編集Aの後へ進みません");
    check(controller.redoLastEdit() && controller.videoTrackCount() == 3 &&
              controller.audioTrackCount() == 2 && !controller.canRedo() && controller.canUndo(),
          "2回目のRedoが編集Bの後へ進みません");
    check(!controller.redoLastEdit(), "やり直す編集が無いRedoを受理しました");

    // Undo の後に新しい編集をしたら、やり直し先は無くなる。
    check(controller.undoLastEdit() && controller.canRedo(), "Redo破棄試験のUndoに失敗しました");
    check(controller.addTrack("video") && !controller.canRedo() &&
              controller.videoTrackCount() == 4 && controller.audioTrackCount() == 1,
          "新しい編集の後もRedoが残っています");
    check(controller.undoLastEdit() && controller.videoTrackCount() == 3 &&
              controller.audioTrackCount() == 1,
          "新しい編集のUndoが編集Aの後へ戻りません");
}

void testClipboardAndMarks(const std::filesystem::path& path) {
    auto initial = videoProject();
    const auto media = path.parent_path() / L"clipboard-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    initial.timelineClips[0].mediaPath = media;
    registerFixtureMedia(initial, media);
    check(mvm::project::saveProjectJson(initial, path).success,
          "コピー試験のProjectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.selectTimelineClips({QStringLiteral("video")}) &&
              controller.copySelectedClips(),
          "選択clipをコピーできません");
    check(controller.duplicateSelectedClips() && controller.clipCount() == 2 &&
              controller.videoTrackCount() == 2,
          "重なる位置の複製で空き映像trackを使えません");
    check(controller.undoLastEdit() && controller.clipCount() == 1 && controller.redoLastEdit() &&
              controller.clipCount() == 2,
          "複製全体を一回のUndo/Redoで戻せません");
    check(controller.duplicateSelectedClips() && controller.clipCount() == 3 &&
              controller.videoTrackCount() == 3 && controller.undoLastEdit(),
          "空きtrackがない複製で新しい映像trackを追加できません");
    check(controller.cutSelectedClips() && controller.clipCount() == 1 && controller.pasteClips() &&
              controller.clipCount() == 2,
          "カット後にclipを再生ヘッドへペーストできません");
    check(controller.addTimelineMarker() && controller.timelineMarkers().size() == 1 &&
              controller.addTimelineMarker() && controller.timelineMarkers().size() == 1,
          "同じframeのマーカーを重複させました");
    check(controller.markIn() && !controller.markOut(), "同じframeのイン・アウトを受理しました");
    controller.seekTimelineFrame(100);
    check(controller.playheadFrame() == 100 && controller.markOut() && controller.outFrame() == 100,
          "アウトを再生ヘッドに設定できません");
    check(controller.addTimelineMarker() && controller.timelineMarkers().size() == 2,
          "別frameのマーカーを追加できません");
    controller.jumpToMarker(-1);
    const bool atPreviousMarker = controller.playheadFrame() == 0;
    controller.jumpToMarker(1);
    check(atPreviousMarker && controller.playheadFrame() == 100, "前後のマーカーへ移動できません");
    check(!controller.deleteTimelineMarker(99) && controller.timelineMarkers().size() == 2,
          "存在しないマーカーの削除でProjectが変わりました");
    check(controller.deleteTimelineMarker(100) && controller.timelineMarkers().size() == 1 &&
              controller.undoLastEdit() && controller.timelineMarkers().size() == 2 &&
              controller.redoLastEdit() && controller.timelineMarkers().size() == 1,
          "マーカー削除を一回のUndo/Redoで戻せません");
    controller.seekTimelineFrame(90);
    check(controller.playheadFrame() == 90, "マーカー試験の開始位置へ移動できません");
    check(controller.addTimelineMarker() && controller.timelineMarkers().size() == 2,
          "編集点として使うマーカーを追加できません");
    controller.seekTimelineFrame(100);
    check(controller.playheadFrame() == 100, "マーカーの次のframeへ移動できません");
    controller.jumpToEditPoint(-1);
    check(controller.playheadFrame() == 90, "マーカーを編集点として移動できません");
    controller.jumpToIn();
    const bool atIn = controller.playheadFrame() == 0;
    controller.jumpToOut();
    check(atIn && controller.playheadFrame() == 100 && controller.clearIn() &&
              controller.inFrame() == -1 && controller.outFrame() == 100 &&
              controller.undoLastEdit() && controller.inFrame() == 0,
          "インだけを消去してUndoできません");
    check(controller.clearOut() && controller.inFrame() == 0 && controller.outFrame() == -1 &&
              controller.undoLastEdit() && controller.outFrame() == 100,
          "アウトだけを消去してUndoできません");
    check(controller.clearInOut() && controller.inFrame() == -1 && controller.outFrame() == -1 &&
              !controller.clearIn() && !controller.clearOut(),
          "イン・アウトの消去と未設定時の拒否ができません");
}

void testClipboardAcrossProject(const std::filesystem::path& sourcePath) {
    auto source = videoProject();
    const auto media = sourcePath.parent_path() / L"cross-project-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    source.timelineClips[0].mediaPath = media;
    source.mediaFolders.push_back({"folder", "素材", ""});
    registerFixtureMedia(source, media, "folder");
    const auto destinationPath = sourcePath.parent_path() / L"clipboard-destination.mvm";
    check(mvm::project::saveProjectJson(source, sourcePath).success &&
              mvm::project::saveProjectJson(mvm::project::createDefaultProject(), destinationPath)
                  .success,
          "別Project貼り付け用のProjectを保存できません");
    mvm::app::MvmController controller(sourcePath, {}, source);
    check(controller.selectTimelineClips({QStringLiteral("video")}) &&
              controller.copySelectedClips(),
          "別Projectへ移すclipをコピーできません");
    const auto destinationUrl =
        QUrl::fromLocalFile(QString::fromStdWString(destinationPath.wstring()));
    check(controller.openProject(destinationUrl) && controller.clipCount() == 0 &&
              controller.pasteClips() && controller.clipCount() == 1,
          "Projectを切り替えた後もコピー内容を貼り付けられません");
    check(controller.saveProject(), "別Projectへの貼り付け結果を保存できません");
    const auto pasted = mvm::project::loadProjectJson(destinationPath);
    check(pasted.success && pasted.project.mediaItems.size() == 1 &&
              pasted.project.mediaItems[0].mediaPath == media &&
              pasted.project.mediaItems[0].folderId.empty() &&
              pasted.project.mediaItems[0].frameCount == 120,
          "別Projectへ貼ったclipの素材をProjectパネルのrootへ登録しません");
    check(controller.undoLastEdit() && controller.clipCount() == 0 && controller.saveProject(),
          "別Projectへの貼り付けを一回のUndoで戻せません");
    const auto undone = mvm::project::loadProjectJson(destinationPath);
    check(undone.success && undone.project.mediaItems.empty(),
          "貼り付けのUndoでProjectパネルの素材が残りました");
    check(controller.redoLastEdit() && controller.pasteClips() && controller.clipCount() == 2 &&
              controller.saveProject(),
          "同じ素材を再度貼り付けできません");
    const auto again = mvm::project::loadProjectJson(destinationPath);
    check(again.success && again.project.mediaItems.size() == 1,
          "登録済みの素材をProjectパネルへ重複登録しました");
    check(controller.undoLastEdit() && controller.clipCount() == 1, "再貼り付けを戻せません");
    const auto firstId = again.success && !again.project.timelineClips.empty()
                             ? QString::fromStdString(again.project.timelineClips.front().id)
                             : QString();
    check(controller.selectTimelineClips({firstId}) && controller.cutSelectedClips() &&
              controller.clipCount() == 0 && controller.pasteClips() && controller.clipCount() == 1,
          "最後のclipをカットしてから再配置できません");
}

// fps の違う Project へ貼ると、自動化 key も同じ秒位置へ移る。60 -> 30 -> 60 の往復。
void testClipboardAcrossFps(const std::filesystem::path& sourcePath) {
    auto source = videoProject();
    const auto media = sourcePath.parent_path() / L"cross-fps-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    source.timelineClips[0].mediaPath = media;
    // 2 秒の clip。0 秒 / 0.5 秒 / 1 秒 / 末尾 (119 frame) に key を置く。
    source.timelineClips[0].effects.opacityKeys = {{0, 100.0}, {30, 50.0}, {60, 20.0}, {119, 0.0}};
    registerFixtureMedia(source, media);
    auto thirty = mvm::project::createDefaultProject();
    thirty.timelineFpsNum = 30;
    const auto thirtyPath = sourcePath.parent_path() / L"clipboard-30fps.mvm";
    auto sixty = mvm::project::createDefaultProject();
    const auto sixtyPath = sourcePath.parent_path() / L"clipboard-60fps.mvm";
    check(mvm::project::saveProjectJson(source, sourcePath).success &&
              mvm::project::saveProjectJson(thirty, thirtyPath).success &&
              mvm::project::saveProjectJson(sixty, sixtyPath).success,
          "fps違いの貼り付け試験のProjectを保存できません");
    mvm::app::MvmController controller(sourcePath, {}, source);
    const auto urlOf = [](const std::filesystem::path& path) {
        return QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
    };
    check(controller.selectTimelineClips({QStringLiteral("video")}) &&
              controller.copySelectedClips() && controller.openProject(urlOf(thirtyPath)) &&
              controller.pasteClips() && controller.clipCount() == 1 && controller.saveProject(),
          "60fpsのclipを30fpsのProjectへ貼り付けられません");
    const auto down = mvm::project::loadProjectJson(thirtyPath);
    const std::vector<mvm::project::ClipKeyframe> downKeys = {
        {0, 100.0}, {15, 50.0}, {30, 20.0}, {59, 0.0}};
    check(down.success && down.project.timelineClips.size() == 1 &&
              down.project.timelineClips[0].effects.opacityKeys == downKeys,
          "60fps -> 30fps でkeyが同じ秒位置へ移りません");
    if (!down.success || down.project.timelineClips.empty())
        return;
    const auto thirtyClip = QString::fromStdString(down.project.timelineClips[0].id);
    check(controller.selectTimelineClips({thirtyClip}) && controller.copySelectedClips() &&
              controller.openProject(urlOf(sixtyPath)) && controller.pasteClips() &&
              controller.saveProject(),
          "30fpsのclipを60fpsのProjectへ貼り付けられません");
    const auto up = mvm::project::loadProjectJson(sixtyPath);
    const std::vector<mvm::project::ClipKeyframe> upKeys = {
        {0, 100.0}, {30, 50.0}, {60, 20.0}, {118, 0.0}};
    check(up.success && up.project.timelineClips.size() == 1 &&
              up.project.timelineClips[0].effects.opacityKeys == upKeys,
          "30fps -> 60fps でkeyが同じ秒位置へ移りません");
}

// Alt+ドラッグの複数 clip 複製。QML は timelineDragBounds で群全体のドラッグ量を丸め、
// 確定は渡された位置のまま置く (後から寄せない)。範囲外の位置は拒否する。
void testGroupDuplicateBounds(const std::filesystem::path& path) {
    auto source = videoProject();
    const auto media = path.parent_path() / L"group-duplicate-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    source.timelineClips[0].mediaPath = media;
    registerFixtureMedia(source, media);
    auto upper = source.timelineClips[0];
    upper.id = "upper";
    upper.name = "upper";
    upper.track = {mvm::project::TrackKind::Video, 1};
    upper.timelineStartFrame = 100;
    source.timelineClips.push_back(upper);
    // 空き track 探しは mute 中の track を飛ばす。Alt+ドラッグでは飛ばさないことを見る。
    source.videoTracks[1].muted = true;
    check(mvm::project::saveProjectJson(source, path).success,
          "複数clip複製の境界試験のProjectを保存できません");
    mvm::app::MvmController controller(path, {}, source);
    check(controller.selectTimelineClips({QStringLiteral("video")}), "前提: clipを選択できません");
    const auto alone = controller.timelineDragBounds(QStringLiteral("upper"));
    check(alone.value(QStringLiteral("minStartFrame")).toLongLong() == 100 &&
              alone.value(QStringLiteral("videoMinTrack")).toInt() == 1 &&
              alone.value(QStringLiteral("videoMaxTrack")).toInt() == 1 &&
              !alone.contains(QStringLiteral("audioMinTrack")),
          "未選択のclipを掴んだときの範囲がそのclipだけになりません");
    check(controller.selectTimelineClips({QStringLiteral("video"), QStringLiteral("upper")}),
          "前提: 2つのclipを選択できません");
    const auto group = controller.timelineDragBounds(QStringLiteral("upper"));
    check(group.value(QStringLiteral("minStartFrame")).toLongLong() == 0 &&
              group.value(QStringLiteral("videoMinTrack")).toInt() == 0 &&
              group.value(QStringLiteral("videoMaxTrack")).toInt() == 1,
          "選択全体のドラッグ範囲を返しません");
    // upper を frame 0 へ: video は -100 になるので、後から右へ寄せずに拒否する。
    check(!controller.duplicateTimelineClipsAt(QStringLiteral("upper"), QStringLiteral("video"), 1,
                                               0) &&
              controller.clipCount() == 2,
          "群の左端が0未満になる複製を右へ寄せて確定しました");
    // upper を V1 へ: video は V0 より下になるので拒否する。
    check(!controller.duplicateTimelineClipsAt(QStringLiteral("upper"), QStringLiteral("video"), 0,
                                               300) &&
              controller.clipCount() == 2,
          "群の下端がtrack範囲外になる複製を確定しました");
    // 元と同じ位置 (ghost は元の V1 / V2 に重なって見える) は、別 track へ逃がさず拒否する。
    check(!controller.duplicateTimelineClipsAt(QStringLiteral("upper"), QStringLiteral("video"), 1,
                                               100) &&
              controller.clipCount() == 2 && controller.videoTrackCount() == 2,
          "元clipと重なるAlt+ドラッグ複製を別trackへ移して確定しました");
    // 少しだけ右へずらして一部が重なる場合も同じ。
    check(!controller.duplicateTimelineClipsAt(QStringLiteral("upper"), QStringLiteral("video"), 1,
                                               150) &&
              controller.clipCount() == 2 && controller.videoTrackCount() == 2,
          "一部が重なるAlt+ドラッグ複製を別trackへ移して確定しました");
    // 空いている位置なら ghost と同じ track (mute 中の V2 を含む) へそのまま置く。
    check(controller.duplicateTimelineClipsAt(QStringLiteral("upper"), QStringLiteral("video"), 1,
                                              400) &&
              controller.clipCount() == 4 && controller.videoTrackCount() == 2 &&
              controller.saveProject(),
          "空いた位置へのAlt+ドラッグ複製ができません");
    const auto placed = mvm::project::loadProjectJson(path);
    if (placed.success && placed.project.timelineClips.size() == 4) {
        const auto& clips = placed.project.timelineClips;
        check(clips[2].timelineStartFrame == 300 && clips[2].track.index == 0 &&
                  clips[3].timelineStartFrame == 400 && clips[3].track.index == 1,
              "Alt+ドラッグ複製がghostと違う位置またはtrackへ置かれました");
    } else {
        check(false, "複数clip複製の結果を読み込めません");
    }
}

void testLinkedClipboard(const std::filesystem::path& path) {
    auto source = linkedProject();
    const auto media = path.parent_path() / L"linked-clipboard-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    for (auto& clip : source.timelineClips)
        clip.mediaPath = media;
    registerFixtureMedia(source, media);
    check(mvm::project::saveProjectJson(source, path).success,
          "リンク複製試験のProjectを保存できません");
    mvm::app::MvmController controller(path, {}, source);
    controller.selectTimelineClip(QStringLiteral("video"), false);
    check(controller.copySelectedClips() && controller.pasteClips() && controller.clipCount() == 3,
          "リンク片側を単独でコピー・貼り付けできません");
    check(controller.saveProject(), "単独コピー結果を保存できません");
    const auto single = mvm::project::loadProjectJson(path);
    check(single.success && single.project.timelineClips.back().linkGroupId.empty(),
          "片側だけをコピーしたclipに壊れたリンクが残りました");
    check(controller.selectTimelineClips({QStringLiteral("video"), QStringLiteral("audio")}) &&
              controller.copySelectedClips() && controller.pasteClips() &&
              controller.clipCount() == 5,
          "リンク組を一括コピー・貼り付けできません");
    check(controller.saveProject(), "リンク組のコピー結果を保存できません");
    const auto paired = mvm::project::loadProjectJson(path);
    if (paired.success) {
        const auto& clips = paired.project.timelineClips;
        check(clips[3].linkGroupId == clips[4].linkGroupId && !clips[3].linkGroupId.empty() &&
                  clips[3].linkGroupId != clips[0].linkGroupId,
              "複製したリンク組へ新しい共通IDを付けられません");
    } else {
        check(false, "リンク組を保存後に読み込めません");
    }
}

void testMultipleClipClipboard(const std::filesystem::path& path) {
    auto source = videoProject();
    const auto media = path.parent_path() / L"multiple-clipboard-fixture.mp4";
    std::ofstream(media, std::ios::binary).put('x');
    source.timelineClips[0].mediaPath = media;
    registerFixtureMedia(source, media);
    auto overlay = source.timelineClips[0];
    overlay.id = "overlay";
    overlay.name = "overlay";
    overlay.track = {mvm::project::TrackKind::Video, 1};
    overlay.timelineStartFrame = 40;
    source.timelineClips.push_back(overlay);
    check(mvm::project::saveProjectJson(source, path).success,
          "複数clip複製試験のProjectを保存できません");
    mvm::app::MvmController controller(path, {}, source);
    check(controller.selectTimelineClips({QStringLiteral("video"), QStringLiteral("overlay")}) &&
              controller.copySelectedClips() && controller.pasteClips() &&
              controller.clipCount() == 4 && controller.videoTrackCount() == 4,
          "複数clipを相対位置ごと空きtrackへ配置できません");
    check(controller.saveProject(), "複数clipの貼り付け結果を保存できません");
    const auto placed = mvm::project::loadProjectJson(path);
    check(placed.success && placed.project.timelineClips[2].timelineStartFrame == 0 &&
              placed.project.timelineClips[3].timelineStartFrame == 40,
          "複数clipの時間差を保持できません");
    check(controller.undoLastEdit() && controller.clipCount() == 2,
          "複数clipと新規trackを一回のUndoで戻せません");
}

// スリップの drag 中 preview は Project を変えず、確定時と同じ規則で素材の端に止めた
// in の移動量を返す。
void testSlipPreviewDoesNotEdit(const std::filesystem::path& path) {
    auto initial = videoProject();
    initial.timelineClips[0].sourceFrameCount = 300;
    initial.timelineClips[0].sourceInFrame = 100;
    initial.timelineClips[0].sourceOutFrame = 220;
    check(mvm::project::saveProjectJson(initial, path).success,
          "slip preview試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.previewSlip(10) == 0, "slip preview開始前にpreviewを受理しました");
    check(controller.beginSlipPreview(QStringLiteral("video"), true),
          "slip previewを開始できません");
    check(controller.previewSlip(30) == 30, "slip previewの移動量が違います");
    check(controller.previewSlip(1000) == 80 && controller.previewSlip(-1000) == -100,
          "slip previewを素材の端で止めません");
    controller.endSlipPreview();
    check(!controller.dirty() && !controller.canUndo(), "slip previewがProjectを編集しました");
    check(controller.slipClip(QStringLiteral("video"), 1000, true) && controller.dirty(),
          "slip previewの後にslipを確定できません");
}

void testPenKeyUndoRedo(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "ペン試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    const auto candidate = controller.previewClipKey(QStringLiteral("video"), -1, 20, 75.0);
    check(candidate.value(QStringLiteral("success")).toBool() && !controller.dirty(),
          "ペンのdrag候補がProjectを編集しました");
    check(controller.commitClipKey(QStringLiteral("video"), -1, 20, 75.0) && controller.dirty(),
          "ペンのキーを一回の編集として確定できません");
    const auto* model = controller.timelineModel();
    const int role = model->roleNames().key("automationKeys");
    check(model->data(model->index(0, 0), role).toList() ==
              candidate.value(QStringLiteral("keys")).toList(),
          "ペンのdrag候補と確定表示が一致しません");
    check(controller.undoLastEdit() && model->data(model->index(0, 0), role).toList().empty(),
          "ペンのキーをUndoできません");
    check(controller.redoLastEdit() && model->data(model->index(0, 0), role).toList() ==
                                           candidate.value(QStringLiteral("keys")).toList(),
          "ペンのキーをRedoできません");
    check(controller.saveProject(), "ペンのキーを保存できません");
    const auto loaded = mvm::project::loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips[0].effects.opacityKeys.size() == 1 &&
              loaded.project.timelineClips[0].effects.opacityKeys[0].valuePercent == 75.0,
          "ペンのキーを再読込できません");
}

void testRedoRestoresDirtyState(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "Redo dirty試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.addTrack("video") && controller.saveProject() && !controller.dirty(),
          "Redo dirty試験の保存済みチェックポイントを作れません");
    check(controller.undoLastEdit() && controller.dirty(),
          "保存済みチェックポイントより前へUndoしてもdirtyになりません");
    check(controller.redoLastEdit() && !controller.dirty(),
          "保存済みチェックポイントまでRedoしてもcleanになりません");
}

void testDirtyCheckpoint(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "dirty試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(!controller.dirty(), "起動直後のProjectがdirtyです");
    check(controller.addTrack("video") && controller.dirty(), "編集後のProjectがdirtyになりません");
    const auto beforeSave = mvm::project::loadProjectJson(path);
    check(beforeSave.success && beforeSave.project.videoTracks.size() == 2,
          "明示保存前の編集がcanonical Projectへ書き込まれました");
    const bool checkpointSaved = controller.saveProject();
    if (!checkpointSaved)
        std::fprintf(stderr, "保存失敗: %s\n", controller.statusText().toUtf8().constData());
    check(checkpointSaved && !controller.dirty(), "明示保存後のProjectがcleanになりません");
    check(controller.addTrack("audio") && controller.dirty(), "保存後の再編集がdirtyになりません");
    check(controller.undoLastEdit() && !controller.dirty(),
          "保存済みチェックポイントまでUndoしてもcleanになりません");

    check(controller.addTrack("audio") && controller.dirty(), "破棄対象の編集がdirtyになりません");
    check(controller.discardUnsavedChanges() && !controller.dirty(),
          "未保存変更を破棄してもcleanになりません");
    check(controller.videoTrackCount() == 3 && controller.audioTrackCount() == 1,
          "未保存変更の破棄でメモリ上のProjectが復元されません");
    const auto restored = mvm::project::loadProjectJson(path);
    check(restored.success && restored.project.videoTracks.size() == 3 &&
              restored.project.audioTracks.size() == 1,
          "未保存変更の破棄で保存済みチェックポイントを復元できません");
}

void testProjectVideoSettings(const std::filesystem::path& path) {
    auto project = videoProject();
    project.timelineClips.front().timelineStartFrame = 60;
    check(mvm::project::saveProjectJson(project, path).success,
          "映像設定試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, project);

    check(controller.setProjectVideoSettings(1280, 720, 24, 1),
          "clipがあるProjectの映像設定を変更できません");
    check(controller.outputWidth() == 1280 && controller.outputHeight() == 720 &&
              controller.timelineFpsNum() == 24 && controller.timelineFpsDen() == 1,
          "controllerへProject映像設定が反映されません");
    const auto beforeSave = mvm::project::loadProjectJson(path);
    check(beforeSave.success && beforeSave.project.outputWidth == 1920 &&
              beforeSave.project.timelineClips.front().timelineStartFrame == 60,
          "映像設定の編集が明示保存前にcanonical Projectを書き換えました");
    const bool settingsSaved = controller.saveProject();
    if (!settingsSaved)
        std::fprintf(stderr, "設定保存失敗: %s\n", controller.statusText().toUtf8().constData());
    check(settingsSaved, "変更した映像設定を明示保存できません");
    const auto saved = mvm::project::loadProjectJson(path);
    check(saved.success && saved.project.outputWidth == 1280 && saved.project.outputHeight == 720 &&
              saved.project.timelineFpsNum == 24 && saved.project.timelineFpsDen == 1 &&
              saved.project.timelineClips.front().timelineStartFrame == 24,
          "Project映像設定または換算後のclip位置を保存できません");

    check(controller.undoLastEdit(), "Project映像設定をUndoできません");
    check(controller.outputWidth() == 1920 && controller.outputHeight() == 1080 &&
              controller.timelineFpsNum() == 60 && controller.timelineFpsDen() == 1,
          "Undoで以前のProject映像設定へ戻りません");
}

void testUnlinkUndo(const std::filesystem::path& path) {
    const auto initial = linkedProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "リンク解除試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    controller.seekTimelineFrame(23);
    controller.selectTimelineClip("audio", true);
    check(controller.currentClipIndex() == 1 && controller.playheadFrame() == 23,
          "Undo前のaudio選択と再生位置を設定できません");
    check(controller.unlinkTimelineClip("audio"), "audioのリンク解除に失敗しました");
    const auto* model = controller.timelineModel();
    const auto roleNames = model->roleNames();
    const int linkGroupRole = roleNames.key("linkGroupId", -1);
    check(linkGroupRole >= 0 &&
              model->data(model->index(0, 0), linkGroupRole).toString().isEmpty() &&
              model->data(model->index(1, 0), linkGroupRole).toString().isEmpty(),
          "リンク解除が作業中Projectへ反映されません");
    const auto canonicalBeforeUndo = mvm::project::loadProjectJson(path);
    check(canonicalBeforeUndo.success &&
              canonicalBeforeUndo.project.timelineClips[0].linkGroupId == "pair" &&
              canonicalBeforeUndo.project.timelineClips[1].linkGroupId == "pair",
          "リンク解除が明示保存前にcanonical Projectを書き換えました");
    check(controller.undoLastEdit(), "リンク解除をUndoできません");
    check(model->data(model->index(0, 0), linkGroupRole).toString() == "pair" &&
              model->data(model->index(1, 0), linkGroupRole).toString() == "pair",
          "Undoでリンク関係が復元されません");
    check(controller.currentClipIndex() == 1 && controller.playheadFrame() == 23,
          "Undoでaudio選択または再生位置が復元されません");
    const int selectedRole = roleNames.key("selected", -1);
    check(selectedRole >= 0 && model->data(model->index(0, 0), selectedRole).toBool() &&
              model->data(model->index(1, 0), selectedRole).toBool(),
          "Undoでリンク選択が復元されません");
}

void testRecoveryAutosave(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "復旧試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        mvm::app::MvmController controller(path, {}, initial);
        check(controller.addTrack("video"), "復旧対象の編集を作成できません");
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
              "編集後に自動復旧データが作成されません");
        const auto canonical = mvm::project::loadProjectJson(path);
        const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
        check(canonical.success && canonical.project.videoTracks.size() == 2,
              "自動保存がcanonical Projectを書き換えました");
        check(recovery.success && !recovery.legacy && recovery.project.videoTracks.size() == 3 &&
                  !recovery.sessionId.empty() && !recovery.savedAt.empty(),
              "自動復旧データに作業中Projectが保存されません");
        QFile canonicalFile(QString::fromStdWString(path.wstring()));
        check(canonicalFile.open(QIODevice::ReadOnly), "照合用のcanonicalを読めません");
        const QByteArray canonicalHash =
            QCryptographicHash::hash(canonicalFile.readAll(), QCryptographicHash::Sha256).toHex();
        check(recovery.canonicalSha256 == canonicalHash.toStdString(),
              "自動復旧データが開いた時点のcanonical hashを保持しません");
    }

    mvm::app::MvmController reopened(path, {}, initial);
    check(reopened.recoveryAvailable(), "再起動時に自動復旧データを検出できません");
    check(reopened.restoreRecovery() && reopened.dirty() && reopened.videoTrackCount() == 3,
          "自動復旧データをdirtyな作業中Projectとして復元できません");
    const bool recoverySaved = reopened.saveProject();
    if (!recoverySaved)
        std::fprintf(stderr, "復旧保存失敗: %s\n", reopened.statusText().toUtf8().constData());
    check(recoverySaved && !reopened.dirty(), "復元したProjectを明示保存できません");
    const auto saved = mvm::project::loadProjectJson(path);
    check(saved.success && saved.project.videoTracks.size() == 3 &&
              !std::filesystem::exists(recoveryPath),
          "復元後の保存でcanonical更新または復旧データ削除ができません");
}

void testExplicitSaveContract(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "契約試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.holdsProjectLock() && !controller.dirty(), "明示保存試験の初期状態が不正です");
    check(controller.addTrack("video") && controller.dirty() && controller.videoTrackCount() == 3,
          "編集がworking stateへ反映されません");
    const auto canonicalBefore = mvm::project::loadProjectJson(path);
    check(canonicalBefore.success && canonicalBefore.project.videoTracks.size() == 2,
          "編集直後のcanonicalがinitialのままではありません");
    check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
          "編集後のrecoveryが作成されません");
    const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
    check(recovery.success && recovery.project.videoTracks.size() == 3,
          "recoveryが編集後のworking stateではありません");
    check(controller.saveProject() && !controller.dirty(), "Ctrl+Sでcleanになりません");
    const auto canonicalAfter = mvm::project::loadProjectJson(path);
    check(canonicalAfter.success && canonicalAfter.project.videoTracks.size() == 3 &&
              !std::filesystem::exists(recoveryPath),
          "明示保存でcanonical更新またはrecovery削除ができません");
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        check(entry.path().filename().wstring().find(L".mvmtmp.") == std::wstring::npos,
              "atomic writeの一時fileが残っています");
    }
}

void testUndoRemovesRecovery(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "Undo復旧試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.addTrack("audio") && controller.dirty(), "recovery削除対象の編集ができません");
    check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
          "Undo前のrecoveryが作成されません");
    check(controller.undoLastEdit() && !controller.dirty() &&
              !std::filesystem::exists(recoveryPath),
          "保存済みrevisionまでUndoしてもrecoveryが残ります");
}

// Undo / Redo も working Project の変更である。recovery は切り替え後の Project を指す。
void testUndoRedoRewritesRecovery(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "Undo/Redo復旧試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    const auto recoveredTracks = [&](std::size_t video, std::size_t audio) {
        return pumpUntil(
            [&] {
                const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
                return recovery.success && recovery.project.videoTracks.size() == video &&
                       recovery.project.audioTracks.size() == audio;
            },
            4000);
    };
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.addTrack("video") && controller.addTrack("audio"),
          "Undo/Redo復旧試験の編集ができません");
    check(recoveredTracks(3, 2), "編集後のrecoveryが最新のProjectではありません");
    check(controller.undoLastEdit() && controller.dirty(), "Undo/Redo復旧試験のUndoに失敗しました");
    check(recoveredTracks(3, 1), "Undo後のrecoveryがUndo前のProjectのまま残っています");
    check(controller.redoLastEdit(), "Undo/Redo復旧試験のRedoに失敗しました");
    check(recoveredTracks(3, 2), "Redo後のrecoveryがRedo前のProjectのまま残っています");
}

void testCorruptRecoveryKept(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "破損復旧試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        std::ofstream broken(recoveryPath, std::ios::binary | std::ios::trunc);
        broken << "{ this is not recovery json";
        check(broken.good(), "破損した自動復旧データを用意できません");
    }
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.recoveryCorrupt() && !controller.recoveryAvailable() &&
              std::filesystem::exists(recoveryPath) &&
              controller.statusText().contains(QStringLiteral("壊れています")),
          "破損recoveryを消すか、警告せずに復元対象へしました");
    check(controller.dismissRecovery() && !controller.recoveryCorrupt() &&
              std::filesystem::exists(recoveryPath),
          "破損recoveryの確認を閉じるとfileまで削除します");
}

void testStaleRecoveryRemoved(const std::filesystem::path& path) {
    const auto initial = videoProject();
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    check(mvm::project::saveProjectJson(initial, path).success &&
              mvm::project::saveProjectJson(initial, recoveryPath).success,
          "同一recoveryの試験を準備できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(!controller.recoveryAvailable() && !std::filesystem::exists(recoveryPath),
          "canonicalと同一のrecoveryをstaleとして削除できません");
}

void testCanonicalChangedRecovery(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "外部変更試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        mvm::app::MvmController controller(path, {}, initial);
        check(controller.addTrack("video"), "外部変更と比較する編集を作成できません");
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
              "外部変更試験のrecoveryが作成されません");
    }
    auto changed = initial;
    changed.outputWidth = 1280;
    check(mvm::project::saveProjectJson(changed, path).success,
          "外部変更後のcanonicalを書けません");
    mvm::app::MvmController reopened(path, {}, changed);
    check(reopened.recoveryCanonicalChanged() && reopened.recoveryAvailable() &&
              reopened.outputWidth() == 1280 && reopened.videoTrackCount() == 2 &&
              std::filesystem::exists(recoveryPath),
          "canonical hash不一致を外部変更として確認できません");
    check(reopened.dismissRecovery() && !reopened.recoveryAvailable() &&
              std::filesystem::exists(recoveryPath) && reopened.outputWidth() == 1280,
          "キャンセルでrecoveryを削除するか、canonical以外を開きました");
}

void testDirtyProjectSwitchRefused(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "dirty guard試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.addTrack("video") && controller.dirty(), "拒否対象の未保存変更を作れません");
    const auto other = path.parent_path() / L"other-dirty-guard.mvm";
    const QUrl otherUrl = QUrl::fromLocalFile(QString::fromStdWString(other.wstring()));
    check(!controller.newProject(otherUrl) && !controller.openProject(otherUrl) &&
              controller.dirty() && controller.videoTrackCount() == 3 &&
              controller.projectPath() == QString::fromStdWString(path.wstring()),
          "dirty中のNew/Openが未保存変更を飛ばしました");
}

void testProjectLock(const std::filesystem::path& path) {
    const auto initial = videoProject();
    {
        mvm::app::MvmController first(path, {}, initial);
        check(first.holdsProjectLock(), "最初のProject lockを取得できません");
        mvm::app::MvmController second(path, {}, initial);
        check(!second.holdsProjectLock() && !second.addTrack("audio") &&
                  second.audioTrackCount() == 1,
              "別プロセス相当の二重編集を拒否できません");
        check(first.addTrack("video") && first.videoTrackCount() == 3,
              "lockを持たない側の失敗が、持っている側の編集まで止めました");
    }
    mvm::app::MvmController reopened(path, {}, initial);
    check(reopened.holdsProjectLock(), "終了後にProject lockを再取得できません");
}

void testRecoveryDisposition() {
    const auto initial = videoProject();
    auto edited = initial;
    edited.videoTracks.push_back({"V3", false});
    const std::string hashA(64, 'a');
    const std::string hashB(64, 'b');
    using Disposition = mvm::project::RecoveryDisposition;
    check(mvm::project::classifyRecovery(edited, edited, hashA, hashB) == Disposition::Stale,
          "内容が同一のrecoveryをhash不一致として扱いました");
    check(mvm::project::classifyRecovery(edited, initial, hashA, hashA) == Disposition::Restorable,
          "hashが一致する未保存編集を復元対象にしません");
    check(mvm::project::classifyRecovery(edited, initial, hashA, hashB) ==
              Disposition::CanonicalChanged,
          "hash不一致を外部変更として扱いません");
    check(mvm::project::classifyRecovery(edited, initial, "", hashB) ==
              Disposition::CanonicalChanged,
          "hashが無いrecoveryを照合済みとして復元対象にしました");
    check(mvm::project::classifyRecovery(edited, initial, "", "") == Disposition::Restorable,
          "canonical fileが無い未保存編集を復元対象にしません");
}

void testDiscardRecovery(const std::filesystem::path& path) {
    const auto initial = videoProject();
    auto edited = initial;
    edited.videoTracks.push_back({"V3", false});
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    check(mvm::project::saveProjectJson(initial, path).success &&
              mvm::project::saveProjectJson(edited, recoveryPath).success,
          "破棄試験のProjectを準備できません");
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.recoveryAvailable(), "破棄対象の自動復旧データを検出できません");
    check(controller.discardRecovery() && !controller.recoveryAvailable() && !controller.dirty() &&
              controller.videoTrackCount() == 2 && !std::filesystem::exists(recoveryPath),
          "自動復旧データを破棄してcanonical状態を維持できません");
}

std::string fileSha256(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

void testRecoveryRecordsOpenedCanonicalHash(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "基準hash試験の初期Projectを保存できません");
    const std::string openedHash = fileSha256(path);
    check(openedHash.size() == 64, "開いたcanonicalのhashを計算できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        mvm::app::MvmController controller(path, {}, initial);
        auto external = initial;
        external.outputWidth = 1280;
        check(mvm::project::saveProjectJson(external, path).success,
              "開いたあとのcanonicalを外部変更できません");
        check(fileSha256(path) != openedHash, "外部変更がcanonicalのhashを変えていません");
        check(controller.addTrack("video"), "基準hash試験の編集を作成できません");
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
              "外部変更後の自動復旧データが作成されません");
        const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
        check(recovery.success && !recovery.foreignProject && !recovery.legacy &&
                  recovery.canonicalSha256 == openedHash &&
                  recovery.canonicalSha256 != fileSha256(path),
              "自動復旧データが開いた時点ではなくdisk上のhashを記録しました");
    }

    const auto disk = mvm::project::loadProjectJson(path);
    check(disk.success, "外部変更後のcanonicalを読めません");
    mvm::app::MvmController reopened(path, {}, disk.project);
    check(reopened.recoveryCanonicalChanged() && reopened.recoveryAvailable() &&
              reopened.outputWidth() == 1280 && reopened.videoTrackCount() == 2,
          "開いた時点のhashと現在のdiskが違うrecoveryを外部変更にしません");
    check(reopened.restoreRecovery() && reopened.dirty() && reopened.videoTrackCount() == 3 &&
              reopened.outputWidth() == 1920,
          "外部変更があるrecoveryをworking stateへ復元できません");
    check(!reopened.saveProject() &&
              reopened.statusText() == QStringLiteral("Project fileが外部で変更されています"),
          "復元後の通常保存が、基準と違うcanonicalを上書きしました");
    const auto stillExternal = mvm::project::loadProjectJson(path);
    check(stillExternal.success && stillExternal.project.outputWidth == 1280 &&
              stillExternal.project.videoTracks.size() == 2,
          "拒否した保存が外部変更を残していません");
}

void testSaveRefusesExternalCanonical(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "上書き拒否試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    auto external = initial;
    external.outputWidth = 1280;
    check(mvm::project::saveProjectJson(external, path).success,
          "保存前のcanonicalを外部変更できません");
    check(controller.addTrack("video") && controller.dirty(), "上書き拒否試験の編集ができません");
    check(!controller.saveProject() && controller.dirty() &&
              controller.statusText() == QStringLiteral("Project fileが外部で変更されています"),
          "外部変更されたcanonicalへの通常保存を止められません");
    const auto other = path.parent_path() / L"external-save-as.mvm";
    const QUrl otherUrl = QUrl::fromLocalFile(QString::fromStdWString(other.wstring()));
    check(controller.saveProjectAs(otherUrl) && !controller.dirty(),
          "外部変更されたcanonicalから別名保存へ逃げられません");
    const auto original = mvm::project::loadProjectJson(path);
    const auto escaped = mvm::project::loadProjectJson(other);
    check(original.success && original.project.outputWidth == 1280 &&
              original.project.videoTracks.size() == 2 && escaped.success &&
              escaped.project.outputWidth == 1920 && escaped.project.videoTracks.size() == 3,
          "別名保存が元fileを残すか、working stateを書き出せていません");
}

void testExplicitOverwriteAfterExternalChange(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "明示上書き試験の初期Projectを保存できません");
    mvm::app::MvmController controller(path, {}, initial);
    auto external = initial;
    external.outputWidth = 1280;
    check(mvm::project::saveProjectJson(external, path).success && controller.addTrack("video"),
          "明示上書き試験の外部変更または編集ができません");
    check(!controller.saveProject(), "明示上書きの前に通常保存が通ってしまいました");
    check(controller.saveProjectOverwritingExternalChange() && !controller.dirty(),
          "利用者が明示した上書き保存ができません");
    const auto overwritten = mvm::project::loadProjectJson(path);
    check(overwritten.success && overwritten.project.outputWidth == 1920 &&
              overwritten.project.videoTracks.size() == 3,
          "明示した上書きがworking stateをcanonicalへ書きません");
    check(controller.saveProject() && !controller.dirty(),
          "上書き後の基準hashが更新されず、次の保存まで拒否されます");
}

void testForeignRecoveryIsNotRebased(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "別Project試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        mvm::app::MvmController controller(path, {}, initial);
        check(controller.addTrack("video"), "別Project試験の編集を作成できません");
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, 4000),
              "別Project試験のrecoveryが作成されません");
    }
    const auto home = mvm::project::loadProjectRecovery(recoveryPath, path);
    const std::wstring generic = path.generic_wstring();
    check(home.success && !home.foreignProject && home.project.videoTracks.size() == 3 &&
              mvm::project::sameCanonicalPath(path, std::filesystem::path(generic)),
          "記録したcanonical pathのrecoveryを同一Projectとして読めません");

    const auto other = path.parent_path() / L"foreign-canonical.mvm";
    check(mvm::project::saveProjectJson(initial, other).success,
          "recoveryのコピー先Projectを保存できません");
    std::filesystem::path otherRecovery = other;
    otherRecovery += L".recovery";
    std::error_code copyError;
    std::filesystem::copy_file(recoveryPath, otherRecovery,
                               std::filesystem::copy_options::overwrite_existing, copyError);
    check(!copyError, "recoveryを別Projectの隣へコピーできません");
    const auto foreign = mvm::project::loadProjectRecovery(otherRecovery, other);
    check(foreign.success && foreign.foreignProject,
          "別canonical pathのrecoveryを、開いているpath基準で解釈しました");
    mvm::app::MvmController reopened(other, {}, initial);
    check(reopened.recoveryForeign() && !reopened.recoveryAvailable() &&
              reopened.videoTrackCount() == 2 && std::filesystem::exists(otherRecovery) &&
              reopened.statusText().contains(QStringLiteral("別のProject")),
          "別Projectのrecoveryを復元するか、fileを消しました");
}

void testPreservedRecoverySurvivesNewAndSaveAs(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "保全試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    {
        std::ofstream broken(recoveryPath, std::ios::binary | std::ios::trunc);
        broken << "{ this is not recovery json";
        check(broken.good(), "保全する破損recoveryを用意できません");
    }
    {
        mvm::app::MvmController controller(path, {}, initial);
        check(controller.recoveryCorrupt(), "保全試験の破損recoveryを検出できません");
        const QUrl sameUrl = QUrl::fromLocalFile(QString::fromStdWString(path.wstring()));
        check(controller.saveProjectAs(sameUrl) && std::filesystem::exists(recoveryPath),
              "同じ場所へのSave Asが保全したrecoveryを削除しました");
        const auto savedAs = path.parent_path() / L"preserved-save-as.mvm";
        const QUrl savedAsUrl = QUrl::fromLocalFile(QString::fromStdWString(savedAs.wstring()));
        check(controller.saveProjectAs(savedAsUrl) && std::filesystem::exists(recoveryPath),
              "別pathへのSave Asが保全したrecoveryを削除しました");
    }
    mvm::app::MvmController controller(path, {}, initial);
    check(controller.recoveryCorrupt(), "Save As後も破損recoveryが残っていません");
    const auto created = path.parent_path() / L"preserved-new.mvm";
    const QUrl createdUrl = QUrl::fromLocalFile(QString::fromStdWString(created.wstring()));
    check(controller.newProject(createdUrl) && std::filesystem::exists(recoveryPath),
          "Newが保全したrecoveryを削除しました");
}

void testShiftSelectionToggle(const std::filesystem::path& path) {
    auto project = linkedProject();
    auto other = project.timelineClips[0];
    other.id = "other";
    other.name = "other";
    other.linkGroupId.clear();
    other.timelineStartFrame = 120;
    project.timelineClips.push_back(std::move(other));
    mvm::app::MvmController controller(path, {}, std::move(project));

    const auto* model = controller.timelineModel();
    const int selectedRole = model->roleNames().key("selected", -1);
    const auto selected = [&](int row) {
        return selectedRole >= 0 && model->data(model->index(row, 0), selectedRole).toBool();
    };

    // 再生位置はルーラーの操作でだけ動く。clip の選択 (通常・Shift) では動かさない。
    controller.seekTimelineFrame(37);
    const auto playheadBefore = controller.playheadFrame();
    controller.selectTimelineClip("other", true);
    controller.toggleTimelineClipSelection("video");
    check(selected(0) && selected(1) && selected(2),
          "Shift選択で既存選択へリンクclip一組を追加できません");
    controller.toggleTimelineClipSelection("audio");
    check(!selected(0) && !selected(1) && selected(2),
          "選択済みリンクclipのShift選択で一組を解除できません");
    check(playheadBefore == 37 && controller.playheadFrame() == playheadBefore,
          "clipの選択で再生位置が動きました");
}

void testDeleteMultipleSelection(const std::filesystem::path& path) {
    auto project = videoProject();
    auto audio = project.timelineClips[0];
    audio.id = "audio";
    audio.name = "audio";
    audio.kind = mvm::project::TimelineClipKind::Audio;
    audio.track = {mvm::project::TrackKind::Audio, 0};
    project.timelineClips.push_back(std::move(audio));
    auto remaining = project.timelineClips[0];
    remaining.id = "remaining";
    remaining.name = "remaining";
    remaining.timelineStartFrame = 120;
    project.timelineClips.push_back(std::move(remaining));
    mvm::app::MvmController controller(path, {}, std::move(project));

    check(controller.selectTimelineClips({"video", "audio"}), "削除対象の複数clipを選択できません");
    check(controller.deleteCurrentClip() && controller.clipCount() == 1,
          "Delete操作で選択中の全clipを削除できません");
    const auto* model = controller.timelineModel();
    const int clipIdRole = model->roleNames().key("clipId", -1);
    check(clipIdRole >= 0 && model->data(model->index(0, 0), clipIdRole).toString() == "remaining",
          "複数削除で選択外のclipまで削除しました");
    check(controller.undoLastEdit() && controller.clipCount() == 3,
          "複数clip削除を1回のUndoで復元できません");
}

// -4x の timed shuttle を始め、tick で playhead が動き出すまで待つ。
// 開始直後の status を startStatus へ返す (tick 後は preview の無い seek 失敗で上書きされる)。
bool startReverseShuttle(mvm::app::MvmController& controller, QString* startStatus = nullptr) {
    const auto start = controller.playheadFrame();
    if (!controller.shuttleLeft() || !controller.shuttleLeft() || !controller.shuttleLeft() ||
        controller.shuttleRate() != -4)
        return false;
    if (startStatus)
        *startStatus = controller.statusText();
    return pumpUntil([&controller, start] { return controller.playheadFrame() < start; }, 1000);
}

// playhead が動かないことを、shuttle の tick (40ms) を何度も処理して確かめる。
bool playheadStaysAt(mvm::app::MvmController& controller, qint64 frame) {
    return !pumpUntil([&controller, frame] { return controller.playheadFrame() != frame; }, 250);
}

// timed shuttle は tick の間にも進んでいる。停止や frame step が、最後の tick の
// 古い位置を基準にしたり、止まらずに次の tick で上書きされたりしないこと。
// preview surface が無いので seek 自体は失敗を返すが、playhead は更新される。
void testShuttleStopAndStep(const std::filesystem::path& path) {
    mvm::app::MvmController controller(path, {}, videoProject());
    controller.seekTimelineFrame(110);
    check(controller.playheadFrame() == 110, "shuttle試験の開始位置へ移動できません");

    // audio clip が無いので、2x/4x 以下でも WASAPI に依存せず timer clock で動く。
    QString startStatus;
    check(startReverseShuttle(controller, &startStatus), "-4倍速のシャトルが進みません");
    check(startStatus == QStringLiteral("シャトル -4 倍速（音声なし）"),
          "audio clipの無いtimelineで音声経路を作りました");

    // tick を処理させずに 50ms (-4x / 60fps で 12 frame) 待ってから止める。
    auto lastTick = controller.playheadFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(controller.pauseTimeline() && controller.shuttleRate() == 0,
          "Kでシャトルを停止できません");
    check(controller.playheadFrame() <= lastTick - 5,
          "シャトル停止位置が最後のtickのまま (停止直前のclockを反映していません)");
    check(playheadStaysAt(controller, controller.playheadFrame()),
          "シャトル停止後もplayheadが動きます");

    check(startReverseShuttle(controller), "2回目の-4倍速シャトルが進みません");
    lastTick = controller.playheadFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    controller.stepTimelineFrames(1);
    const auto stepped = controller.playheadFrame();
    check(controller.shuttleRate() == 0, "シャトル中のframe stepでシャトルが止まりません");
    check(stepped <= lastTick - 4, "シャトル中のframe stepが停止前の古い位置を基準にしています");
    check(playheadStaysAt(controller, stepped), "シャトル中のframe stepが次のtickで上書きされます");

    check(startReverseShuttle(controller), "3回目の-4倍速シャトルが進みません");
    controller.jumpToEditPoint(-1);
    check(controller.shuttleRate() == 0 && controller.playheadFrame() == 0,
          "シャトル中の編集点ジャンプでシャトルが止まらないか、編集点へ移動しません");
    check(playheadStaysAt(controller, 0), "シャトル中の編集点ジャンプが次のtickで上書きされます");
    controller.shutdown();
}
} // namespace

QString binEntryKind(const mvm::app::MediaBinModel& model, const QString& id) {
    return model.data(model.index(model.rowOfEntry(id), 0), mvm::app::MediaBinModel::EntryKindRole)
        .toString();
}

QString binEntryIdNamed(const mvm::app::MediaBinModel& model, const QString& name) {
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto index = model.index(row, 0);
        if (model.data(index, mvm::app::MediaBinModel::NameRole).toString() == name)
            return model.data(index, mvm::app::MediaBinModel::EntryIdRole).toString();
    }
    return {};
}

// 実素材を内容 probe で読み込み、bin の編集が 1 操作 1 undo になることを見る。
// MLT の初期化と終了は main が 1 回だけ行う (下の testMediaFilePlacement と共有する)。
void testMediaBinImport(const std::filesystem::path& path, const std::filesystem::path& smoke) {
    const auto png = smoke / L"png_alpha.png";
    const auto wav = smoke / L"wav_48k.wav";
    const auto mp4 = smoke / L"v1080p60_h264.mp4";
    if (!std::filesystem::exists(png) || !std::filesystem::exists(wav) ||
        !std::filesystem::exists(mp4)) {
        check(false, "Smoke素材がありません。pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
                     "実行してください");
        return;
    }
    auto bogus = path;
    bogus.replace_extension(L".txt");
    {
        std::ofstream text(bogus, std::ios::binary);
        text << "not media";
    }
    const auto url = [](const std::filesystem::path& file) {
        return QUrl::fromLocalFile(QString::fromStdWString(file.wstring()));
    };

    {
        // 素材の数を数えるので、素材の無い Project から始める。
        const auto initial = mvm::project::createDefaultProject();
        check(mvm::project::saveProjectJson(initial, path).success,
              "素材読み込み試験の初期Projectを保存できません");
        mvm::app::MvmController controller(path, {}, initial);
        const auto& bin = *controller.mediaBinModel();

        // 読めないファイルが混ざっても、読める素材は 1 つの編集として取り込む。
        // 戻り値は「commit したか」であり、部分失敗は status で知らせる。
        check(controller.importMediaFiles({url(png), url(wav), url(mp4), url(bogus)}, {}),
              "一部の素材をcommitしたのにfalseを返しました");
        check(controller.statusText().contains(QStringLiteral("1 件は読み込めません")),
              "読めなかった素材がstatusで知らされません");
        check(controller.canUndo() && controller.undoLastEdit() &&
                  controller.mediaBinModel()->entryCount() == 0 &&
                  controller.importMediaFiles({url(png), url(wav), url(mp4), url(bogus)}, {}),
              "部分失敗した読み込みが1つのUndoになりません");
        check(bin.entryCount() == 3, "読める素材だけが読み込まれていません");
        const QString pngId = binEntryIdNamed(bin, QStringLiteral("png_alpha.png"));
        const QString wavId = binEntryIdNamed(bin, QStringLiteral("wav_48k.wav"));
        const QString mp4Id = binEntryIdNamed(bin, QStringLiteral("v1080p60_h264.mp4"));
        check(binEntryKind(bin, pngId) == QStringLiteral("image") &&
                  binEntryKind(bin, wavId) == QStringLiteral("audio") &&
                  binEntryKind(bin, mp4Id) == QStringLiteral("video"),
              "内容probeによる素材種別が違います");
        check(bin.data(bin.index(bin.rowOfEntry(mp4Id), 0), mvm::app::MediaBinModel::RateTextRole)
                      .toString() == QStringLiteral("60.00 fps"),
              "読み込んだ動画のfps表示が違います");

        check(!controller.importMediaFiles({url(wav)}, {}) && bin.entryCount() == 3,
              "読み込み済みの素材を重複して読み込みました");

        const QString folderId = controller.createMediaFolder({});
        check(!folderId.isEmpty() && bin.entryCount() == 4, "フォルダを作成できません");
        check(controller.moveMediaBinEntries({pngId}, folderId) &&
                  bin.parentFolderOf(pngId) == folderId,
              "素材をフォルダへ移動できません");
        check(controller.undoLastEdit() && bin.parentFolderOf(pngId).isEmpty(),
              "素材の移動をUndoできません");

        // 画像は再生ヘッドの位置に画像 clip として置ける。後続のフォルダ削除 (clip も巻き込む) を
        // 変えないよう、確かめたら Undo で戻す。
        {
            // preview を付けていないので戻り値ではなく clip の増減で見る (addAudioClip と同じ)。
            const int beforeImage = controller.clipCount();
            controller.addMediaItemToTimeline(pngId);
            check(controller.clipCount() == beforeImage + 1, "画像をtimelineへ配置できません");
            check(controller.undoLastEdit() && controller.clipCount() == beforeImage,
                  "画像の配置をUndoできません");
        }

        // timeline へ置いた素材は bin に重複登録しない。使用中の印が付く。
        const int clipsBefore = controller.clipCount();
        controller.addAudioClip(url(wav));
        check(controller.clipCount() == clipsBefore + 1 && bin.entryCount() == 4,
              "timelineへ置いた素材がbinへ重複登録されました");
        check(bin.data(bin.index(bin.rowOfEntry(wavId), 0), mvm::app::MediaBinModel::InUseRole)
                  .toBool(),
              "timelineで使用中の印が付きません");

        // 使用中の素材は、参照する clip ごと 1 回の Undo 単位で削除できる。
        check(controller.mediaBinRemovalClipCount({wavId}) == 1,
              "使用中素材の削除で消えるclip数が違います");
        check(controller.mediaBinRemovalClipCount({QStringLiteral("missing")}) == -1,
              "存在しない素材の削除見積もりが成功しました");
        check(!controller.removeMediaBinEntries({QStringLiteral("missing")}) &&
                  controller.clipCount() == clipsBefore + 1 && bin.entryCount() == 4,
              "存在しない素材の削除でProjectが変わりました");
        check(controller.removeMediaBinEntries({wavId}) && bin.rowOfEntry(wavId) < 0 &&
                  controller.clipCount() == clipsBefore,
              "使用中の素材をclipごと削除できません");
        check(controller.undoLastEdit() && bin.rowOfEntry(wavId) >= 0 &&
                  controller.clipCount() == clipsBefore + 1,
              "素材とclipの同時削除を1回のUndoで戻せません");
        controller.undoLastEdit(); // addAudioClip も戻し、後続のフォルダ削除を未使用素材で行う。
        check(controller.clipCount() == clipsBefore, "音声clipの配置をUndoできません");

        check(controller.moveMediaBinEntries({pngId}, folderId) &&
                  controller.removeMediaBinEntries({folderId}) && bin.entryCount() == 2 &&
                  bin.rowOfEntry(pngId) < 0,
              "フォルダを中身ごと削除できません");
        controller.shutdown();
    }
    std::filesystem::remove(bogus);
}

// timeline model の行から、指定した clip 種別の行を数える / 取り出す。
struct PlacedClip {
    QString kind;
    QString trackKind;
    int trackIndex = -1;
    qint64 start = -1;
    qint64 duration = -1;
};

std::vector<PlacedClip> placedClips(const mvm::app::MvmController& controller) {
    auto* model = controller.timelineModel();
    const auto roles = model->roleNames();
    const auto roleOf = [&](const char* name) {
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            if (role.value() == name)
                return role.key();
        return -1;
    };
    std::vector<PlacedClip> clips;
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        clips.push_back({model->data(index, roleOf("clipKind")).toString(),
                         model->data(index, roleOf("trackKind")).toString(),
                         model->data(index, roleOf("trackIndex")).toInt(),
                         model->data(index, roleOf("timelineStartFrame")).toLongLong(),
                         model->data(index, roleOf("timelineDurationFrames")).toLongLong()});
    }
    return clips;
}

// 素材を内容で判定して timeline へ置く経路 (メニューのダイアログと drop)。
void testMediaFilePlacement(const std::filesystem::path& path, const std::filesystem::path& smoke) {
    const auto importDir = smoke / L"_import";
    const auto jpg = importDir / L"jpg_quadrant.jpg";
    const auto gif = importDir / L"gif_animated.gif";
    const auto wav = smoke / L"wav_48k.wav";
    if (!std::filesystem::exists(jpg) || !std::filesystem::exists(gif) ||
        !std::filesystem::exists(wav)) {
        check(false,
              "読み込み判定用素材がありません。pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
              "実行してください");
        return;
    }
    const auto url = [](const std::filesystem::path& file) {
        return QUrl::fromLocalFile(QString::fromStdWString(file.wstring()));
    };
    {
        // V1 に 0..120 の映像。A track は無い (音声は track を足して置く)。
        auto initial = videoProject();
        initial.audioTracks.clear();
        check(mvm::project::saveProjectJson(initial, path).success,
              "配置試験の初期Projectを保存できません");
        mvm::app::MvmController controller(path, {}, initial);
        const auto countKind = [&](const char* kind) {
            int count = 0;
            for (const auto& clip : placedClips(controller))
                count += clip.kind == QLatin1String(kind) ? 1 : 0;
            return count;
        };
        // preview を付けていないので戻り値は見ない (playhead は動く。testShuttleStopAndStep
        // と同じ)。
        controller.seekTimelineFrame(30);
        check(controller.playheadFrame() == 30, "再生ヘッドを動かせません");

        // preview を付けていないので、配置後の選択 (preview の seek) は失敗しうる。
        // 既存の addAudioClip の検査と同じく、戻り値ではなく clip の増減で見る。
        // 増えなかったときは controller の status を出す (原因の手がかり)。
        const auto placeFile = [&](const std::filesystem::path& file) {
            const auto before = placedClips(controller).size();
            controller.addMediaFileToTimeline(url(file));
            const bool placed = placedClips(controller).size() == before + 1;
            if (!placed)
                std::fprintf(stderr, "  status: %s\n",
                             controller.statusText().toUtf8().constData());
            return placed;
        };

        // 画像: 再生ヘッドの位置に、V1 の映像の上 (V2) へ 5 秒 (60fps で 300 frame)。
        check(placeFile(jpg) && countKind("image") == 1, "画像を内容で判定して置けません");
        for (const auto& clip : placedClips(controller)) {
            if (clip.kind == QLatin1String("image"))
                check(clip.trackKind == QLatin1String("video") && clip.trackIndex == 1 &&
                          clip.start == 30 && clip.duration == 300,
                      "画像が再生ヘッドの V2 に 5 秒で置かれていません");
        }

        // 音声: A track が無くても、再生ヘッドの位置に A1 を足して置く。
        // 同じ位置へもう 1 つ置くと、A1 は使用中なので A2 を足す。
        check(placeFile(wav) && countKind("audio") == 1 && controller.audioTrackCount() == 1,
              "音声を内容で判定して置けません");
        check(placeFile(wav) && countKind("audio") == 2 && controller.audioTrackCount() == 2,
              "使用中の A1 を避けて A2 に置けません");
        bool atPlayhead = true;
        for (const auto& clip : placedClips(controller))
            if (clip.kind == QLatin1String("audio"))
                atPlayhead =
                    atPlayhead && clip.start == 30 && clip.trackKind == QLatin1String("audio");
        check(atPlayhead, "音声が再生ヘッドの位置に置かれていません");

        // アニメーション画像は理由を添えて拒否し、Project を変えない。
        const auto before = placedClips(controller).size();
        check(!controller.addMediaFileToTimeline(url(gif)) &&
                  placedClips(controller).size() == before &&
                  controller.statusText().contains(QStringLiteral("アニメーション")),
              "アニメーション GIF を拒否しません");
        // 画像を動画として追加する経路は拒否する (内容で判定し、種別をすり替えない)。
        check(!controller.addVideoClip(url(jpg)) && placedClips(controller).size() == before,
              "画像を動画として追加できてしまいます");
        controller.shutdown();
    }
    {
        // ドロップ位置への配置。素材はプロジェクトパネルへ登録し、指定した track と frame から
        // 順に後ろへ並べる。全体を 1 回の Undo で戻せる。
        const auto initial = videoProject();
        check(mvm::project::saveProjectJson(initial, path).success,
              "ドロップ配置試験の初期Projectを保存できません");
        mvm::app::MvmController controller(path, {}, initial);
        const auto& bin = *controller.mediaBinModel();
        const auto clipsBefore = placedClips(controller).size();
        const int binBefore = bin.entryCount();

        controller.addMediaFilesToTimelineAt({url(jpg), url(jpg)}, QStringLiteral("video"), 1, 200);
        std::vector<qint64> imageStarts;
        for (const auto& clip : placedClips(controller))
            if (clip.kind == QLatin1String("image") && clip.trackKind == QLatin1String("video") &&
                clip.trackIndex == 1)
                imageStarts.push_back(clip.start);
        std::sort(imageStarts.begin(), imageStarts.end());
        check(imageStarts == std::vector<qint64>{200, 500} && bin.entryCount() == binBefore + 1,
              "ドロップした画像がV2の指定位置から並ばない、またはbinへ1件だけ登録されません");
        check(controller.undoLastEdit() && placedClips(controller).size() == clipsBefore &&
                  bin.entryCount() == binBefore,
              "ドロップ配置を1回のUndoで戻せません");

        // 最下段より下へのドロップは audio track を足して置く。
        const int audioTracks = controller.audioTrackCount();
        controller.addMediaFilesToTimelineAt({url(wav)}, QStringLiteral("audio"), audioTracks, 10);
        bool onNewTrack = false;
        for (const auto& clip : placedClips(controller))
            onNewTrack = onNewTrack || (clip.kind == QLatin1String("audio") &&
                                        clip.trackIndex == audioTracks && clip.start == 10);
        check(onNewTrack && controller.audioTrackCount() == audioTracks + 1,
              "最下段より下へのドロップで新しいaudio trackへ置けません");

        // プロジェクトパネルの素材を指定位置へ置く。
        const QString wavId = binEntryIdNamed(bin, QStringLiteral("wav_48k.wav"));
        const auto beforeItem = placedClips(controller).size();
        controller.addMediaItemsToTimelineAt({wavId}, QStringLiteral("audio"), 0, 400);
        bool itemPlaced = false;
        for (const auto& clip : placedClips(controller))
            itemPlaced = itemPlaced || (clip.kind == QLatin1String("audio") &&
                                        clip.trackIndex == 0 && clip.start == 400);
        check(itemPlaced && placedClips(controller).size() == beforeItem + 1,
              "プロジェクトパネルの素材を指定位置へ置けません");

        // 失敗は何も登録・配置しない。
        const auto beforeFailure = placedClips(controller).size();
        const int binBeforeFailure = bin.entryCount();
        check(!controller.addMediaFilesToTimelineAt({url(gif)}, QStringLiteral("video"), 1, 0) &&
                  placedClips(controller).size() == beforeFailure &&
                  bin.entryCount() == binBeforeFailure,
              "判定できない素材のドロップで何かが登録・配置されました");
        // 置けない行 (種別違い・使用中) へ落とした素材は、置ける track へ回す。無言で失敗しない。
        const auto audioStartsAt = [&](qint64 frame) {
            for (const auto& clip : placedClips(controller))
                if (clip.kind == QLatin1String("audio") && clip.start == frame)
                    return clip.trackKind == QLatin1String("audio");
            return false;
        };
        controller.addMediaItemsToTimelineAt({wavId}, QStringLiteral("video"), 1, 900);
        check(placedClips(controller).size() == beforeFailure + 1 && audioStartsAt(900),
              "映像trackへ落とした音声を音声trackへ回せません");
        controller.addMediaItemsToTimelineAt({wavId}, QStringLiteral("audio"), 0, 450);
        check(placedClips(controller).size() == beforeFailure + 2 && audioStartsAt(450),
              "使用中の音声trackへ落とした音声を空いているtrackへ回せません");
        check(!controller.addMediaItemsToTimelineAt({QStringLiteral("missing")},
                                                    QStringLiteral("audio"), 0, 0),
              "存在しない素材のドロップが成功しました");

        // 登録後に中身が差し替わった・消えた画像は、保存済みの種別を信じて置かない。
        const auto swapped = path.parent_path() / L"swap-still.jpg";
        std::error_code copyError;
        std::filesystem::copy_file(jpg, swapped, std::filesystem::copy_options::overwrite_existing,
                                   copyError);
        check(!copyError && controller.importMediaFiles({url(swapped)}, {}),
              "差し替え試験の静止画を登録できません");
        const QString swappedId = binEntryIdNamed(bin, QStringLiteral("swap-still.jpg"));
        // 対照: 差し替える前なら置ける。
        const auto beforeControl = placedClips(controller).size();
        controller.addMediaItemsToTimelineAt({swappedId}, QStringLiteral("video"), 1, 2000);
        check(placedClips(controller).size() == beforeControl + 1 && controller.undoLastEdit() &&
                  placedClips(controller).size() == beforeControl,
              "差し替え前の静止画をパネルから置けません (対照群)");
        std::filesystem::copy_file(gif, swapped, std::filesystem::copy_options::overwrite_existing,
                                   copyError);
        const auto beforeSwap = placedClips(controller).size();
        check(!copyError &&
                  !controller.addMediaItemsToTimelineAt({swappedId}, QStringLiteral("video"), 1,
                                                        2000) &&
                  placedClips(controller).size() == beforeSwap,
              "動く画像へ差し替わった素材をパネルから置けてしまいます");
        std::filesystem::remove(swapped);
        check(
            !controller.addMediaItemsToTimelineAt({swappedId}, QStringLiteral("video"), 1, 2000) &&
                placedClips(controller).size() == beforeSwap,
            "消えた画像をパネルから置けてしまいます");
        controller.shutdown();
    }
}

// プレビュー上の枠 (移動・拡縮) の controller 側。素材の寸法はプロジェクトパネルから取り、
// ドラッグ中は preview だけ、確定は 1 つの undo にする。
void testPreviewTransform(const std::filesystem::path& path) {
    const auto near = [](const QVariant& value, double expected) {
        return std::abs(value.toDouble() - expected) < 1e-6;
    };
    const auto initial = videoProject(); // V1 に 0..120 の 1920x1080 動画 (素材付き)
    check(mvm::project::saveProjectJson(initial, path).success,
          "枠の試験の初期Projectを保存できません");
    {
        mvm::app::MvmController controller(path, {}, initial);
        const QString video = QStringLiteral("video");
        check(controller.transformClipId().isEmpty(), "選択前に枠の対象がありました");
        controller.selectTimelineClips({video});
        check(controller.transformClipId() == video, "選択した動画が枠の対象になりません");

        const auto full = controller.clipVisualGeometry(video);
        check(near(full.value("x"), 0) && near(full.value("width"), 1920) &&
                  near(full.value("height"), 1080) && full.value("visible").toBool(),
              "既定の動画の枠が出力全体になりません");
        check(controller.visualClipAt(100, 100) == video, "枠の中を掴めません");

        // 見えている矩形から効果の値を決める (半分の幅・1/4 の高さ)。
        const auto values = controller.effectsForVisualRect(video, 100, 50, 960, 270);
        check(near(values.value("scaleX"), 50) && near(values.value("scaleY"), 25),
              "枠の大きさから縦横の拡大率を決められません");

        // ドラッグ中 (commit = false) は Project も undo も変えない。
        check(controller.setEffectValues(values, false) && !controller.canUndo() &&
                  near(controller.clipVisualGeometry(video).value("x"), 100),
              "ドラッグ中のpreviewに枠が追従しない、またはundoが積まれました");
        check(controller.cancelEffectPreview() &&
                  near(controller.clipVisualGeometry(video).value("x"), 0),
              "ドラッグの取り消しで枠が戻りません");

        // 確定は位置と拡大率をまとめて 1 つの undo にする。
        check(controller.setEffectValues(values, true) && controller.canUndo() &&
                  near(controller.effectScaleX(), 50) && near(controller.effectScaleY(), 25),
              "枠の変更を確定できません");
        check(controller.visualClipAt(50, 50).isEmpty() &&
                  controller.visualClipAt(500, 200) == video,
              "縮めた枠の外を掴めてしまう、または中を掴めません");
        const auto rects = controller.previewSnapRects({});
        check(rects.size() == 1 && near(rects.front().toMap().value("x"), 100) &&
                  controller.previewSnapRects(video).isEmpty(),
              "吸着の相手の矩形が違います");
        check(controller.undoLastEdit() && !controller.canUndo() &&
                  near(controller.effectScaleX(), 100) && near(controller.effectScaleY(), 100),
              "位置と拡大率の変更が1回のundoで戻りません");

        // 負例: 範囲外の値・未知の項目は拒否し、Project を変えない。
        check(!controller.setEffectValues({{QStringLiteral("scaleX"), 0.0}}, true) &&
                  !controller.setEffectValues({{QStringLiteral("scale"), 50.0}}, true) &&
                  !controller.setEffectValues({}, true) && !controller.canUndo() &&
                  near(controller.effectScaleX(), 100),
              "不正な効果の値を受理しました");
        check(
            controller.effectsForVisualRect(video, 0, 0, 0, 100).isEmpty() &&
                controller.effectsForVisualRect(QStringLiteral("missing"), 0, 0, 10, 10).isEmpty(),
            "不正な枠から効果の値を返しました");
        controller.shutdown();
    }
    {
        // リンクした音声側を選んでも枠は映像に出る。ハンドルの変更は映像だけに効き、
        // 選択 (current clip = 音声) に引きずられない。
        const auto linked = linkedProject();
        check(mvm::project::saveProjectJson(linked, path).success,
              "リンク対の枠の試験のProjectを保存できません");
        mvm::app::MvmController controller(path, {}, linked);
        controller.selectTimelineClip(QStringLiteral("audio"), true);
        check(controller.currentClipIndex() == 1 &&
                  controller.transformClipId() == QStringLiteral("video"),
              "リンク対の音声を選んだとき枠が映像に出ません");
        const auto values =
            controller.effectsForVisualRect(QStringLiteral("video"), 0, 0, 960, 1080);
        check(controller.setClipEffectValues(QStringLiteral("video"), values, true),
              "枠の対象clipを指定してeffectを確定できません");
        check(near(controller.clipVisualGeometry(QStringLiteral("video")).value("width"), 960) &&
                  near(controller.effectScaleX(), 100) && controller.currentClipIndex() == 1,
              "枠の変更が映像以外 (選択中の音声) に効きました");
        check(!controller.setClipEffectValues(QStringLiteral("missing"), values, true),
              "存在しないclipへeffectを適用できてしまいます");
        controller.shutdown();
    }
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 3)
        return 2;
    const std::filesystem::path directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    testCompleteAndRestart(directory / L"complete.mvm");
    testExportQualitySelection(directory / L"quality.mvm");
    testQueuedProgressAfterCancel(directory / L"cancel.mvm");
    testFailedExportNotification(directory / L"failed.mvm");
    testRevealFailureKeepsSuccess(directory / L"reveal-failure.mvm");
    testEtaProgressText(directory / L"eta.mvm");
    testShutdown(directory / L"shutdown-active.mvm", false);
    testShutdown(directory / L"shutdown-finished.mvm", true);
    testThreadFailure(directory / L"thread-failure.mvm");
    testUndo(directory / L"undo.mvm");
    testClipboardAndMarks(directory / L"clipboard-marks.mvm");
    testClipboardAcrossProject(directory / L"clipboard-source.mvm");
    testClipboardAcrossFps(directory / L"clipboard-fps-source.mvm");
    testGroupDuplicateBounds(directory / L"clipboard-group-bounds.mvm");
    testLinkedClipboard(directory / L"clipboard-linked.mvm");
    testMultipleClipClipboard(directory / L"clipboard-multiple.mvm");
    testRedoRestoresDirtyState(directory / L"redo-dirty.mvm");
    testSlipPreviewDoesNotEdit(directory / L"slip-preview.mvm");
    testPenKeyUndoRedo(directory / L"pen-undo-redo.mvm");
    testDirtyCheckpoint(directory / L"dirty-checkpoint.mvm");
    testProjectVideoSettings(directory / L"project-video-settings.mvm");
    testUnlinkUndo(directory / L"unlink-undo.mvm");
    testRecoveryAutosave(directory / L"recovery-autosave.mvm");
    testExplicitSaveContract(directory / L"explicit-save.mvm");
    testUndoRemovesRecovery(directory / L"undo-recovery.mvm");
    testUndoRedoRewritesRecovery(directory / L"undo-redo-recovery.mvm");
    testCorruptRecoveryKept(directory / L"corrupt-recovery.mvm");
    testStaleRecoveryRemoved(directory / L"stale-recovery.mvm");
    testCanonicalChangedRecovery(directory / L"canonical-changed.mvm");
    testRecoveryRecordsOpenedCanonicalHash(directory / L"recovery-base-hash.mvm");
    testSaveRefusesExternalCanonical(directory / L"save-refuses-external.mvm");
    testExplicitOverwriteAfterExternalChange(directory / L"save-overwrite-external.mvm");
    testForeignRecoveryIsNotRebased(directory / L"foreign-recovery.mvm");
    testPreservedRecoverySurvivesNewAndSaveAs(directory / L"preserved-recovery.mvm");
    testDirtyProjectSwitchRefused(directory / L"dirty-guard.mvm");
    testProjectLock(directory / L"project-lock.mvm");
    testRecoveryDisposition();
    testDiscardRecovery(directory / L"recovery-discard.mvm");
    testShiftSelectionToggle(directory / L"shift-selection.mvm");
    testDeleteMultipleSelection(directory / L"delete-multiple.mvm");
    testShuttleStopAndStep(directory / L"shuttle-stop-step.mvm");
    testPreviewTransform(directory / L"preview-transform.mvm");
    // MLT を初期化するので最後に置く。ほかの試験は MLT 無しの前提で書かれている。
    // 初期化は 1 回だけにする。1 プロセスで init / shutdown を繰り返すと、2 回目の init で
    // 読み込み直された ggml (FFmpeg の whisper filter の依存) の静的初期化が assert して落ちた。
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        check(false, "素材読み込み試験のMLTを初期化できません");
    } else {
        testMediaBinImport(directory / L"media-bin-import.mvm", std::filesystem::path(argv[2]));
        testMediaFilePlacement(directory / L"media-file-placement.mvm",
                               std::filesystem::path(argv[2]));
        mvm_mlt_runtime_shutdown();
    }
    return failures == 0 ? 0 : 1;
}
