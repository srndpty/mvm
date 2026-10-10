#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "project/path_identity.h"
#include "project/project_json.h"

#include <system_error>
#include <thread>
#include <utility>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QMetaObject>
#include <QPointer>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mvm::app {
using detail::fromPath;
using detail::indexOfClipId;
namespace {
bool atomicSaveProject(const project::Project& project, const std::filesystem::path& path,
                       QString& error) {
    const auto saved = project::saveProjectJson(project, path);
    if (!saved.success) {
        error = QString::fromStdString(saved.error);
        return false;
    }
    return true;
}

} // namespace

bool MvmController::writeCanonicalProject(const project::Project& project,
                                          const std::filesystem::path& path, QString& error) const {
    return atomicSaveProject(project, path, error);
}

std::filesystem::path MvmController::recoveryPath() const {
    std::filesystem::path path = projectPath_;
    path += L".recovery";
    return path;
}

bool MvmController::removeRecoveryBeside(const std::filesystem::path& projectPath, QString& error) {
    settleRecoveryWrite();
    std::filesystem::path path = projectPath;
    path += L".recovery";
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if (removeError) {
        error = QString::fromStdString(removeError.message());
        return false;
    }
    return true;
}

bool MvmController::removeRecoveryFile(QString& error) {
    if (!removeRecoveryBeside(projectPath_, error))
        return false;
    recoveryRevision_ = 0;
    return true;
}

bool MvmController::acquireProjectLock(const std::filesystem::path& path, void*& acquired,
                                       QString& error) {
    acquired = nullptr;
    std::error_code pathError;
    const auto absolute = std::filesystem::absolute(path, pathError).lexically_normal();
    if (pathError) {
        error = QString::fromStdString(pathError.message());
        return false;
    }
    if (projectLockHeld_ && projectLockPath_ == absolute)
        return true;

    const auto parent = absolute.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, pathError);
        if (pathError) {
            error = QString::fromStdString(pathError.message());
            return false;
        }
    }

    const std::wstring lockPath = absolute.wstring() + L".lock";
    const HANDLE handle =
        CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        error = code == ERROR_SHARING_VIOLATION
                    ? QStringLiteral("他のmvmがこのProjectを開いています")
                    : QStringLiteral("Project lockを取得できません (Win32 error %1)").arg(code);
        return false;
    }
    acquired = handle;
    return true;
}

void MvmController::adoptProjectLock(void* acquired, const std::filesystem::path& path) {
    if (!acquired)
        return;
    std::error_code pathError;
    const auto absolute = std::filesystem::absolute(path, pathError).lexically_normal();
    releaseProjectLock();
    projectLockHandle_ = acquired;
    projectLockPath_ = pathError ? path : absolute;
    projectLockHeld_ = true;
}

void MvmController::releaseProjectLock() {
    releaseLockHandle(projectLockHandle_);
    projectLockHandle_ = nullptr;
    projectLockHeld_ = false;
    projectLockPath_.clear();
}

void MvmController::releaseLockHandle(void* handle) {
    if (handle)
        CloseHandle(static_cast<HANDLE>(handle));
}

QString MvmController::canonicalFileSha256(bool& readable) const {
    readable = false;
    QFile file(fromPath(projectPath_));
    if (!file.exists()) {
        readable = true;
        return {};
    }
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    readable = true;
    return QString::fromLatin1(hash.result().toHex());
}

bool MvmController::rememberCanonicalBase() {
    bool readable = false;
    const QString hash = canonicalFileSha256(readable);
    if (!readable) {
        canonicalBaseKnown_ = false;
        savedCanonicalSha256_.clear();
        return false;
    }
    savedCanonicalSha256_ = hash.toStdString();
    canonicalBaseKnown_ = true;
    return true;
}

bool MvmController::canonicalBaseMatchesDisk(QString& error) const {
    if (!canonicalBaseKnown_) {
        error = QStringLiteral("Project fileの基準hashが不明です");
        return false;
    }
    bool readable = false;
    const QString diskHash = canonicalFileSha256(readable);
    if (!readable) {
        error = QStringLiteral("Project fileを照合できません");
        return false;
    }
    if (diskHash.toStdString() != savedCanonicalSha256_) {
        error = QStringLiteral("Project fileが外部で変更されています");
        return false;
    }
    return true;
}

