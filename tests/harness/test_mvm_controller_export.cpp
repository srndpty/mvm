#include "mvm_controller.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <QCoreApplication>
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
          "Undo後のProjectファイルが復元されません");
}

void testUnlinkUndo(const std::filesystem::path& path) {
    mvm::app::MvmController controller(path, {}, linkedProject());
    controller.selectTimelineClip("audio", 23);
    check(controller.currentClipIndex() == 1 && controller.playheadFrame() == 23,
          "Undo前のaudio選択と再生位置を設定できません");
    check(controller.unlinkTimelineClip("audio"), "audioのリンク解除に失敗しました");
    auto unlinked = mvm::project::loadProjectJson(path);
    check(unlinked.success && unlinked.project.timelineClips[0].linkGroupId.empty() &&
              unlinked.project.timelineClips[1].linkGroupId.empty(),
          "リンク解除がProjectへ保存されません");
    check(controller.undoLastEdit(), "リンク解除をUndoできません");
    const auto restored = mvm::project::loadProjectJson(path);
    check(restored.success && restored.project.timelineClips[0].linkGroupId == "pair" &&
              restored.project.timelineClips[1].linkGroupId == "pair",
          "Undoでリンク関係が復元されません");
    check(controller.currentClipIndex() == 1 && controller.playheadFrame() == 23,
          "Undoでaudio選択または再生位置が復元されません");
    const auto* model = controller.timelineModel();
    const auto roleNames = model->roleNames();
    const int selectedRole = roleNames.key("selected", -1);
    check(selectedRole >= 0 && model->data(model->index(0, 0), selectedRole).toBool() &&
              model->data(model->index(1, 0), selectedRole).toBool(),
          "Undoでリンク選択が復元されません");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 2)
        return 2;
    const std::filesystem::path directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    testCompleteAndRestart(directory / L"complete.mvm");
    testQueuedProgressAfterCancel(directory / L"cancel.mvm");
    testFailedExportNotification(directory / L"failed.mvm");
    testRevealFailureKeepsSuccess(directory / L"reveal-failure.mvm");
    testEtaProgressText(directory / L"eta.mvm");
    testShutdown(directory / L"shutdown-active.mvm", false);
    testShutdown(directory / L"shutdown-finished.mvm", true);
    testThreadFailure(directory / L"thread-failure.mvm");
    testUndo(directory / L"undo.mvm");
    testUnlinkUndo(directory / L"unlink-undo.mvm");
    return failures == 0 ? 0 : 1;
}
