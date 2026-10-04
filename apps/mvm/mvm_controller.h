#ifndef MVM_APPS_MVM_MVM_CONTROLLER_H
#define MVM_APPS_MVM_MVM_CONTROLLER_H

#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "audio_adjustment_job.h"
#include "math_raster_cache.h"
#include "media/audio_preview/audio_mixer_bus.h"
#include "media/audio_preview/wasapi_audio_sink.h"
#include "media/transcribe/transcribe.h"
#include "media_bin_model.h"
#include "media_source_identity.h"
#include "preview_engine/preview_engine.h"
#include "project/media_bin.h"
#include "project/project.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "subtitle_model.h"
#include "timeline_clip_model.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QSet>
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
class ImageRasterCache;
struct MediaImportResult;
class TrackModel;
class ScrubAudioPlayback;
class ShuttleAudioPlayback;

class MvmController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(MvmController)
    QML_UNCREATABLE("アプリが生成したコントローラーを使用してください")
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(bool audioAdjusting READ audioAdjusting NOTIFY audioAdjustmentChanged)
    Q_PROPERTY(
        bool audioAdjustmentApplying READ audioAdjustmentApplying NOTIFY audioAdjustmentChanged)
    Q_PROPERTY(
        int audioAdjustmentProgress READ audioAdjustmentProgress NOTIFY audioAdjustmentChanged)
    Q_PROPERTY(QVariantList audioAdjustmentResults READ audioAdjustmentResults NOTIFY
                   audioAdjustmentResultsChanged)
    Q_PROPERTY(QVariantList audioAdjustmentRanges READ audioAdjustmentRanges NOTIFY
                   audioAdjustmentResultsChanged)
    Q_PROPERTY(QString audioAdjustmentError READ audioAdjustmentError NOTIFY audioAdjustmentChanged)
    Q_PROPERTY(
        bool canApplyAudioAdjustment READ canApplyAudioAdjustment NOTIFY audioAdjustmentChanged)
    Q_PROPERTY(bool audioAdjustmentAuditioning READ audioAdjustmentAuditioning NOTIFY
                   audioAdjustmentChanged)
    Q_PROPERTY(bool audioAdjustmentNeedsRegeneration READ audioAdjustmentNeedsRegeneration NOTIFY
                   stateChanged)
    Q_PROPERTY(QVariantMap savedAudioAdjustmentSettings READ savedAudioAdjustmentSettings NOTIFY
                   stateChanged)
    Q_PROPERTY(QString currentClipName READ currentClipName NOTIFY stateChanged)
    Q_PROPERTY(QString currentClipPath READ currentClipPath NOTIFY stateChanged)
    Q_PROPERTY(bool transcribing READ transcribing NOTIFY stateChanged)
    Q_PROPERTY(int transcriptionProgress READ transcriptionProgress NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel* transcriptionModel READ transcriptionModel CONSTANT)
    Q_PROPERTY(QString transcriptionError READ transcriptionError NOTIFY stateChanged)
    Q_PROPERTY(bool canApplyTranscription READ canApplyTranscription NOTIFY stateChanged)
    Q_PROPERTY(QVariantList transcriptionSources READ transcriptionSources NOTIFY stateChanged)
    Q_PROPERTY(QAbstractItemModel* subtitleModel READ subtitleModel CONSTANT)
    // timeline の S1 に描く字幕。表示範囲 (± 余白) の字幕だけを通す (字幕が数千あっても
    // delegate を表示範囲に比例する数に抑える)。
    Q_PROPERTY(mvm::app::TimelineClipWindowModel* subtitleWindow READ subtitleWindow CONSTANT)
    Q_PROPERTY(QString selectedSubtitleId READ selectedSubtitleId NOTIFY stateChanged)
    Q_PROPERTY(QStringList selectedSubtitleIds READ selectedSubtitleIds NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedSubtitle READ selectedSubtitle NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap subtitleStyle READ subtitleStyle NOTIFY stateChanged)
    Q_PROPERTY(bool hasSubtitleTrack READ hasSubtitleTrack NOTIFY stateChanged)
    Q_PROPERTY(bool subtitlesVisible READ subtitlesVisible NOTIFY stateChanged)
    Q_PROPERTY(bool burnSubtitles MEMBER burnSubtitles_ NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedTextClip READ selectedTextClip NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedMathClip READ selectedMathClip NOTIFY stateChanged)
    Q_PROPERTY(bool previewVideoAtPlayhead READ previewVideoAtPlayhead NOTIFY stateChanged)
    Q_PROPERTY(QString textOverlayClip READ textOverlayClip NOTIFY stateChanged)
    // プレビューで移動・拡縮できる選択中の素材 (画像・動画が 1 つだけ選ばれているとき)。
    Q_PROPERTY(QString transformClipId READ transformClipId NOTIFY stateChanged)
    // ドラッグ中の文字 preview が変わるたびに増える。QML の文字画像の再読込に使う。
    Q_PROPERTY(int textPreviewSerial READ textPreviewSerial NOTIFY stateChanged)
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
    // timeline の clip delegate 用。表示範囲と固定する clip だけを通す (timelineModel を絞る)。
    Q_PROPERTY(
        mvm::app::TimelineClipWindowModel* timelineClipWindow READ timelineClipWindow CONSTANT)
    // preview の文字 layer 用。文字 clip だけを通す。
    Q_PROPERTY(mvm::app::TextClipFilterModel* textClipModel READ textClipModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* videoTrackModel READ videoTrackModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* audioTrackModel READ audioTrackModel CONSTANT)
    Q_PROPERTY(mvm::app::MediaBinModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(int videoTrackCount READ videoTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int audioTrackCount READ audioTrackCount NOTIFY stateChanged)
    Q_PROPERTY(int clipCount READ clipCount NOTIFY stateChanged)
    Q_PROPERTY(int currentClipIndex READ currentClipIndex NOTIFY stateChanged)
    Q_PROPERTY(qint64 playheadFrame READ playheadFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 totalTimelineFrames READ totalTimelineFrames NOTIFY stateChanged)
    Q_PROPERTY(qint64 navigationTimelineFrames READ navigationTimelineFrames NOTIFY stateChanged)
    Q_PROPERTY(QVariantList timelineMarkers READ timelineMarkers NOTIFY stateChanged)
    // timeline に描くトランジション。{transitionId, trackKind, trackIndex, start, cut, end}。
    // 選択は selectedTransitionId と比べる (選択で model を変えない)。
    // Repeater の model なので stateChanged (再生中も頻繁に出る) では通知しない。通知のたびに
    // delegate が作り直される。
    Q_PROPERTY(
        QVariantList timelineTransitions READ timelineTransitions NOTIFY timelineTransitionsChanged)
    // 選択中の編集点 {trackKind, trackIndex, frame}。無ければ空。clip の選択とは排他。
    Q_PROPERTY(QVariantMap selectedEditPoint READ selectedEditPoint NOTIFY stateChanged)
    Q_PROPERTY(QString selectedTransitionId READ selectedTransitionId NOTIFY stateChanged)
    // エフェクトコントロールに出す選択中のトランジション。無ければ空。
    // {transitionId, trackKind, cut, framesBeforeCut, framesAfterCut, durationText (timecode),
    //  maxBefore, maxAfter,
    //  outgoingClipId, outgoingName, outgoingStart, outgoingEnd,
    //  incomingClipId, incomingName, incomingStart, incomingEnd} (frame は timeline frame)。
    // max* は cut の前後に置ける長さの上限 (transitionSpanLimits, リンク相手込み)。
    // 上限の計算は Project を複写するので、再生中も出る stateChanged では通知しない。
    Q_PROPERTY(
        QVariantMap selectedTransition READ selectedTransition NOTIFY selectedTransitionChanged)
    // Delete で消せるもの (トランジション、または clip) が選ばれている。
    Q_PROPERTY(bool canDeleteSelection READ canDeleteSelection NOTIFY stateChanged)
    Q_PROPERTY(qint64 inFrame READ inFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 outFrame READ outFrame NOTIFY stateChanged)
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
    // ファイル選択ダイアログの拡張子の表 (media_file_filters.h)。判定には使わない。
    Q_PROPERTY(QStringList mediaFileNameFilters READ mediaFileNameFilters CONSTANT)
    // audio meter。dBFS。無音時は kMeterSilenceDb を返す。
    Q_PROPERTY(double audioMeterDbLeft READ audioMeterDbLeft NOTIFY meterChanged)
    Q_PROPERTY(double audioMeterDbRight READ audioMeterDbRight NOTIFY meterChanged)
    Q_PROPERTY(bool audioMeterClipped READ audioMeterClipped NOTIFY meterChanged)
    Q_PROPERTY(double masterVolume READ masterVolume WRITE setMasterVolume NOTIFY stateChanged)
    Q_PROPERTY(int outputWidth READ outputWidth NOTIFY stateChanged)
    Q_PROPERTY(int outputHeight READ outputHeight NOTIFY stateChanged)
    Q_PROPERTY(double effectPositionX READ effectPositionX NOTIFY stateChanged)
    Q_PROPERTY(double effectPositionY READ effectPositionY NOTIFY stateChanged)
    Q_PROPERTY(double effectScaleX READ effectScaleX NOTIFY stateChanged)
    Q_PROPERTY(double effectScaleY READ effectScaleY NOTIFY stateChanged)
    Q_PROPERTY(double effectRotation READ effectRotation NOTIFY stateChanged)
    Q_PROPERTY(double effectOpacity READ effectOpacity NOTIFY stateChanged)
    Q_PROPERTY(double effectCropLeft READ effectCropLeft NOTIFY stateChanged)
    Q_PROPERTY(double effectCropTop READ effectCropTop NOTIFY stateChanged)
    Q_PROPERTY(double effectCropRight READ effectCropRight NOTIFY stateChanged)
    Q_PROPERTY(double effectCropBottom READ effectCropBottom NOTIFY stateChanged)
    Q_PROPERTY(qint64 effectFadeIn READ effectFadeIn NOTIFY stateChanged)
    Q_PROPERTY(qint64 effectFadeOut READ effectFadeOut NOTIFY stateChanged)

public:
    bool audioAdjusting() const { return audioAdjustmentJob_ != nullptr; }

    bool audioAdjustmentApplying() const { return audioApplyPending_; }

    // 取消した worker の破棄まで終わっている。中止そのものは待たない。
    // 100 ms の poll が動いているか。待機中に常駐しないことの試験用。
    bool audioAdjustmentPollingForTest() const { return audioAdjustmentTimer_.isActive(); }
    bool audioWatchFallbackActiveForTest() const { return audioWatchFallbackTimer_.isActive(); }
    // watcher が通知を取りこぼして path を外した状態を作る。
    void dropAudioFileWatchForTest();
    // 5 秒の取りこぼし確認を 1 回だけ実行する (timer の発火と同じ処理)。
    void runAudioWatchFallbackForTest() { checkAudioWatchFallback(); }

    bool audioAdjustmentWorkersIdle() const {
        return audioAdjustmentJob_ == nullptr && audioAdjustmentRetired_.empty() &&
               audioContentJob_ == nullptr && audioContentRetired_.empty();
    }

    int audioAdjustmentProgress() const;
    QVariantList audioAdjustmentResults() const;
    QVariantList audioAdjustmentRanges() const;

    QString audioAdjustmentError() const { return audioAdjustmentError_; }

    bool canApplyAudioAdjustment() const;

    bool audioAdjustmentAuditioning() const { return audioAdjustmentAudition_ != nullptr; }

    bool audioAdjustmentNeedsRegeneration() const;
    QVariantMap savedAudioAdjustmentSettings() const;
    Q_INVOKABLE bool startAudioAdjustment(const QVariantMap& settings);
    Q_INVOKABLE void cancelAudioAdjustment();
    Q_INVOKABLE bool auditionAudioAdjustment();
    Q_INVOKABLE void stopAudioAdjustmentAudition();
    // 非同期照合の開始時は false。確定時に audioAdjustmentApplied を通知する。
    Q_INVOKABLE bool applyAudioAdjustment();
    using ExportRunner =
        std::function<TimelineExportResult(const project::Project&, const TimelineExportRequest&)>;
    using ExportThreadFactory = std::function<std::thread(std::function<void()>)>;
    // 書き出し完了後に出力ファイルをExplorerで表示する。失敗時はfalseとerrorを返す。
    using FileRevealer = std::function<bool(const std::filesystem::path& path, QString& error)>;
    // meter の下限。linear 0 を -inf にすると QML 側で扱いにくいので床を決めておく。
    static constexpr double kMeterSilenceDb = -96.0;

    MvmController(std::filesystem::path projectPath, std::filesystem::path manimExecutablePath,
                  project::Project project, QObject* parent = nullptr,
                  ExportRunner exportRunner = {}, ExportThreadFactory exportThreadFactory = {},
                  FileRevealer fileRevealer = {});
    ~MvmController() override;

    void attachPreview(PreviewEngineRhiItem* surface);

    QString projectPath() const;

    QString statusText() const { return statusText_; }

    std::uint64_t playbackRebuildCount() const { return playbackRebuildCount_; }

    QString lastPlaybackRebuildReason() const { return lastPlaybackRebuildReason_; }

    double playbackMaxPreparationMs() const { return playbackMaxPreparationMs_; }

    // 先読みの準備に失敗した回数。同じ境界を何度も準備し直していないことの検査に使う。
    std::uint64_t playbackPreparationFailureCount() const {
        return playbackPreparationFailureCount_;
    }

    // 境界の前に準備して、まだ引き継いでいない source の数の最大。先読みが次の境界だけに
    // 留まっていることの検査に使う。
    std::size_t playbackMaxPreparedSourceCount() const { return playbackMaxPreparedSourceCount_; }

    // 先読みの準備のうち、要求の後に再生や Project が変わったので取り消した、または完了しても
    // 使わずに外した回数 (controller を通さない engine の pause / seek で捨てたものは含まない)。
    std::uint64_t playbackStalePreparationCount() const { return playbackStalePreparationCount_; }

    // clip 境界までに先読みの準備が終わらず、境界で完了を待った回数。
    std::uint64_t playbackPreparationWaitCount() const { return playbackPreparationWaitCount_; }

    std::size_t pendingSourcePreparationCount() const {
        return pendingVideoPreparations_.size() + pendingAudioPreparations_.size();
    }

    // 準備が済み、まだ引き継いでいない source の数。
    std::size_t preparedPlaybackSourceCountForTest() const {
        return preparedVideoSources_.size() + preparedAudioSources_.size();
    }

    std::shared_ptr<audio::AudioMixerBus> audioMixerBusForTest(int index) const {
        return audioMixerBuses_.at(static_cast<std::size_t>(index));
    }

    audio::WasapiSnapshot scrubAudioSnapshotForTest() const;

    // 再生中の composition が使っている source の数。
    std::size_t activePlaybackSourceCountForTest() const {
        return trackSources_.size() + audioSources_.size();
    }

    void holdSourcePreparationsForTest(bool held);
    // recovery の書き込み (serialize + atomic write) を差し替える (試験用)。
    using RecoveryWriter = std::function<project::ProjectIoResult(
        const project::Project&, const std::filesystem::path&, const std::filesystem::path&,
        const std::string&, const std::string&, const std::string&)>;

    void setRecoveryWriterForTest(RecoveryWriter writer) { recoveryWriter_ = std::move(writer); }

    // recovery の worker thread の作り方を差し替える (試験用。作れない場合を試す)。
    using RecoveryThreadFactory = std::function<std::thread(std::function<void()>)>;

    void setRecoveryThreadFactoryForTest(RecoveryThreadFactory factory) {
        recoveryThreadFactory_ = std::move(factory);
    }

    // debounce を待たずに自動保存を始める (試験用)。書き込みの完了は待たない。
    void writeRecoveryAutosaveForTest() { writeRecoveryAutosave(); }

    bool recoveryWriteInFlightForTest() const {
        return recoveryWrite_ != nullptr || !recoveryQueue_.empty();
    }

    // 自動保存に失敗した後の再試行の timer が動いている (試験用)。
    bool recoveryRetryScheduledForTest() const { return recoveryMaximumTimer_.isActive(); }

    std::uint64_t recoveryRevisionForTest() const { return recoveryRevision_; }

    std::uint64_t currentRevisionForTest() const { return currentRevision_; }

    std::uint64_t recoveryWriteCompletionCountForTest() const {
        return recoveryWriteCompletionCount_;
    }

    // 次に要求する準備を、取り消しの効かない段 (decoder の seek の途中に相当) で止める。
    void blockNextSourcePreparationForTest(int milliseconds);

    // controller を通さずに engine の transport を変える負例 (engine 側の古さの判定) に使う。
    std::shared_ptr<preview::PreviewEngine> previewEngineForTest() const { return previewEngine_; }

    bool resetPreviewEngineForTest() { return resetPreviewEngine(); }

    // engine が公開している source の数と、古くなって捨てた準備の数。
    std::uint64_t publishedPreviewSourceCountForTest() const;
    std::uint64_t engineStaleSourcePreparationCountForTest() const;

    // clip 境界で登録枠の不足と判定して Preview engine を作り直した回数。登録枠の不足ではない
    // 失敗 (恒久的に扱えない構成など) で作り直していないことの検査に使う。
    std::uint64_t playbackCapacityResetCount() const { return playbackCapacityResetCount_; }

    // 境界で登録枠の一時的な不足に当たり、engine を作り直さずに枠が返るのを待った回数。
    std::uint64_t playbackSlotWaitCount() const { return playbackSlotWaitCount_; }

    bool setPreviewRegistrationLimitForTest(std::size_t limit);
    bool disablePreviewAudioSourcesForTest();
    std::vector<std::int64_t> presentedFrameHistoryForTest() const;
    std::vector<std::int64_t> unpairedFrameHistoryForTest() const;
    // 直近に提示した output frame と、そのとき提示した composition の最前面 layer の不透明度。
    // layer が 1 枚以下の frame は負の値にする。
    std::vector<std::pair<std::int64_t, float>> presentedOverlayOpacityHistoryForTest() const;

    // 最後に提示した output frame、そのときの composition の layer 数、最背面の decode layer の
    // 素材 frame (無ければ -1)。
    struct PresentedFrameForTest {
        std::int64_t outputFrame = -1;
        std::uint32_t layerCount = 0;
        std::int64_t baseSourceFrame = -1;
    };

    PresentedFrameForTest lastPresentedFrameForTest() const;
    // 音声の endpoint へ実際に設定した音量 (試験用の倍率を掛けた値)。
    float audioEndpointVolumeForTest() const;
    // 直近に提示した frame (古い順、最大 256)。
    std::vector<PresentedFrameForTest> presentedFramesForTest() const;

    preview::PreviewTelemetry previewTelemetry() const { return previewEngine_->telemetry(); }

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

    TimelineClipWindowModel* timelineClipWindow() const { return timelineClipWindow_.get(); }

    TextClipFilterModel* textClipModel() const { return textClipModel_.get(); }

    QAbstractItemModel* videoTrackModel() const;
    QAbstractItemModel* audioTrackModel() const;
    MediaBinModel* mediaBinModel() const;

    int videoTrackCount() const { return static_cast<int>(project_.videoTracks.size()); }

    int audioTrackCount() const { return static_cast<int>(project_.audioTracks.size()); }

    int clipCount() const { return static_cast<int>(project_.timelineClips.size()); }

    int currentClipIndex() const { return currentClipIndex_; }

    qint64 playheadFrame() const { return playheadFrame_; }

    qint64 navigationTimelineFrames() const;
    QVariantList timelineMarkers() const;
    QVariantList timelineTransitions() const;
    QVariantMap selectedEditPoint() const;

    QString selectedTransitionId() const { return QString::fromStdString(selectedTransitionId_); }

    QVariantMap selectedTransition() const { return shownSelectedTransition_; }

    bool canDeleteSelection() const;

    qint64 inFrame() const { return project_.inFrame.value_or(-1); }

    qint64 outFrame() const { return project_.outFrame.value_or(-1); }

    qint64 totalTimelineFrames() const { return totalTimelineFrames_; }

    QString currentTimeText() const;

    bool playing() const { return playing_ || shuttleRate_ != 0; }

    int shuttleRate() const { return shuttleRate_; }

    bool canPlay() const;

    bool canUndo() const { return !undoHistory_.empty() && !busy_; }

    // 受理されなかった操作が Undo 履歴を積んでいないことの検査に使う。
    std::size_t undoDepthForTest() const { return undoHistory_.size(); }

    // 試験の場面の前後で Project が元へ戻ったかを比べるための読み取り。
    const project::Project& projectForTest() const { return project_; }

    std::shared_ptr<preview::CompositionSnapshot> subtitleCompositionForTest(qint64 frame,
                                                                             QString& error) const;

    // Undo / Redo 履歴が持つ Project の複製の概算 byte 数の合計。
    std::size_t editHistoryBytes() const;

    std::size_t redoDepthForTest() const { return redoHistory_.size(); }

    // 履歴の byte 予算を差し替える (試験用)。次の編集・Undo・Redo から効く。
    void setEditHistoryByteBudgetForTest(std::size_t bytes) { editHistoryByteBudget_ = bytes; }

    bool canRedo() const { return !redoHistory_.empty() && !busy_; }

    bool dirty() const { return currentRevision_ != savedRevision_; }

    bool recoveryAvailable() const { return recoveryProject_.has_value(); }

    bool recoveryCanonicalChanged() const { return recoveryCanonicalChanged_; }

    bool recoveryCorrupt() const { return recoveryCorrupt_; }

    bool recoveryForeign() const { return recoveryForeign_; }

    bool holdsProjectLock() const { return projectLockHeld_; }

    QString recoveryProjectPath() const;

    bool canExport() const { return totalTimelineFrames_ > 0 && !busy_; }

    bool exporting() const { return exporting_; }

    bool exportCancelling() const { return exportCancelling_; }

    double exportProgress() const { return exportProgress_; }

    QString exportProgressText() const { return exportProgressText_; }

    int timelineFpsNum() const { return static_cast<int>(project_.timelineFpsNum); }

    int timelineFpsDen() const { return static_cast<int>(project_.timelineFpsDen); }

    QString timelineFpsText() const;
    bool frameRateMeasured() const;
    QVariantList supportedFrameRates() const;
    QStringList mediaFileNameFilters() const;

    double audioMeterDbLeft() const { return audioMeterDbLeft_; }

    double audioMeterDbRight() const { return audioMeterDbRight_; }

    double masterVolume() const { return masterVolume_; }

    int outputWidth() const { return project_.outputWidth; }

    int outputHeight() const { return project_.outputHeight; }

    void setMasterVolume(double volume);

    bool audioMeterClipped() const { return audioMeterClipped_; }

    Q_INVOKABLE void clearMasterAudioClip();

    double effectPositionX() const;
    double effectPositionY() const;
    double effectScaleX() const;
    double effectScaleY() const;
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
    // 画像を再生ヘッドの位置へ、最上位の clip より上の空いた映像 track に 5 秒で置く。
    Q_INVOKABLE bool addImageClip(const QUrl& fileUrl);
    // 素材の種別を内容で判定し、動画・音声・画像のどれかとして timeline へ置く。
    // メニューのダイアログと timeline への drop はここを通る。拡張子は見ない。
    Q_INVOKABLE bool addMediaFileToTimeline(const QUrl& fileUrl);
    using TranscriptionRunner =
        std::function<transcribe::Result(const transcribe::Request&, const std::atomic<bool>*)>;

    // 素材の内容の hash を 1 MiB 読むたびに、読む前に呼ぶ (worker thread から)。
    // hash の途中でキャンセルが効くことを確かめるための差し込み口。
    void setTranscriptionHashObserverForTest(std::function<void()> observer) {
        transcriptionHashObserver_ = std::move(observer);
    }

    void setTranscriptionRunnerForTest(TranscriptionRunner runner) {
        transcriptionRunner_ = std::move(runner);
    }

    bool transcribing() const { return transcribing_; }

    int transcriptionProgress() const { return transcriptionProgress_; }

    QAbstractItemModel* transcriptionModel() { return &transcriptionModel_; }

    QString transcriptionError() const { return transcriptionError_; }

    bool canApplyTranscription() const {
        return !transcribing_ && !transcriptionCues_.empty() &&
               transcriptionRevision_ == currentRevision_ &&
               transcriptionProjectPath_ == projectPath_ &&
               transcriptionEditSerial_ == nextRevision_ &&
               transcriptionProjectGeneration_ == projectGeneration_;
    }

    QVariantList transcriptionSources() const;
    Q_INVOKABLE bool startTranscription(const QString& sourceId, bool timelineClip,
                                        const QUrl& modelUrl, const QString& backend,
                                        const QString& language, qint64 insertionFrame,
                                        const QString& initialPrompt = {});
    Q_INVOKABLE void cancelTranscription();
    Q_INVOKABLE bool updateTranscriptionCue(const QString& id, const QString& content, qint64 start,
                                            qint64 end);
    Q_INVOKABLE bool applyTranscription(bool replace);

    QAbstractItemModel* subtitleModel() { return &subtitleModel_; }

    TimelineClipWindowModel* subtitleWindow() const { return subtitleWindow_.get(); }

    QString selectedSubtitleId() const { return selectedSubtitleId_; }

    QVariantMap selectedSubtitle() const;
    QVariantMap subtitleStyle() const;

    bool hasSubtitleTrack() const { return project_.subtitles.has_value(); }

    bool subtitlesVisible() const { return project_.subtitles && project_.subtitles->visible; }

    // 字幕パネルの一覧から選ぶ。開始位置へシークし、timeline の選択も字幕 1 件にする。
    Q_INVOKABLE bool selectSubtitle(const QString& id);
    // timeline 上の字幕のクリック。additive (Ctrl / Shift) なら選択へ足し引きする。
    // clip の選択は外し、Delete・コピー・カット・ペースト・複製の対象を字幕にする。
    Q_INVOKABLE bool selectTimelineSubtitle(const QString& id, bool additive);
    // S1 の矩形選択。frame の区間 [fromFrame, toFrame] に掛かる字幕をすべて選ぶ。
    Q_INVOKABLE bool selectTimelineSubtitlesInRange(qint64 fromFrame, qint64 toFrame);
    // timeline のドラッグの確定。anchorId を startFrame へ置く量だけ、選択中の字幕
    // (anchor が選択外なら anchor だけ) を動かす。duplicate (Alt+ドラッグ) なら元を残して
    // 複製を置く。複製はリンクを持たない。重なる配置は全体を拒否する。
    Q_INVOKABLE bool placeTimelineSubtitles(const QString& anchorId, qint64 startFrame,
                                            bool duplicate);
    QStringList selectedSubtitleIds() const;
    Q_INVOKABLE bool addSubtitle(const QString& content, qint64 start, qint64 end);
    Q_INVOKABLE bool updateSubtitle(const QString& id, const QString& content, qint64 start,
                                    qint64 end);
    Q_INVOKABLE bool deleteSelectedSubtitle();
    // 再生位置で分け、本文も分ける。textCursor は本文欄のカーソル位置 (UTF-16)。0 以下なら
    // 再生位置の比率に近い句読点で分ける。
    Q_INVOKABLE bool splitSelectedSubtitle(int textCursor = -1);
    Q_INVOKABLE bool mergeSelectedSubtitle();
    Q_INVOKABLE bool setSubtitleStyle(const QVariantMap& values);
    // 共通書式のドラッグ・フォント一覧の hover 中に、Project を変えずに preview だけを
    // 描き直す。確定は setSubtitleStyle、取り消しは cancelSubtitleStylePreview。
    Q_INVOKABLE bool previewSubtitleStyle(const QVariantMap& values);
    Q_INVOKABLE void cancelSubtitleStylePreview();
    Q_INVOKABLE bool setSubtitlesVisible(bool visible);
    Q_INVOKABLE bool importSubtitles(const QUrl& url, bool replace);
    Q_INVOKABLE bool exportSubtitles(const QUrl& url);
    Q_INVOKABLE bool rippleDeleteSelection();
    Q_INVOKABLE bool createTextClip(const QString& content, int x, int y);
    Q_INVOKABLE bool updateTextClip(const QString& clipId, const QVariantMap& values);
    // 数値のドラッグ中に、Project を変えずに preview だけを values で描き直す。
    // 確定は updateTextClip、取り消しは cancelTextPreview。
    Q_INVOKABLE bool previewTextClip(const QString& clipId, const QVariantMap& values);
    // テロップの定位置へ置く (textPresetPlacement)。揃えも同じ値にする。
    Q_INVOKABLE bool placeTextClip(const QString& clipId, const QString& alignment);
    Q_INVOKABLE void cancelTextPreview();

    // 数式 clip (docs/math-clips.md)。確定は描けるかどうかと無関係に行い、描けない式も
    // Project に残す。描画は MathRasterCache が worker で行う。
    Q_INVOKABLE bool createMathClip(const QString& source);
    // values の key: source / fontSize / color / backgroundColor。Undo 1 回分。
    Q_INVOKABLE bool updateMathClip(const QString& clipId, const QVariantMap& values);
    // 入力中の式・書式を Project を変えずに描かせて preview する。取り消しは cancelMathPreview。
    Q_INVOKABLE bool previewMathClip(const QString& clipId, const QVariantMap& values);
    Q_INVOKABLE void cancelMathPreview();
    // 再試行: backend を確かめ直し、覚えている失敗を忘れて描き直す。描けている式は disk の
    // 結果を使い続ける (強制の描き直しではない。MiKTeX の導入後や一時的な失敗の後に使う)。
    Q_INVOKABLE void retryMathRendering();
    // clipId / source / fontSize / color / backgroundColor と描画の状態
    // (state: checking / rendering / stale / ready / error / unavailable、message、log、toolchain)。
    Q_INVOKABLE QVariantMap mathClipData(const QString& clipId) const;
    QVariantMap selectedMathClip() const;
    // 試験用: 数式の backend の確認を差し替えて確かめ直す (偽の backend を注入する)。
    void setMathPreflightForTest(MathRasterCache::PreflightFunction preflight);
    MathRasterCache& mathRastersForTest() { return *mathRasters_; }

    int textPreviewSerial() const { return textPreviewSerial_; }

    Q_INVOKABLE QVariantMap textClipData(const QString& clipId) const;
    Q_INVOKABLE QString textClipAt(int x, int y);
    Q_INVOKABLE QUrl textRasterUrl(int index);
    Q_INVOKABLE bool textClipVisible(int index) const;
    // 再生位置での文字の不透明度 (opacity の値・key・fade)。UI が文字を重ねるときに使う。
    Q_INVOKABLE double textClipOpacity(int index) const;
    // 文字の描画範囲 (出力画素)。preview の選択枠と掴める範囲に使う。
    Q_INVOKABLE QRect textClipBounds(const QString& clipId) const;
    // 映像のある frame では文字を preview engine が track 順に合成する。
    // ドラッグ中・編集中の文字だけは UI が重ねて表示するので、その clip を合成から外す。
    // 空文字で解除する。
    Q_INVOKABLE void setTextOverlayClip(const QString& clipId);

    // --- プレビュー上の素材の枠 (出力画素) ---------------------------------------
    QString transformClipId() const;
    // 画像・動画の見えている矩形 {x, y, width, height, pivotX, pivotY, rotation, visible}。
    // drag 中の override を含む。素材の寸法が分からない・何も見えていないときは空。
    Q_INVOKABLE bool textClipHasMotion(const QString& clipId) const;
    Q_INVOKABLE QVariantMap clipVisualGeometry(const QString& clipId) const;
    // 再生位置で (x, y) に見えている最も上の素材 (文字・画像・動画)。無ければ空。
    Q_INVOKABLE QString visualClipAt(double x, double y);
    // 再生位置で見えている excludeClipId 以外の素材の外接矩形 (吸着の相手)。
    Q_INVOKABLE QVariantList previewSnapRects(const QString& excludeClipId);
    // 見えている矩形を (x, y, width, height) にする {positionX, positionY, scaleX, scaleY}。
    // 決められなければ空。setEffectValues へそのまま渡せる。
    // 最後に preview engine へ渡した composition で、動画 clip の layer を置いた矩形
    // (出力を 0..1 とした座標)。preview が最新の effect を受け取ったかを試験で確かめる。
    std::optional<QRectF> submittedLayerDestination(const QString& clipId) const;
    // 最後に preview engine へ渡した composition の layer の不透明度 (背面 -> 前面)。
    // トランジションの incoming が重なって上がっていくことを試験で確かめる。
    std::vector<float> submittedLayerOpacities() const;
    // 保留中の作り直しが無く、engine が最後に受理した composition を提示し終えて止まっている。
    bool previewPresentedLatest() const;
    Q_INVOKABLE QVariantMap effectsForVisualRect(const QString& clipId, double x, double y,
                                                 double width, double height) const;

    QString textOverlayClip() const { return textOverlayClipId_; }

    QVariantMap selectedTextClip() const;
    bool previewVideoAtPlayhead() const;
    Q_INVOKABLE bool selectClip(int index);
    // linked=false (Alt+クリック) ならリンク相手を選択に含めない。
    Q_INVOKABLE bool selectTimelineClip(const QString& clipId, bool linked);
    Q_INVOKABLE bool toggleTimelineClipSelection(const QString& clipId);
    Q_INVOKABLE bool selectTimelineClips(const QStringList& clipIds);
    // timeline の全 clip を選択する (Ctrl+A)。
    Q_INVOKABLE bool selectAllClips();
    Q_INVOKABLE bool seekTimelineFrame(qint64 frame);
    // scrub。drag 中は最新位置だけを coalesce して seek し、release で確定する。
    // timeline の frame を timecode (currentTimeText と同じ書式)
    // にする。ルーラーの目盛りの文字に使う。
    Q_INVOKABLE QString frameTimecode(qint64 frame) const;
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
    Q_INVOKABLE bool copySelectedClips();
    Q_INVOKABLE bool cutSelectedClips();
    Q_INVOKABLE bool pasteClips();
    Q_INVOKABLE bool duplicateSelectedClips();
    // clipId をドラッグしたとき一緒に動く clip 群の端。minStartFrame と、含まれる種別ごとの
    // videoMinTrack / videoMaxTrack / audioMinTrack / audioMaxTrack、一緒に動く clipIds。
    // clip が無ければ空。
    Q_INVOKABLE QVariantMap timelineDragBounds(const QString& clipId) const;
    Q_INVOKABLE bool duplicateTimelineClipsAt(const QString& clipId, const QString& trackKind,
                                              int trackIndex, qint64 timelineStartFrame);
    Q_INVOKABLE bool addTimelineMarker();
    Q_INVOKABLE bool deleteTimelineMarker(qint64 frame);
    Q_INVOKABLE bool jumpToMarker(int direction);
    Q_INVOKABLE bool markIn();
    Q_INVOKABLE bool markOut();
    Q_INVOKABLE bool jumpToIn();
    Q_INVOKABLE bool jumpToOut();
    Q_INVOKABLE bool clearInOut();
    Q_INVOKABLE bool clearIn();
    Q_INVOKABLE bool clearOut();
    Q_INVOKABLE bool trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta,
                              bool linked);
    // レート調整ツール。素材範囲を変えずに速度を変えて、edge 側の端を動かす。
    Q_INVOKABLE bool rateStretchClip(const QString& clipId, const QString& edge,
                                     qint64 projectFrameDelta, bool linked);
    Q_INVOKABLE QVariantMap clipSpeedDurationState(const QString& clipId) const;
    Q_INVOKABLE QVariantMap previewClipSpeedDuration(const QString& clipId, const QString& input,
                                                     double speedPercent,
                                                     const QString& durationText,
                                                     bool preservePitch, bool ripple) const;
    // ripple しない速度・尺の変更が、延びた先の clip と重なるか。UI は上書きの確認に使う。
    Q_INVOKABLE bool clipSpeedDurationNeedsOverwrite(const QString& clipId, const QString& input,
                                                     double speedPercent,
                                                     const QString& durationText) const;
    // overwrite は、重なる後続 clip を削ってよいとユーザーが確認した場合だけ true にする。
    Q_INVOKABLE bool applyClipSpeedDuration(const QString& clipId, const QString& input,
                                            double speedPercent, const QString& durationText,
                                            bool preservePitch, bool ripple, bool overwrite);
    Q_INVOKABLE bool canInsertFrameHold(const QString& clipId) const;
    Q_INVOKABLE bool insertFrameHoldAtPlayhead(const QString& clipId);
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
    // 再生ヘッドを内側に含む選択 clip (リンク相手を含む) を再生ヘッドで分割する (Ctrl+K)。
    // 該当する選択 clip が無ければ、再生ヘッドを含む current clip を分割する。
    Q_INVOKABLE bool splitSelectionAtPlayhead();
    // 選択 clip (無ければ current clip) の音量を stepDb だけ変える ([ / ])。映像はリンク相手の
    // audio clip を変える。1 回の呼び出しが 1 undo。
    Q_INVOKABLE bool stepSelectedClipVolume(double stepDb);
    // 選択 clip (無ければ current clip) とリンク相手の有効/無効を切り換える (Shift+E)。
    // 1 つでも有効なら全部を無効にし、全部が無効なら全部を有効にする。1 回が 1 undo。
    Q_INVOKABLE bool toggleSelectedClipsEnabled();
    // 右クリックメニュー用。指定した clip (とリンク相手) だけを切り換える。
    Q_INVOKABLE bool toggleTimelineClipEnabled(const QString& clipId);
    // 既定のトランジションを適用する (Shift+D)。clip を選択していれば、その clip (とリンク相手)
    // の先頭と末尾に 1 秒のフェードを付ける。1 回が 1 undo。
    // 編集点 (またはトランジション) を選んでいれば、そこへ 1 秒のクロスディゾルブ /
    // クロスフェードを 置く (リンク相手も同じ cut なら一緒に)。
    Q_INVOKABLE bool applyDefaultTransition();
    Q_INVOKABLE QVariantMap previewClipKey(const QString& clipId, qint64 originalFrame,
                                           qint64 requestedFrame, double value) const;
    Q_INVOKABLE bool commitClipKey(const QString& clipId, qint64 originalFrame,
                                   qint64 requestedFrame, double value);
    Q_INVOKABLE bool deleteClipKey(const QString& clipId, qint64 frame);
    // direction は "forward" / "backward"。trackKind が空なら全 track。
    Q_INVOKABLE bool selectClipsFromFrame(qint64 frame, const QString& direction,
                                          const QString& trackKind, int trackIndex);
    Q_INVOKABLE bool deleteCurrentClip();
    // clip の edge ("left" / "right") の編集点を選ぶ。接している clip が無ければ clip を選ぶ。
    Q_INVOKABLE bool selectEditPoint(const QString& clipId, const QString& edge);
    Q_INVOKABLE bool selectTransition(const QString& transitionId);
    // 選択中のトランジションの cut 前後の長さを変える (リンク相手の既存トランジションも)。
    // 1 回が 1 undo。素材 frame に乗らない値は最も近い置ける長さへ吸着させる
    // (nearestTransitionSpan)。keepTotal は長さ・配置・本体のドラッグで総尺を保つ吸着、false は
    // 片側の端のドラッグ (前後を別々に)。置ける長さが無い、または吸着すると今の値と同じなら、
    // 理由を status に出して false を返す (Project も再生も変えない)。
    Q_INVOKABLE bool setTransitionSpan(qint64 framesBeforeCut, qint64 framesAfterCut,
                                       bool keepTotal);
    // Delete。トランジションを選んでいればそれを消し、そうでなければ clip を消す。
    Q_INVOKABLE bool deleteSelection();
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
    Q_PROPERTY(QVariantList keyframeChannels READ keyframeChannels NOTIFY stateChanged)
    QVariantList keyframeChannels() const;
    Q_INVOKABLE bool editEffectKey(const QString& name, qint64 from, qint64 to, double value,
                                   bool commit);
    Q_INVOKABLE bool setEffectSpline(const QString& name, qint64 frame, double control1,
                                     double control2, bool commit);
    Q_INVOKABLE bool copyEffectKeys(const QString& name, const QVariantList& frames, bool cut);
    Q_INVOKABLE bool pasteEffectKeys(const QString& name);
    Q_INVOKABLE bool deleteEffectKeys(const QString& name, const QVariantList& frames);

    Q_INVOKABLE bool setEffectAnimation(const QString& name, bool enabled);
    Q_INVOKABLE bool toggleEffectKey(const QString& name);
    Q_INVOKABLE bool moveEffectKey(const QString& name, qint64 from, qint64 to, bool commit);
    Q_INVOKABLE bool setEffectInterpolation(const QString& name, qint64 frame, int interpolation);
    Q_INVOKABLE bool seekEffectKey(const QString& name, int direction);
    Q_INVOKABLE bool seekEffectFrame(qint64 localFrame, bool scrub = false);
    Q_INVOKABLE bool setEffectValue(const QString& key, double value, bool commit);
    // 複数の項目 ({"positionX": 10, "scaleX": 120} など) を 1 つの変更として適用する。
    // commit なら 1 つの undo、そうでなければ preview だけを更新する。
    Q_INVOKABLE bool setEffectValues(const QVariantMap& values, bool commit);
    // 対象の clip を ID で明示する版。選択 (current clip) に頼らない。プレビューの枠は
    // 掴んだ clip を最後までこれで指す。
    Q_INVOKABLE bool setClipEffectValues(const QString& clipId, const QVariantMap& values,
                                         bool commit);
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
    // 削除すると一緒に消える timeline clip の数。削除できないときは -1 (status に理由)。
    Q_INVOKABLE int mediaBinRemovalClipCount(const QStringList& entryIds);
    Q_INVOKABLE bool removeMediaBinEntries(const QStringList& entryIds);
    Q_INVOKABLE bool addMediaItemToTimeline(const QString& itemId);
    // ドロップした位置 (track と frame) へ素材を置く。複数なら frame から順に後ろへ並べる。
    // trackIndex が track 数と等しければ track を足して置く。全体を 1 つの undo にする。
    Q_INVOKABLE bool addMediaItemsToTimelineAt(const QStringList& itemIds, const QString& trackKind,
                                               int trackIndex, qint64 frame);
    // 外部ファイルのドロップ。プロジェクトパネルへ登録してから置く。
    Q_INVOKABLE bool addMediaFilesToTimelineAt(const QList<QUrl>& fileUrls,
                                               const QString& trackKind, int trackIndex,
                                               qint64 frame);

    // track 編集
    Q_INVOKABLE bool addTrack(const QString& trackKind);
    Q_INVOKABLE bool removeTrack(const QString& trackKind, int trackIndex);
    Q_INVOKABLE bool setTrackMuted(const QString& trackKind, int trackIndex, bool muted);
    // 目玉のドラッグ塗りで通った track をまとめて確定する。1 回の undo になる。
    Q_INVOKABLE bool setTracksMuted(const QString& trackKind, const QVariantList& trackIndices,
                                    bool muted);
    Q_INVOKABLE bool setAudioTrackMix(int index, double gainDb, double pan, bool commit = true);
    Q_INVOKABLE void cancelAudioTrackMix(int index);
    Q_INVOKABLE bool setAudioMixerName(int index, const QString& name);
    Q_INVOKABLE QVariantMap audioTrackMeter(int index);
    Q_INVOKABLE void clearAudioTrackClip(int index);
    Q_INVOKABLE bool setTrackSolo(const QString& trackKind, int trackIndex, bool solo);

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
    // 外部で差し替えられた画像素材を見つけて preview を描き直す。
    // アプリが前面へ戻ったときに呼ぶ (WaveformCache::revalidateAll と同じ契機)。
    void revalidateMedia();

