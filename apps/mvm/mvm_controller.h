#ifndef MVM_APPS_MVM_MVM_CONTROLLER_H
#define MVM_APPS_MVM_MVM_CONTROLLER_H

#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media_bin_model.h"
#include "preview_engine/preview_engine.h"
#include "project/media_bin.h"
#include "project/project.h"
#include "project/timeline_edit.h"
#include "timeline_clip_model.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QTemporaryDir;

namespace mvm::app {

class PreviewEngineRhiItem;
class TrackModel;
class ShuttleAudioPlayback;

class MvmController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(MvmController)
    QML_UNCREATABLE("アプリが生成したコントローラーを使用してください")
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipName READ currentClipName NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipPath READ currentClipPath NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedTextClip READ selectedTextClip NOTIFY stateChanged)
    Q_PROPERTY(bool previewVideoAtPlayhead READ previewVideoAtPlayhead NOTIFY stateChanged)
    Q_PROPERTY(bool hasCurrentClip READ hasCurrentClip NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool previewReady READ previewReady NOTIFY stateChanged)
    Q_PROPERTY(bool hasManimAsset READ hasManimAsset NOTIFY stateChanged)
    Q_PROPERTY(bool hasManimTimelineClip READ hasManimTimelineClip NOTIFY stateChanged)
    Q_PROPERTY(QString manimScriptPath READ manimScriptPath NOTIFY stateChanged)
    Q_PROPERTY(QString manimSceneName READ manimSceneName NOTIFY stateChanged)
    Q_PROPERTY(QString manimStateText READ manimStateText NOTIFY stateChanged)
    Q_PROPERTY(QStringList clipNames READ clipNames NOTIFY stateChanged)
    Q_PROPERTY(mvm::app::TimelineClipModel* timelineModel READ timelineModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* videoTrackModel READ videoTrackModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* audioTrackModel READ audioTrackModel CONSTANT)
    Q_PROPERTY(mvm::app::MediaBinModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(int videoTrackCount READ videoTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int audioTrackCount READ audioTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY stateChanged)
    Q_PROPERTY(int currentClipIndex READ currentClipIndex NOTIFY stateChanged)
    Q_PROPERTY(qint64 playheadFrame READ playheadFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalTimelineFrames READ totalTimelineFrames NOTIFY stateChanged)
    Q_PROPERTY(QString currentTimeText READ currentTimeText NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(int shuttleRate READ shuttleRate NOTIFY stateChanged)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY stateChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryCanonicalChanged READ recoveryCanonicalChanged NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryCorrupt READ recoveryCorrupt NOTIFY stateChanged)
    Q_PROPERTY(bool recoveryForeign READ recoveryForeign NOTIFY stateChanged)
    Q_PROPERTY(QString recoveryProjectPath READ recoveryProjectPath NOTIFY stateChanged)
    Q_PROPERTY(bool canExport READ canExport NOTIFY stateChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY stateChanged)
    Q_PROPERTY(bool exportCancelling READ exportCancelling NOTIFY stateChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY stateChanged)
    Q_PROPERTY(QString exportProgressText READ exportProgressText NOTIFY stateChanged)
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
    using ExportRunner =
        std::function<TimelineExportResult(const project::Project&, const TimelineExportRequest&)>;
    using ExportThreadFactory = std::function<std::thread(std::function<void()>)>;
    // 書き出し完了後に出力ファイルをExplorerで表示する。失敗時はfalseとerrorを返す。
    using FileRevealer = std::function<bool(const std::filesystem::path& path, QString& error)>;
    // meter の下限。linear 0 を -inf にすると QML 側で扱いにくいので床を決めておく。
    static constexpr double kMeterSilenceDb = -60.0;

    MvmController(std::filesystem::path projectPath, std::filesystem::path manimExecutablePath,
                  project::Project project, QObject* parent = nullptr,
                  ExportRunner exportRunner = {}, ExportThreadFactory exportThreadFactory = {},
                  FileRevealer fileRevealer = {});
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
    TimelineClipModel* timelineModel() const;
    QAbstractItemModel* videoTrackModel() const;
    QAbstractItemModel* audioTrackModel() const;
    MediaBinModel* mediaBinModel() const;

    int videoTrackCount() const { return static_cast<int>(project_.videoTracks.size()); }

    int audioTrackCount() const { return static_cast<int>(project_.audioTracks.size()); }

    int clipCount() const { return static_cast<int>(project_.timelineClips.size()); }

    int currentClipIndex() const { return currentClipIndex_; }

    qint64 playheadFrame() const { return playheadFrame_; }

    qint64 totalTimelineFrames() const { return totalTimelineFrames_; }

    QString currentTimeText() const;

    bool playing() const { return playing_ || shuttleRate_ != 0; }

    int shuttleRate() const { return shuttleRate_; }

    bool canPlay() const;

    bool canUndo() const { return !undoHistory_.empty() && !busy_; }

    bool canRedo() const { return !redoHistory_.empty() && !busy_; }

    bool dirty() const { return currentRevision_ != savedRevision_; }

    bool recoveryAvailable() const { return recoveryProject_.has_value(); }

    bool recoveryCanonicalChanged() const { return recoveryCanonicalChanged_; }

    bool recoveryCorrupt() const { return recoveryCorrupt_; }

    bool recoveryForeign() const { return recoveryForeign_; }

    bool holdsProjectLock() const { return projectLockHeld_; }

    QString recoveryProjectPath() const;

    bool canExport() const { return !project_.timelineClips.empty() && !busy_; }

    bool exporting() const { return exporting_; }

    bool exportCancelling() const { return exportCancelling_; }

    double exportProgress() const { return exportProgress_; }

    QString exportProgressText() const { return exportProgressText_; }

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
    Q_INVOKABLE bool createTextClip(const QString& content, int x, int y);
    Q_INVOKABLE bool updateTextClip(const QString& clipId, const QVariantMap& values);
    Q_INVOKABLE QVariantMap textClipData(const QString& clipId) const;
    Q_INVOKABLE QString textClipAt(int x, int y);
    Q_INVOKABLE QUrl textRasterUrl(int index);
    Q_INVOKABLE bool textClipVisible(int index) const;
    QVariantMap selectedTextClip() const;
    bool previewVideoAtPlayhead() const;
    Q_INVOKABLE bool selectClip(int index);
    // linked=false (Alt+クリック) ならリンク相手を選択に含めない。
    Q_INVOKABLE bool selectTimelineClip(const QString& clipId, qint64 frame, bool linked);
    Q_INVOKABLE bool toggleTimelineClipSelection(const QString& clipId, qint64 frame);
    Q_INVOKABLE bool selectTimelineClips(const QStringList& clipIds);
    Q_INVOKABLE bool seekTimelineFrame(qint64 frame);
    // scrub。drag 中は最新位置だけを coalesce して seek し、release で確定する。
    Q_INVOKABLE void beginScrub();
    Q_INVOKABLE void scrubToFrame(qint64 frame);
    Q_INVOKABLE void endScrub();
    Q_INVOKABLE bool playTimeline();
    Q_INVOKABLE bool pauseTimeline();
    Q_INVOKABLE bool shuttleLeft();
    Q_INVOKABLE bool shuttleRight();
    Q_INVOKABLE bool stepTimelineFrames(int delta);
    Q_INVOKABLE bool jumpToEditPoint(int direction);
    // 以下の編集は linked=true ならリンク相手にも同じ編集を適用する (Premiere の
    // リンクされた選択)。QML は Alt を押しながらの操作で linked=false を渡す。
    Q_INVOKABLE bool moveTimelineClip(const QString& clipId, const QString& trackKind,
                                      int trackIndex, qint64 timelineStartFrame, bool linked);
    Q_INVOKABLE bool trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta,
                              bool linked);
    // レート調整ツール。素材範囲を変えずに速度を変えて、edge 側の端を動かす。
    Q_INVOKABLE bool rateStretchClip(const QString& clipId, const QString& edge,
                                     qint64 projectFrameDelta, bool linked);
    // レート調整の drag 中の表示。確定と同じ Project の計算 (project::previewRateStretch) で、
    // {delta: 実際に動かす量, clips: {clipId: {startDelta, endDelta, speed}}} を返す。
    // startDelta / endDelta は clip の開始 / 終端が現在の位置から動く frame 数。リンク相手の
    // 尺が違っても相手固有の値になる。伸縮できなければ delta 0 と空の clips。
    Q_INVOKABLE QVariantMap previewRateStretch(const QString& clipId, const QString& edge,
                                               qint64 projectFrameDelta, bool linked) const;
    // タイムラインツール。edge は "left" / "right"。
    Q_INVOKABLE bool rippleTrimClip(const QString& clipId, const QString& edge,
                                    qint64 projectFrameDelta, bool linked);
    // 端のドラッグ量を、確定時と同じ規則 (素材の端・1 frame 以上の尺) で止めた値。
    // drag 中の表示に使う。tool は "select" / "ripple" / "rolling"。
    Q_INVOKABLE qint64 clampEdgeDrag(const QString& clipId, const QString& edge,
                                     const QString& tool, qint64 projectFrameDelta,
                                     bool linked) const;
    Q_INVOKABLE bool rollClipEdge(const QString& clipId, const QString& edge,
                                  qint64 projectFrameDelta, bool linked);
    Q_INVOKABLE bool slipClip(const QString& clipId, qint64 projectFrameDelta, bool linked);
    // スリップのドラッグ中 preview。Project は変更せず、新しい in の素材 frame を表示する。
    // previewSlip は素材の端で止めた後の in の移動量 (素材 frame) を返す。
    // endSlipPreview で通常の preview (playhead 位置) へ戻す。
    Q_INVOKABLE bool beginSlipPreview(const QString& clipId, bool linked);
    Q_INVOKABLE qint64 previewSlip(qint64 projectFrameDelta);
    Q_INVOKABLE void endSlipPreview();
    Q_INVOKABLE bool slideClip(const QString& clipId, qint64 projectFrameDelta, bool linked);
    // スライド量を確定時と同じ規則 (前後の clip の素材の端など) で止めた値。drag 中の表示に使う。
    // スライドできない (前後に clip が無い) ときは 0。
    Q_INVOKABLE qint64 clampSlideDrag(const QString& clipId, qint64 projectFrameDelta,
                                      bool linked) const;
    // allTracks=true なら frame を内側に含む全 track の clip を分割する。
    Q_INVOKABLE bool splitClipAt(const QString& clipId, qint64 frame, bool allTracks, bool linked);
    Q_INVOKABLE QVariantMap previewClipKey(const QString& clipId, qint64 originalFrame,
                                           qint64 requestedFrame, double valuePercent) const;
    Q_INVOKABLE bool commitClipKey(const QString& clipId, qint64 originalFrame,
                                   qint64 requestedFrame, double valuePercent);
    Q_INVOKABLE bool deleteClipKey(const QString& clipId, qint64 frame);
    // direction は "forward" / "backward"。trackKind が空なら全 track。
    Q_INVOKABLE bool selectClipsFromFrame(qint64 frame, const QString& direction,
                                          const QString& trackKind, int trackIndex);
    Q_INVOKABLE bool deleteCurrentClip();
    Q_INVOKABLE bool deleteTimelineClip(const QString& clipId);
    Q_INVOKABLE bool unlinkTimelineClip(const QString& clipId);
    Q_INVOKABLE bool undoLastEdit();
    Q_INVOKABLE bool redoLastEdit();
    Q_INVOKABLE QVariantMap exportSettingsSummary() const;
    Q_INVOKABLE bool exportTimeline(const QUrl& outputUrl);
    Q_INVOKABLE bool exportTimelineWithQuality(const QUrl& outputUrl, const QString& quality);
    Q_INVOKABLE void cancelTimelineExport();
    // effect の 1 値だけを更新する。
    //   commit=false : Project を書き換えず、preview だけを ephemeral な override で
    //                  追従させる (drag 中)。
    //   commit=true  : override を確定して Project transaction にする。
    Q_INVOKABLE bool setEffectValue(const QString& key, double value, bool commit);
    // drag が release されずに終わった場合に override を捨てる。
    Q_INVOKABLE bool cancelEffectPreview();

    // プロジェクトパネル (素材とフォルダ)。folderId が空なら root。
    // 読み込みは timeline へ置かずに bin へ登録するだけ。1 回の呼び出しが 1 undo になる。
    // 戻り値は「1 件以上 commit したか」。読めなかった素材や読み込み済みの素材は status で知らせ、
    // 一部が失敗しても読めた分は commit する。false のときは Project を変更していない。
    Q_INVOKABLE bool importMediaFiles(const QList<QUrl>& fileUrls, const QString& folderId);
    // 作成した folder の id を返す。失敗時は空文字列。
    Q_INVOKABLE QString createMediaFolder(const QString& parentFolderId);
    Q_INVOKABLE bool renameMediaBinEntry(const QString& entryId, const QString& name);
    Q_INVOKABLE bool moveMediaBinEntries(const QStringList& entryIds, const QString& folderId);
    Q_INVOKABLE bool removeMediaBinEntries(const QStringList& entryIds);
    Q_INVOKABLE bool addMediaItemToTimeline(const QString& itemId);

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
    Q_INVOKABLE bool saveProject();
    // 外部変更されたcanonicalを、利用者が明示したときだけ上書きする。
    Q_INVOKABLE bool saveProjectOverwritingExternalChange();
    Q_INVOKABLE bool saveProjectAs(const QUrl& fileUrl);
    Q_INVOKABLE bool discardUnsavedChanges();
    Q_INVOKABLE bool restoreRecovery();
    Q_INVOKABLE bool discardRecovery();
    // 確認を閉じるだけでrecovery fileは残す。次回Openで再度確認する。
    Q_INVOKABLE bool dismissRecovery();
    Q_INVOKABLE QVariantMap projectSettingsForClip(const QString& clipId) const;
    Q_INVOKABLE bool setProjectVideoSettings(int width, int height, int fpsNum, int fpsDen);
    Q_INVOKABLE bool setTimelineFrameRate(int fpsNum, int fpsDen);

