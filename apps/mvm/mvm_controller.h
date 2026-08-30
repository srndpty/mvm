#ifndef MVM_APPS_MVM_MVM_CONTROLLER_H
#define MVM_APPS_MVM_MVM_CONTROLLER_H

#include "preview_engine/preview_engine.h"
#include "project/project.h"

#include <memory>
#include <optional>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

namespace mvm::app {

class PreviewEngineRhiItem;

class MvmController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectPath READ projectPath CONSTANT)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipName READ currentClipName NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipPath READ currentClipPath NOTIFY stateChanged)
    Q_PROPERTY(bool hasCurrentClip READ hasCurrentClip NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool previewReady READ previewReady NOTIFY stateChanged)

public:
    MvmController(std::filesystem::path projectPath, std::filesystem::path manimExecutablePath,
                  project::Project project, QObject* parent = nullptr);
    ~MvmController() override;

    void attachPreview(PreviewEngineRhiItem* surface);

    QString projectPath() const;

    QString statusText() const { return statusText_; }

    QString currentClipName() const { return currentClipName_; }

    QString currentClipPath() const { return currentClipPath_; }

    bool hasCurrentClip() const { return !currentClipPath_.isEmpty(); }

    bool busy() const { return busy_; }

    bool previewReady() const { return previewReady_; }

    Q_INVOKABLE bool generateManimClip(const QUrl& scriptUrl, const QString& sceneName);

public Q_SLOTS:
    void shutdown();

Q_SIGNALS:
    void stateChanged();

private:
    void pollPreviewState();
    void setStatus(QString status);
    bool installVideoClip(const std::filesystem::path& videoPath, const QString& clipName);
    void resumeCurrentClip();

    std::filesystem::path projectPath_;
    std::filesystem::path manimExecutablePath_;
    project::Project project_;
    std::shared_ptr<preview::PreviewEngine> previewEngine_;
    std::shared_ptr<preview::PreviewEventDispatcher> dispatcher_;
    PreviewEngineRhiItem* previewSurface_ = nullptr;
    std::optional<preview::PreviewSourceId> currentSource_;
    std::optional<preview::PreviewSourceId> staleSource_;
    QString statusText_ = QStringLiteral("Previewを初期化しています");
    QString currentClipName_;
    QString currentClipPath_;
    bool busy_ = false;
    bool previewReady_ = false;
    bool shutdownStarted_ = false;
    QTimer stateTimer_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MVM_CONTROLLER_H