void MvmController::scheduleRecoveryAutosave() {
    if (!dirty()) {
        recoveryDebounceTimer_.stop();
        recoveryMaximumTimer_.stop();
        // 保存済みの状態へ戻った (Undo など)。書き込み中の recovery を待たず、その後に消す。
        enqueueRecoveryDelete();
        return;
    }
    recoveryDebounceTimer_.start();
    if (!recoveryMaximumTimer_.isActive())
        recoveryMaximumTimer_.start();
}

struct MvmController::RecoveryTask {
    enum class Kind { Write, Delete };
    Kind kind = Kind::Write;
    // 積んだ順の番号。
    std::uint64_t sequence = 0;
    // 書き込みは積んだ時点の revision と Project file の path を持つ。
    std::uint64_t revision = 0;
    std::filesystem::path projectPath;
    std::filesystem::path recoveryPath;
    project::Project snapshot;
    std::string canonicalSha256;
    std::string savedAt;
    std::string sessionId;
};

struct MvmController::RecoveryWriteJob {
    std::thread thread;
    std::shared_ptr<RecoveryTask> task;
    // thread が書き、join の後に control thread が読む。
    project::ProjectIoResult result;
};

void MvmController::writeRecoveryAutosave() {
    startRecoveryWrite(false);
}

void MvmController::startRecoveryWrite(bool waitForCompletion) {
    recoveryDebounceTimer_.stop();
    recoveryMaximumTimer_.stop();
    if (!dirty()) {
        // 保存済みの状態へ戻った。書き込み中のものがあっても、その後に消えるので待たない。
        enqueueRecoveryDelete();
        if (waitForCompletion)
            settleRecoveryWrite();
        return;
    }
    if (recoveryWrite_ || !recoveryQueue_.empty()) {
        // 書き込みは 1 つずつ行う (古い revision の書き込みが後から新しいものを上書きしない)。
        recoveryWriteAgain_ = true;
        if (waitForCompletion) {
            settleRecoveryWrite();
            startRecoveryWrite(true);
        }
        return;
    }
    recoveryWriteAgain_ = false;
    if (!projectLockHeld_ || recoveryRevision_ == currentRevision_)
        return;
    // diskを読み直すと、開いたあとの外部変更を基準hashとして記録してしまう。
    if (!canonicalBaseKnown_) {
        setStatus(
            QStringLiteral("自動復旧データを保存できません: Project fileの基準hashが不明です"));
        recoveryMaximumTimer_.start();
        return;
    }
    // control thread で行うのは Project の複製まで。serialize と書き込みは worker で行う。
    auto task = std::make_shared<RecoveryTask>();
    task->kind = RecoveryTask::Kind::Write;
    task->sequence = ++recoveryTaskSequence_;
    task->revision = currentRevision_;
    task->projectPath = projectPath_;
    task->recoveryPath = recoveryPath();
    task->snapshot = project_;
    task->canonicalSha256 = savedCanonicalSha256_;
    task->savedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString();
    task->sessionId = sessionId_;
    recoveryQueue_.push_back(std::move(task));
    pumpRecoveryQueue();
    if (waitForCompletion)
        settleRecoveryWrite();
}

void MvmController::enqueueRecoveryDelete() {
    auto task = std::make_shared<RecoveryTask>();
    task->kind = RecoveryTask::Kind::Delete;
    task->sequence = ++recoveryTaskSequence_;
    task->projectPath = projectPath_;
    task->recoveryPath = recoveryPath();
    recoveryDeleteSequence_ = task->sequence;
    recoveryRevision_ = 0;
    recoveryQueue_.push_back(std::move(task));
    pumpRecoveryQueue();
}

