#ifndef MVM_APPS_MVM_MVM_CONTROLLER_H
#define MVM_APPS_MVM_MVM_CONTROLLER_H

#include "preview_engine/preview_engine.h"
#include "project/project.h"

#include <memory>
#include <optional>
#include <array>
#include <vector>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace mvm::app {

class PreviewEngineRhiItem;
class TimelineClipModel;

class MvmController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectPath READ projectPath CONSTANT)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipName READ currentClipName NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipPath READ currentClipPath NOTIFY stateChanged)
    Q_PROPERTY(bool hasCurrentClip READ hasCurrentClip NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool previewReady READ previewReady NOTIFY stateChanged)
    Q_PROPERTY(bool hasManimAsset READ hasManimAsset NOTIFY stateChanged)
    Q_PROPERTY(bool hasManimTimelineClip READ hasManimTimelineClip NOTIFY stateChanged)
    Q_PROPERTY(QString manimScriptPath READ manimScriptPath NOTIFY stateChanged)
    Q_PROPERTY(QString manimSceneName READ manimSceneName NOTIFY stateChanged)
    Q_PROPERTY(QString manimStateText READ manimStateText NOTIFY stateChanged)
    Q_PROPERTY(QStringList clipNames READ clipNames NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel* timelineModel READ timelineModel CONSTANT)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY stateChanged)
    Q_PROPERTY(int currentClipIndex READ currentClipIndex NOTIFY stateChanged)
    Q_PROPERTY(qint64 playheadFrame READ playheadFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalTimelineFrames READ totalTimelineFrames NOTIFY stateChanged)
    Q_PROPERTY(QString currentTimeText READ currentTimeText NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY stateChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged)
    Q_PROPERTY(double effectPositionX READ effectPositionX NOTIFY stateChanged)
    Q_PROPERTY(double effectPositionY READ effectPositionY NOTIFY stateChanged)
    Q_PROPERTY(double effectScale READ effectScale NOTIFY stateChanged)
    Q_PROPERTY(double effectRotation READ effectRotation NOTIFY stateChanged)
    Q_PROPERTY(double effectOpacity READ effectOpacity NOTIFY stateChanged)
    Q_PROPERTY(double effectCropLeft READ effectCropLeft NOTIFY stateChanged)
    Q_PROPERTY(double effectCropTop READ effectCropTop NOTIFY stateChanged)
    Q_PROPERTY(double effectCropRight READ effectCropRight NOTIFY stateChanged)
    Q_PROPERTY(double effectCropBottom READ effectCropBottom NOTIFY stateChanged)
    Q_PROPERTY(qint64 effectFadeIn READ effectFadeIn NOTIFY stateChanged)
    Q_PROPERTY(qint64 effectFadeOut READ effectFadeOut NOTIFY stateChanged)

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

    bool hasManimAsset() const { return !project_.manimAssets.empty(); }

    bool hasManimTimelineClip() const;

    QString manimScriptPath() const { return manimScriptPath_; }

    QString manimSceneName() const { return manimSceneName_; }

    QString manimStateText() const { return manimStateText_; }

    QStringList clipNames() const;
    QAbstractItemModel* timelineModel() const;

    int clipCount() const { return static_cast<int>(project_.timelineClips.size()); }

    int currentClipIndex() const { return currentClipIndex_; }

    qint64 playheadFrame() const { return playheadFrame_; }

    qint64 totalTimelineFrames() const { return totalTimelineFrames_; }

    QString currentTimeText() const;

    bool playing() const { return playing_; }

    bool canPlay() const;

    bool canExport() const { return !project_.timelineClips.empty() && !busy_; }

    double effectPositionX() const;
    double effectPositionY() const;
    double effectScale() const;
    double effectRotation() const;
    double effectOpacity() const;
    double effectCropLeft() const;
    double effectCropTop() const;
    double effectCropRight() const;
    double effectCropBottom() const;
    qint64 effectFadeIn() const;
    qint64 effectFadeOut() const;

    Q_INVOKABLE bool generateManimClip(const QUrl& scriptUrl, const QString& sceneName);
    Q_INVOKABLE bool regenerateManimClip();
    Q_INVOKABLE bool addManimToTimeline();
    Q_INVOKABLE bool addVideoClip(const QUrl& fileUrl);
    Q_INVOKABLE bool selectClip(int index);
    Q_INVOKABLE bool selectTimelineClip(const QString& clipId, qint64 frame);
    Q_INVOKABLE bool seekTimelineFrame(qint64 frame);
    Q_INVOKABLE bool playTimeline();
    Q_INVOKABLE bool pauseTimeline();
    Q_INVOKABLE bool moveTimelineClip(const QString& clipId, int destinationTrack,
                                      qint64 timelineStartFrame);
    Q_INVOKABLE bool trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta);
    Q_INVOKABLE bool deleteCurrentClip();
    Q_INVOKABLE bool exportTimeline(const QUrl& outputUrl);
    Q_INVOKABLE bool applyCurrentClipEffects(double positionX, double positionY, double scale,
                                             double rotation, double opacity, double cropLeft,
                                             double cropTop, double cropRight, double cropBottom,
                                             qint64 fadeInSourceFrames, qint64 fadeOutSourceFrames);

