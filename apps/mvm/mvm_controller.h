#ifndef MVM_APPS_MVM_MVM_CONTROLLER_H
#define MVM_APPS_MVM_MVM_CONTROLLER_H

#include "preview_engine/preview_engine.h"
#include "project/project.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

namespace mvm::app {

class PreviewEngineRhiItem;
class TimelineClipModel;
class TrackModel;

class MvmController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY stateChanged)
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
    Q_PROPERTY(QAbstractItemModel* videoTrackModel READ videoTrackModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* audioTrackModel READ audioTrackModel CONSTANT)
    Q_PROPERTY(int videoTrackCount READ videoTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int audioTrackCount READ audioTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY stateChanged)
    Q_PROPERTY(int currentClipIndex READ currentClipIndex NOTIFY stateChanged)
    Q_PROPERTY(qint64 playheadFrame READ playheadFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalTimelineFrames READ totalTimelineFrames NOTIFY stateChanged)
    Q_PROPERTY(QString currentTimeText READ currentTimeText NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY stateChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged)
    Q_PROPERTY(int timelineFpsNum READ timelineFpsNum NOTIFY stateChanged)
    Q_PROPERTY(int timelineFpsDen READ timelineFpsDen NOTIFY stateChanged)
    Q_PROPERTY(QString timelineFpsText READ timelineFpsText NOTIFY stateChanged)
    // 現在の frame rate が実測済みか。設定できること != 計測済み。
    Q_PROPERTY(bool frameRateMeasured READ frameRateMeasured NOTIFY stateChanged)
    Q_PROPERTY(QVariantList supportedFrameRates READ supportedFrameRates CONSTANT)
    // audio meter。dBFS。無音時は kMeterSilenceDb を返す。
    Q_PROPERTY(double audioMeterDbLeft READ audioMeterDbLeft NOTIFY meterChanged)
    Q_PROPERTY(double audioMeterDbRight READ audioMeterDbRight NOTIFY meterChanged)
    Q_PROPERTY(double masterVolume READ masterVolume WRITE setMasterVolume NOTIFY stateChanged)
    Q_PROPERTY(int outputWidth READ outputWidth NOTIFY stateChanged)
    Q_PROPERTY(int outputHeight READ outputHeight NOTIFY stateChanged)
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
    // meter の下限。linear 0 を -inf にすると QML 側で扱いにくいので床を決めておく。
    static constexpr double kMeterSilenceDb = -60.0;

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
    QAbstractItemModel* videoTrackModel() const;
    QAbstractItemModel* audioTrackModel() const;

    int videoTrackCount() const { return static_cast<int>(project_.videoTracks.size()); }

    int audioTrackCount() const { return static_cast<int>(project_.audioTracks.size()); }

    int clipCount() const { return static_cast<int>(project_.timelineClips.size()); }

    int currentClipIndex() const { return currentClipIndex_; }

    qint64 playheadFrame() const { return playheadFrame_; }

    qint64 totalTimelineFrames() const { return totalTimelineFrames_; }

    QString currentTimeText() const;

    bool playing() const { return playing_; }

    bool canPlay() const;

    bool canExport() const { return !project_.timelineClips.empty() && !busy_; }

    int timelineFpsNum() const { return static_cast<int>(project_.timelineFpsNum); }

    int timelineFpsDen() const { return static_cast<int>(project_.timelineFpsDen); }

    QString timelineFpsText() const;
    bool frameRateMeasured() const;
    QVariantList supportedFrameRates() const;

    double audioMeterDbLeft() const { return audioMeterDbLeft_; }

    double audioMeterDbRight() const { return audioMeterDbRight_; }
    double masterVolume() const { return masterVolume_; }
    int outputWidth() const { return project_.outputWidth; }
    int outputHeight() const { return project_.outputHeight; }
    void setMasterVolume(double volume);

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
    Q_INVOKABLE bool addAudioClip(const QUrl& fileUrl);
    Q_INVOKABLE bool selectClip(int index);
    Q_INVOKABLE bool selectTimelineClip(const QString& clipId, qint64 frame);
    Q_INVOKABLE bool seekTimelineFrame(qint64 frame);
    // scrub。drag 中は最新位置だけを coalesce して seek し、release で確定する。
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void scrubToFrame(qint64 frame);
    Q_INVOKABLE void endScrub();
    Q_INVOKABLE bool playTimeline();
    Q_INVOKABLE bool pauseTimeline();
    Q_INVOKABLE bool moveTimelineClip(const QString& clipId, const QString& trackKind,
                                      int trackIndex, qint64 timelineStartFrame);
    Q_INVOKABLE bool trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta);
    Q_INVOKABLE bool deleteCurrentClip();
    Q_INVOKABLE bool deleteTimelineClip(const QString& clipId);
    Q_INVOKABLE bool unlinkTimelineClip(const QString& clipId);
    Q_INVOKABLE bool exportTimeline(const QUrl& outputUrl);
    // effect の 1 値だけを更新する。
    //   commit=false : Project を書き換えず、preview だけを ephemeral な override で
    //                  追従させる (drag 中)。
    //   commit=true  : override を確定して Project transaction にする。
    Q_INVOKABLE bool setEffectValue(const QString& key, double value, bool commit);
    // drag が release されずに終わった場合に override を捨てる。
    Q_INVOKABLE bool cancelEffectPreview();

    // track 編集
    Q_INVOKABLE bool addTrack(const QString& trackKind);
    Q_INVOKABLE bool removeTrack(const QString& trackKind, int trackIndex);
    Q_INVOKABLE bool setTrackMuted(const QString& trackKind, int trackIndex, bool muted);

    // 空白部分の ripple delete。gap が無ければ false を返し status に理由を出す。
    Q_INVOKABLE bool hasGapAt(const QString& trackKind, int trackIndex, qint64 frame) const;
    Q_INVOKABLE bool hasClipAt(const QString& trackKind, int trackIndex, qint64 frame) const;
    Q_INVOKABLE bool rippleDeleteGap(const QString& trackKind, int trackIndex, qint64 frame);

    // Project ファイル (.mvm)
    Q_INVOKABLE bool newProject(const QUrl& fileUrl);
    Q_INVOKABLE bool openProject(const QUrl& fileUrl);
    Q_INVOKABLE bool saveProjectAs(const QUrl& fileUrl);
    Q_INVOKABLE bool setTimelineFrameRate(int fpsNum, int fpsDen);