project::ProjectIoResult MvmController::runRecoveryTask(const RecoveryTask& task) const {
    // 例外は失敗として control thread へ返す (worker の例外で process を終わらせない)。
    try {
        if (task.kind == RecoveryTask::Kind::Delete) {
            std::error_code error;
            std::filesystem::remove(task.recoveryPath, error);
            if (error)
                return {false, error.message()};
            return {true, {}};
        }
        const auto writer =
            recoveryWriter_ ? recoveryWriter_ : RecoveryWriter(project::saveProjectRecovery);
        return writer(task.snapshot, task.recoveryPath, task.projectPath, task.canonicalSha256,
                      task.savedAt, task.sessionId);
    } catch (const std::exception& error) {
        return {false, std::string("例外: ") + error.what()};
    } catch (...) {
        return {false, "不明な例外"};
    }
}

void MvmController::pumpRecoveryQueue() {
    while (!recoveryWrite_ && !recoveryQueue_.empty()) {
        auto job = std::make_shared<RecoveryWriteJob>();
        job->task = std::move(recoveryQueue_.front());
        recoveryQueue_.pop_front();
        try {
            auto work = [this, job] {
                job->result = runRecoveryTask(*job->task);
                QMetaObject::invokeMethod(
                    this, [this, job] { completeRecoveryWrite(job); }, Qt::QueuedConnection);
            };
            job->thread = recoveryThreadFactory_ ? recoveryThreadFactory_(work) : std::thread(work);
            recoveryWrite_ = job;
        } catch (const std::system_error& error) {
            // thread を作れない。削除はここで行う (小さく、残すと消したはずの recovery が残る)。
            // 書き込みは失敗として扱い、再試行の timer に任せる。
            if (job->task->kind == RecoveryTask::Kind::Delete)
                job->result = runRecoveryTask(*job->task);
            else
                job->result = {false,
                               std::string("自動復旧の thread を作れません: ") + error.what()};
            applyRecoveryWriteResult(*job);
        }
    }
}

void MvmController::completeRecoveryWrite(const std::shared_ptr<RecoveryWriteJob>& job) {
    // 先に settleRecoveryWrite が受け取った job の通知は捨てる。
    if (job != recoveryWrite_)
        return;
    job->thread.join();
    recoveryWrite_.reset();
    applyRecoveryWriteResult(*job);
    pumpRecoveryQueue();
    if (recoveryWriteAgain_ && !recoveryWrite_ && recoveryQueue_.empty())
        startRecoveryWrite(false);
}

void MvmController::settleRecoveryWrite() {
    if (recoveryWrite_) {
        const auto job = std::move(recoveryWrite_);
        recoveryWrite_.reset();
        job->thread.join();
        applyRecoveryWriteResult(*job);
    }
    // 積んだままのものは、順に control thread で行う (呼ぶのは明示した操作と shutdown だけ)。
    while (!recoveryQueue_.empty()) {
        RecoveryWriteJob job;
        job.task = std::move(recoveryQueue_.front());
        recoveryQueue_.pop_front();
        job.result = runRecoveryTask(*job.task);
        applyRecoveryWriteResult(job);
    }
}

void MvmController::applyRecoveryWriteResult(const RecoveryWriteJob& job) {
    ++recoveryWriteCompletionCount_;
    const auto& task = *job.task;
    if (task.kind == RecoveryTask::Kind::Delete) {
        if (!job.result.success)
            setStatus(QStringLiteral("自動復旧データを削除できません: ") +
                      QString::fromStdString(job.result.error));
        return;
    }
    // 書いている間に Project file の path が変わった (別名で保存・切り替え)。書いた recovery は
    // 前の path のもので、今の Project の recovery ではない。
    if (task.projectPath != projectPath_)
        return;
    if (!job.result.success) {
        setStatus(QStringLiteral("自動復旧データを保存できません: ") +
                  QString::fromStdString(job.result.error));
        recoveryMaximumTimer_.start();
        return;
    }
    // 後に積んだ削除で消える。
    if (task.sequence < recoveryDeleteSequence_)
        return;
    // recovery にあるのは書き始めた時点の revision。その後の編集は次の自動保存で書く。
    recoveryRevision_ = task.revision;
}