public Q_SLOTS:
    void shutdown();

Q_SIGNALS:
    void stateChanged();
    void meterChanged();
    void exportFailed(const QString& message);
    void recoveryDetected();
    void externalCanonicalChangeOnSave();

private:
    bool startTimelineExport(const QUrl& outputUrl, int videoCrf);

    struct TrackPreviewSource {
        preview::PreviewSourceId source;
        // いま表示している clip。source を作った clip とは限らない (連続した clip へ引き継ぐ)。
        std::string clipId;
        int clipIndex = -1;
        // source の descriptor へ渡した対応。引き継いでも書き換えない。
        PreviewVideoMapping mapping;
    };

    // audio source を作り直すべきかの判定に使う identity。descriptor を決める値そのもの。
    // clip を動かす / trim する / Project fps が変わると offset が変わるので作り直す。
    // 逆に clip が違っても素材と offset が同じ (分割直後の連続した clip) なら使い回す。
    // offset の換算は audioPreviewSampleOffset に一本化している。
    struct AudioSourceIdentity {
        std::filesystem::path mediaPath;
        std::int64_t sampleOffset = 0;
        project::ClipEffects effects;
        // 速度が違えば decoder の伸縮が違うので別の source になる。
        std::int64_t speedNum = 1;
        std::int64_t speedDen = 1;
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
        // いま鳴らしている clip。source を作った clip とは限らない。
        std::string clipId;
        int clipIndex = -1;
    };

    void pollPreviewState();
    void pollAudioMeter();
    void advanceTimelinePlayback();
    void advanceTimelineShuttle();
    // timed shuttle の clock (音声があれば audio clock) から現在の timeline frame を求める。
    bool shuttleFrameFromClock(std::int64_t& frame, QString& error) const;
    QString shuttleStatusText() const;
    bool changeShuttleRate(int direction);
    void setStatus(QString status);
    void reportExportFailure(QString message);
    bool initializePreviewEngine(const QString& failurePrefix);
    bool resetPreviewEngine();
    void restoreFirstManimClip();
    void syncFirstManimAsset();
    // Manim asset が確定したら timeline 上の Manim clip を追従させる。
    // timeline と asset の対応を決める箇所はここだけにする。
    bool syncManimTimelineClip(bool addIfMissing);
    bool commitProjectEdit(project::Project candidate, const QString& failurePrefix);
    // キーフレーム編集の確定。変化が無ければ何もせず、確定後は preview を合わせる。
    bool commitClipKeyCandidate(project::Project candidate);
    // timeline 編集の共通手順。一時停止 -> candidate へ edit -> commit -> preview 更新。
    bool
    applyTimelineEdit(const std::function<project::TimelineEditResult(project::Project&)>& edit,
                      const std::string& selectedClipId, const QString& successStatus);
    bool resolveTrimEdge(const QString& edge, project::TrimEdge& trimEdge);
    // bin 編集を candidate へ適用し、成功したら 1 つの undo として commit する。
    bool
    applyMediaBinEdit(const std::function<project::MediaBinEditResult(project::Project&)>& edit,
                      const QString& successStatus);
    // timeline へ置いた素材を bin にも登録する。既に同じ file の素材があれば何もしない。
    bool registerMediaItem(project::Project& candidate, const std::filesystem::path& mediaPath,
                           QString& error) const;
    bool writeCanonicalProject(const project::Project& project, const std::filesystem::path& path,
                               QString& error) const;
    bool saveCurrentProject(bool overwriteExternalChange);
    bool rememberCanonicalBase();
    bool canonicalBaseMatchesDisk(QString& error) const;
    std::filesystem::path recoveryPath() const;
    bool removeRecoveryFile(QString& error);
    bool removeRecoveryBeside(const std::filesystem::path& projectPath, QString& error);
    bool acquireProjectLock(const std::filesystem::path& path, void*& acquired, QString& error);
    void adoptProjectLock(void* acquired, const std::filesystem::path& path);
    void releaseProjectLock();
    static void releaseLockHandle(void* handle);
    QString canonicalFileSha256(bool& readable) const;
    void scheduleRecoveryAutosave();
    void writeRecoveryAutosave();
    void detectRecovery();
    void setCurrentClipSelection(int index);
    // expandLinks なら選んだ clip のリンク相手も選択に含める。
    void setTimelineSelection(const std::vector<std::string>& clipIds, bool expandLinks = true);
    bool refreshPreviewAfterSavedEdit(const std::string& selectedClipId,
                                      const QString& successStatus);
    std::string currentClipId() const;
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
        bool engineReset = false;
        std::vector<AudioPreviewSource> previous;
    };

    bool applyAudioSourceFor(std::int64_t timelineFrame, AudioSwitchUndo& undo, QString& error);
    bool audioIdentitiesFor(const TimelinePreviewAudioMapping& mapped,
                            std::vector<AudioSourceIdentity>& identities, QString& error) const;
    // mappedFrame の layer を sources で合成する composition と seek request を組む。
    std::shared_ptr<preview::CompositionSnapshot>
    previewCompositionFor(const TimelinePreviewFrameMapping& mappedFrame,
                          const std::map<int, TrackPreviewSource>& sources,
                          preview::PreviewFrameRequest& request) const;
    // 再生中、frame の clip を今の source のまま表示できれば source を引き継いで true。
    // 引き継げなければ何も変更せず false (呼び出し側が一時停止して組み直す)。
    // 引き継げなかったら reason に理由を入れる。
    bool handOffPlaybackSources(std::int64_t frame, QString& reason);
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
    void finishTimelineExport(TimelineExportResult result);
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
    std::unique_ptr<MediaBinModel> mediaBinModel_;
    PreviewEngineRhiItem* previewSurface_ = nullptr;
    std::optional<preview::PreviewSourceId> currentSource_;
    // video track index -> preview source。track を増やしても添字を取り違えない。
    std::map<int, TrackPreviewSource> trackSources_;
    std::vector<AudioPreviewSource> audioSources_;
    // 最後に engine が受理した composition。同じ内容を出し直さないために持つ。
    std::shared_ptr<const preview::CompositionSnapshot> submittedComposition_;
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
    std::vector<std::string> selectedClipIds_;
    std::unique_ptr<QTemporaryDir> textRasterDirectory_;
    QHash<QString, QImage> textRasterImages_;
    QHash<QString, QUrl> textRasterUrls_;

    struct UndoEntry {
        project::Project project;
        std::vector<std::string> selectedClipIds;
        std::string currentClipId;
        std::int64_t playheadFrame = 0;
        std::uint64_t revision = 0;
    };

    std::vector<UndoEntry> undoHistory_;
    // undo で戻した編集。新しい編集を commit すると捨てる。
    std::vector<UndoEntry> redoHistory_;
    void pushUndoEntry(UndoEntry entry);
    void clearEditHistory();
    // from の末尾へ戻し、いまの状態を to へ積む。undo / redo の共通手順。
    bool stepEditHistory(std::vector<UndoEntry>& from, std::vector<UndoEntry>& to, bool redo);
    project::Project savedProject_;
    std::optional<project::Project> recoveryProject_;
    bool recoveryCanonicalChanged_ = false;
    bool recoveryCorrupt_ = false;
    bool recoveryForeign_ = false;
    std::string savedCanonicalSha256_;
    bool canonicalBaseKnown_ = false;
    std::string recoveryRecordedSha256_;
    void* projectLockHandle_ = nullptr;
    bool projectLockHeld_ = false;
    std::filesystem::path projectLockPath_;
    std::string sessionId_;
    std::uint64_t currentRevision_ = 0;
    std::uint64_t savedRevision_ = 0;
    std::uint64_t nextRevision_ = 1;
    std::uint64_t recoveryRevision_ = 0;
    std::int64_t playheadFrame_ = 0;
    std::int64_t totalTimelineFrames_ = 0;
    double audioMeterDbLeft_ = kMeterSilenceDb;
    double audioMeterDbRight_ = kMeterSilenceDb;
    double masterVolume_ = 0.35;
    std::thread exportThread_;
    ExportRunner exportRunner_;
    ExportThreadFactory exportThreadFactory_;
    FileRevealer fileRevealer_;
    std::atomic<bool> exportCancelRequested_{false};
    // ETAの基準点。最初に届いた進捗で固定し、準備時間を速度から除外する。
    long long exportBaselineFrame_ = -1;
    std::chrono::steady_clock::time_point exportBaselineTime_{};
    bool exporting_ = false;
    bool exportCancelling_ = false;
    double exportProgress_ = 0.0;
    QString exportProgressText_;
    bool busy_ = false;
    bool previewReady_ = false;
    bool shutdownStarted_ = false;
    bool playing_ = false;
    bool clockOnlyPlayback_ = false;
    int shuttleRate_ = 0;
    bool shuttleSeeking_ = false;
    std::int64_t shuttleBaseFrame_ = 0;
    QElapsedTimer shuttleClock_;
    QTimer shuttleTimer_;
    std::unique_ptr<ShuttleAudioPlayback> shuttleAudio_;
    // 音声を開始できず無音でシャトルしている理由。空なら失敗していない。
    QString shuttleAudioFailure_;
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

    struct SlipPreview {
        std::string clipId;
        project::LinkMode linkMode = project::LinkMode::Linked;
        // timeline frame N = 素材 frame N と写す preview 専用 source。
        std::optional<preview::PreviewSourceId> source;
        std::filesystem::path mediaPath;
        std::int64_t sourceFpsNum = 0;
        std::int64_t sourceFpsDen = 1;
        std::int64_t sourceFrameCount = 0;
        // 表示したい素材 frame (新しい in)。
        std::int64_t sourceFrame = -1;
        // まだ engine へ反映できていない。
        bool pending = false;
    };

    void applySlipPreview();
    std::optional<SlipPreview> slipPreview_;
    QTimer slipPreviewTimer_;
    QTimer meterTimer_;
    QTimer recoveryDebounceTimer_;
    QTimer recoveryMaximumTimer_;
};

} // namespace mvm::app

#endif // MVM_APPS_MVM_MVM_CONTROLLER_H