Q_SIGNALS:
    void audioAdjustmentChanged();
    void audioAdjustmentApplied();
    void audioAdjustmentResultsChanged();
    void stateChanged();
    void timelineTransitionsChanged();
    void selectedTransitionChanged();
    void meterChanged();
    void exportFailed(const QString& message);
    void recoveryDetected();
    void externalCanonicalChangeOnSave();

private:
    void connectAudioFileWatch();
    void pollAudioAdjustment();
    void refreshAudioInputAuthority(bool notify);
    void updateAudioAdjustmentRegeneration(bool notify);
    bool computeAudioAdjustmentNeedsRegeneration() const;
    void reapAudioAdjustmentJobs();
    void refreshAudioFileCacheCheap();
    void checkAudioWatchFallback();
    void invalidateAudioFile(const std::string& key);
    void scheduleAudioContentRecheck();
    void finishAudioContentRecheck();
    bool commitAudioAdjustment();
    void dropAudioAdjustmentResult(const QString& error);
    void syncAudioFileWatch();
    void ensureAudioAdjustmentTimer();
    bool audioCheapIdentityMatches(const std::vector<AudioFileIdentity>& files) const;
    bool projectHasAudioAdjustmentFingerprint() const;
    QStringList audioWatchPaths() const;

    std::string audioProjectionHash_;
    std::map<std::string, std::string> audioSavedProjectionHashes_;
    bool audioAdjustmentNeedsRegeneration_ = false;
    bool audioContentConfirmed_ = false;
    bool audioContentDirty_ = false;
    bool audioApplyPending_ = false;
    project::AudioAdjustmentSettings audioAuthoritySettings_;
    int audioAdjustmentLastProgress_ = -1;
    std::map<std::string, AudioFileIdentity> audioFileCache_;
    std::unique_ptr<AudioAdjustmentJob> audioAdjustmentJob_;
    std::vector<std::unique_ptr<AudioAdjustmentJob>> audioAdjustmentRetired_;
    std::unique_ptr<AudioContentHashJob> audioContentJob_;
    std::vector<std::unique_ptr<AudioContentHashJob>> audioContentRetired_;
    std::optional<AudioAdjustmentResult> audioAdjustmentResult_;
    QVariantMap audioAdjustmentOptions_;
    QString audioAdjustmentError_;
    QTimer audioAdjustmentTimer_;
    QTimer audioWatchFallbackTimer_;
    QFileSystemWatcher audioFileWatcher_;
    // 一度でも監視に載せた path。外れていた path の再登録を判別する。
    QSet<QString> audioWatchedPaths_;
    std::unique_ptr<ShuttleAudioPlayback> audioAdjustmentAudition_;
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
        std::int64_t segmentStart = 0;
        std::int64_t segmentEnd = 0;
        project::ClipEffects effects;
        // 速度が違えば decoder の伸縮が違うので別の source になる。
        std::int64_t speedNum = 1;
        std::int64_t speedDen = 1;
        bool preservePitch = false;
        // クロスフェードの区間。gain の評価が変わるので別の source になる。
        std::optional<project::TransitionEnvelope> fadeIn;
        std::optional<project::TransitionEnvelope> fadeOut;
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
    // 引き継ぎで外した旧 source を、新しい composition の提示を見届けてから engine から削除する。
    void removeRetiredSources(const preview::PreviewStatus& status);
    void pollAudioMeter();
    void advanceTimelinePlayback();
    bool prepareUpcomingPlaybackSources(std::int64_t frame, QString& reason);
    // frame で使う source のうち active でないものを準備する。needsHandOff は active だけでは
    // 足りない (境界で source 集合が変わる) ことを返す。
    bool preparePlaybackSourcesAt(std::int64_t frame, bool& needsHandOff, QString& reason);
    void retirePreparedPlaybackSources();
    void advanceTimelineShuttle();
    // timed shuttle の clock (音声があれば audio clock) から現在の timeline frame を求める。
    bool shuttleFrameFromClock(std::int64_t& frame, QString& error) const;
    QString shuttleStatusText() const;
    void stopScrubAudio();
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
    enum class PlaybackInvalidation { Sources, Mixer };
    bool commitProjectEdit(project::Project candidate, const QString& failurePrefix,
                           PlaybackInvalidation invalidation = PlaybackInvalidation::Sources);
    // キーフレーム編集の確定。変化が無ければ何もせず、確定後は preview を合わせる。
    qint64 effectEditFrame(const project::TimelineClip& clip) const;
    bool removeEffectKeys(const QString& name, const QVariantList& frames);
    bool commitClipKeyCandidate(project::Project candidate);
    // drag 中の effect の一時表示を捨てて Project の値へ戻す。Preview の更新に失敗したら false。
    bool discardEffectPreview(QString& previewError);
    // effect 編集の失敗。commit なら一時表示も捨てる (保存されていない値を表示に残さない)。
    // 常に false を返すので、失敗の return にそのまま使う。
    bool failEffectEdit(bool commit);
    // timeline 編集の共通手順。一時停止 -> candidate へ edit -> commit -> preview 更新。
    bool
    applyTimelineEdit(const std::function<project::TimelineEditResult(project::Project&)>& edit,
                      const std::string& selectedClipId, const QString& successStatus);
    bool resolveTrimEdge(const QString& edge, project::TrimEdge& trimEdge);
    // 再生位置の preview を、選択 (current clip) を変えずに今の Project で作り直す。
    // engine が seek 中などで受けられなければ保留し、受けられるようになったら最新の状態で
    // 1 回だけ行う (drag 中の連続した effect 変更の最後を取りこぼさない)。
    bool refreshPreviewAtPlayhead(QString& error);
    // clip を削除する commit の後始末。preview が削除済み clip を掴んだままにしない。
    // preview を作り直せなかったときはその理由を返す (成功なら空)。
    QString resetAfterClipRemoval();
    // bin 編集を candidate へ適用し、成功したら 1 つの undo として commit する。
    bool
    applyMediaBinEdit(const std::function<project::MediaBinEditResult(project::Project&)>& edit,
                      const QString& successStatus);
    // timeline へ置く素材を bin に登録し、その素材を返す (clip は素材の id を持つ)。
    // 既に同じ file の素材があればそれを返す。失敗したら nullptr (error に理由)。
    // probed を渡すとそれを使い、素材を調べ直さない (画像の decode は重い)。
    // 戻り値は candidate.mediaItems の中を指すので、candidate を変える前に使うこと。
    const project::MediaItem* registerMediaItem(project::Project& candidate,
                                                const std::filesystem::path& mediaPath,
                                                QString& error,
                                                const MediaImportResult* probed = nullptr) const;

    // 置く素材。itemId はプロジェクトパネルの素材 (空なら path を登録する)。
    // probed は判定済みの結果 (あれば bin 登録で調べ直さない)。
    struct DropMedia {
        std::filesystem::path path;
        project::MediaKind kind = project::MediaKind::Video;
        const MediaImportResult* probed = nullptr;
        std::string itemId;
    };

    bool placeMediaAtDropPoint(const std::vector<DropMedia>& media, const QString& trackKind,
                               int trackIndex, qint64 frame);
    // 呼び出し側が確かめたローカルファイルを、判定済みの結果で画像 clip として置く。
    bool placeImageClip(const std::filesystem::path& mediaPath, const QString& fileName,
                        const MediaImportResult& probed);
    // url がローカルに存在するファイルなら path を返す。そうでなければ status を出して空。
    std::filesystem::path localMediaFile(const QUrl& fileUrl, const QString& missingText);
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
    // timer から呼ぶ。serialize と書き込みは worker thread で行い、control thread では
    // Project の複製だけを作る (UI スレッドでファイル I/O を行わない)。
    void writeRecoveryAutosave();
    // waitForCompletion なら書き終えるまで待つ (shutdown の最後の書き込み)。
    void startRecoveryWrite(bool waitForCompletion);
    // recovery の書き込み・削除は、積んだ順に 1 つずつ worker thread で行う。書き込み中に
    // 保存済みの状態へ戻ったら削除を積んで待たずに戻る (書き込みの後に消える)。
    struct RecoveryTask;
    struct RecoveryWriteJob;
    void enqueueRecoveryDelete();
    void pumpRecoveryQueue();
    // 書き込み中・積んだままの recovery の処理を終わらせ、結果を反映する。利用者が明示した
    // 操作 (保存・破棄・切り替え・復元) と shutdown で、recovery file を消す・path を変える・
    // recoveryRevision_ を読み替える前に呼ぶ (後から書き込みが届いて、消した recovery や古い
    // path の recovery を作り直さない)。
    void settleRecoveryWrite();
    void completeRecoveryWrite(const std::shared_ptr<RecoveryWriteJob>& job);
    void applyRecoveryWriteResult(const RecoveryWriteJob& job);
    project::ProjectIoResult runRecoveryTask(const RecoveryTask& task) const;
    void detectRecovery();
    void setCurrentClipSelection(int index);
    // expandLinks なら選んだ clip のリンク相手も選択に含める。
    void setTimelineSelection(const std::vector<std::string>& clipIds, bool expandLinks = true);
    bool refreshPreviewAfterSavedEdit(const std::string& selectedClipId,
                                      const QString& successStatus);
    std::string currentClipId() const;
    bool toggleClipsEnabled(const std::vector<std::string>& clipIds);
    project::ClipEffects currentEffects() const;
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
    // 文字は track 順で静止画 layer として挟む。文字画像を作れなければ nullptr と error。
    std::shared_ptr<preview::CompositionSnapshot>
    previewCompositionFor(const TimelinePreviewFrameMapping& mappedFrame,
                          const std::map<int, TrackPreviewSource>& sources,
                          preview::PreviewFrameRequest& request, QString& error) const;
    // 文字を編集した後、映像のある frame なら engine の composition を組み直す。
    void refreshTextPreview();
    // ドラッグ中の preview を反映する (その clip の画像 cache を捨てて描き直す)。
    void applyTextPreview(const QString& clipId);
    // 文字 clip の画像。revision と clip ID ごとに 1 度だけ描く。
    const QImage* textRasterImage(int clipIndex, QString& error) const;
    // 文字画像の不透明な画素を囲む矩形。clip ID ごとに 1 度だけ求める。
    QRect textRasterBounds(int clipIndex) const;
    // 再生位置で表示される clip か (mute されていない映像 track で、再生位置が範囲内)。
    bool clipVisibleAtPlayhead(int clipIndex) const;
    // 画像・動画の見えている矩形。素材の寸法はプロジェクトパネルの素材から取る。
    project::ClipVisualGeometry visualGeometryOf(int clipIndex) const;
    // preview engine へ渡す静止画。同じ文字には同じ instance を返し、composition の
    // 再送を no-op にする。
    std::shared_ptr<const preview::PreviewStillImage> textStillImage(int clipIndex,
                                                                     QString& error) const;
    // 画像 clip の画素。素材を decode し、出力解像度の raster へ縦横比を保って置いたもの
    // (書き出しと同じ画素)。decode は ImageRasterCache が worker で行う。まだ生成中なら
    // nullptr を返して pending を true にする (error は空)。読めなければ error を入れる。
    std::shared_ptr<const preview::PreviewStillImage> imageStillImage(int clipIndex, QString& error,
                                                                      bool& pending) const;
    // 数式 clip の画素 (出力解像度へ置いて着色したもの)。描画中・失敗中は最後に描けた
    // 画素 (last-good) を返す。どれも無ければ nullptr で pending を true にする (合成から外す)。
    std::shared_ptr<const preview::PreviewStillImage> mathStillImage(int clipIndex,
                                                                     bool& pending) const;
    // preview 中の値を反映した数式 clip の値。
    project::MathClipData effectiveMathData(const project::TimelineClip& clip) const;
    // 現在の数式 clip がすべて描かれるよう要求し、使わなくなった描画を止める。
    void requestMathRenders();
    // <project の directory>/cache/math/<project の file 名>。同じ directory の別の Project と分ける。
    std::filesystem::path mathCacheDirectory() const;
    // cache の場所と権限 (Project lock を持つか) を cache へ伝える。lock か保存先が変わるたびに呼ぶ。
    void syncMathCacheAuthority();
    // 再生中、frame の clip を今の source のまま表示できれば source を引き継いで true。
    // 引き継げなければ何も変更せず false (呼び出し側が一時停止して組み直す)。
    // 引き継げなかったら reason に理由を入れる。
    bool handOffPlaybackSources(std::int64_t frame, QString& reason);
    // applyAudioSourceFor の結果を打ち消す。控えておいた descriptor をそのまま
    // 使い、現在の Project からは作り直さない。戻せなかった場合は黙って成功に
    // せず false を返す。
    bool revertAudioSource(const AudioSwitchUndo& undo, QString& error);
    // clip から audio source descriptor を組む。offset の換算は mapping 側へ委譲する。
    bool audioDescriptorFor(const TimelinePreviewAudioLayerMapping& layer,
                            preview::PreviewSourceDescriptor& descriptor, QString& error);
    void refreshTimelineModel(PlaybackInvalidation invalidation = PlaybackInvalidation::Sources);
    void refreshAudioMixerModel();
    bool refreshScrubAudioMix(int index, double gainDb, double pan);
    // trackKind 文字列を TrackRef へ解決する。失敗時は status を設定して false。
    bool resolveTrackRef(const QString& trackKind, int trackIndex, project::TrackRef& track) const;
    // mute / solo の確定。どちらも preview の layer 構成を変えるので、停止中は現在位置で
    // 組み直す。再生中は次の tick が引き直す。
    bool pauseForTrackOutputEdit();
    bool commitTrackOutputEdit(project::Project candidate, const QString& doneStatus);
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
    // timelineModel_ より後に置き、先に破棄する。
    std::unique_ptr<TimelineClipWindowModel> timelineClipWindow_;
    std::unique_ptr<TextClipFilterModel> textClipModel_;
    std::unique_ptr<TrackModel> videoTrackModel_;
    std::unique_ptr<TrackModel> audioTrackModel_;
    std::unique_ptr<MediaBinModel> mediaBinModel_;
    PreviewEngineRhiItem* previewSurface_ = nullptr;
    std::optional<preview::PreviewSourceId> currentSource_;
    // video track index -> preview source。track を増やしても添字を取り違えない。
    std::map<int, TrackPreviewSource> trackSources_;
    std::vector<AudioPreviewSource> audioSources_;
    std::vector<TrackPreviewSource> preparedVideoSources_;
    std::vector<AudioPreviewSource> preparedAudioSources_;

    // 先読みを要求して engine の準備用の thread が open / seek している source。完了は
    // collectSourcePreparations が受け取り、prepared*Sources_ へ移す。
    struct PendingVideoPreparation {
        preview::PreviewPreparationId id;
        TrackPreviewSource entry; // source は完了するまで未定
        std::int64_t boundary = 0;
        std::uint64_t generation = 0;
    };

    struct PendingAudioPreparation {
        preview::PreviewPreparationId id;
        AudioPreviewSource entry; // source は完了するまで未定
        std::int64_t boundary = 0;
        std::uint64_t generation = 0;
    };

    std::vector<PendingVideoPreparation> pendingVideoPreparations_;
    std::vector<PendingAudioPreparation> pendingAudioPreparations_;
    // 停止・組み直し・engine の作り直し・Project の変更で進める。要求した後にこれが進んだ
    // 準備の完了は使わずに外す (古い Project・古い再生で決めた source を残さない)。
    std::uint64_t playbackPreparationGeneration_ = 0;
    // 世代が進んで取り消した準備。engine が取り消しを終えて登録の枠を返すまで残る。
    // 新しい世代の要求がこの枠のために登録の上限に当たっても、control thread で取り消しの完了を
    // 待たない (decoder の open / seek の途中では取り消しが効かず、待つと止まる)。完了は毎 tick と
    // poll が受け取り、枠が空いた後の tick で要求し直す。
    std::vector<preview::PreviewPreparationId> stalePreparations_;
    std::uint64_t playbackStalePreparationCount_ = 0;
    std::uint64_t playbackPreparationWaitCount_ = 0;
    void collectSourcePreparations();
    // boundary までに完了しなかった準備を待って受け取る (境界でだけ待つ)。
    void waitDueSourcePreparations(std::int64_t frame);
    // 準備の完了を受け取る。使えるなら prepared*Sources_ へ移し、古ければ source を外す。
    void adoptPreparationOutcome(preview::PreviewPreparationId id,
                                 preview::Result<preview::PreviewSourceId> outcome);
    // 準備中のものを取り消して stalePreparations_ へ移し、世代を進める。次の tick は今の
    // Project と再生で準備し直す (古い準備が境界まで残って、新しい要求を塞がない)。
    void cancelSourcePreparations();
    QString playbackPreparationFailure_;
    // 準備に失敗した境界。その境界を越えるまで準備し直さない (壊れた素材の seek 待ちを
    // 毎 tick 繰り返さない)。
    std::optional<std::int64_t> failedPreparationStart_;
    std::uint64_t playbackPreparationFailureCount_ = 0;
    std::size_t playbackMaxPreparedSourceCount_ = 0;
    bool playbackCapacityFailure_ = false;
    std::optional<std::int64_t> pendingCapacityRebuildFrame_;
    // 境界の登録上限が、取り消した準備・削除待ちの旧 source が枠を持っているための一時的な
    // 不足だった。engine を作り直さず (作り直しは取り消した準備の thread を join する)、境界で
    // 止めたまま枠が返るのを poll で待ち、同じ engine で組み直して再生を続ける。
    std::optional<std::int64_t> pendingSlotRebuildFrame_;
    std::uint64_t playbackSlotWaitCount_ = 0;
    QString lastPlaybackRebuildReason_;
    std::uint64_t playbackRebuildCount_ = 0;
    std::uint64_t playbackCapacityResetCount_ = 0;
    double playbackMaxPreparationMs_ = 0.0;
    // 最後に engine が受理した composition。同じ内容を出し直さないために持つ。
    std::shared_ptr<const preview::CompositionSnapshot> submittedComposition_;
    // refreshPreviewAtPlayhead を engine が受けられず保留している。pollPreviewState が行う。
    bool previewRefreshPending_ = false;
    // preview の大きさ変更による再提示を event loop の 1 周にまとめる。
    bool previewResizeRefreshQueued_ = false;
    // preview の描画先の大きさ。最初に決まったときと、大きさの変更を区別する。
    QSize previewBufferSize_;
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
    // preview の frame 問い合わせに使う描画区間。project_ が変わると refreshTimelineModel が捨て、
    // 次の問い合わせで 1 度だけ作り直す (再生中の毎 frame に timeline 全体を組み直さない)。
    mutable std::optional<TimelinePreviewPlan> previewPlan_;
    const TimelinePreviewPlan& previewPlan() const;
    // 選択中の編集点 (outgoing / incoming の clip ID) とトランジション。clip の選択とは排他で、
    // setTimelineSelection が消す。
    std::string selectedEditOutgoing_;
    std::string selectedEditIncoming_;
    std::string selectedTransitionId_;
    // 最後に通知した timelineTransitions。変わったときだけ timelineTransitionsChanged を出す。
    QVariantList shownTransitions_;
    // 最後に通知した selectedTransition。
    QVariantMap shownSelectedTransition_;
    void notifyTimelineTransitions();
    QVariantMap computeSelectedTransition() const;
    std::vector<project::TimelineClip> clipboardClips_;
    // コピー元 Project の bin にあった、clipboardClips_ の素材。
    std::vector<project::MediaItem> clipboardMediaItems_;
    std::int64_t clipboardFpsNum_ = 0;
    std::int64_t clipboardFpsDen_ = 1;
    // anchorId が選択中 (または空) なら選択中の clip、そうでなければ anchor だけを
    // Project の並び順で返す。
    std::vector<project::TimelineClip>
    selectedTimelineClipsInOrder(const std::string& anchorId) const;
    void storeClipboard(std::vector<project::TimelineClip> clips);
    // 貼り付け先の track の決め方。
    //   FindFreeTrack: 希望 track が塞がっていれば空き track を探し、無ければ足す
    //                  (Ctrl+V / Ctrl+D。置き場所を見せていない)。
    //   ExactTrack   : 希望 track にそのまま置き、重なれば拒否する
    //                  (Alt+ドラッグ。ghost で見せた場所と確定を一致させる)。
    enum class CopyPlacement { FindFreeTrack, ExactTrack };
    // clips を destinationFrame / track delta の位置へ新しい id で置き、mediaItems を使って
    // 素材を bin にも登録する。すべて 1 つの undo になる。
    bool placeCopiedClips(const std::vector<project::TimelineClip>& clips,
                          const std::vector<project::MediaItem>& mediaItems,
                          std::int64_t sourceFpsNum, std::int64_t sourceFpsDen,
                          std::int64_t destinationFrame, int videoTrackDelta, int audioTrackDelta,
                          CopyPlacement placement);
    TranscriptionRunner transcriptionRunner_ = transcribe::transcribe;
    std::function<void()> transcriptionHashObserver_;
    SubtitleListModel transcriptionModel_;
    std::vector<project::SubtitleCue> transcriptionCues_;
    std::thread transcriptionThread_;
    std::atomic<bool> transcriptionCancel_{false};
    bool transcribing_ = false;
    int transcriptionProgress_ = 0;
    QString transcriptionError_;
    std::uint64_t transcriptionRevision_ = 0;
    std::uint64_t transcriptionEditSerial_ = 0;
    std::uint64_t transcriptionProjectGeneration_ = 0;
    std::uint64_t projectGeneration_ = 0;
    std::filesystem::path transcriptionProjectPath_;
    SubtitleListModel subtitleModel_;
    std::unique_ptr<TimelineClipWindowModel> subtitleWindow_;
    QString selectedSubtitleId_;
    // timeline 上で選んだ字幕。空でなければ Delete・クリップボード操作の対象は字幕になる。
    // clip・トランジション・編集点を選ぶと空になる (setTimelineSelection)。
    std::vector<std::string> selectedSubtitleIds_;
    // 字幕のクリップボード。開始 frame は先頭の字幕からの相対値で持つ。clip の
    // クリップボードとは別に持ち、最後にコピーした種類をペーストする。
    std::vector<project::SubtitleCue> subtitleClipboard_;
    bool clipboardHoldsSubtitles_ = false;
    std::optional<project::SubtitleStyle> subtitleStylePreview_;
    std::string transcriptionLinkClipId_;
    // 認識した素材と、認識を始めたときの出どころ (実体・size・更新時刻)。適用の直前に照合する。
    QString transcriptionSourcePath_;
    MediaSourceProbe transcriptionSource_;
    bool burnSubtitles_ = true;
    bool commitSubtitleEdit(project::Project candidate);
    bool copySelectedSubtitles(bool cut);
    bool pasteSubtitles(std::int64_t frame);
    bool deleteSelectedSubtitles();
    bool splitSelectedSubtitlesAtPlayhead();
    // 字幕の選択の唯一の入口 (selectedSubtitleIds_ と selectedSubtitleId_ を一緒に決める)。
    void setSubtitleSelection(std::vector<std::string> ids, const std::string& preferredPrimary);
    mutable QString subtitleRasterId_;
    mutable std::shared_ptr<const preview::PreviewStillImage> subtitleRaster_;
    mutable std::shared_ptr<const preview::PreviewMotion> subtitleMotion_;
    std::unique_ptr<QTemporaryDir> textRasterDirectory_;
    // preview の合成 (const) からも埋めるので mutable。Project を変えるたびに捨てる。
    mutable QHash<QString, QImage> textRasterImages_;
    mutable QHash<QString, std::shared_ptr<const preview::PreviewStillImage>> textStillImages_;
    // 画像 clip の preview 用 raster。decode が重いので Project の変更では捨てず、
    // 現在の画像 clip と出力解像度が使わない key だけを refreshTimelineModel で捨てる。
    std::unique_ptr<ImageRasterCache> imageRasters_;
    // 数式 clip の描画結果 (key 単位、<project>/cache/math)。
    std::unique_ptr<MathRasterCache> mathRasters_;
    // clip ごとの最後に描けた mask。式を直して描き直している間・失敗した間はこれを出す。
    // 派生物なので Project には入れず、session の間だけ持つ。
    struct MathLastGood {
        QString key;
        std::shared_ptr<const media::StillImage> mask;
    };
    mutable QHash<QString, MathLastGood> mathLastGood_;
    // clip ごとの合成済みの画素。同じ見た目なら同じ instance を engine へ渡す。
    struct MathComposed {
        QString memo;
        std::shared_ptr<const preview::PreviewStillImage> image;
    };
    mutable QHash<QString, MathComposed> mathStillImages_;
    // 入力中の数式 (clip ID と、Project へまだ保存していない値)。
    std::optional<std::pair<std::string, project::MathClipData>> mathPreviewOverride_;
    mutable QHash<QString, QRect> textRasterBounds_;
    QHash<QString, QUrl> textRasterUrls_;
    QString textOverlayClipId_;
    // ドラッグ中の文字書式 (clip ID と、Project へまだ保存していない値)。
    std::optional<std::pair<std::string, project::TextClipData>> textPreviewOverride_;
    int textPreviewSerial_ = 0;
    // engine が Seeking の間に来た文字 preview の描き直し要求。
    bool textPreviewRefreshPending_ = false;

    struct UndoEntry {
        project::Project project;
        std::vector<std::string> selectedClipIds;
        std::string currentClipId;
        std::int64_t playheadFrame = 0;
        std::uint64_t revision = 0;
        // project の approximateProjectBytes。履歴へ積むときに埋める。
        std::size_t bytes = 0;
        QString selectedSubtitleId{};
        // timeline で選んだ字幕 (複数)。clip の選択と同じく、戻した Project に残るものだけを戻す。
        std::vector<std::string> selectedSubtitleIds{};
    };

    // Undo / Redo 履歴は、両方の合計の件数と Project の複製の概算 byte 数で上限を決める。
    // clip・素材・キーフレームの多い Project では、件数だけだと 1 世代ごとの大きさに比例して
    // memory が増える。新しい編集・Undo・Redo のたびに切り詰め、現在の状態に隣り合う Undo と
    // Redo の 1 件ずつは予算を超えても残す (project::editHistoryEntriesToDrop)。
    static constexpr std::size_t kMaximumUndoEntries = 100;
    static constexpr std::size_t kMaximumUndoBytes = 256 * 1024 * 1024;
    std::size_t editHistoryByteBudget_ = kMaximumUndoBytes;
    std::vector<UndoEntry> undoHistory_;
    // undo で戻した編集。新しい編集を commit すると捨てる。
    std::vector<UndoEntry> redoHistory_;
    void pushUndoEntry(UndoEntry entry);
    void trimEditHistory();
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
    // 処理中の recovery の書き込み・削除 (同時に 1 つだけ)。完了はこの job と同じときだけ反映する。
    std::shared_ptr<RecoveryWriteJob> recoveryWrite_;
    // 処理を待っている書き込み・削除 (積んだ順に行う)。
    std::deque<std::shared_ptr<RecoveryTask>> recoveryQueue_;
    std::uint64_t recoveryTaskSequence_ = 0;
    // 最後に積んだ削除の番号。これより前に積んだ書き込みの完了は recovery 済みにしない。
    std::uint64_t recoveryDeleteSequence_ = 0;
    RecoveryWriter recoveryWriter_;
    RecoveryThreadFactory recoveryThreadFactory_;
    // 書き込み中に次の自動保存の時刻が来た。完了した後にもう一度書く。
    bool recoveryWriteAgain_ = false;
    std::uint64_t recoveryWriteCompletionCount_ = 0;
    std::int64_t playheadFrame_ = 0;
    std::int64_t totalTimelineFrames_ = 0;
    bool audioMeterClipped_ = false;
    double audioMeterDbLeft_ = kMeterSilenceDb;
    double audioMeterDbRight_ = kMeterSilenceDb;
    std::vector<std::shared_ptr<audio::AudioMixerBus>> audioMixerBuses_;
    std::vector<std::pair<float, float>> audioMixerPeaks_;
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
    std::vector<project::ClipKeyframe> effectKeyClipboard_;
    mutable project::ClipEffects keyframeDisplayEffects_;
    mutable int keyframeDisplayClip_ = -1;
    mutable std::map<std::string, QVariantList> keyframeDisplayKeys_;
    bool shutdownStarted_ = false;
    bool playing_ = false;
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
    // scrub 中の音声。鳴らす clip が無いか開始に失敗したら null (映像の scrub は続ける)。
    std::unique_ptr<ScrubAudioPlayback> scrubAudio_;
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