void MvmController::detectRecovery() {
    settleRecoveryWrite();
    recoveryProject_.reset();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    recoveryRecordedSha256_.clear();
    recoveryRevision_ = 0;
    std::error_code existsError;
    const bool exists = std::filesystem::exists(recoveryPath(), existsError);
    if (existsError) {
        statusText_ = QStringLiteral("自動復旧データを確認できません: ") +
                      QString::fromStdString(existsError.message());
        return;
    }
    if (!exists)
        return;

    const auto loaded = project::loadProjectRecovery(recoveryPath(), projectPath_);
    if (!loaded.success) {
        recoveryCorrupt_ = true;
        statusText_ = QStringLiteral("自動復旧データが壊れています。fileは保持しました: ") +
                      QString::fromStdString(loaded.error);
        Q_EMIT stateChanged();
        Q_EMIT recoveryDetected();
        return;
    }
    if (loaded.foreignProject) {
        recoveryForeign_ = true;
        statusText_ =
            QStringLiteral("自動復旧データは別のProjectに属しています。fileは保持しました: ") +
            QString::fromStdString(loaded.canonicalPath);
        Q_EMIT stateChanged();
        Q_EMIT recoveryDetected();
        return;
    }

    bool hashReadable = false;
    const QString canonicalHash = canonicalFileSha256(hashReadable);
    if (!hashReadable) {
        recoveryProject_ = loaded.project;
        recoveryRecordedSha256_ = loaded.canonicalSha256;
        recoveryCanonicalChanged_ = true;
        statusText_ =
            QStringLiteral("Project fileを照合できないため、自動復旧の扱いを確認してください");
        Q_EMIT recoveryDetected();
        return;
    }

    const auto disposition = project::classifyRecovery(
        loaded.project, savedProject_, loaded.canonicalSha256, canonicalHash.toStdString());
    if (disposition == project::RecoveryDisposition::Stale) {
        QString ignored;
        if (!removeRecoveryFile(ignored)) {
            statusText_ = QStringLiteral("同一内容の自動復旧データを削除できません: ") + ignored;
            Q_EMIT stateChanged();
        }
        return;
    }

    recoveryProject_ = loaded.project;
    recoveryRecordedSha256_ = loaded.canonicalSha256;
    recoveryCanonicalChanged_ = disposition == project::RecoveryDisposition::CanonicalChanged;
    if (recoveryCanonicalChanged_) {
        statusText_ = QStringLiteral(
            "Project "
            "fileが自動保存のあとで変わっています。復元するか、現在のfileを開くか選んでください");
    }
    Q_EMIT recoveryDetected();
}

