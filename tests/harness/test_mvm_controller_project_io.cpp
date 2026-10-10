#include "mvm_controller.h"
#include "project/project_json.h"
#include "test_mvm_controller_fixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QUrl>

namespace {
using mvm::test::controller::check;
using mvm::test::controller::failures;
using mvm::test::controller::kRecoveryWaitMs;
using mvm::test::controller::pumpUntil;
using mvm::test::controller::videoProject;

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

// recovery の自動保存は、serialize と書き込みを worker で行う (UI スレッドでファイル I/O を
// 行わない)。書き込みは 1 つずつで、書き始めた revision だけを recovery 済みとして扱う。
// 書き込み中に保存済みの状態まで Undo したら、書き込みを待たずに戻り、書き込みの後に消す。
void testRecoveryAutosaveOffControlThread(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "非同期の自動保存の試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    std::filesystem::remove(recoveryPath);
    mvm::app::MvmController controller(path, {}, initial);
    std::mutex mutex;
    std::condition_variable changed;
    int started = 0;
    int released = 0;
    // 1 番目と 2 番目の書き込みは試験が許すまで止める。3 番目以降は 1000ms 掛かる。
    controller.setRecoveryWriterForTest([&](const mvm::project::Project& project,
                                            const std::filesystem::path& recovery,
                                            const std::filesystem::path& canonical,
                                            const std::string& sha, const std::string& savedAt,
                                            const std::string& session) {
        int index = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            index = ++started;
            changed.notify_all();
            if (index <= 2)
                changed.wait_for(lock, std::chrono::seconds(5), [&] { return released >= index; });
        }
        if (index > 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        return mvm::project::saveProjectRecovery(project, recovery, canonical, sha, savedAt,
                                                 session);
    });
    const auto release = [&](int count) {
        std::lock_guard<std::mutex> lock(mutex);
        released = count;
        changed.notify_all();
    };

    check(controller.addTrack("video"), "自動保存の対象の編集ができません");
    const auto firstRevision = controller.currentRevisionForTest();
    const auto began = std::chrono::steady_clock::now();
    controller.writeRecoveryAutosaveForTest();
    const double startMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    check(startMs < 1000 && controller.recoveryWriteInFlightForTest(),
          "自動保存の書き込みを control thread で待ちました");

    // 書き込み中の編集。1 番目の完了は書き始めた revision までを recovery 済みにする。
    check(controller.addTrack("video"), "書き込み中の編集ができません");
    controller.writeRecoveryAutosaveForTest();
    release(1);
    check(pumpUntil([&] { return controller.recoveryWriteCompletionCountForTest() >= 1; },
                    kRecoveryWaitMs),
          "1 番目の自動保存が完了しません");
    check(controller.recoveryRevisionForTest() == firstRevision &&
              controller.recoveryRevisionForTest() != controller.currentRevisionForTest() &&
              controller.recoveryWriteInFlightForTest(),
          "書き込みより後の編集を recovery 済みとして扱ったか、書き直しを始めません");
    release(2);
    check(pumpUntil(
              [&] {
                  return controller.recoveryWriteCompletionCountForTest() >= 2 &&
                         !controller.recoveryWriteInFlightForTest();
              },
              4000),
          "2 番目の自動保存が完了しません");
    const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
    check(controller.recoveryRevisionForTest() == controller.currentRevisionForTest() &&
              recovery.success && recovery.project.videoTracks.size() == 4,
          "書き込み中の編集を recovery へ書き直しません");

    // 書き込み中に保存済みの状態まで戻す。Undo は書き込み (1000ms) を待たずに戻る。recovery は
    // 書き込みの後に消すので、後から書き込みが届いて recovery を作り直さない。
    check(controller.addTrack("video"), "3 番目の自動保存の対象の編集ができません");
    controller.writeRecoveryAutosaveForTest();
    check(controller.recoveryWriteInFlightForTest(), "前提: 3 番目の自動保存を始めません");
    double slowestUndoMs = 0.0;
    while (controller.canUndo()) {
        const auto undoBegan = std::chrono::steady_clock::now();
        check(controller.undoLastEdit(), "保存済みの状態まで Undo できません");
        slowestUndoMs = std::max(slowestUndoMs, std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - undoBegan)
                                                    .count());
    }
    check(!controller.dirty() && slowestUndoMs < 400,
          "保存済みの状態まで戻す Undo が recovery の書き込みを待ちました");
    check(pumpUntil(
              [&] {
                  return !std::filesystem::exists(recoveryPath) &&
                         !controller.recoveryWriteInFlightForTest();
              },
              5000),
          "書き込み中に保存済みの状態へ戻した後も recovery が残っています");
    pumpUntil([] { return false; }, 300);
    check(!std::filesystem::exists(recoveryPath) && controller.recoveryRevisionForTest() == 0,
          "消した後に書き込みが届いて recovery を作り直しました");
    std::printf("recovery async: 保存済みまでの Undo の最大 %.1fms\n", slowestUndoMs);
    controller.shutdown();
}