public Q_SLOTS:
    void shutdown();

Q_SIGNALS:
    void stateChanged();
    void meterChanged();

private:
    struct TrackPreviewSource {
        preview::PreviewSourceId source;
        std::string clipId;
        int clipIndex = -1;
        std::int64_t sourceInFrame = 0;
        std::int64_t timelineStartFrame = 0;
        std::int64_t sourceFpsNum = 0;
        std::int64_t sourceFpsDen = 1;
    };

    // audio source を作り直すべきかの判定に使う identity。
    // clip ID は media identity であって timing identity ではない。
    // clip を動かす / trim する / Project fps が変わると offset が変わるため、
    // offset を決めた入力そのものを identity に含める。
    struct AudioSourceIdentity {
        std::string clipId;
        std::int64_t sourceInFrame = 0;
        std::int64_t timelineStartFrame = 0;
        std::int64_t sourceFpsNum = 0;
        std::int64_t sourceFpsDen = 1;
        std::int64_t timelineFpsNum = 0;
        std::int64_t timelineFpsDen = 1;
        bool operator==(const AudioSourceIdentity&) const = default;
    };

    struct AudioPreviewSource {
        preview::PreviewSourceId source;
        AudioSourceIdentity identity;
        // addSource へ実際に渡した descriptor をそのまま持つ。
        // rollback で現在の Project から作り直すと、move / ripple / trim で
        // timelineStartFrame が変わった後は「戻したはずの source」が新しい
        // offset を持ってしまい、identity と実体が食い違う。
        preview::PreviewSourceDescriptor descriptor;
        int clipIndex = -1;
    };

    void pollPreviewState();
    void pollAudioMeter();
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
    // preview override を適用した effects を返す。composition はこれを使う。
    project::ClipEffects effectsForPreview(int clipIndex) const;
    bool applyEffectKey(project::ClipEffects& effects, const QString& key, double value);
    bool syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error);
    // audio source set の差し替えは、master/mix inputの参照寿命を守るため
    // remove -> add の順に行い、prepare/commitへ素直に割れない。
    // そこで「切り替え前の状態」を持ち、後段が失敗したら元へ戻す compensation
    // transaction にする。video 側だけ rollback して audio が新しいまま残る、
    // という部分 commit を作らない。
    struct AudioSwitchUndo {
        bool changed = false;
        std::vector<AudioPreviewSource> previous;
    };

    bool applyAudioSourceFor(std::int64_t timelineFrame, AudioSwitchUndo& undo, QString& error);
    // applyAudioSourceFor の結果を打ち消す。控えておいた descriptor をそのまま
    // 使い、現在の Project からは作り直さない。戻せなかった場合は黙って成功に
    // せず false を返す。
    bool revertAudioSource(const AudioSwitchUndo& undo, QString& error);
    // clip から audio source descriptor を組む。offset の換算は mapping 側へ委譲する。
    bool audioDescriptorFor(int clipIndex, preview::PreviewSourceDescriptor& descriptor,
                            QString& error);
    bool refreshCurrentClipEffectsPreview(QString& error);
    void refreshTimelineModel();
    // trackKind 文字列を TrackRef へ解決する。失敗時は status を設定して false。
    bool resolveTrackRef(const QString& trackKind, int trackIndex, project::TrackRef& track) const;
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
    // Project を丸ごと差し替える (New / Open)。preview も作り直す。
    bool adoptProject(project::Project loaded, std::filesystem::path path, QString successStatus);

    std::filesystem::path projectPath_;
    std::filesystem::path manimExecutablePath_;
    project::Project project_;
    std::shared_ptr<preview::PreviewEngine> previewEngine_;
    std::shared_ptr<preview::PreviewEventDispatcher> dispatcher_;
    std::unique_ptr<TimelineClipModel> timelineModel_;
    std::unique_ptr<TrackModel> videoTrackModel_;
    std::unique_ptr<TrackModel> audioTrackModel_;
    PreviewEngineRhiItem* previewSurface_ = nullptr;
    std::optional<preview::PreviewSourceId> currentSource_;
    // video track index -> preview source。track を増やしても添字を取り違えない。
    std::map<int, TrackPreviewSource> trackSources_;
    std::vector<AudioPreviewSource> audioSources_;
    // drag 中だけ生きる effect の上書き。Project へは書かない。
    // これがあるのは currentClipIndex_ の clip に対してだけである。
    std::optional<project::ClipEffects> previewEffectsOverride_;
    int previewEffectsClipIndex_ = -1;
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
    double audioMeterDbLeft_ = kMeterSilenceDb;
    double audioMeterDbRight_ = kMeterSilenceDb;
    double masterVolume_ = 0.35;
    bool busy_ = false;
    bool previewReady_ = false;
    bool shutdownStarted_ = false;
    bool playing_ = false;
    bool pendingPlaybackStart_ = false;
    bool scrubbing_ = false;
    bool scrubPending_ = false;
    std::int64_t scrubTargetFrame_ = 0;
    int playbackClipIndex_ = -1;
    std::int64_t playbackBaseFrame_ = 0;
    int pendingPlaybackClipIndex_ = -1;
    std::int64_t pendingPlaybackBaseFrame_ = 0;
    QElapsedTimer playbackClock_;
    QTimer playbackTimer_;
    QTimer stateTimer_;
    QTimer scrubTimer_;
    QTimer meterTimer_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MVM_CONTROLLER_H