public Q_SLOTS:
    void shutdown();

Q_SIGNALS:
    void stateChanged();

private:
    void pollPreviewState();
    void advanceTimelinePlayback();
    void setStatus(QString status);
    bool initializePreviewEngine(const QString& failurePrefix);
    bool resetPreviewEngine();
    void restoreFirstManimClip();
    void syncFirstManimAsset();
    // Manim asset が確定したら timeline 上の Manim clip を追従させる。
    // timeline と asset の対応を決める箇所はここだけにする。
    bool syncManimTimelineClip(bool addIfMissing);
    bool saveProject(project::Project candidate, const QString& failurePrefix);
    void setCurrentClipSelection(int index);
    bool refreshPreviewAfterSavedEdit(const std::string& selectedClipId,
                                      const QString& successStatus);
    const project::ClipEffects& currentEffects() const;
    bool submitClipComposition(preview::PreviewSourceId source, const project::TimelineClip& clip,
                               QString& error);
    bool syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error);
    bool refreshCurrentClipEffectsPreview(QString& error);
    void refreshTimelineModel();
    bool generateAndInstallManimClip(const std::filesystem::path& scriptPath,
                                     const QString& sceneName, bool requirePreviewReady);
    void queueVideoClipInstall(const std::filesystem::path& videoPath, QString clipName,
                               int clipIndex, std::int64_t sourceFrame);
    bool installVideoClip(const std::filesystem::path& videoPath, const QString& clipName,
                          int clipIndex, std::int64_t sourceFrame);
    bool prepareTimelineFrameForPlayback(int clipIndex, std::int64_t timelineFrame);
    bool queuePreparedPlayback(int clipIndex, std::int64_t timelineFrame);
    void startPendingPlayback();
    bool cancelPendingPlaybackForPause();
    void stopPlaybackWithError(QString error);

    std::filesystem::path projectPath_;
    std::filesystem::path manimExecutablePath_;
    project::Project project_;
    std::shared_ptr<preview::PreviewEngine> previewEngine_;
    std::shared_ptr<preview::PreviewEventDispatcher> dispatcher_;
    std::unique_ptr<TimelineClipModel> timelineModel_;
    PreviewEngineRhiItem* previewSurface_ = nullptr;
    std::optional<preview::PreviewSourceId> currentSource_;
    std::optional<preview::PreviewSourceId> staleSource_;
    struct TrackPreviewSource {
        preview::PreviewSourceId source;
        std::string clipId;
        int clipIndex = -1;
    };
    std::array<std::optional<TrackPreviewSource>, 2> trackSources_;
    std::vector<preview::PreviewSourceId> retiredSources_;
    QString statusText_ = QStringLiteral("Previewを初期化しています");
    QString currentClipName_;
    QString currentClipPath_;
    QString manimScriptPath_;
    QString manimSceneName_;
    QString manimStateText_;
    std::optional<std::filesystem::path> pendingVideoPath_;
    QString pendingClipName_;
    int pendingClipIndex_ = -1;
    std::int64_t pendingSourceFrame_ = 0;
    int currentClipIndex_ = -1;
    std::int64_t playheadFrame_ = 0;
    std::int64_t totalTimelineFrames_ = 0;
    bool busy_ = false;
    bool previewReady_ = false;
    bool shutdownStarted_ = false;
    bool playing_ = false;
    bool pendingPlaybackStart_ = false;
    int playbackClipIndex_ = -1;
    std::int64_t playbackBaseFrame_ = 0;
    int pendingPlaybackClipIndex_ = -1;
    std::int64_t pendingPlaybackBaseFrame_ = 0;
    QElapsedTimer playbackClock_;
    QTimer playbackTimer_;
    QTimer stateTimer_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MVM_CONTROLLER_H