bool MvmController::restoreRecovery() {
    settleRecoveryWrite();
    if (busy_ || !projectLockHeld_ || !recoveryProject_ || !pauseTimeline())
        return false;
    project_ = *recoveryProject_;
    refreshAudioInputAuthority(true);
    audioMixerBuses_.clear();
    audioMixerPeaks_.clear();
    // 復元した作業状態の基準は、今のdiskではなくrecoveryに記録されたcanonical。
    savedCanonicalSha256_ = recoveryRecordedSha256_;
    canonicalBaseKnown_ = true;
    recoveryProject_.reset();
    recoveryRecordedSha256_.clear();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    currentRevision_ = nextRevision_++;
    recoveryRevision_ = currentRevision_;
    clearEditHistory();
    selectedClipIds_.clear();
    currentClipIndex_ = -1;
    currentClipName_.clear();
    currentClipPath_.clear();
    playheadFrame_ = 0;
    syncFirstManimAsset();
    refreshTimelineModel();
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("自動復旧データは復元しましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    Q_EMIT stateChanged();
    setStatus(QStringLiteral(
        "自動保存された編集を復元しました。保存するまで元のProject fileは変更されません"));
    return true;
}

bool MvmController::discardRecovery() {
    if (busy_ || !projectLockHeld_ || !recoveryProject_)
        return false;
    QString error;
    if (!removeRecoveryFile(error)) {
        setStatus(QStringLiteral("自動復旧データを破棄できません: ") + error);
        return false;
    }
    recoveryProject_.reset();
    recoveryRecordedSha256_.clear();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    const project::Project beforeRestore = project_;
    restoreFirstManimClip();
    if (project_ != beforeRestore && currentRevision_ == savedRevision_) {
        currentRevision_ = nextRevision_++;
        scheduleRecoveryAutosave();
    }
    clearEditHistory();
    Q_EMIT stateChanged();
    if (!dirty())
        setStatus(QStringLiteral("自動復旧データを破棄し、最後に保存したProjectを開きました"));
    return true;
}

bool MvmController::dismissRecovery() {
    if (!recoveryProject_ && !recoveryCorrupt_ && !recoveryForeign_)
        return false;
    recoveryProject_.reset();
    recoveryRecordedSha256_.clear();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    Q_EMIT stateChanged();
    setStatus(QStringLiteral("自動復旧データは残したまま、確認を閉じました"));
    return true;
}

bool MvmController::newProject(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (dirty()) {
        setStatus(
            QStringLiteral("未保存の変更を保存または破棄してから新規Projectを作成してください"));
        return false;
    }
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの保存先を指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    QString error;
    void* acquiredLock = nullptr;
    if (!acquireProjectLock(path, acquiredLock, error)) {
        setStatus(QStringLiteral("新規Projectを作成できません: ") + error);
        return false;
    }
    project::Project fresh = project::createDefaultProject();
    fresh.timelineFpsNum = project_.timelineFpsNum;
    fresh.timelineFpsDen = project_.timelineFpsDen;
    if (!writeCanonicalProject(fresh, path, error)) {
        releaseLockHandle(acquiredLock);
        setStatus(QStringLiteral("新規Projectを保存できません: ") + error);
        return false;
    }
    // dismissや破損で残したrecoveryは、Newでは消さない。
    adoptProjectLock(acquiredLock, path);
    return adoptProject(std::move(fresh), path, QStringLiteral("新規Projectを作成しました"));
}

bool MvmController::openProject(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (dirty()) {
        setStatus(QStringLiteral("未保存の変更を保存または破棄してからProjectを開いてください"));
        return false;
    }
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルのProjectファイルを指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    QString error;
    void* acquiredLock = nullptr;
    if (!acquireProjectLock(path, acquiredLock, error)) {
        setStatus(QStringLiteral("Projectを開けません: ") + error);
        return false;
    }
    const auto loaded = project::loadProjectJson(path);
    if (!loaded.success) {
        releaseLockHandle(acquiredLock);
        setStatus(QStringLiteral("Projectを開けません: ") + QString::fromStdString(loaded.error));
        return false;
    }
    adoptProjectLock(acquiredLock, path);
    return adoptProject(loaded.project, path, QStringLiteral("Projectを開きました"));
}

bool MvmController::saveProjectAs(const QUrl& fileUrl) {
    settleRecoveryWrite();
    if (busy_)
        return false;
    if (!projectLockHeld_) {
        setStatus(QStringLiteral("Projectを保存できません: Projectを排他できません"));
        return false;
    }
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの保存先を指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    QString error;
    void* acquiredLock = nullptr;
    if (!acquireProjectLock(path, acquiredLock, error)) {
        setStatus(QStringLiteral("Projectを保存できません: ") + error);
        return false;
    }
    // 同じ実体か確定できないときも「同じかもしれない」として外部変更を検査する。
    // 別物と決めつけると、開いている canonical を検査なしで上書きしうる。
    const bool sameTarget =
        project::comparePathIdentity(path, projectPath_) != project::PathSameness::Different;
    if (sameTarget) {
        QString mismatch;
        if (!canonicalBaseMatchesDisk(mismatch)) {
            releaseLockHandle(acquiredLock);
            setStatus(mismatch);
            Q_EMIT externalCanonicalChangeOnSave();
            return false;
        }
    }
    if (!writeCanonicalProject(project_, path, error)) {
        releaseLockHandle(acquiredLock);
        setStatus(QStringLiteral("Projectを保存できません: ") + error);
        return false;
    }
    const auto previousPath = projectPath_;
    // このsessionが書いたautosaveだけ消す。dismissや破損で残したfileは残す。
    const bool sessionWroteRecovery = recoveryRevision_ != 0;
    QString recoveryError;
    bool removedRecovery = true;
    if (sessionWroteRecovery)
        removedRecovery = removeRecoveryBeside(previousPath, recoveryError);
    adoptProjectLock(acquiredLock, path);
    projectPath_ = path;
    // 数式の描画は保存先の cache/math/<file 名> に置く。保存先が変われば描き直す (cache
    // を移さない)。
    syncMathCacheAuthority();
    savedProject_ = project_;
    savedRevision_ = currentRevision_;
    const bool rememberedBase = rememberCanonicalBase();
    recoveryRevision_ = 0;
    recoveryProject_.reset();
    recoveryRecordedSha256_.clear();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    recoveryDebounceTimer_.stop();
    recoveryMaximumTimer_.stop();
    Q_EMIT stateChanged();
    if (!removedRecovery) {
        setStatus(
            QStringLiteral("Projectは保存しましたが、以前の自動復旧データを削除できません: ") +
            recoveryError);
        return true;
    }
    if (!rememberedBase) {
        setStatus(QStringLiteral("Projectは保存しましたが、保存後のhashを記録できません"));
        return true;
    }
    setStatus(QStringLiteral("Projectを保存しました: ") + fromPath(path));
    return true;
}

bool MvmController::saveProject() {
    return saveCurrentProject(false);
}

bool MvmController::saveProjectOverwritingExternalChange() {
    return saveCurrentProject(true);
}

bool MvmController::saveCurrentProject(bool overwriteExternalChange) {
    settleRecoveryWrite();
    if (busy_)
        return false;
    if (!projectLockHeld_) {
        setStatus(QStringLiteral("Projectを保存できません: Projectを排他できません"));
        return false;
    }
    if (!overwriteExternalChange) {
        QString mismatch;
        if (!canonicalBaseMatchesDisk(mismatch)) {
            setStatus(mismatch);
            Q_EMIT externalCanonicalChangeOnSave();
            return false;
        }
    }
    QString error;
    if (!writeCanonicalProject(project_, projectPath_, error)) {
        setStatus(QStringLiteral("Projectを保存できません: ") + error);
        return false;
    }
    savedProject_ = project_;
    savedRevision_ = currentRevision_;
    const bool rememberedBase = rememberCanonicalBase();
    recoveryProject_.reset();
    recoveryRecordedSha256_.clear();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryForeign_ = false;
    recoveryDebounceTimer_.stop();
    recoveryMaximumTimer_.stop();
    QString recoveryError;
    // このsessionのautosaveだけ消す。保全したrecoveryは次回Openまで残す。
    const bool sessionWroteRecovery = recoveryRevision_ != 0;
    recoveryRevision_ = 0;
    const bool removedRecovery = !sessionWroteRecovery || removeRecoveryFile(recoveryError);
    if (!removedRecovery) {
        setStatus(QStringLiteral("Projectは保存しましたが、自動復旧データを削除できません: ") +
                  recoveryError);
        return true;
    }
    if (!rememberedBase) {
        setStatus(QStringLiteral("Projectは保存しましたが、保存後のhashを記録できません"));
        return true;
    }
    setStatus(QStringLiteral("Projectを保存しました: ") + fromPath(projectPath_));
    return true;
}

bool MvmController::discardUnsavedChanges() {
    if (busy_)
        return false;
    if (!projectLockHeld_) {
        setStatus(QStringLiteral("未保存の変更を破棄できません: Projectを排他できません"));
        return false;
    }
    QString recoveryError;
    if (!removeRecoveryFile(recoveryError)) {
        setStatus(QStringLiteral("未保存の変更を破棄できません: ") + recoveryError);
        return false;
    }
    const std::string selectedId = currentClipId();
    project_ = savedProject_;
    refreshAudioInputAuthority(true);
    audioMixerBuses_.clear();
    audioMixerPeaks_.clear();
    clearEditHistory();
    selectedClipIds_.clear();
    currentRevision_ = savedRevision_;
    recoveryProject_.reset();
    recoveryCanonicalChanged_ = false;
    recoveryCorrupt_ = false;
    recoveryDebounceTimer_.stop();
    recoveryMaximumTimer_.stop();
    refreshTimelineModel();
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, selectedId));
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("未保存の変更は破棄しましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    setStatus(QStringLiteral("未保存の変更を破棄しました"));
    return true;
}

} // namespace mvm::app