// recovery の書き込みが例外を投げても、worker thread を作れなくても、process を終わらせず
// 保存の失敗として扱い、再試行の timer を動かす。直れば次の自動保存で書ける。
void testRecoveryAutosaveFailures(const std::filesystem::path& path) {
    const auto initial = videoProject();
    check(mvm::project::saveProjectJson(initial, path).success,
          "自動保存の失敗の試験の初期Projectを保存できません");
    std::filesystem::path recoveryPath = path;
    recoveryPath += L".recovery";
    std::filesystem::remove(recoveryPath);
    mvm::app::MvmController controller(path, {}, initial);

    controller.setRecoveryWriterForTest(
        [](const mvm::project::Project&, const std::filesystem::path&, const std::filesystem::path&,
           const std::string&, const std::string&,
           const std::string&) -> mvm::project::ProjectIoResult {
            throw std::runtime_error("試験の例外");
        });
    check(controller.addTrack("video"), "自動保存の失敗の試験の編集ができません");
    const auto beforeThrow = controller.recoveryWriteCompletionCountForTest();
    controller.writeRecoveryAutosaveForTest();
    check(pumpUntil([&] { return controller.recoveryWriteCompletionCountForTest() > beforeThrow; },
                    4000),
          "例外を投げた自動保存が完了しません");
    check(controller.statusText().contains(QStringLiteral("自動復旧データを保存できません")) &&
              controller.statusText().contains(QStringLiteral("試験の例外")) &&
              controller.recoveryRevisionForTest() == 0 &&
              controller.recoveryRetryScheduledForTest() && !std::filesystem::exists(recoveryPath),
          "書き込みの例外を保存の失敗として扱い、再試行を予定しません");

    controller.setRecoveryWriterForTest({});
    controller.setRecoveryThreadFactoryForTest([](std::function<void()>) -> std::thread {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    });
    const auto beforeThread = controller.recoveryWriteCompletionCountForTest();
    controller.writeRecoveryAutosaveForTest();
    check(controller.recoveryWriteCompletionCountForTest() > beforeThread &&
              controller.statusText().contains(QStringLiteral("thread を作れません")) &&
              controller.recoveryRetryScheduledForTest() &&
              !controller.recoveryWriteInFlightForTest(),
          "worker thread を作れない自動保存を保存の失敗として扱い、再試行を予定しません");

    // 直れば次の自動保存で書ける。
    controller.setRecoveryThreadFactoryForTest({});
    controller.writeRecoveryAutosaveForTest();
    check(pumpUntil(
              [&] {
                  return controller.recoveryRevisionForTest() ==
                             controller.currentRevisionForTest() &&
                         std::filesystem::exists(recoveryPath);
              },
              4000),
          "失敗の後の自動保存で recovery を書けません");
    controller.shutdown();
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
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
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
    check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
          "編集後のrecoveryが作成されません");
    // file が現れた直後は自動保存の書き込みと読み込みが重なり得る (負荷の高い並列実行で落ちた)。
    // 読めて編集後の内容になるまで待つ。期限内に揃わなければ失敗にする。
    check(pumpUntil(
              [&] {
                  const auto recovery = mvm::project::loadProjectRecovery(recoveryPath, path);
                  return recovery.success && recovery.project.videoTracks.size() == 3;
              },
              4000),
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
    check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
          "Undo前のrecoveryが作成されません");
    // recovery は worker が消す (Undo は削除を待たない)。
    check(controller.undoLastEdit() && !controller.dirty() &&
              pumpUntil([&] { return !std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
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
            kRecoveryWaitMs);
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
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
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
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
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
        check(pumpUntil([&] { return std::filesystem::exists(recoveryPath); }, kRecoveryWaitMs),
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

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (argc != 2)
        return 2;
    const std::filesystem::path directory(argv[1]);
    // 前回の復旧データで検査が空振りしないよう、毎回空から始める。
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    testDirtyCheckpoint(directory / L"dirty-checkpoint.mvm");
    testRecoveryAutosave(directory / L"recovery-autosave.mvm");
    testRecoveryAutosaveOffControlThread(directory / L"recovery-async.mvm");
    testRecoveryAutosaveFailures(directory / L"recovery-failures.mvm");
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
    return failures == 0 ? 0 : 1;
}
