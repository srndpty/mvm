#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "app/audio_source_set_transaction.h"
#include "app/equation_sequence_authoring.h"
#include "app/graph_render_compile.h"
#include "app/manim_clip_workflow.h"
#include "app/math_clip_render.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "core/checked_integer.h"
#include "core/checked_output_timebase.h"
#include "core/timecode.h"
#include "equation_sequence_editor.h"
#include "image_raster_cache.h"
#include "media/graph_manim/graph_manim_backend.h"
#include "media/manim/manim_math_tex.h"
#include "media_file_filters.h"
#include "media_import.h"
#include "preview_engine/preview_engine_internal.h"
#include "project/clip_effects.h"
#include "project/equation_sequence_edit.h"
#include "project/graph_edit.h"
#include "project/subtitles.h"
#include "project/timeline_edit.h"
#include "scrub_audio_playback.h"
#include "shuttle_audio_mix.h"
#include "shuttle_audio_playback.h"
#include "timeline_clip_model.h"
#include "track_model.h"
#include "util/mvm_reveal_in_explorer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantMap>


namespace mvm::app {
using detail::fromPath;
using detail::indexOfClipId;
using detail::linearToDb;
using detail::newClipId;
using detail::previewErrorText;
using detail::probeMedia;
using detail::ProbedMedia;
namespace {
class SubtitlePreviewMotion final : public preview::PreviewMotion {
public:
    explicit SubtitlePreviewMotion(project::SubtitleCue cue) : cue_(std::move(cue)) {}

    preview::PreviewMotionValue evaluate(std::int64_t frame) const override {
        preview::PreviewMotionValue value;
        value.opacity = project::subtitleContainsFrame(cue_, frame) ? 1.0F : 0.0F;
        return value;
    }

private:
    project::SubtitleCue cue_;
};

// 同じ source・静止画を同じ順に重ねているか (不透明度や位置などの値は問わない)。
bool sameLayerSources(const preview::CompositionSnapshot& a,
                      const preview::CompositionSnapshot& b) {
    return std::equal(a.layers.begin(), a.layers.end(), b.layers.begin(), b.layers.end(),
                      [](const auto& x, const auto& y) {
                          return x.source == y.source && x.stillImage == y.stillImage;
                      });
}


bool revealFileInExplorer(const std::filesystem::path& path, QString& error) {
    char detail[512] = {};
    if (mvm_reveal_in_explorer(path.c_str(), detail, sizeof(detail)) == 0)
        return true;
    error = QString::fromUtf8(detail);
    return false;
}

QString previewStateText(preview::PreviewEngineState state) {
    switch (state) {
    case preview::PreviewEngineState::Uninitialized:
        return QStringLiteral("未初期化");
    case preview::PreviewEngineState::WaitingForRenderDevice:
        return QStringLiteral("render device待機中");
    case preview::PreviewEngineState::ReadyPaused:
        return QStringLiteral("停止・準備完了");
    case preview::PreviewEngineState::Playing:
        return QStringLiteral("再生中");
    case preview::PreviewEngineState::Seeking:
        return QStringLiteral("seek中");
    case preview::PreviewEngineState::ShuttingDown:
        return QStringLiteral("終了処理中");
    case preview::PreviewEngineState::Shutdown:
        return QStringLiteral("終了済み");
    case preview::PreviewEngineState::Error:
        return QStringLiteral("エラー");
    }
    return QStringLiteral("不明");
}

class QtEventDispatcher final : public preview::PreviewEventDispatcher {
public:
    explicit QtEventDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> callback) override {
        if (!context_)
            return false;
        return QMetaObject::invokeMethod(
            context_, [callback = std::move(callback)] { callback(); }, Qt::QueuedConnection);
    }

private:
    QPointer<QObject> context_;
};

// timeline 上の Manim clip の位置。M4 は Manim clip を 1 本しか扱わない。
int indexOfManimClip(const std::vector<project::TimelineClip>& clips) {
    for (std::size_t index = 0; index < clips.size(); ++index) {
        if (clips[index].kind == project::TimelineClipKind::Manim)
            return static_cast<int>(index);
    }
    return -1;
}

// timeline 上で最も上の video track に載っている有効な clip。inspector の対象と再生開始の
// clip を決める。無効にした clip は映らないので選ばない。
const project::TimelineClip* topVideoClipAt(const project::Project& project,
                                            std::int64_t timelineFrame) {
    const auto active = project::activeClipsAt(project, project::TrackKind::Video, timelineFrame);
    for (auto entry = active.rbegin(); entry != active.rend(); ++entry)
        if (*entry && (*entry)->enabled)
            return *entry;
    return nullptr;
}


} // namespace

MvmController::MvmController(std::filesystem::path projectPath,
                             std::filesystem::path manimExecutablePath, project::Project project,
                             QObject* parent, ExportRunner exportRunner,
                             ExportThreadFactory exportThreadFactory, FileRevealer fileRevealer)
    : QObject(parent), projectPath_(std::move(projectPath)),
      manimExecutablePath_(std::move(manimExecutablePath)), project_(std::move(project)),
      previewEngine_(std::make_shared<preview::PreviewEngine>()),
      dispatcher_(std::make_shared<QtEventDispatcher>(this)),
      timelineModel_(std::make_unique<TimelineClipModel>()),
      timelineClipWindow_(std::make_unique<TimelineClipWindowModel>()),
      textClipModel_(std::make_unique<TextClipFilterModel>()),
      videoTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Video)),
      audioTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Audio)),
      mediaBinModel_(std::make_unique<MediaBinModel>()),
      exportRunner_(exportRunner ? std::move(exportRunner) : mvm::app::exportTimeline),
      exportThreadFactory_(
          exportThreadFactory
              ? std::move(exportThreadFactory)
              : [](std::function<void()> task) { return std::thread(std::move(task)); }),
      fileRevealer_(fileRevealer ? std::move(fileRevealer) : revealFileInExplorer) {
    timelineClipWindow_->setSourceModel(timelineModel_.get());
    subtitleWindow_ = std::make_unique<TimelineClipWindowModel>();
    subtitleWindow_->setRoles(SubtitleListModel::CueId, SubtitleListModel::StartFrame,
                              SubtitleListModel::EndFrame, true);
    subtitleWindow_->setSourceModel(&subtitleModel_);
    textClipModel_->setSourceModel(timelineModel_.get());
    // preview の文字 layer は再生位置に掛かる文字 clip だけを作る。playheadFrame は
    // stateChanged で通知する。
    textClipModel_->setPlayheadFrame(playheadFrame_);
    connect(this, &MvmController::stateChanged, this,
            [this] { textClipModel_->setPlayheadFrame(playheadFrame_); });
    imageRasters_ = std::make_unique<ImageRasterCache>();
    // 画像の raster ができた (または素材が変わった) ら preview を組み直す。再生中は
    // 毎 tick composition を組み直すので、そこで拾われる。
    connect(imageRasters_.get(), &ImageRasterCache::entryChanged, this, [this] {
        if (!playing_)
            refreshTextPreview();
    });
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    QString lockError;
    void* acquiredLock = nullptr;
    const bool locked = acquireProjectLock(projectPath_, acquiredLock, lockError);
    if (locked)
        adoptProjectLock(acquiredLock, projectPath_);

    // 数式の backend は Manim (MathTex)。確認 (preflight) は worker で行い、使えるかは
    // 起動を待たずに後から分かる。Project には backend を保存しない (docs/math-clips.md)。
    // cache の変更と外部 renderer の起動は Project lock を取った後でだけ許可する
    // (syncMathCacheAuthority)。lock を取れない instance は数式の作業を何も始めない。
    mathRasters_ = std::make_unique<MathRasterCache>(
        sessionId_,
        [manimExecutable = manimExecutablePath_](const std::filesystem::path& workDirectory,
                                                 const std::atomic<bool>* cancel) {
            return mvm::manim::preflightManimMathTex({manimExecutable, workDirectory}, cancel);
        });
    connect(mathRasters_.get(), &MathRasterCache::entryChanged, this, [this](const QString& key) {
        if (shutdownStarted_)
            return;
        // 数式 clip が無ければ描き直すものは無い。起動時の backend の確認が終わった時点など、
        // 利用者の操作と無関係な時刻に preview を組み直さない (フレーム送りの途中に割り込む)。
        if (std::none_of(project_.timelineClips.begin(), project_.timelineClips.end(),
                         [](const auto& clip) {
                             return clip.kind == project::TimelineClipKind::Math ||
                                    clip.kind == project::TimelineClipKind::EquationSequence;
                         }))
            return;
        // backend の状態が変わった (key が計算できるようになった) ら、すべての数式を要求し直す。
        if (key.isEmpty())
            requestMathRenders();
        if (!playing_)
            refreshTextPreview();
        // 変形の描画・memory の状態は選択中のトランジションの inspector が示す。
        notifyTimelineTransitions();
        Q_EMIT stateChanged();
    });
    graphRasters_ = std::make_unique<GraphPreviewCache>(
        [python = manimExecutablePath_.parent_path() / L"python.exe"](
            const std::filesystem::path& work, const std::atomic<bool>* cancel) {
            return graph_manim::preflightGraph(python, MVM_GRAPH_BACKEND_SCRIPT, work, cancel);
        });
    connect(graphRasters_.get(), &GraphPreviewCache::changed, this, [this] {
        if (shutdownStarted_)
            return;
        requestMathRenders();
        // 可視性は既存 mapping だけに従う。無関係な Graph の通知で初期 seek を増やさない。
        const auto mapped = mapTimelinePreviewFrame(project_, previewPlan(), playheadFrame_);
        if (!mapped.success || std::none_of(mapped.stillLayers.begin(), mapped.stillLayers.end(),
                                            [](const auto& layer) {
                                                return layer.kind ==
                                                       project::TimelineClipKind::Graph;
                                            }))
            return;
        bool contributes = false;
        for (const auto& still : mapped.stillLayers) {
            if (still.kind != project::TimelineClipKind::Graph)
                continue;
            const auto record = graphAnimations_.constFind(QString::fromStdString(still.clipId));
            if (record == graphAnimations_.constEnd() || !submittedComposition_ ||
                std::none_of(
                    submittedComposition_->layers.begin(), submittedComposition_->layers.end(),
                    [&](const auto& layer) { return layer.stillAnimation == record->animation; }))
                continue;
            contributes = true;
        }
        if (!contributes)
            return;
        // 再生中の resident 更新は slot だけで届く。停止中は一度提示を要求する。
        if (!playing_)
            refreshTextPreview();
        Q_EMIT stateChanged();
    });
    syncMathCacheAuthority();
    equationEditor_ = std::make_unique<EquationSequenceEditor>(*this);
    QQmlEngine::setObjectOwnership(equationEditor_.get(), QQmlEngine::CppOwnership);

    recoveryDebounceTimer_.setSingleShot(true);
    recoveryDebounceTimer_.setInterval(2000);
    recoveryMaximumTimer_.setSingleShot(true);
    recoveryMaximumTimer_.setInterval(30000);
    connect(&recoveryDebounceTimer_, &QTimer::timeout, this, &MvmController::writeRecoveryAutosave);
    connect(&recoveryMaximumTimer_, &QTimer::timeout, this, &MvmController::writeRecoveryAutosave);

    refreshTimelineModel();
    initializePreviewEngine(QStringLiteral("Preview初期化に失敗しました: "));
    savedProject_ = project_;
    if (!projectLockHeld_) {
        statusText_ = QStringLiteral("このProjectは他のプロセスが編集中です: ") + lockError;
    } else {
        if (!rememberCanonicalBase())
            statusText_ = QStringLiteral("Project fileの基準hashを記録できません");
        detectRecovery();
    }
    if (projectLockHeld_ && !recoveryAvailable() && !recoveryCorrupt_ && !recoveryForeign_) {
        const project::Project beforeRestore = project_;
        restoreFirstManimClip();
        if (project_ != beforeRestore && currentRevision_ == savedRevision_) {
            currentRevision_ = nextRevision_++;
            scheduleRecoveryAutosave();
        }
        // 起動時のasset同期は利用者の編集ではないため、Undo履歴へ残さない。
        clearEditHistory();
    }

    playbackTimer_.setInterval(16);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this, &MvmController::advanceTimelinePlayback);
    shuttleTimer_.setInterval(40);
    shuttleTimer_.setTimerType(Qt::PreciseTimer);
    connect(&shuttleTimer_, &QTimer::timeout, this, &MvmController::advanceTimelineShuttle);

    stateTimer_.setInterval(100);
    connect(&stateTimer_, &QTimer::timeout, this, &MvmController::pollPreviewState);
    stateTimer_.start();

    // scrub は drag の 1 移動ごとに seek せず、最新位置だけを一定間隔で処理する。
    // seek は engine の Seeking state を挟むため、coalesce しないと詰まる。
    scrubTimer_.setInterval(40);
    connect(&scrubTimer_, &QTimer::timeout, this, [this] {
        if (scrubAudio_ && !scrubAudio_->error().empty()) {
            // 映像の scrub は続け、音声だけ止めて理由を出す。
            const QString error = QString::fromStdString(scrubAudio_->error());
            stopScrubAudio();
            setStatus(QStringLiteral("scrub音声を再生できません: ") + error);
        }
        if (!scrubPending_) {
            // drag 終了後は、最後の位置を反映し終えてから timer を止める。
            if (!scrubbing_)
                scrubTimer_.stop();
            return;
        }
        // seek が Seeking 中で弾かれた場合は pending のままにする。
        // ここで落とすと、drag が止まった位置の preview が更新されないまま残る。
        if (seekTimelineFrame(scrubTargetFrame_)) {
            scrubPending_ = false;
        } else if (previewEngine_->status().state == preview::PreviewEngineState::Error) {
            // 復旧不能な error を Seeking と同じ一時状態として再試行すると、
            // 同じ通知を 40 ms ごとに出し続けて本来の原因まで上書きしてしまう。
            scrubPending_ = false;
            scrubbing_ = false;
            scrubTimer_.stop();
        }
    });

    meterTimer_.setInterval(50);
    connect(&meterTimer_, &QTimer::timeout, this, &MvmController::pollAudioMeter);
    meterTimer_.start();

    // slip preview も scrub と同じく最新の位置だけを反映し、Seeking 中は次の tick で再試行する。
    slipPreviewTimer_.setInterval(16);
    connect(&slipPreviewTimer_, &QTimer::timeout, this, &MvmController::applySlipPreview);
    connect(&audioAdjustmentTimer_, &QTimer::timeout, this, &MvmController::pollAudioAdjustment);
    connect(&audioWatchFallbackTimer_, &QTimer::timeout, this,
            &MvmController::checkAudioWatchFallback);
    connectAudioFileWatch();
    refreshAudioInputAuthority(false);
}

MvmController::~MvmController() {
    cancelAudioAdjustment();
    shutdown();
}

void MvmController::attachPreview(PreviewEngineRhiItem* surface) {
    if (previewSurface_)
        disconnect(previewSurface_, nullptr, this, nullptr);
    previewSurface_ = surface;
    if (!previewSurface_)
        return;
    previewSurface_->setEngine(previewEngine_);
    // 大きさが変わると (window の最大化・解除など) preview の描画先が黒で作り直される。
    // 停止中は次の提示が来ないので、再生位置の frame を提示し直す。大きさの変更は
    // ドラッグ中に連続するので、event loop の 1 周にまとめる。再生中は毎 frame 描くので不要。
    // 最初の大きさが決まったとき (空 → 初期の大きさ) は、初回の seek が提示するので何もしない。
    // ここで提示し直すと、起動直後に同じ frame を 2 回提示する。
    previewBufferSize_ = previewSurface_->effectiveColorBufferSize();
    connect(previewSurface_, &QQuickRhiItem::effectiveColorBufferSizeChanged, this, [this] {
        const QSize size = previewSurface_->effectiveColorBufferSize();
        const bool resized = !previewBufferSize_.isEmpty() && size != previewBufferSize_;
        previewBufferSize_ = size;
        if (!resized || previewResizeRefreshQueued_)
            return;
        previewResizeRefreshQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            previewResizeRefreshQueued_ = false;
            if (playing_)
                return;
            QString error;
            if (!refreshPreviewAtPlayhead(error))
                setStatus(QStringLiteral("Previewを再描画できません: ") + error);
        });
    });
}

QString MvmController::projectPath() const {
    return fromPath(projectPath_);
}

QString MvmController::recoveryProjectPath() const {
    return fromPath(recoveryPath());
}

bool MvmController::frameRateMeasured() const {
    // engine の envelope 一致判定だけを authority にする。
    // Project 側の rate 表へ fallback すると、engine の initialize が失敗していても
    // 「60fps だから測定済み」と答えてしまい、false positive になる。
    // 構成が未確定の engine では matchesMeasuredEnvelope() が false を返す。
    return previewEngine_ && previewEngine_->capabilities().matchesMeasuredEnvelope();
}

bool MvmController::hasManimTimelineClip() const {
    return indexOfManimClip(project_.timelineClips) >= 0;
}

void MvmController::setStatus(QString status) {
    statusText_ = std::move(status);
    Q_EMIT stateChanged();
}

bool MvmController::initializePreviewEngine(const QString& failurePrefix) {
    preview::PreviewEngineConfig config;
    config.output.frameRate = {static_cast<std::uint32_t>(project_.timelineFpsNum),
                               static_cast<std::uint32_t>(project_.timelineFpsDen)};
    const auto initialized = previewEngine_->initialize(config, dispatcher_);
    if (!initialized) {
        statusText_ = failurePrefix + previewErrorText(initialized.error());
        return false;
    }
    const auto volume = previewEngine_->setMasterVolume(static_cast<float>(masterVolume_));
    if (!volume) {
        statusText_ = failurePrefix + previewErrorText(volume.error());
        return false;
    }
    return true;
}

void MvmController::clearMasterAudioClip() {
    previewEngine_->clearAudioMeterClip();
    if (shuttleAudio_)
        shuttleAudio_->clearMeterClip();
    if (scrubAudio_)
        scrubAudio_->clearMeterClip();
    audioMeterClipped_ = false;
    Q_EMIT meterChanged();
}

void MvmController::setMasterVolume(double volume) {
    if (!std::isfinite(volume)) {
        setStatus(QStringLiteral("マスター音量が不正です"));
        return;
    }
    const double clamped = std::clamp(volume, 0.0, static_cast<double>(audio::kMaximumMasterGain));
    if (std::abs(masterVolume_ - clamped) < 0.0001)
        return;
    if (shuttleAudio_) {
        std::string error;
        if (!shuttleAudio_->setVolume(static_cast<float>(clamped), error)) {
            setStatus(QStringLiteral("シャトル音声のボリュームを変更できません: ") +
                      QString::fromStdString(error));
            return;
        }
    }
    const auto changed = previewEngine_->setMasterVolume(static_cast<float>(clamped));
    if (changed && scrubAudio_) {
        std::string error;
        if (!scrubAudio_->setVolume(static_cast<float>(clamped), error)) {
            previewEngine_->setMasterVolume(static_cast<float>(masterVolume_));
            setStatus(QStringLiteral("スクラブ音声のボリュームを変更できません: ") +
                      QString::fromStdString(error));
            return;
        }
    }
    if (!changed) {
        if (shuttleAudio_) {
            std::string ignored;
            shuttleAudio_->setVolume(static_cast<float>(masterVolume_), ignored);
        }
        setStatus(QStringLiteral("マスターボリュームを変更できません: ") +
                  previewErrorText(changed.error()));
        return;
    }
    masterVolume_ = clamped;
    Q_EMIT stateChanged();
}

bool MvmController::resetPreviewEngine() {
    auto replacement = std::make_shared<preview::PreviewEngine>();
    const auto previous = previewEngine_;
    previewEngine_ = replacement;
    if (!initializePreviewEngine(QStringLiteral("Preview再初期化に失敗しました: "))) {
        replacement->requestShutdown();
        previewEngine_ = previous;
        Q_EMIT stateChanged();
        return false;
    }
    const auto shutdown = previous->requestShutdown();
    if (!shutdown) {
        replacement->requestShutdown();
        previewEngine_ = previous;
        setStatus(QStringLiteral("Preview再初期化前の終了に失敗しました: ") +
                  previewErrorText(shutdown.error()));
        return false;
    }

    currentSource_.reset();
    trackSources_.clear();
    audioSources_.clear();
    preparedVideoSources_.clear();
    preparedAudioSources_.clear();
    // 準備は古い engine の requestShutdown が取り消して捨てた。新しい engine には無い。
    pendingVideoPreparations_.clear();
    pendingAudioPreparations_.clear();
    stalePreparations_.clear();
    pendingSlotRebuildFrame_.reset();
    ++playbackPreparationGeneration_;
    playbackPreparationFailure_.clear();
    submittedComposition_.reset();
    retiredSources_.clear();
    previewReady_ = false;
    if (previewSurface_)
        previewSurface_->setEngine(previewEngine_);
    Q_EMIT stateChanged();
    return true;
}

void MvmController::syncFirstManimAsset() {
    if (project_.manimAssets.empty()) {
        manimScriptPath_.clear();
        manimSceneName_.clear();
        manimStateText_.clear();
        return;
    }
    const project::ManimAsset& asset = project_.manimAssets.front();
    manimScriptPath_ = fromPath(asset.scriptPath);
    manimSceneName_ = QString::fromStdString(asset.sceneName);
    manimStateText_ = QString::fromLatin1(project::manimGenerationStateName(asset.generationState));
}

void MvmController::restoreFirstManimClip() {
    const ManimClipRestoreResult restored = mvm::app::restoreFirstManimClip(project_, projectPath_);
    syncFirstManimAsset();
    if (!restored.hasAsset)
        return;

    if (restored.generatedVideoAvailable) {
        const int manimIndex = indexOfManimClip(project_.timelineClips);
        if (manimIndex < 0) {
            statusText_ = restored.success
                              ? QStringLiteral("生成済みManim assetをtimelineへ追加できます")
                              : QString::fromStdString(restored.error);
            return;
        }
        if (!syncManimTimelineClip(false))
            return;
        const project::ManimAsset& asset = project_.manimAssets.front();
        const QString clipName = QString::fromStdString(asset.sceneName) + QStringLiteral(" — ") +
                                 QFileInfo(fromPath(asset.generatedVideoPath)).fileName();
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
        if (project::sourceRateMatchesTimelineRate(project_, clip))
            queueVideoClipInstall(asset.generatedVideoPath, clipName, manimIndex,
                                  clip.sourceInFrame);
        statusText_ = restored.success ? QStringLiteral("保存済みManim clipを復元しました")
                                       : QString::fromStdString(restored.error);
    } else {
        statusText_ = restored.success
                          ? QStringLiteral("生成済みvideoがありません。Regenerateしてください")
                          : QString::fromStdString(restored.error);
    }
}

void MvmController::queueVideoClipInstall(const std::filesystem::path& videoPath, QString clipName,
                                          int clipIndex, std::int64_t sourceFrame) {
    pendingVideoPath_ = videoPath;
    pendingClipName_ = std::move(clipName);
    pendingClipIndex_ = clipIndex;
    pendingSourceFrame_ = sourceFrame;
}

QStringList MvmController::clipNames() const {
    QStringList names;
    names.reserve(static_cast<qsizetype>(project_.timelineClips.size()));
    for (const auto& clip : project_.timelineClips)
        names.append(QString::fromStdString(clip.name));
    return names;
}

TimelineClipModel* MvmController::timelineModel() const {
    return timelineModel_.get();
}

QAbstractItemModel* MvmController::videoTrackModel() const {
    return videoTrackModel_.get();
}

QAbstractItemModel* MvmController::audioTrackModel() const {
    return audioTrackModel_.get();
}

MediaBinModel* MvmController::mediaBinModel() const {
    return mediaBinModel_.get();
}

QString MvmController::timelineFpsText() const {
    if (project_.timelineFpsDen == 1)
        return QString::number(project_.timelineFpsNum) + QStringLiteral(" fps");
    const double value =
        static_cast<double>(project_.timelineFpsNum) / static_cast<double>(project_.timelineFpsDen);
    return QString::number(value, 'f', 2) + QStringLiteral(" fps");
}

QStringList MvmController::mediaFileNameFilters() const {
    return app::mediaFileNameFilters();
}

QVariantList MvmController::supportedFrameRates() const {
    QVariantList rates;
    for (const auto& rate : project::configurableTimelineFrameRates()) {
        QVariantMap entry;
        entry[QStringLiteral("num")] = static_cast<int>(rate.numerator);
        entry[QStringLiteral("den")] = static_cast<int>(rate.denominator);
        const double value =
            static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator);
        entry[QStringLiteral("label")] = rate.denominator == 1 ? QString::number(rate.numerator)
                                                               : QString::number(value, 'f', 2);
        rates.append(entry);
    }
    return rates;
}

QString MvmController::currentTimeText() const {
    return frameTimecode(playheadFrame_);
}

QString MvmController::frameTimecode(qint64 frame) const {
    return QString::fromStdString(
        core::formatTimecode(frame, project_.timelineFpsNum, project_.timelineFpsDen));
}

bool MvmController::canPlay() const {
    return timelineCanPlay(project_, busy_, playing_, playheadFrame_, totalTimelineFrames_);
}

const TimelinePreviewPlan& MvmController::previewPlan() const {
    if (!previewPlan_)
        previewPlan_ = buildTimelinePreviewPlan(project_);
    return *previewPlan_;
}

void MvmController::refreshTimelineModel(PlaybackInvalidation invalidation) {
    // project_ が替わった直後の current clip の index は、前の clip 列を指したままのことがある
    // (呼び出し側が後で付け直す)。下の model の通知で QML が keyframeChannels などを同期的に
    // 読むので、通知より前に範囲外の index を無効にする。最後の clip を削除した直後に、空の
    // clip 列を index 0 で読んでいた (debug の範囲検査で停止)。
    if (currentClipIndex_ >= static_cast<int>(project_.timelineClips.size())) {
        currentClipIndex_ = -1;
        currentClipName_.clear();
        currentClipPath_.clear();
    }
    refreshAudioMixerModel();
    if (invalidation == PlaybackInvalidation::Mixer)
        return;
    previewPlan_.reset();
    // 先読みの状態 (準備中・準備済みの source、失敗した境界) は変わる前の Project で決めた。
    // 再生中の track の表示・M/S の切り替えは再生を止めないので、ここでまとめて無効にする。
    // 準備中のものは取り消し、準備済みの source は登録枠を返し、失敗した境界も忘れる。次の
    // tick が変わった後の Project で準備し直す (再生中の source はそのまま使う)。
    retirePreparedPlaybackSources();
    if (!selectedTransitionId_.empty() &&
        std::none_of(
            project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
            [&](const auto& transition) { return transition.id == selectedTransitionId_; }))
        selectedTransitionId_.clear();
    if (!selectedEditOutgoing_.empty() &&
        project::touchingClipId(project_, selectedEditOutgoing_, project::TrimEdge::Right) !=
            selectedEditIncoming_) {
        selectedEditOutgoing_.clear();
        selectedEditIncoming_.clear();
    }
    notifyTimelineTransitions();
    textPreviewOverride_.reset();
    subtitleRaster_.reset();
    subtitleMotion_.reset();
    subtitleRasterId_.clear();
    {
        // 編集・Undo で消えた字幕を選択から外す。主選択もこの規則で揃える。
        auto kept = selectedSubtitleIds_;
        std::erase_if(kept, [&](const std::string& id) {
            return !project_.subtitles ||
                   std::none_of(project_.subtitles->cues.begin(), project_.subtitles->cues.end(),
                                [&](const auto& cue) { return cue.id == id; });
        });
        setSubtitleSelection(std::move(kept), selectedSubtitleId_.toStdString());
    }
    subtitleStylePreview_.reset();
    subtitleModel_.setCues(project_.subtitles ? project_.subtitles->cues
                                              : std::vector<project::SubtitleCue>{});
    textRasterImages_.clear();
    textStillImages_.clear();
    // 画像の raster は、現在の画像 clip の素材と現在の出力解像度の組だけを残す。
    // 出力解像度を変えたら旧解像度の raster は捨てる。
    if (imageRasters_) {
        QSet<QString> keep;
        for (const auto& clip : project_.timelineClips)
            if (clip.kind == project::TimelineClipKind::Image)
                keep.insert(ImageRasterCache::keyFor(clip.mediaPath, project_.outputWidth,
                                                     project_.outputHeight));
        imageRasters_->retainOnly(keep);
    }
    // 数式: 入力中の preview は文字と同じく Project の変更で捨てる。消えた clip の
    // last-good と合成済みの画素を捨て、現在の数式をすべて要求する。
    mathPreviewOverride_.reset();
    {
        QSet<QString> mathClipIds;
        for (const auto& clip : project_.timelineClips)
            if (clip.kind == project::TimelineClipKind::Math)
                mathClipIds.insert(QString::fromStdString(clip.id));
        mathLastGood_.removeIf([&](const auto& item) { return !mathClipIds.contains(item.key()); });
        mathStillImages_.removeIf(
            [&](const auto& item) { return !mathClipIds.contains(item.key()); });
        mathPreviewAnimations_.removeIf(
            [&](const auto& item) { return !mathClipIds.contains(item.key()); });
        // Equation Sequence: 消えた clip の compile と animation を捨てる。静止の被覆は今の
        // Project で要る分を次の合成で作り直す。
        QSet<QString> sequenceIds;
        for (const auto& clip : project_.timelineClips)
            if (clip.kind == project::TimelineClipKind::EquationSequence)
                sequenceIds.insert(QString::fromStdString(clip.id));
        equationCompiles_.removeIf(
            [&](const auto& item) { return !sequenceIds.contains(item.key()); });
        equationPreviewAnimations_.removeIf(
            [&](const auto& item) { return !sequenceIds.contains(item.key()); });
        equationStaticCoverage_.clear();
    }
    requestMathRenders();
    textRasterBounds_.clear();
    textRasterUrls_.clear();
    textRasterDirectory_.reset();
    if (timelineModel_) {
        timelineModel_->setProject(project_);
        QSet<QString> selectedIds;
        for (const auto& id : selectedClipIds_)
            selectedIds.insert(QString::fromStdString(id));
        timelineModel_->setSelectedClipIds(selectedIds);
    }
    if (videoTrackModel_)
        videoTrackModel_->setProject(project_);
    if (mediaBinModel_)
        mediaBinModel_->setProject(project_);
    const auto timeline = project::validateTimeline(project_);
    totalTimelineFrames_ = timeline.success ? timeline.totalFrames : 0;
    if (navigationTimelineFrames() == 0)
        playheadFrame_ = 0;
    else
        playheadFrame_ =
            std::clamp<std::int64_t>(playheadFrame_, 0, navigationTimelineFrames() - 1);
}

void MvmController::refreshAudioMixerModel() {
    // 削除で index が移動したら、別トラックのピーク・clip latch を引き継がない。
    // 末尾への追加では既存 index が変わらないため、再生中の source と同じバスを使い続ける。
    if (audioMixerBuses_.size() > project_.audioTracks.size()) {
        audioMixerBuses_.clear();
        audioMixerPeaks_.clear();
    }
    audioMixerBuses_.resize(project_.audioTracks.size());
    audioMixerPeaks_.resize(project_.audioTracks.size());
    for (std::size_t i = 0; i < audioMixerBuses_.size(); ++i) {
        if (!audioMixerBuses_[i])
            audioMixerBuses_[i] = std::make_shared<audio::AudioMixerBus>();
        const auto gains = project::audioMixGains(project_.audioTracks[i].mixerGainDb,
                                                  project_.audioTracks[i].mixerPan);
        audioMixerBuses_[i]->leftGain.store(static_cast<float>(gains.first));
        audioMixerBuses_[i]->rightGain.store(static_cast<float>(gains.second));
    }
    if (audioTrackModel_)
        audioTrackModel_->setProject(project_);
}

void MvmController::pushUndoEntry(UndoEntry entry) {
    entry.selectedSubtitleId = selectedSubtitleId_;
    entry.selectedSubtitleIds = selectedSubtitleIds_;
    entry.bytes = project::approximateProjectBytes(entry.project);
    undoHistory_.push_back(std::move(entry));
    // 新しい編集をした時点で、やり直し先の未来は無くなる。
    redoHistory_.clear();
    trimEditHistory();
}

void MvmController::trimEditHistory() {
    // Undo と Redo の合計で予算を守る。Undo / Redo は現在の Project を反対側へ積むので、
    // 積む側だけを見ると、現在の Project が大きいときに合計が予算を超える。
    const auto farthestFirst = [](const std::vector<UndoEntry>& history) {
        std::vector<std::size_t> bytes;
        bytes.reserve(history.size());
        for (const auto& kept : history)
            bytes.push_back(kept.bytes);
        return bytes;
    };
    const auto drop =
        project::editHistoryEntriesToDrop(farthestFirst(undoHistory_), farthestFirst(redoHistory_),
                                          kMaximumUndoEntries, editHistoryByteBudget_);
    undoHistory_.erase(undoHistory_.begin(),
                       undoHistory_.begin() + static_cast<std::ptrdiff_t>(drop.undo));
    redoHistory_.erase(redoHistory_.begin(),
                       redoHistory_.begin() + static_cast<std::ptrdiff_t>(drop.redo));
}

std::size_t MvmController::editHistoryBytes() const {
    std::size_t total = 0;
    for (const auto* history : {&undoHistory_, &redoHistory_})
        for (const auto& entry : *history)
            total += entry.bytes;
    return total;
}

void MvmController::clearEditHistory() {
    undoHistory_.clear();
    redoHistory_.clear();
}

bool MvmController::commitProjectEdit(project::Project candidate, const QString& failurePrefix,
                                      PlaybackInvalidation invalidation) {
    if (!projectLockHeld_) {
        setStatus(failurePrefix + QStringLiteral("Projectを排他できません"));
        return false;
    }
    if (candidate == project_)
        return true;
    const auto serialized = project::serializeProjectJson(candidate, projectPath_);
    if (!serialized.success) {
        setStatus(failurePrefix + QString::fromStdString(serialized.error));
        return false;
    }
    // 確定した後は現在の Project を複製せずに履歴へ移す (編集 1 回で Project を余分に
    // 1 つ複製しない)。
    // currentClipId() は project_ を読むので、移す前に取る。
    std::string currentId = currentClipId();
    UndoEntry undo{std::move(project_), selectedClipIds_, currentId, playheadFrame_,
                   currentRevision_};
    project_ = std::move(candidate);
    // clip 列の前方が消えると、範囲内の旧 index でも別 ID を指す。同期的な model 通知より
    // 前に ID から付け直し、inspector が別の clip の property を読むことを防ぐ。
    const int currentIndex = indexOfClipId(project_.timelineClips, currentId);
    if (currentClipIndex_ != currentIndex)
        setCurrentClipSelection(currentIndex);
    refreshAudioInputAuthority(true);
    pushUndoEntry(std::move(undo));
    currentRevision_ = nextRevision_++;
    refreshTimelineModel(invalidation);
    scheduleRecoveryAutosave();
    return true;
}


std::string MvmController::currentClipId() const {
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return {};
    return project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id;
}

bool MvmController::resolveTrackRef(const QString& trackKind, int trackIndex,
                                    project::TrackRef& track) const {
    if (trackKind == QStringLiteral("video"))
        track.kind = project::TrackKind::Video;
    else if (trackKind == QStringLiteral("audio"))
        track.kind = project::TrackKind::Audio;
    else
        return false;
    track.index = trackIndex;
    return project::isValidTrackRef(project_, track);
}

void MvmController::setCurrentClipSelection(int index) {
    if (previewEffectsOverride_ && previewEffectsClipIndex_ != index) {
        // 別 clip を選んだ時点で、確定していない override は捨てる。
        previewEffectsOverride_.reset();
        previewEffectsClipIndex_ = -1;
    }
    currentClipIndex_ = index;
    currentSource_.reset();
    if (index < 0 || index >= static_cast<int>(project_.timelineClips.size())) {
        currentClipName_.clear();
        currentClipPath_.clear();
        Q_EMIT stateChanged();
        return;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    currentClipName_ = QString::fromStdString(clip.name);
    currentClipPath_ = fromPath(clip.mediaPath);
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == index)
            currentSource_ = slot.source;
    }
    Q_EMIT stateChanged();
}

void MvmController::setTimelineSelection(const std::vector<std::string>& clipIds,
                                         bool expandLinks) {
    selectedEditOutgoing_.clear();
    selectedEditIncoming_.clear();
    selectedTransitionId_.clear();
    setSubtitleSelection({}, {});
    selectedClipIds_ = clipIds;
    std::vector<std::string> selectedLinkGroups;
    for (const auto& id : clipIds) {
        const int index = indexOfClipId(project_.timelineClips, id);
        if (index < 0)
            continue;
        const auto& linkGroup = project_.timelineClips[static_cast<std::size_t>(index)].linkGroupId;
        if (expandLinks && !linkGroup.empty() &&
            std::find(selectedLinkGroups.begin(), selectedLinkGroups.end(), linkGroup) ==
                selectedLinkGroups.end())
            selectedLinkGroups.push_back(linkGroup);
    }
    for (const auto& clip : project_.timelineClips) {
        if (!clip.linkGroupId.empty() &&
            std::find(selectedLinkGroups.begin(), selectedLinkGroups.end(), clip.linkGroupId) !=
                selectedLinkGroups.end() &&
            std::find(selectedClipIds_.begin(), selectedClipIds_.end(), clip.id) ==
                selectedClipIds_.end())
            selectedClipIds_.push_back(clip.id);
    }
    QSet<QString> selectedIds;
    for (const auto& id : selectedClipIds_)
        selectedIds.insert(QString::fromStdString(id));
    timelineModel_->setSelectedClipIds(selectedIds);
    notifyTimelineTransitions();
}

bool MvmController::refreshPreviewAfterSavedEdit(const std::string& selectedClipId,
                                                 const QString& successStatus) {
    const qint64 frame = playheadFrame_;
    const bool refreshed = seekTimelineFrame(frame);
    const QString previewFailure = statusText_;
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, selectedClipId));
    if (!refreshed) {
        setStatus(QStringLiteral("編集は反映されましたが、Previewの更新に失敗しました: ") +
                  previewFailure);
        return true;
    }
    setStatus(successStatus);
    return true;
}

bool MvmController::syncManimTimelineClip(bool addIfMissing) {
    if (project_.manimAssets.empty())
        return true;
    const project::ManimAsset& asset = project_.manimAssets.front();
    if (asset.generatedVideoPath.empty())
        return true;

    const ProbedMedia media = probeMedia(asset.generatedVideoPath);
    if (!media.success) {
        setStatus(QStringLiteral("Manim videoをtimelineへ反映できません: ") + media.error);
        return false;
    }

    project::Project candidate = project_;
    const int index = indexOfManimClip(candidate.timelineClips);
    if (index < 0) {
        if (!addIfMissing)
            return true;
        // Manim clip は overlay として扱うため、上側の video track を使う。
        const int overlayTrack = candidate.videoTracks.size() > 1 ? 1 : 0;
        const auto placed = project::appendManimTimelineClipAt(
            candidate, asset, newClipId(), media.fpsNum, media.fpsDen, media.frameCount,
            playheadFrame_, project::TrackRef{project::TrackKind::Video, overlayTrack});
        if (!placed.success) {
            setStatus(QString::fromStdString(placed.error));
            return false;
        }
    } else {
        project::TimelineClip& existing = candidate.timelineClips[static_cast<std::size_t>(index)];
        if (existing.sourceFpsNum != media.fpsNum || existing.sourceFpsDen != media.fpsDen ||
            existing.sourceOutFrame > media.frameCount) {
            setStatus(QStringLiteral("再生成後のManim素材では既存trimを保持できません。"
                                     "timelineから削除して追加し直してください"));
            return false;
        }
        if (existing.mediaPath == asset.generatedVideoPath && existing.name == asset.sceneName &&
            existing.sourceFrameCount == media.frameCount)
            return true; // 変化が無ければ project を書き直さない
        existing.mediaPath = asset.generatedVideoPath;
        existing.name = asset.sceneName;
        existing.sourceFrameCount = media.frameCount;
        const auto valid = project::finalizeTimelineCandidate(candidate);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
    }
    return commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: "));
}

void MvmController::pollAudioMeter() {
    for (std::size_t i = 0; i < audioMixerBuses_.size(); ++i)
        audioMixerPeaks_[i] = {audioMixerBuses_[i]->peakLeft.exchange(0.0F),
                               audioMixerBuses_[i]->peakRight.exchange(0.0F)};
    if (!previewEngine_)
        return;
    const auto telemetry = previewEngine_->telemetry();
    // シャトルと scrub は preview engine と別の endpoint で鳴らすので、鳴っている側の peak を出す。
    const bool ownSink = shuttleAudio_ || scrubAudio_;
    const auto own = shuttleAudio_ ? shuttleAudio_->sinkSnapshot()
                     : scrubAudio_ ? scrubAudio_->sinkSnapshot()
                                   : audio::WasapiSnapshot{};
    const double left = linearToDb((ownSink ? own.meterPeakLeft : telemetry.audioMeterPeakLeft) *
                                       static_cast<float>(std::min(1.0, masterVolume_)),
                                   kMeterSilenceDb);
    const double right = linearToDb((ownSink ? own.meterPeakRight : telemetry.audioMeterPeakRight) *
                                        static_cast<float>(std::min(1.0, masterVolume_)),
                                    kMeterSilenceDb);
    const bool clipped =
        audioMeterClipped_ || (ownSink ? own.meterClipped : telemetry.audioMeterClipped);
    if (clipped == audioMeterClipped_ && std::abs(left - audioMeterDbLeft_) < 0.05 &&
        std::abs(right - audioMeterDbRight_) < 0.05)
        return;
    audioMeterClipped_ = clipped;
    audioMeterDbLeft_ = left;
    audioMeterDbRight_ = right;
    Q_EMIT meterChanged();
}

void MvmController::removeRetiredSources(const preview::PreviewStatus& status) {
    if (status.lastPresentedComposition != status.latestAcceptedDesiredComposition ||
        retiredSources_.empty())
        return;
    const auto pendingRetirement = std::move(retiredSources_);
    retiredSources_.clear();
    for (const auto source : pendingRetirement) {
        const auto removed = previewEngine_->removeSource(source);
        if (!removed)
            retiredSources_.push_back(source);
    }
}

void MvmController::pollPreviewState() {
    if (!previewEngine_)
        return;
    const auto status = previewEngine_->status();
    collectSourcePreparations();
    removeRetiredSources(status);
    const bool ready = status.state == preview::PreviewEngineState::ReadyPaused ||
                       status.state == preview::PreviewEngineState::Playing;
    if (previewReady_ != ready) {
        previewReady_ = ready;
        // preview が使えるようになったら playhead 位置の frame を出す。
        // Project を開いた直後や engine を作り直した直後に黒画面のままにしない。
        // 判定は「source が 1 つも載っていないか」で行う。選択 clip の有無で見ると、
        // engine reset 直後に seek が弾かれて選択だけ残った状態を拾えない。
        const bool showInitialFrame = ready && !busy_ && !pendingVideoPath_ &&
                                      trackSources_.empty() && audioSources_.empty() &&
                                      !submittedComposition_ && !pendingCapacityRebuildFrame_ &&
                                      totalTimelineFrames_ > 0;
        Q_EMIT stateChanged();
        if (showInitialFrame) {
            seekTimelineFrame(playheadFrame_);
            return;
        }
        if (ready && !busy_ && currentClipPath_.isEmpty() && !hasManimAsset())
            statusText_ = QStringLiteral("素材を追加してください");
        Q_EMIT stateChanged();
    }
    if (ready && pendingSlotRebuildFrame_ && stalePreparations_.empty() &&
        retiredSources_.empty()) {
        const auto frame = *pendingSlotRebuildFrame_;
        pendingSlotRebuildFrame_.reset();
        const auto* selected = topVideoClipAt(project_, frame);
        const int clipIndex =
            selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
        if (!queuePreparedPlayback(clipIndex, frame))
            stopPlaybackWithError(QStringLiteral("登録枠が空いた後のPreviewを準備できません: ") +
                                  statusText_);
        return;
    }
    if (ready && pendingCapacityRebuildFrame_) {
        const auto frame = *pendingCapacityRebuildFrame_;
        pendingCapacityRebuildFrame_.reset();
        const auto* selected = topVideoClipAt(project_, frame);
        const int clipIndex =
            selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
        if (!queuePreparedPlayback(clipIndex, frame))
            stopPlaybackWithError(QStringLiteral("登録上限後のPreviewを準備できません: ") +
                                  statusText_);
        return;
    }
    if (status.state == preview::PreviewEngineState::ReadyPaused && pendingVideoPath_) {
        const std::filesystem::path videoPath = std::move(*pendingVideoPath_);
        const QString clipName = std::move(pendingClipName_);
        const int clipIndex = pendingClipIndex_;
        const std::int64_t sourceFrame = pendingSourceFrame_;
        pendingVideoPath_.reset();
        pendingClipName_.clear();
        pendingClipIndex_ = -1;
        pendingSourceFrame_ = 0;
        installVideoClip(videoPath, clipName, clipIndex, sourceFrame);
    }
    const auto playbackState = previewEngine_->status().state;
    if (textPreviewRefreshPending_ && playbackState == preview::PreviewEngineState::ReadyPaused)
        refreshTextPreview();
    if (previewRefreshPending_ &&
        previewEngine_->status().state == preview::PreviewEngineState::ReadyPaused) {
        QString error;
        if (!refreshPreviewAtPlayhead(error))
            setStatus(QStringLiteral("Previewを更新できません: ") + error);
    }
    if (pendingPlaybackStart_ && playbackState == preview::PreviewEngineState::ReadyPaused)
        startPendingPlayback();
    if (status.state == preview::PreviewEngineState::Error && status.lastError) {
        const QString message =
            QStringLiteral("Preview error: ") + previewErrorText(*status.lastError);
        if (pendingPlaybackStart_)
            stopPlaybackWithError(message);
        else if (statusText_ != message)
            setStatus(message);
    }
}

bool MvmController::installVideoClip(const std::filesystem::path& videoPath,
                                     const QString& clipName, int clipIndex,
                                     std::int64_t sourceFrame) {
    const auto state = previewEngine_->status().state;
    if (state == preview::PreviewEngineState::Playing) {
        const auto paused = previewEngine_->pause();
        if (!paused) {
            setStatus(QStringLiteral("現在のclipを停止できません: ") +
                      previewErrorText(paused.error()));
            return false;
        }
    } else if (state != preview::PreviewEngineState::ReadyPaused) {
        setStatus(QStringLiteral("Previewがclip追加可能な状態ではありません"));
        return false;
    }

    QString compositionError;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: 対象clipがありません"));
        return false;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    const std::int64_t timelineFrame = clip.timelineStartFrame + (sourceFrame - clip.sourceInFrame);
    if (!syncPreviewSourcesAt(timelineFrame, compositionError)) {
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: ") + compositionError);
        return false;
    }
    currentClipName_ = clipName;
    currentClipPath_ = fromPath(videoPath);
    currentClipIndex_ = clipIndex;
    currentSource_.reset();
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == currentClipIndex_)
            currentSource_ = slot.source;
    }
    statusText_ = clipName + QStringLiteral(" を表示しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::audioDescriptorFor(const TimelinePreviewAudioLayerMapping& layer,
                                       preview::PreviewSourceDescriptor& descriptor,
                                       QString& error) {
    // 鳴らす区間の clip (クロスフェードで延ばした素材範囲を含む)。
    const auto& clip = layer.segment.clip;
    if (!std::filesystem::is_regular_file(clip.mediaPath)) {
        error = QString::fromStdString(clip.name) + QStringLiteral(" のファイルがありません: ") +
                fromPath(clip.mediaPath);
        return false;
    }
    // output frame -> media sample のずれ。換算式は mapping 側へ一本化している。
    const auto offset = audioPreviewSampleOffset(project_, clip);
    if (!offset.success) {
        error = QString::fromStdString(offset.error);
        return false;
    }
    descriptor = preview::PreviewSourceDescriptor{};
    descriptor.mediaPath = clip.mediaPath;
    descriptor.audioEnabled = true;
    descriptor.audioMixerBus = audioMixerBuses_.at(static_cast<std::size_t>(clip.track.index));
    descriptor.audioSampleOffset = offset.sampleOffset;
    descriptor.audioTimelineStartFrame = clip.timelineStartFrame;
    descriptor.speedNum = clip.speedNum;
    descriptor.speedDen = clip.speedDen;
    descriptor.audioPreservePitch = clip.preservePitch;
    const auto timebase = core::CheckedOutputTimebase::create(
        project_.timelineFpsNum, project_.timelineFpsDen, audio::kInternalSampleRate);
    if (!timebase) {
        error = QStringLiteral("音量カーブのtimebaseを作成できません");
        return false;
    }
    const auto timelineFpsNum = project_.timelineFpsNum;
    const auto timelineFpsDen = project_.timelineFpsDen;
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success) {
        error = QString::fromStdString(duration.error);
        return false;
    }
    const auto segmentStart = clip.timelineStartFrame;
    std::int64_t segmentEnd = 0;
    if (!core::checkedAdd(segmentStart, duration.frame, segmentEnd)) {
        error = QStringLiteral("音声区間の終端を表せません");
        return false;
    }
    // 音量カーブ・フェード・クロスフェードは書き出しと同じ区間の評価を使う。
    auto gainSegment = layer.segment;
    gainSegment.mixerGainDb = 0.0;
    descriptor.audioGainAtMediaSample =
        [segment = gainSegment, sampleOffset = offset.sampleOffset, timelineFpsNum, timelineFpsDen,
         segmentStart, segmentEnd, timebase = timebase.value()](std::int64_t mediaSample) -> float {
        const auto timelineSample = mediaSample - sampleOffset;
        if (timelineSample < 0)
            return 0.0F;
        const auto frame = timebase.schedulerOutputFrame(timelineSample);
        if (!frame)
            return 0.0F;
        if (frame.value() < segmentStart || frame.value() >= segmentEnd)
            return 0.0F;
        const auto gain =
            project::renderSegmentGain(segment, timelineFpsNum, timelineFpsDen, frame.value());
        return gain ? static_cast<float>(*gain) : 0.0F;
    };
    return true;
}

bool MvmController::audioIdentitiesFor(const TimelinePreviewAudioMapping& mapped,
                                       std::vector<AudioSourceIdentity>& identities,
                                       QString& error) const {
    identities.clear();
    identities.reserve(mapped.layers.size());
    for (const auto& layer : mapped.layers) {
        const auto& clip = layer.segment.clip;
        const auto offset = audioPreviewSampleOffset(project_, clip);
        if (!offset.success) {
            error = QString::fromStdString(offset.error);
            return false;
        }
        const auto duration = project::timelineClipDuration(project_, clip);
        std::int64_t segmentEnd = 0;
        if (!duration.success ||
            !core::checkedAdd(clip.timelineStartFrame, duration.frame, segmentEnd)) {
            error = duration.success ? QStringLiteral("音声区間の終端を表せません")
                                     : QString::fromStdString(duration.error);
            return false;
        }
        identities.push_back({clip.mediaPath, offset.sampleOffset, clip.timelineStartFrame,
                              segmentEnd, layer.segment.original.effects, clip.speedNum,
                              clip.speedDen, clip.preservePitch, layer.segment.fadeIn,
                              layer.segment.fadeOut});
    }
    return true;
}

bool MvmController::applyAudioSourceFor(std::int64_t timelineFrame, AudioSwitchUndo& undo,
                                        QString& error) {
    undo = AudioSwitchUndo{};
    const auto mapped = mapTimelinePreviewAudio(project_, previewPlan(), timelineFrame);
    if (!mapped.success) {
        error = QString::fromStdString(mapped.error);
        return false;
    }

    std::vector<AudioSourceIdentity> desired;
    if (!audioIdentitiesFor(mapped, desired, error))
        return false;
    bool unchanged = audioSources_.size() == desired.size();
    for (std::size_t index = 0; unchanged && index < desired.size(); ++index)
        unchanged = audioSources_[index].identity == desired[index];
    if (unchanged) {
        for (std::size_t index = 0; index < mapped.layers.size(); ++index) {
            audioSources_[index].clipId = mapped.layers[index].clipId;
            audioSources_[index].clipIndex = mapped.layers[index].clipIndex;
        }
        return true;
    }

    std::vector<AudioPreviewSource> target;
    target.reserve(desired.size());
    for (std::size_t index = 0; index < desired.size(); ++index) {
        preview::PreviewSourceDescriptor descriptor;
        if (!audioDescriptorFor(mapped.layers[index], descriptor, error))
            return false;
        target.push_back({{},
                          desired[index],
                          descriptor,
                          mapped.layers[index].clipId,
                          mapped.layers[index].clipIndex});
    }

    undo.changed = true;
    undo.previous = audioSources_;

    QString operationError;
    const auto replaced = replaceSourceSet(
        audioSources_, target,
        [&](const AudioPreviewSource& current) {
            const auto removed = previewEngine_->removeSource(current.source);
            if (!removed)
                operationError = previewErrorText(removed.error());
            return static_cast<bool>(removed);
        },
        [&](const AudioPreviewSource& requested, AudioPreviewSource& installed) {
            const auto added = previewEngine_->addSource(requested.descriptor);
            if (!added) {
                operationError = previewErrorText(added.error());
                return false;
            }
            installed.source = added.value();
            return true;
        },
        [&] { return resetPreviewEngine(); });
    if (replaced == SourceSetReplaceResult::Success)
        return true;
    undo.changed = false;
    undo.engineReset = replaced == SourceSetReplaceResult::OperationFailedReset;
    error = operationError;
    if (replaced == SourceSetReplaceResult::OperationFailedReset)
        error += QStringLiteral("; audio source復元にも失敗したためPreviewを再初期化しました");
    else if (replaced == SourceSetReplaceResult::OperationFailedResetFailed)
        error += QStringLiteral("; audio source復元とPreview再初期化に失敗しました");
    return false;
}

bool MvmController::revertAudioSource(const AudioSwitchUndo& undo, QString& error) {
    if (!undo.changed)
        return true;
    const auto replaced = replaceSourceSet(
        audioSources_, undo.previous,
        [&](const AudioPreviewSource& current) {
            const auto removed = previewEngine_->removeSource(current.source);
            if (!removed)
                error = previewErrorText(removed.error());
            return static_cast<bool>(removed);
        },
        [&](const AudioPreviewSource& requested, AudioPreviewSource& installed) {
            const auto added = previewEngine_->addSource(requested.descriptor);
            if (!added) {
                error = previewErrorText(added.error());
                return false;
            }
            installed.source = added.value();
            return true;
        },
        [&] { return resetPreviewEngine(); });
    if (replaced == SourceSetReplaceResult::Success)
        return true;
    if (replaced == SourceSetReplaceResult::OperationFailedReset)
        error += QStringLiteral("; compensation失敗のためPreviewを再初期化しました");
    else if (replaced == SourceSetReplaceResult::OperationFailedResetFailed)
        error += QStringLiteral("; compensationとPreview再初期化に失敗しました");
    return false;
}

namespace {
class ClipPreviewMotion final : public preview::PreviewMotion {
public:
    ClipPreviewMotion(project::TimelineClip clip, std::int64_t fpsNum, std::int64_t fpsDen,
                      std::int64_t duration)
        : clip_(std::move(clip)), fpsNum_(fpsNum), fpsDen_(fpsDen), duration_(duration) {}

    preview::PreviewMotionValue evaluate(std::int64_t frame) const override {
        const auto local =
            std::clamp(frame - clip_.timelineStartFrame, std::int64_t{0}, duration_ - 1);
        const auto mapped =
            project::mapClipEffects(project::evaluateClipEffects(clip_.effects, local));
        const auto fade = project::clipFadeSourceFrameAt(clip_, fpsNum_, fpsDen_, local);
        const double opacity =
            fade.success ? project::evaluateClipOpacity(clip_.effects, local, fade.frame,
                                                        clip_.sourceOutFrame - clip_.sourceInFrame)
                         : std::numeric_limits<double>::quiet_NaN();
        return {{static_cast<float>(mapped.destinationRect.x),
                 static_cast<float>(mapped.destinationRect.y),
                 static_cast<float>(mapped.destinationRect.width),
                 static_cast<float>(mapped.destinationRect.height)},
                {static_cast<float>(mapped.sourceRect.x), static_cast<float>(mapped.sourceRect.y),
                 static_cast<float>(mapped.sourceRect.width),
                 static_cast<float>(mapped.sourceRect.height)},
                static_cast<float>(mapped.rotationDegrees),
                static_cast<float>(opacity)};
    }

private:
    project::TimelineClip clip_;
    std::int64_t fpsNum_, fpsDen_, duration_;
};

preview::PreviewPixelRect pixelRectUnion(const preview::PreviewPixelRect& a,
                                         const preview::PreviewPixelRect& b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    const int left = std::min(a.x, b.x);
    const int top = std::min(a.y, b.y);
    const int right = std::max(a.x + a.width, b.x + b.width);
    const int bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

// 数式 clip の preview の animation。静止の画素 (書き終えた式) の一部の矩形を、Write の間は
// Write の連番の frame、変形の区間は変形の frame の画素に変える (区間の外は静止のまま)。
// frame の選び方 (mathIntroFrameAt / mathTransformFrameAt)・artifact の位置
// (mathTransformArtifactOriginAt)・文字色 (mathTransformColorAt)・画素の式 (composeMathPatch)
// は書き出しと共有する。mask は 1 画素 1 byte で持ち、着色は state が変わったときだけ行う。
//
// engine は animation の instance ごとに静止画の texture を持つので、1 本の clip の Write と
// 変形 (前の clip からの変形と次の clip への変形) を 1 つの instance にまとめる。再生中の合成の
// 切り替えは tick 単位で提示より遅れて届くので、区間の境ごとに instance を替えると、その間の
// frame が前の instance (区間の外の静止) で提示されてしまう。state は出力 frame だけから決まり、
// 前の clip の layer が cut の後まで残っていても同じ変形の frame を見せる。
//
// state の番号: [0, Write の枚数) は Write、その後に変形の部分ごとの連番を順に並べる。
class MathClipPreviewAnimation final : public preview::PreviewStillAnimation {
public:
    struct WritePart {
        std::shared_ptr<const MathCoverageSequence> frames;
        math::MathComposeStyle style;
        // 静止の mask を置いた矩形。
        preview::PreviewPixelRect rect;
    };

    struct TransformPart {
        std::string transitionId;
        MathTransformWindow window;
        // 切り出した artifact の被覆 (window.frames 枚)。
        std::shared_ptr<const MathCoverageSequence> frames;
        math::MathTransformRasterPlacement placement;
        std::uint32_t sourceColor = 0;
        std::uint32_t targetColor = 0;
    };

    MathClipPreviewAnimation(project::TimelineClip clip, std::int64_t fpsNum, std::int64_t fpsDen,
                             std::shared_ptr<const preview::PreviewStillImage> still,
                             std::optional<WritePart> write, std::vector<TransformPart> transforms,
                             MvmController::MathWriteObserver writeObserver,
                             MvmController::MathTransformObserver transformObserver)
        : clip_(std::move(clip)), fpsNum_(fpsNum), fpsDen_(fpsDen), still_(std::move(still)),
          write_(std::move(write)), transforms_(std::move(transforms)),
          writeObserver_(std::move(writeObserver)),
          transformObserver_(std::move(transformObserver)) {
        if (write_)
            rect_ = write_->rect;
        // 変形の artifact は source の位置から target の位置へ動く (各軸 1 画素以内)。両方の
        // 位置の矩形の和が、途中の frame のすべての位置を含む。
        for (const auto& part : transforms_) {
            const int width = part.frames->width;
            const int height = part.frames->height;
            rect_ = pixelRectUnion(
                rect_, {part.placement.sourceLeft, part.placement.sourceTop, width, height});
            rect_ = pixelRectUnion(
                rect_, {part.placement.targetLeft, part.placement.targetTop, width, height});
        }
    }

    preview::PreviewPixelRect patchRect() const override { return rect_; }

    std::int64_t stateAt(std::int64_t outputFrame) const override {
        std::int64_t state = -1;
        std::int64_t base = 0;
        if (write_) {
            const auto index =
                mathIntroFrameAt(clip_, fpsNum_, fpsDen_, outputFrame - clip_.timelineStartFrame);
            const auto count = static_cast<std::int64_t>(write_->frames->frames.size());
            const std::int64_t shown = !index || *index < 0 || *index >= count ? -1 : *index;
            if (writeObserver_)
                writeObserver_(clip_.id, outputFrame, shown);
            state = shown;
            base = count;
        }
        for (const auto& part : transforms_) {
            const auto count = static_cast<std::int64_t>(part.frames->frames.size());
            const std::int64_t local = mathTransformFrameAt(part.window, outputFrame);
            const std::int64_t shown = local < count ? local : -1;
            if (transformObserver_)
                transformObserver_(part.transitionId, clip_.id, outputFrame, shown);
            if (state < 0 && shown >= 0)
                state = base + shown;
            base += count;
        }
        return state;
    }

    void fillPatch(std::int64_t state, std::uint8_t* out) const override {
        std::int64_t base = 0;
        if (write_) {
            const auto count = static_cast<std::int64_t>(write_->frames->frames.size());
            if (state < count) {
                fillWrite(state, out);
                return;
            }
            base = count;
        }
        for (const auto& part : transforms_) {
            const auto count = static_cast<std::int64_t>(part.frames->frames.size());
            if (state < base + count) {
                fillTransform(part, state - base, out);
                return;
            }
            base += count;
        }
    }

private:
    void fillWrite(std::int64_t frame, std::uint8_t* out) const {
        const auto& frames = *write_->frames;
        const auto& coverage = frames.frames[static_cast<std::size_t>(frame)];
        if (rect_ == write_->rect) {
            math::composeMathPatch(coverage.data(), frames.width, frames.height, write_->style,
                                   out);
            return;
        }
        // 変形の部分の分だけ広い矩形: 外側は静止の画素のまま。
        const std::size_t row = static_cast<std::size_t>(rect_.width) * 4U;
        for (int y = 0; y < rect_.height; ++y)
            std::memcpy(out + static_cast<std::size_t>(y) * row,
                        still_->rgba.data() + (static_cast<std::size_t>(rect_.y + y) *
                                                   static_cast<std::size_t>(still_->width) +
                                               static_cast<std::size_t>(rect_.x)) *
                                                  4U,
                        row);
        math::composeMathPatchAt(coverage.data(), frames.width, frames.height, write_->style, out,
                                 rect_.width, write_->rect.x - rect_.x, write_->rect.y - rect_.y);
    }

    void fillTransform(const TransformPart& part, std::int64_t frame, std::uint8_t* out) const {
        // 背景は透明 (P2-1 で両端の背景は透明に限る)。矩形の中の静止の glyph は artifact が
        // 必ず含むので、artifact の外は透明で埋める (1 画素動いた位置で静止の glyph を残さない)。
        std::memset(out, 0,
                    static_cast<std::size_t>(rect_.width) * static_cast<std::size_t>(rect_.height) *
                        4U);
        int left = 0;
        int top = 0;
        math::MathComposeStyle style;
        style.backgroundArgb = 0;
        if (!math::mathTransformArtifactOriginAt(part.placement, frame, part.window.frames, left,
                                                 top) ||
            !math::mathTransformColorAt(part.sourceColor, part.targetColor, frame,
                                        part.window.frames, style.colorArgb))
            return;
        const auto& frames = *part.frames;
        math::composeMathPatchAt(frames.frames[static_cast<std::size_t>(frame)].data(),
                                 frames.width, frames.height, style, out, rect_.width,
                                 left - rect_.x, top - rect_.y);
    }

    project::TimelineClip clip_;
    std::int64_t fpsNum_, fpsDen_;
    std::shared_ptr<const preview::PreviewStillImage> still_;
    std::optional<WritePart> write_;
    std::vector<TransformPart> transforms_;
    preview::PreviewPixelRect rect_;
    MvmController::MathWriteObserver writeObserver_;
    MvmController::MathTransformObserver transformObserver_;
};

// Equation Sequence (P3-4) の preview の animation。見せるもの・配置・色・合成は
// EquationPreviewModel (Qt・cache に依存しない) が output frame だけから決める。sequence の全区間を
// 1 つの instance にまとめ、再生中の合成の切り替えの遅れで区間の境の frame が古い instance で
// 提示されても、同じ output frame は同じ画素になる。ClipEffects は layer が 1 回だけ掛ける。
class EquationSequencePreviewAnimation final : public preview::PreviewStillAnimation {
public:
    EquationSequencePreviewAnimation(std::shared_ptr<const EquationPreviewModel> model,
                                     MvmController::EquationPreviewObserver observer)
        : model_(std::move(model)), observer_(std::move(observer)) {
        const auto rect = model_->patchRect();
        rect_ = {rect.x, rect.y, rect.width, rect.height};
    }

    preview::PreviewPixelRect patchRect() const override { return rect_; }

    std::int64_t stateAt(std::int64_t outputFrame) const override {
        const auto shown = model_->shownAt(outputFrame);
        if (observer_)
            observer_(model_->inputs().clip.id, outputFrame, model_->timeAt(outputFrame), shown);
        return model_->stateCode(shown);
    }

    void fillPatch(std::int64_t state, std::uint8_t* out) const override {
        model_->fill(state, out);
    }

    const EquationPreviewModel& model() const { return *model_; }

private:
    std::shared_ptr<const EquationPreviewModel> model_;
    MvmController::EquationPreviewObserver observer_;
    preview::PreviewPixelRect rect_;
};

void attachClipMotion(preview::PreviewCompositionLayer& layer, const project::ClipEffects& effects,
                      const project::TimelineClip& clip, const project::Project& project,
                      double transitionOpacity = 1) {
    bool animated = effects.fadeInFrames > 0 || effects.fadeOutFrames > 0;
    for (const auto& channel : project::effectChannels())
        animated = animated || !(effects.*channel.keys).empty();
    if (!animated)
        return;
    auto captured = clip;
    captured.effects = effects;
    const auto duration = project::timelineClipDuration(project, clip);
    layer.motion = std::make_shared<ClipPreviewMotion>(std::move(captured), project.timelineFpsNum,
                                                       project.timelineFpsDen, duration.frame);
    layer.motionOpacityMultiplier = static_cast<float>(transitionOpacity);
}
} // namespace

std::shared_ptr<preview::CompositionSnapshot>
MvmController::previewCompositionForTest(qint64 frame, QString& error, bool reuse) const {
    const auto mapped = mapTimelinePreviewFrame(project_, frame);
    if (!mapped.success) {
        error = QString::fromStdString(mapped.error);
        return {};
    }
    const auto saved = graphComposition_;
    const auto memo = graphCompositionMemo_;
    if (!reuse)
        graphComposition_.reset();
    preview::PreviewFrameRequest request;
    auto result = previewCompositionFor(mapped, trackSources_, request, error);
    if (!reuse) {
        graphComposition_ = saved;
        graphCompositionMemo_ = memo;
    }
    return result;
}

std::shared_ptr<preview::CompositionSnapshot>
MvmController::previewCompositionFor(const TimelinePreviewFrameMapping& mappedFrame,
                                     const std::map<int, TrackPreviewSource>& sources,
                                     preview::PreviewFrameRequest& request, QString& error) const {
    request = preview::PreviewFrameRequest{};
    request.outputFrameNumber = mappedFrame.outputFrameNumber;
    error.clear();
    // 安定した Graph/video の層集合は clock が進んでも同じ composition を使う。
    // source-frame は既存 evaluator、出入りと順序は既存 mapping を引き続き正にする。
    QString graphMemo;
    bool stableGraph = !mappedFrame.stillLayers.empty() && !previewEffectsOverride_ &&
                       !project::activeSubtitleAt(project_, mappedFrame.outputFrameNumber);
    if (stableGraph) {
        graphMemo = QString::number(currentRevision_) + QLatin1Char('/');
        for (const auto& still : mappedFrame.stillLayers) {
            const auto& clip = project_.timelineClips[static_cast<std::size_t>(still.clipIndex)];
            const auto existing = graphAnimations_.constFind(QString::fromStdString(clip.id));
            if (still.kind != project::TimelineClipKind::Graph ||
                existing == graphAnimations_.constEnd() || existing->clip != clip ||
                graphRasters_->presentation(existing->spec) != existing->presentation) {
                stableGraph = false;
                break;
            }
            std::string mappingError;
            const auto frame = project::evaluateGraphClip(
                clip, {project_.timelineFpsNum, project_.timelineFpsDen},
                mappedFrame.outputFrameNumber - clip.timelineStartFrame, mappingError);
            if (!frame) {
                stableGraph = false;
                break;
            }
            graphRasters_->requestFrame(existing->spec, frame->sourceFrame);
            graphMemo += QString::fromStdString(clip.id) + QLatin1Char(':') +
                         QString::number(still.slot) + QLatin1Char(':') +
                         QString::number(still.opacity, 'g', 17) + QLatin1Char('/');
        }
        for (const auto& video : mappedFrame.layers) {
            const auto source = sources.find(video.slot);
            if (source == sources.end() || video.transitionOpacity != 1 || video.dissolveIncoming) {
                stableGraph = false;
                break;
            }
            graphMemo += QString::fromStdString(video.clipId) + QLatin1Char(':') +
                         QString::number(video.slot) + QLatin1Char(':') +
                         QString::number(source->second.source.value) + QLatin1Char('/');
        }
    }
    if (stableGraph && graphComposition_ && graphMemo == graphCompositionMemo_) {
        for (const auto& video : mappedFrame.layers)
            request.sources.push_back({sources.at(video.slot).source, video.sourceFrameNumber});
        return graphComposition_;
    }
    auto composition = std::make_shared<preview::CompositionSnapshot>();
    // Write・変形の preview の mask は、この frame に見える数式 clip の分だけを参照する。見えない
    // clip の animation を残すと、cache が追い出した mask が memory に残り、全体の上限
    // (MathRasterCache の residency) に新しい mask が入らなくなる。
    {
        QSet<QString> visibleMath;
        QSet<QString> visibleSequences;
        for (const auto& still : mappedFrame.stillLayers) {
            if (still.kind == project::TimelineClipKind::Math)
                visibleMath.insert(QString::fromStdString(still.clipId));
            if (still.kind == project::TimelineClipKind::EquationSequence)
                visibleSequences.insert(QString::fromStdString(still.clipId));
        }
        mathPreviewAnimations_.removeIf(
            [&](const auto& item) { return !visibleMath.contains(item.key()); });
        // Equation Sequence も同じ: 見えない clip の層を参照し続けない。
        equationPreviewAnimations_.removeIf(
            [&](const auto& item) { return !visibleSequences.contains(item.key()); });
    }
    // previewLayerStack が video と文字を track 順 (背面 -> 前面) に並べる。
    // この挿入順が engine の z 順の authority になる。
    for (const auto& entry : previewLayerStack(mappedFrame)) {
        if (entry.still) {
            const auto& stillMapping = mappedFrame.stillLayers[entry.index];
            const double opacity = std::clamp(stillMapping.opacity, 0.0, 1.0);
            preview::PreviewCompositionLayer layer;
            if (stillMapping.kind == project::TimelineClipKind::Graph) {
                const auto& clip =
                    project_.timelineClips[static_cast<std::size_t>(stillMapping.clipIndex)];
                const bool transition = std::any_of(
                    project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
                    [&](const auto& value) {
                        return value.outgoingClipId == clip.id || value.incomingClipId == clip.id;
                    });
                if (transition)
                    continue;
                auto compiled = compileGraphRender(clip.graph, clip.sourceFrameCount,
                                                   project_.outputWidth, project_.outputHeight);
                const auto* spec = std::get_if<graph::GraphRenderSpec>(&compiled);
                std::string mappingError;
                const auto frame = project::evaluateGraphClip(
                    clip, {project_.timelineFpsNum, project_.timelineFpsDen},
                    mappedFrame.outputFrameNumber - clip.timelineStartFrame, mappingError);
                if (!spec || !frame)
                    continue;
                graphRasters_->requestFrame(*spec, frame->sourceFrame);
                const auto presentation = graphRasters_->presentation(*spec);
                layer.stillImage = equationTransparentOutput();
                const auto clipId = QString::fromStdString(clip.id);
                auto existing = graphAnimations_.find(clipId);
                if (existing == graphAnimations_.end() || existing->clip != clip ||
                    existing->presentation != presentation) {
                    auto animation = std::make_shared<GraphPreviewAnimation>(
                        clip, core::FrameRate{project_.timelineFpsNum, project_.timelineFpsDen},
                        presentation, project::timelineClipDuration(project_, clip).frame,
                        project_.outputWidth, project_.outputHeight);
                    graphAnimations_.insert(clipId,
                                            {clip, presentation, std::move(animation), *spec});
                }
                layer.stillAnimation = graphAnimations_[clipId].animation;
                const auto effects = effectsForPreview(stillMapping.clipIndex);
                applyPreviewLayerEffects(
                    layer,
                    project::evaluateClipEffects(effects, mappedFrame.outputFrameNumber -
                                                              clip.timelineStartFrame),
                    opacity, 0, clip.sourceOutFrame - clip.sourceInFrame);
                attachClipMotion(layer, effects, clip, project_);
                composition->layers.push_back(std::move(layer));
                continue;
            }
            if (stillMapping.kind == project::TimelineClipKind::EquationSequence) {
                // 内部の合成 (静止・変形・action の 2 層) は animation の中で済ませ、外側の
                // ClipEffects は数式と同じく layer に 1 回だけ掛ける。何も見せられない間
                // (どの状態の静止も描けていない) は合成に入れず、揃ったら entryChanged で組み直す。
                layer.stillAnimation = equationSequencePreviewAnimation(
                    stillMapping.clipIndex, mappedFrame.outputFrameNumber);
                if (!layer.stillAnimation)
                    continue;
                layer.stillImage = equationTransparentOutput();
                const auto& clip =
                    project_.timelineClips[static_cast<std::size_t>(stillMapping.clipIndex)];
                const project::ClipEffects effects = effectsForPreview(stillMapping.clipIndex);
                if (!project::clipEffectsAreDefault(effects))
                    applyPreviewLayerEffects(
                        layer,
                        project::evaluateClipEffects(effects, mappedFrame.outputFrameNumber -
                                                                  clip.timelineStartFrame),
                        opacity, 0, clip.sourceOutFrame - clip.sourceInFrame);
                else
                    layer.opacity = static_cast<float>(opacity);
                attachClipMotion(layer, effects, clip, project_);
                composition->layers.push_back(std::move(layer));
                continue;
            }
            if (stillMapping.kind == project::TimelineClipKind::Image ||
                stillMapping.kind == project::TimelineClipKind::Math) {
                bool pending = false;
                layer.stillImage = stillMapping.kind == project::TimelineClipKind::Image
                                       ? imageStillImage(stillMapping.clipIndex, error, pending)
                                       : mathStillImage(stillMapping.clipIndex, pending);
                // raster を worker で生成中 (数式は描いたことが無い、または描けない)。
                // できるまではこの画像を合成に入れず、できたら entryChanged で組み直す。
                if (pending)
                    continue;
                if (!layer.stillImage)
                    return nullptr;
                if (stillMapping.kind == project::TimelineClipKind::Math)
                    layer.stillAnimation =
                        mathPreviewAnimation(stillMapping.clipIndex, layer.stillImage);
                const auto& clip =
                    project_.timelineClips[static_cast<std::size_t>(stillMapping.clipIndex)];
                const project::ClipEffects effects = effectsForPreview(stillMapping.clipIndex);
                // 画像・数式は全画面の raster なので、位置・拡大・回転・crop は video
                // と同じ座標系で効く。
                if (!project::clipEffectsAreDefault(effects))
                    applyPreviewLayerEffects(
                        layer,
                        project::evaluateClipEffects(effects, mappedFrame.outputFrameNumber -
                                                                  clip.timelineStartFrame),
                        opacity, 0, clip.sourceOutFrame - clip.sourceInFrame);
                else
                    layer.opacity = static_cast<float>(opacity);
                attachClipMotion(layer, effects, clip, project_);
                composition->layers.push_back(std::move(layer));
                continue;
            }
            // UI が重ねている (ドラッグ・編集中の) 文字は二重に描かない。
            if (QString::fromStdString(stillMapping.clipId) == textOverlayClipId_)
                continue;
            layer.stillImage = textStillImage(stillMapping.clipIndex, error);
            if (!layer.stillImage)
                return nullptr;
            const auto& clip =
                project_.timelineClips[static_cast<std::size_t>(stillMapping.clipIndex)];
            const auto effects = effectsForPreview(stillMapping.clipIndex);
            applyPreviewLayerEffects(
                layer,
                project::evaluateClipEffects(effects, mappedFrame.outputFrameNumber -
                                                          clip.timelineStartFrame),
                opacity, 0, clip.sourceOutFrame - clip.sourceInFrame);
            attachClipMotion(layer, effects, clip, project_);
            composition->layers.push_back(std::move(layer));
            continue;
        }
        const auto& layerMapping = mappedFrame.layers[entry.index];
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layerMapping.clipIndex)];
        const auto slot = sources.find(layerMapping.slot);
        if (slot == sources.end())
            continue;
        // drag 中の override はここでだけ効かせる。Project は書き換えない。
        const project::ClipEffects effects = effectsForPreview(layerMapping.clipIndex);
        preview::PreviewCompositionLayer layer;
        layer.source = slot->second.source;
        // トランジションの incoming は不透明度を進み具合で上げる。effect が既定値でも掛ける。
        if (!project::clipEffectsAreDefault(effects) || layerMapping.transitionOpacity != 1.0) {
            // トランジションで延ばした区間は clip の端の値のまま評価する (書き出しと同じ)。
            const auto duration = project::timelineClipDuration(project_, clip);
            if (!duration.success) {
                error = QString::fromStdString(duration.error);
                return nullptr;
            }
            const std::int64_t local =
                std::clamp(mappedFrame.outputFrameNumber - clip.timelineStartFrame, std::int64_t{0},
                           duration.frame - 1);
            const auto fadeFrame = project::clipFadeSourceFrameAt(clip, project_.timelineFpsNum,
                                                                  project_.timelineFpsDen, local);
            if (!fadeFrame.success) {
                error = QString::fromStdString(fadeFrame.error);
                return nullptr;
            }
            const auto& shown = layerMapping.renderClip;
            applyPreviewLayerEffects(
                layer, project::evaluateClipEffects(effects, local),
                project::evaluateClipOpacity(effects, local, fadeFrame.frame,
                                             clip.sourceOutFrame - clip.sourceInFrame) *
                    layerMapping.transitionOpacity,
                shown.sourceInFrame, shown.sourceOutFrame - shown.sourceInFrame);
        }
        attachClipMotion(layer, effects, clip, project_, layerMapping.transitionOpacity);
        layer.opaqueBackdrop = layerMapping.dissolveIncoming;
        composition->layers.push_back(layer);
        request.sources.push_back({slot->second.source, layerMapping.sourceFrameNumber});
    }
    if (const auto* cue = project::activeSubtitleAt(project_, mappedFrame.outputFrameNumber)) {
        if (!subtitleRaster_ || subtitleRasterId_ != QString::fromStdString(cue->id)) {
            const auto image = renderSubtitleRaster(
                *cue, subtitleStylePreview_.value_or(project_.subtitles->style),
                project_.outputWidth, project_.outputHeight, error);
            if (image.isNull())
                return nullptr;
            const auto straight = image.convertToFormat(QImage::Format_RGBA8888);
            auto still = std::make_shared<preview::PreviewStillImage>();
            still->width = straight.width();
            still->height = straight.height();
            const auto rowBytes = static_cast<std::size_t>(straight.width()) * 4U;
            still->rgba.resize(rowBytes * static_cast<std::size_t>(straight.height()));
            for (int y = 0; y < straight.height(); ++y)
                std::memcpy(still->rgba.data() + rowBytes * static_cast<std::size_t>(y),
                            straight.constScanLine(y), rowBytes);
            subtitleRaster_ = std::move(still);
            subtitleMotion_ = std::make_shared<SubtitlePreviewMotion>(*cue);
            subtitleRasterId_ = QString::fromStdString(cue->id);
        }
        if (composition->layers.size() >= kMaxPreviewCompositionLayers) {
            error = QStringLiteral("字幕を含むプレビューのレイヤー数が上限を超えています");
            return nullptr;
        }
        preview::PreviewCompositionLayer layer;
        layer.stillImage = subtitleRaster_;
        // GUIの次の通知を待たず、実際の描画フレームで終了境界を閉じる。
        layer.motion = subtitleMotion_;
        composition->layers.push_back(std::move(layer));
    }
    if (stableGraph) {
        graphCompositionMemo_ = graphMemo;
        graphComposition_ = composition;
    } else {
        graphCompositionMemo_.clear();
        graphComposition_.reset();
    }
    return composition;
}

bool MvmController::syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error) {
    const auto mappedFrame = mapTimelinePreviewFrame(project_, previewPlan(), timelineFrame);
    if (!mappedFrame.success) {
        error = QString::fromStdString(mappedFrame.error);
        return false;
    }
    // 映像の無い frame (画像だけ・音声だけ・何も無い) も同じ経路を通す。decode source の無い
    // composition は engine が scheduler の時計で提示し、音声もここで差し替える。
    // 失敗しうる操作を先に済ませ、後戻りできない audio の差し替えを最後へ寄せる。
    //   video prepare -> composition submit -> audio 差し替え -> seek
    // audio は composition の layer ではないので submit の後でよい。
    // seek だけは audio 差し替えの後になるため、失敗時は audio を元へ戻す。
    auto candidateSources = trackSources_;
    std::vector<preview::PreviewSourceId> newlyAdded;
    AudioSwitchUndo audioUndo;
    const auto rollback = [&] {
        if (!audioUndo.engineReset) {
            for (const auto source : newlyAdded) {
                // accepted composition が参照している最中は removeSource が拒否されうる。
                // 落とさず retirement queue へ回し、pollPreviewState に再試行させる。
                if (!previewEngine_->removeSource(source))
                    retiredSources_.push_back(source);
            }
        }
        QString revertError;
        if (!revertAudioSource(audioUndo, revertError)) {
            // 戻せなかったことを黙って握らない。呼び出し側が status を上書きするので、
            // error 文字列へ足して失われないようにする。
            error += QStringLiteral("\naudio sourceを元に戻せませんでした: ") + revertError;
        }
    };

    std::vector<int> desiredSlots;
    for (const auto& layer : mappedFrame.layers) {
        // source はトランジションで延ばした区間の clip で作る (source は in より前の素材を
        // 写せないので、incoming の頭の区間は延ばした in から始める)。
        const auto& clip = layer.renderClip;
        desiredSlots.push_back(layer.slot);
        const auto existing = candidateSources.find(layer.slot);
        if (existing != candidateSources.end() &&
            previewVideoMappingCovers(project_, existing->second.mapping, clip)) {
            existing->second.clipId = clip.id;
            existing->second.clipIndex = layer.clipIndex;
            continue;
        }
        if (!std::filesystem::is_regular_file(clip.mediaPath)) {
            error = QString::fromStdString(clip.name) +
                    QStringLiteral(" のファイルがありません: ") + fromPath(clip.mediaPath);
            rollback();
            return false;
        }
        const auto videoSource = project::clipVideoSource(clip);
        if (videoSource.sourceFpsNum > std::numeric_limits<std::uint32_t>::max() ||
            videoSource.sourceFpsDen > std::numeric_limits<std::uint32_t>::max()) {
            error = QStringLiteral("source FPSをpreview descriptorへ格納できません");
            rollback();
            return false;
        }
        preview::PreviewSourceDescriptor descriptor = previewVideoDescriptorOf(project_, clip);
        const auto added = previewEngine_->addSource(descriptor);
        if (!added) {
            error = previewErrorText(added.error());
            rollback();
            return false;
        }
        newlyAdded.push_back(added.value());
        candidateSources[layer.slot] = TrackPreviewSource{added.value(), clip.id, layer.clipIndex,
                                                          previewVideoMappingOf(clip)};
    }
    for (auto entry = candidateSources.begin(); entry != candidateSources.end();) {
        if (std::find(desiredSlots.begin(), desiredSlots.end(), entry->first) == desiredSlots.end())
            entry = candidateSources.erase(entry);
        else
            ++entry;
    }

    preview::PreviewFrameRequest request;
    const auto composition = previewCompositionFor(mappedFrame, candidateSources, request, error);
    if (!composition) {
        rollback();
        return false;
    }
    // 合成が変形の mask の読み込みを始めた・上限に収まらなかったことを inspector へ出す。
    refreshSelectedMathTransformStatus();
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted) {
        error = previewErrorText(submitted.error());
        rollback();
        return false;
    }
    submittedComposition_ = composition;
    // audio の差し替えはここまでの失敗を通り抜けてから行う。
    if (!applyAudioSourceFor(timelineFrame, audioUndo, error)) {
        rollback();
        return false;
    }
    const auto sought = previewEngine_->seekFrameRequest(request);
    if (!sought) {
        error = previewErrorText(sought.error());
        // scrub は Seeking 中の reject を 40ms ごとに retry する。ここで rollback
        // しないと、retry のたびに source が積み上がって登録上限に達する。
        // audio も同じ境界で戻す。video だけ戻して audio が新しいまま、にしない。
        rollback();
        return false;
    }
    for (const auto& [trackIndex, slot] : trackSources_) {
        const auto kept = candidateSources.find(trackIndex);
        if (kept == candidateSources.end() || kept->second.source != slot.source)
            retiredSources_.push_back(slot.source);
    }
    trackSources_ = std::move(candidateSources);
    currentSource_.reset();
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == currentClipIndex_)
            currentSource_ = slot.source;
    }
    error.clear();
    return true;
}

bool MvmController::generateManimClip(const QUrl& scriptUrl, const QString& sceneName) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (hasManimAsset()) {
        setStatus(QStringLiteral("Manim assetはすでに存在します"));
        return false;
    }
    const QString localScript =
        scriptUrl.isLocalFile() ? scriptUrl.toLocalFile() : scriptUrl.toString();
    return generateAndInstallManimClip(std::filesystem::path(localScript.toStdWString()), sceneName,
                                       true);
}

bool MvmController::regenerateManimClip() {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (project_.manimAssets.empty()) {
        setStatus(QStringLiteral("再生成するManim assetがありません"));
        return false;
    }
    const project::ManimAsset& asset = project_.manimAssets.front();
    return generateAndInstallManimClip(asset.scriptPath, QString::fromStdString(asset.sceneName),
                                       false);
}

bool MvmController::generateAndInstallManimClip(const std::filesystem::path& scriptPath,
                                                const QString& sceneName,
                                                bool requirePreviewReady) {
    if (busy_)
        return false;
    const QString localScript = fromPath(scriptPath);
    const QFileInfo scriptInfo(localScript);
    const QString trimmedScene = sceneName.trimmed();
    if (!scriptInfo.exists() || !scriptInfo.isFile() ||
        scriptInfo.suffix().compare(QStringLiteral("py"), Qt::CaseInsensitive) != 0) {
        setStatus(QStringLiteral("存在する.py scriptを選択してください"));
        return false;
    }
    if (trimmedScene.isEmpty()) {
        setStatus(QStringLiteral("Scene class名を入力してください"));
        return false;
    }
    if (requirePreviewReady && !previewReady_) {
        setStatus(QStringLiteral("Previewの準備が完了していません"));
        return false;
    }
    if (project_.timelineFpsDen != 1) {
        setStatus(QStringLiteral("Manimは整数fpsでしか生成できません。現在のProject frame rate (") +
                  timelineFpsText() +
                  QStringLiteral(") ではProjectと一致するclipを作れないため生成しません"));
        return false;
    }

    if (!projectLockHeld_) {
        setStatus(QStringLiteral("Projectを排他できないため、Manimを生成できません"));
        return false;
    }

    const bool addTimelinePlacement = project_.manimAssets.empty();

    busy_ = true;
    statusText_ = QStringLiteral("Manimを生成しています…");
    Q_EMIT stateChanged();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    ManimClipGenerationRequest request;
    request.manimExecutablePath = manimExecutablePath_;
    request.projectPath = projectPath_;
    request.scriptPath = scriptPath;
    request.sceneName = trimmedScene.toStdString();
    request.width = project_.outputWidth;
    request.height = project_.outputHeight;
    // Manim へ渡せる fps は整数だけである。1001 分母の Project で整数へ丸めると、
    // 生成物は 30/1 になり Project の 30000/1001 と一致せず「Preview未対応」の
    // clip ができる。丸めて対応したことにせず、ここで fail-closed にする。
    request.fps = static_cast<int>(project_.timelineFpsNum / project_.timelineFpsDen);
    UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_, currentRevision_};
    const ManimClipGenerationResult generated = mvm::app::generateManimClip(project_, request);

    busy_ = false;
    if (!generated.success) {
        QString detail = QString::fromStdString(generated.error);
        if (!generated.stderrText.empty())
            detail += QStringLiteral("\n") + QString::fromStdString(generated.stderrText);
        setStatus(detail);
        return false;
    }

    // 生成はworking stateだけを更新する。canonicalへは明示保存まで書かない。
    pushUndoEntry(std::move(undo));
    currentRevision_ = nextRevision_++;
    scheduleRecoveryAutosave();

    syncFirstManimAsset();
    if (!syncManimTimelineClip(addTimelinePlacement))
        return false;
    const int manimIndex = indexOfManimClip(project_.timelineClips);
    if (manimIndex < 0) {
        setStatus(QStringLiteral("Manim assetを生成しましたがtimelineには配置されていません"));
        return true;
    }
    const QString clipName = trimmedScene + QStringLiteral(" — ") +
                             QFileInfo(fromPath(generated.outputVideoPath)).fileName();
    const preview::PreviewEngineState previewState = previewEngine_->status().state;
    if (previewState == preview::PreviewEngineState::ReadyPaused ||
        previewState == preview::PreviewEngineState::Playing) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
        return installVideoClip(generated.outputVideoPath, clipName, manimIndex,
                                clip.sourceInFrame);
    }

    const auto& clip = project_.timelineClips[static_cast<std::size_t>(manimIndex)];
    queueVideoClipInstall(generated.outputVideoPath, clipName, manimIndex, clip.sourceInFrame);
    if (previewState == preview::PreviewEngineState::ShuttingDown ||
        previewState == preview::PreviewEngineState::Shutdown ||
        previewState == preview::PreviewEngineState::Error) {
        if (!resetPreviewEngine()) {
            pendingVideoPath_.reset();
            pendingClipName_.clear();
            pendingClipIndex_ = -1;
            return false;
        }
    }
    statusText_ = QStringLiteral("生成済みclipのPreviewを準備しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::addManimToTimeline() {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (project_.manimAssets.empty()) {
        setStatus(QStringLiteral("timelineへ追加するManim assetがありません"));
        return false;
    }
    if (hasManimTimelineClip()) {
        setStatus(QStringLiteral("Manim assetはすでにtimelineへ配置されています"));
        return false;
    }

    const project::ManimAsset& asset = project_.manimAssets.front();
    if (!std::filesystem::is_regular_file(asset.generatedVideoPath)) {
        setStatus(QStringLiteral("生成済みManim videoがありません: ") +
                  fromPath(asset.generatedVideoPath));
        return false;
    }

    project::Project candidate = project_;
    const ProbedMedia media = probeMedia(asset.generatedVideoPath);
    if (!media.success) {
        setStatus(media.error);
        return false;
    }
    const int overlayTrack = candidate.videoTracks.size() > 1 ? 1 : 0;
    const project::TimelineEditResult placed = project::appendManimTimelineClipAt(
        candidate, candidate.manimAssets.front(), newClipId(), media.fpsNum, media.fpsDen,
        media.frameCount, playheadFrame_,
        project::TrackRef{project::TrackKind::Video, overlayTrack});
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    Q_EMIT stateChanged();
    return selectClip(placed.selectedIndex);
}

QVariantMap MvmController::textClipData(const QString& clipId) const {
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind != project::TimelineClipKind::Text ||
            QString::fromStdString(clip.id) != clipId)
            continue;
        const auto& data = clip.text;
        return {{QStringLiteral("clipId"), clipId},
                {QStringLiteral("content"), QString::fromStdString(data.content)},
                {QStringLiteral("fontFamily"), QString::fromStdString(data.fontFamily)},
                {QStringLiteral("fontSize"), data.fontSize},
                {QStringLiteral("x"), data.x},
                {QStringLiteral("y"), data.y},
                {QStringLiteral("color"), QString::fromStdString(data.color)},
                {QStringLiteral("bold"), data.bold},
                {QStringLiteral("alignment"), QString::fromStdString(data.alignment)},
                {QStringLiteral("outlineColor"), QString::fromStdString(data.outlineColor)},
                {QStringLiteral("outlineWidth"), data.outlineWidth},
                {QStringLiteral("backgroundColor"), QString::fromStdString(data.backgroundColor)}};
    }
    return {};
}

QVariantMap MvmController::selectedTextClip() const {
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return {};
    return textClipData(QString::fromStdString(
        project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id));
}

bool MvmController::previewVideoAtPlayhead() const {
    const auto active = project::activeClipsAt(project_, project::TrackKind::Video, playheadFrame_);
    for (std::size_t index = 0; index < active.size(); ++index)
        if (active[index] && active[index]->enabled &&
            !project::isStillClipKind(active[index]->kind) &&
            project::isTrackOutputEnabled(project_,
                                          {project::TrackKind::Video, static_cast<int>(index)}))
            return true;
    return false;
}

bool MvmController::createTextClip(const QString& content, int x, int y) {
    if (busy_ || content.trimmed().isEmpty() || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Text;
    clip.id = newClipId();
    clip.name = content.simplified().left(32).toStdString();
    clip.sourceFpsNum = candidate.timelineFpsNum;
    clip.sourceFpsDen = candidate.timelineFpsDen;
    clip.sourceFrameCount =
        project::defaultStillClipFrames(candidate.timelineFpsNum, candidate.timelineFpsDen);
    clip.sourceOutFrame = clip.sourceFrameCount;
    clip.text.content = content.toStdString();
    clip.text.x = x;
    clip.text.y = y;
    QString rasterError;
    if (renderTextRaster(clip.text, candidate.outputWidth, candidate.outputHeight, rasterError)
            .isNull()) {
        setStatus(rasterError);
        return false;
    }
    const auto placed = project::placeStillClipAt(candidate, std::move(clip), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("文字 clip を作成できません: ")))
        return false;
    Q_EMIT stateChanged();
    refreshTextPreview();
    return selectClip(placed.selectedIndex);
}

namespace {

// QML から来た書式の変更を TextClipData へ反映する。確定 (updateTextClip) と
// ドラッグ中の preview (previewTextClip) の両方がこれを使う。
void applyTextValues(project::TextClipData& data, const QVariantMap& values) {
    if (values.contains(QStringLiteral("content")))
        data.content = values.value(QStringLiteral("content")).toString().toStdString();
    if (values.contains(QStringLiteral("fontFamily")))
        data.fontFamily = values.value(QStringLiteral("fontFamily")).toString().toStdString();
    if (values.contains(QStringLiteral("fontSize")))
        data.fontSize = values.value(QStringLiteral("fontSize")).toInt();
    if (values.contains(QStringLiteral("x")))
        data.x = values.value(QStringLiteral("x")).toInt();
    if (values.contains(QStringLiteral("y")))
        data.y = values.value(QStringLiteral("y")).toInt();
    if (values.contains(QStringLiteral("color")))
        data.color = values.value(QStringLiteral("color")).toString().toStdString();
    if (values.contains(QStringLiteral("bold")))
        data.bold = values.value(QStringLiteral("bold")).toBool();
    if (values.contains(QStringLiteral("alignment")))
        data.alignment = values.value(QStringLiteral("alignment")).toString().toStdString();
    if (values.contains(QStringLiteral("outlineColor")))
        data.outlineColor = values.value(QStringLiteral("outlineColor")).toString().toStdString();
    if (values.contains(QStringLiteral("outlineWidth")))
        data.outlineWidth = values.value(QStringLiteral("outlineWidth")).toInt();
    if (values.contains(QStringLiteral("backgroundColor")))
        data.backgroundColor =
            values.value(QStringLiteral("backgroundColor")).toString().toStdString();
}

} // namespace

bool MvmController::updateTextClip(const QString& clipId, const QVariantMap& values) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto id = clipId.toStdString();
    const auto found = std::find_if(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == candidate.timelineClips.end() || found->kind != project::TimelineClipKind::Text) {
        setStatus(QStringLiteral("編集する文字 clip がありません"));
        return false;
    }
    auto& data = found->text;
    applyTextValues(data, values);
    if (data.content.empty()) {
        setStatus(QStringLiteral("文字 clip の本文を空にはできません"));
        return false;
    }
    found->name = QString::fromStdString(data.content).simplified().left(32).toStdString();
    QString rasterError;
    if (renderTextRaster(data, candidate.outputWidth, candidate.outputHeight, rasterError)
            .isNull()) {
        setStatus(rasterError);
        return false;
    }
    // 確定したらドラッグ中の preview は不要になる。Project の値へ戻してから保存する。
    textPreviewOverride_.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("文字 clip を更新できません: ")))
        return false;
    Q_EMIT stateChanged();
    refreshTextPreview();
    return true;
}

bool MvmController::placeTextClip(const QString& clipId, const QString& alignment) {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0 || project_.timelineClips[static_cast<std::size_t>(index)].kind !=
                         project::TimelineClipKind::Text) {
        setStatus(QStringLiteral("配置する文字 clip がありません"));
        return false;
    }
    // 揃えを変えると行の並びが変わるので、揃えを変えた後の書式で位置を求める。
    project::TextClipData data = project_.timelineClips[static_cast<std::size_t>(index)].text;
    data.alignment = alignment.toStdString();
    const auto placement =
        textPresetPlacement(data, project_.outputWidth, project_.outputHeight, data.alignment);
    if (!placement.success) {
        setStatus(placement.error);
        return false;
    }
    return updateTextClip(clipId, {{QStringLiteral("alignment"), alignment},
                                   {QStringLiteral("x"), placement.x},
                                   {QStringLiteral("y"), placement.y}});
}

bool MvmController::previewTextClip(const QString& clipId, const QVariantMap& values) {
    if (busy_ || playing_)
        return false;
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0 || project_.timelineClips[static_cast<std::size_t>(index)].kind !=
                         project::TimelineClipKind::Text)
        return false;
    // 同じ clip のドラッグ中なら、前回の preview に重ねる (X と Y を別々に動かす場合など)。
    project::TextClipData data =
        textPreviewOverride_ && textPreviewOverride_->first == clipId.toStdString()
            ? textPreviewOverride_->second
            : project_.timelineClips[static_cast<std::size_t>(index)].text;
    applyTextValues(data, values);
    QString rasterError;
    if (data.content.empty() ||
        renderTextRaster(data, project_.outputWidth, project_.outputHeight, rasterError).isNull())
        return false;
    textPreviewOverride_ = std::make_pair(clipId.toStdString(), std::move(data));
    applyTextPreview(clipId);
    return true;
}

void MvmController::cancelTextPreview() {
    if (!textPreviewOverride_)
        return;
    const QString clipId = QString::fromStdString(textPreviewOverride_->first);
    textPreviewOverride_.reset();
    applyTextPreview(clipId);
}

void MvmController::applyTextPreview(const QString& clipId) {
    // 画像の cache は clip ID を key にしているので、その clip の分だけ捨てて描き直させる。
    textRasterImages_.remove(clipId);
    textStillImages_.remove(clipId);
    textRasterBounds_.remove(clipId);
    ++textPreviewSerial_;
    Q_EMIT stateChanged();
    refreshTextPreview();
}

void MvmController::refreshTextPreview() {
    textPreviewRefreshPending_ = false;
    if (playing_)
        return;
    // ドラッグ中や、保存の直後に続けて組み直すときは、前の seek の完了前に次の要求が来る。
    // ReadyPaused 以外では失敗にせず、ReadyPaused になったら最新の状態で 1 回だけ
    // 描き直す (pollPreviewState)。
    if (previewEngine_->status().state != preview::PreviewEngineState::ReadyPaused) {
        textPreviewRefreshPending_ = true;
        return;
    }
    QString error;
    if (!syncPreviewSourcesAt(playheadFrame_, error))
        setStatus(QStringLiteral("文字のPreview更新に失敗しました: ") + error);
}

const QImage* MvmController::textRasterImage(int clipIndex, QString& error) const {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size()) ||
        project_.timelineClips[static_cast<std::size_t>(clipIndex)].kind !=
            project::TimelineClipKind::Text) {
        error = QStringLiteral("文字 clip ではありません");
        return nullptr;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    // Project を変えると refreshTimelineModel が cache を捨てるので、key は clip ID で足りる。
    // ドラッグ中の preview は applyTextPreview がその clip の分を捨てる。
    const QString key = QString::fromStdString(clip.id);
    const auto found = textRasterImages_.constFind(key);
    if (found != textRasterImages_.constEnd())
        return &found.value();
    const project::TextClipData& data =
        textPreviewOverride_ && textPreviewOverride_->first == clip.id
            ? textPreviewOverride_->second
            : clip.text;
    const QImage image = renderTextRaster(data, project_.outputWidth, project_.outputHeight, error);
    if (image.isNull())
        return nullptr;
    return &textRasterImages_.insert(key, image).value();
}

QRect MvmController::textRasterBounds(int clipIndex) const {
    QString rasterError;
    const QImage* raster = textRasterImage(clipIndex, rasterError);
    if (!raster)
        return {};
    const QString key =
        QString::fromStdString(project_.timelineClips[static_cast<std::size_t>(clipIndex)].id);
    if (const auto found = textRasterBounds_.constFind(key); found != textRasterBounds_.constEnd())
        return found.value();
    // 不透明な画素 (文字・縁取り・背景) を囲む最小の矩形。
    int left = raster->width();
    int top = raster->height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < raster->height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(raster->constScanLine(y));
        for (int x = 0; x < raster->width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    const QRect bounds = right < 0 ? QRect{} : QRect(QPoint(left, top), QPoint(right, bottom));
    textRasterBounds_.insert(key, bounds);
    return bounds;
}

QRect MvmController::textClipBounds(const QString& clipId) const {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    return index < 0 ? QRect{} : textRasterBounds(index);
}

std::shared_ptr<const preview::PreviewStillImage>
MvmController::textStillImage(int clipIndex, QString& error) const {
    const QImage* raster = textRasterImage(clipIndex, error);
    if (!raster)
        return nullptr;
    const QString key =
        QString::fromStdString(project_.timelineClips[static_cast<std::size_t>(clipIndex)].id);
    if (const auto found = textStillImages_.constFind(key); found != textStillImages_.constEnd())
        return found.value();
    // engine は straight alpha の RGBA8 を受け取る。書き出しの PNG も straight alpha である。
    const QImage straight = raster->convertToFormat(QImage::Format_RGBA8888);
    auto still = std::make_shared<preview::PreviewStillImage>();
    still->width = straight.width();
    still->height = straight.height();
    const auto rowBytes = static_cast<std::size_t>(straight.width()) * 4U;
    still->rgba.resize(rowBytes * static_cast<std::size_t>(straight.height()));
    for (int y = 0; y < straight.height(); ++y)
        std::memcpy(still->rgba.data() + rowBytes * static_cast<std::size_t>(y),
                    straight.constScanLine(y), rowBytes);
    textStillImages_.insert(key, still);
    return still;
}

std::shared_ptr<const preview::PreviewStillImage>
MvmController::imageStillImage(int clipIndex, QString& error, bool& pending) const {
    pending = false;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size()) ||
        project_.timelineClips[static_cast<std::size_t>(clipIndex)].kind !=
            project::TimelineClipKind::Image) {
        error = QStringLiteral("画像 clip ではありません");
        return nullptr;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    // 書き出しと同じ decoder と配置 (media/still_image) を worker で通し、同じ画素を使う。
    const auto entry =
        imageRasters_->request(clip.mediaPath, project_.outputWidth, project_.outputHeight);
    switch (entry.state) {
    case ImageRasterCache::State::Loading:
        pending = true;
        return nullptr;
    case ImageRasterCache::State::Ready:
        return entry.image;
    case ImageRasterCache::State::Failed:
        break;
    }
    error = QString::fromStdString(clip.name) + QStringLiteral(" を読めません: ") + entry.error;
    return nullptr;
}

void MvmController::revalidateMedia() {
    if (imageRasters_)
        imageRasters_->revalidateAll();
}

std::filesystem::path MvmController::mathCacheDirectory() const {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(projectPath_, error);
    const auto project = error ? projectPath_ : absolute;
    // 同じ directory の別の .mvm とは cache を分ける。Project lock は file ごとなので、
    // cache を共有すると他の Project の作業 directory を消しうる。
    return project.parent_path() / L"cache" / L"math" / project.filename();
}

void MvmController::syncMathCacheAuthority() {
    if (graphRasters_)
        graphRasters_->setAuthority(mathCacheDirectory().parent_path().parent_path() / L"graph" /
                                        projectPath_.filename(),
                                    projectLockHeld_);
    if (!mathRasters_)
        return;
    mathRasters_->setAuthority(
        mathCacheDirectory(), projectLockHeld_,
        projectLockHeld_
            ? QString()
            : QStringLiteral("この Project は他のプロセスが編集中のため、数式を描画しません"));
}

project::MathClipData MvmController::effectiveMathData(const project::TimelineClip& clip) const {
    if (mathPreviewOverride_ && mathPreviewOverride_->first == clip.id)
        return mathPreviewOverride_->second;
    return clip.math;
}

void MvmController::requestMathRenders() {
    if (graphRasters_) {
        graphAnimations_.removeIf([&](const auto& item) {
            return std::none_of(project_.timelineClips.begin(), project_.timelineClips.end(),
                                [&](const auto& clip) {
                                    return QString::fromStdString(clip.id) == item.key() &&
                                           clip == item.value().clip;
                                });
        });
        QSet<QString> keys;
        std::vector<graph::GraphRenderSpec> specs;
        for (const auto& clip : project_.timelineClips) {
            if (clip.kind != project::TimelineClipKind::Graph || !clip.enabled)
                continue;
            auto compiled = compileGraphRender(clip.graph, clip.sourceFrameCount,
                                               project_.outputWidth, project_.outputHeight);
            if (auto* spec = std::get_if<graph::GraphRenderSpec>(&compiled)) {
                keys.insert(graphRasters_->keyFor(*spec));
                specs.push_back(std::move(*spec));
            }
        }
        graphRasters_->retainOnly(keys);
        for (const auto& spec : specs)
            graphRasters_->request(spec);
    }
    if (!mathRasters_)
        return;
    // 入力中の式を最優先にし、次に再生位置に掛かる clip の静止・Write・変形、残りの静止、残りの
    // Write、残りの変形の順に要求する (worker は 1 本で、要求の順に描く)。Write の連番と変形は
    // 確定した値だけを描く (入力中の式は静止だけを見せる)。変形は Project にある変形をすべて
    // 要求する (Write と同じく書き出しの前に disk に揃えておくため)。preview 用の memory への
    // 読み込みは、前・後ろの clip が見える frame の合成だけが要求する。
    std::vector<math::MathRenderSpec> statics;
    std::vector<math::MathRenderSpec> laterStatics;
    std::vector<math::MathSequenceSpec> writes;
    std::vector<math::MathSequenceSpec> laterWrites;
    std::vector<math::MathTransformSpec> transforms;
    std::vector<math::MathTransformSpec> laterTransforms;
    if (mathPreviewOverride_)
        statics.push_back(mathRenderSpecFor(mathPreviewOverride_->second));
    const auto atPlayhead = [&](const project::TimelineClip& clip) {
        return clip.timelineStartFrame <= playheadFrame_ &&
               playheadFrame_ <
                   clip.timelineStartFrame + (clip.sourceOutFrame - clip.sourceInFrame);
    };
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind != project::TimelineClipKind::Math)
            continue;
        const bool current = atPlayhead(clip);
        (current ? statics : laterStatics).push_back(mathRenderSpecFor(clip.math));
        if (const auto write = mathSequenceSpecFor(clip))
            (current ? writes : laterWrites).push_back(*write);
    }
    for (const auto& transition : project_.timelineTransitions) {
        const int outgoing = indexOfClipId(project_.timelineClips, transition.outgoingClipId);
        const int incoming = indexOfClipId(project_.timelineClips, transition.incomingClipId);
        if (outgoing < 0 || incoming < 0)
            continue;
        const auto& source = project_.timelineClips[static_cast<std::size_t>(outgoing)];
        const auto& target = project_.timelineClips[static_cast<std::size_t>(incoming)];
        const auto spec = mathTransformSpecFor(transition, source, target);
        if (!spec)
            continue;
        // 再生位置がどちらかの clip に掛かる変形は、両端の今の静止も先に描かせる (変形は
        // 両端の静止を待つ)。
        const bool current = atPlayhead(source) || atPlayhead(target);
        if (current) {
            statics.push_back(spec->source);
            statics.push_back(spec->target);
        }
        (current ? transforms : laterTransforms).push_back(*spec);
    }
    // Equation Sequence (P3-4): 再生位置に掛かる (preview に見える) clip だけを要求する。Project に
    // あるだけの sequence を先回りして描かない。残すのは今の sequence の key と、その全状態の今の
    // 静止 (代用と描画の入力)。preview 用の層の key は sequence の key の下なので一緒に残る。
    // compile できない sequence も静止は要求する (代用に使う)。無効にした clip と出力しない track
    // の clip は preview にも書き出しにも出ないので描かない (mapTimelinePreviewFrame と同じ条件)。
    std::vector<math::EquationSequenceRenderSpec> sequences;
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind != project::TimelineClipKind::EquationSequence || !clip.enabled ||
            !project::isTrackOutputEnabled(project_, clip.track))
            continue;
        const auto duration = project::timelineClipDuration(project_, clip);
        if (!duration.success || playheadFrame_ < clip.timelineStartFrame ||
            playheadFrame_ >= clip.timelineStartFrame + duration.frame)
            continue;
        for (const auto& state : clip.equationSequence.states)
            statics.push_back(equationStateStaticSpec(state));
        const auto compiled = compiledEquationSequence(clip);
        std::string error;
        if (compiled.value)
            if (auto spec = equationSequenceRenderSpecFor(*compiled.value, error))
                sequences.push_back(std::move(*spec));
    }
    QSet<QString> keys;
    for (const auto* list : {&statics, &laterStatics})
        for (const auto& spec : *list)
            if (const QString key = mathRasters_->keyFor(spec); !key.isEmpty())
                keys.insert(key);
    for (const auto& spec : sequences)
        if (const QString key = mathRasters_->equationSequenceKeyFor(spec); !key.isEmpty())
            keys.insert(key);
    for (const auto* list : {&writes, &laterWrites})
        for (const auto& spec : *list)
            if (const QString key = mathRasters_->sequenceKeyFor(spec); !key.isEmpty())
                keys.insert(key);
    // 今の変形の key を残す (取り下げない)。端点・長さを変えた前の変形の key は残さない。
    for (const auto* list : {&transforms, &laterTransforms})
        for (const auto& spec : *list)
            if (const QString key = mathRasters_->transformKeyFor(spec); !key.isEmpty())
                keys.insert(key);
    // 使わなくなった式 (書き換えた前の式など) の描画は process ごと止める。
    mathRasters_->retainOnly(keys);
    // 入力中の式の静止がまだ描けていなければ、描きかけ・待ちの連番と変形を止めて先に描かせる
    // (長い Write や、裏で disk に揃えている変形が入力中の preview を待たせない)。止めた連番と
    // 変形はすぐ下で要求し直す。入力中の静止が先に要求済みなので、worker は静止を先に描く。
    if (mathPreviewOverride_ &&
        mathRasters_->request(statics.front()).state == MathRasterCache::State::Pending)
        mathRasters_->cancelPendingAnimations();
    for (const auto& spec : statics)
        mathRasters_->request(spec);
    for (const auto& spec : sequences)
        mathRasters_->requestEquationSequence(spec);
    for (const auto& spec : writes)
        mathRasters_->requestSequence(spec);
    for (const auto& spec : transforms)
        mathRasters_->requestTransform(spec);
    for (const auto& spec : laterStatics)
        mathRasters_->request(spec);
    for (const auto& spec : laterWrites)
        mathRasters_->requestSequence(spec);
    for (const auto& spec : laterTransforms)
        mathRasters_->requestTransform(spec);
}

std::optional<MvmController::MathTransformPreviewInputs>
MvmController::mathTransformPreviewInputs(const project::TimelineTransition& transition,
                                          QString* placementError) const {
    if (!mathRasters_ || !mathTransformIsRendered(project_, transition))
        return std::nullopt;
    const int outgoing = indexOfClipId(project_.timelineClips, transition.outgoingClipId);
    const int incoming = indexOfClipId(project_.timelineClips, transition.incomingClipId);
    if (outgoing < 0 || incoming < 0)
        return std::nullopt;
    const auto& source = project_.timelineClips[static_cast<std::size_t>(outgoing)];
    const auto& target = project_.timelineClips[static_cast<std::size_t>(incoming)];
    // 入力中の式は Project の式と違う。変形は確定した両端の式のものなので付けない (cut で見せる)。
    if (mathPreviewOverride_ &&
        (mathPreviewOverride_->first == source.id || mathPreviewOverride_->first == target.id))
        return std::nullopt;
    const auto spec = mathTransformSpecFor(transition, source, target);
    const auto window = mathTransformWindowFor(project_, transition);
    if (!spec || !window || window->frames != spec->frames)
        return std::nullopt;
    // 両端の今の式の静止 (前に描けた別の式の静止では置かない) と、検証済みの disk の変形。
    const auto sourceStatic = mathRasters_->request(spec->source);
    const auto targetStatic = mathRasters_->request(spec->target);
    if (sourceStatic.state != MathRasterCache::State::Ready || !sourceStatic.mask ||
        targetStatic.state != MathRasterCache::State::Ready || !targetStatic.mask ||
        mathRasters_->requestTransform(*spec).state != MathRasterCache::State::Ready)
        return std::nullopt;
    const auto artifact = mathRasters_->readyTransform(*spec);
    if (!artifact)
        return std::nullopt;
    MathTransformPreviewInputs inputs;
    inputs.spec = *spec;
    inputs.window = *window;
    if (!project::parseArgbColor(source.math.color, inputs.sourceColor) ||
        !project::parseArgbColor(target.math.color, inputs.targetColor))
        return std::nullopt;
    if (!math::mathTransformRasterPlacement(
            artifact->width, artifact->height, artifact->sourceX, artifact->sourceY,
            sourceStatic.mask->width, sourceStatic.mask->height, artifact->targetX,
            artifact->targetY, targetStatic.mask->width, targetStatic.mask->height,
            project_.outputWidth, project_.outputHeight, inputs.placement)) {
        if (placementError)
            *placementError =
                QStringLiteral(
                    "変形の途中の式が出力サイズ (%1x%2) に収まらないため、cut で表示します。"
                    "文字サイズを下げてください (変形 %3x%4)")
                    .arg(project_.outputWidth)
                    .arg(project_.outputHeight)
                    .arg(artifact->width)
                    .arg(artifact->height);
        return std::nullopt;
    }
    return inputs;
}

std::shared_ptr<const preview::PreviewStillAnimation> MvmController::mathPreviewAnimation(
    int clipIndex, const std::shared_ptr<const preview::PreviewStillImage>& still) const {
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    const QString clipId = QString::fromStdString(clip.id);
    const auto none = [&] {
        mathPreviewAnimations_.remove(clipId);
        return nullptr;
    };
    // 入力中の clip は静止だけを見せる (連番・変形は確定した式のもの)。
    if (!still || !mathRasters_ || (mathPreviewOverride_ && mathPreviewOverride_->first == clip.id))
        return none();
    // 下地の still が現在の式の静止の描画であること (last-good の古い式の上に重ねない)。
    const auto staticEntry = mathRasters_->request(mathRenderSpecFor(clip.math));
    if (staticEntry.state != MathRasterCache::State::Ready || !staticEntry.mask)
        return none();
    QString memo = QStringLiteral("%1|%2x%3|%4/%5|%6|%7-%8")
                       .arg(reinterpret_cast<quintptr>(still.get()))
                       .arg(still->width)
                       .arg(still->height)
                       .arg(project_.timelineFpsNum)
                       .arg(project_.timelineFpsDen)
                       .arg(clip.timelineStartFrame)
                       .arg(clip.sourceInFrame)
                       .arg(clip.sourceOutFrame);

    std::optional<MathClipPreviewAnimation::WritePart> write;
    if (const auto spec = mathSequenceSpecFor(clip)) {
        const auto sequence = mathRasters_->requestSequence(*spec);
        math::MathComposeStyle style;
        int left = 0;
        int top = 0;
        if (sequence.state == MathRasterCache::State::Ready &&
            sequence.width == staticEntry.mask->width &&
            sequence.height == staticEntry.mask->height && mathComposeStyleFor(clip.math, style) &&
            math::mathRasterPlacement(sequence.width, sequence.height, still->width, still->height,
                                      left, top)) {
            // preview 用の mask は全体の上限の中で memory に置く。読んでいる間・上限に収まらない
            // 間は静止を見せる (読めたら entryChanged で組み直す)。
            const auto resident = mathRasters_->residentSequence(*spec);
            if (resident.state == MathRasterCache::Residency::Resident && resident.frames) {
                write = MathClipPreviewAnimation::WritePart{
                    resident.frames, style,
                    preview::PreviewPixelRect{left, top, sequence.width, sequence.height}};
                memo +=
                    QStringLiteral("|W:") + mathRasters_->sequenceKeyFor(*spec) +
                    QStringLiteral("|%1|").arg(reinterpret_cast<quintptr>(resident.frames.get())) +
                    QString::fromStdString(clip.math.color) + QLatin1Char('|') +
                    QString::fromStdString(clip.math.backgroundColor) +
                    QStringLiteral("|%1/%2").arg(clip.sourceFpsNum).arg(clip.sourceFpsDen);
            }
        }
    }

    // この clip が前・後ろの端の変形。揃わない・memory に無い変形は付けず、その区間は cut
    // で見せる。 前の端の layer にも後ろの端の layer にも同じ区間の部分を付ける (state は出力 frame
    // から決まる)。
    std::vector<MathClipPreviewAnimation::TransformPart> transforms;
    for (const auto& transition : project_.timelineTransitions) {
        if (transition.kind != project::TransitionKind::MathTransform ||
            (transition.outgoingClipId != clip.id && transition.incomingClipId != clip.id))
            continue;
        const auto inputs = mathTransformPreviewInputs(transition);
        if (!inputs)
            continue;
        const auto resident = mathRasters_->residentTransform(inputs->spec);
        if (resident.state != MathRasterCache::Residency::Resident || !resident.frames ||
            static_cast<std::int64_t>(resident.frames->frames.size()) != inputs->window.frames)
            continue;
        transforms.push_back({transition.id, inputs->window, resident.frames, inputs->placement,
                              inputs->sourceColor, inputs->targetColor});
        memo += QStringLiteral("|T:%1|%2|%3|%4+%5|%6,%7>%8,%9|%10>%11")
                    .arg(QString::fromStdString(transition.id))
                    .arg(mathRasters_->transformKeyFor(inputs->spec))
                    .arg(reinterpret_cast<quintptr>(resident.frames.get()))
                    .arg(inputs->window.start)
                    .arg(inputs->window.frames)
                    .arg(inputs->placement.sourceLeft)
                    .arg(inputs->placement.sourceTop)
                    .arg(inputs->placement.targetLeft)
                    .arg(inputs->placement.targetTop)
                    .arg(inputs->sourceColor)
                    .arg(inputs->targetColor);
    }
    if (!write && transforms.empty())
        return none();
    if (const auto found = mathPreviewAnimations_.constFind(clipId);
        found != mathPreviewAnimations_.constEnd() && found->memo == memo)
        return found->animation;
    auto animation = std::make_shared<MathClipPreviewAnimation>(
        clip, project_.timelineFpsNum, project_.timelineFpsDen, still, std::move(write),
        std::move(transforms), mathWriteObserverForTest_, mathTransformObserverForTest_);
    mathPreviewAnimations_.insert(clipId, {memo, animation});
    return animation;
}

// ---- Equation Sequence (P3-4) ----

namespace {

MathRasterCache::EquationLayerRef equationLayerRef(const EquationPreviewLayerId& id) {
    MathRasterCache::EquationLayerRef ref;
    ref.role = id.role == EquationPreviewLayerRole::TransitionFrame
                   ? MathRasterCache::EquationLayerRole::TransitionFrame
               : id.role == EquationPreviewLayerRole::ActionBase
                   ? MathRasterCache::EquationLayerRole::ActionBase
                   : MathRasterCache::EquationLayerRole::ActionAccent;
    ref.interval = id.interval;
    ref.frame = id.frame;
    return ref;
}

std::vector<MathRasterCache::EquationLayerRef>
equationLayerRefs(const std::vector<EquationPreviewLayerId>& ids) {
    std::vector<MathRasterCache::EquationLayerRef> refs;
    for (const auto& id : ids)
        refs.push_back(equationLayerRef(id));
    return refs;
}

// 束の層を model の入力へ置く (層の被覆は常駐の mask の参照)。
void placeEquationLayers(EquationPreviewInputs& in, const std::vector<EquationPreviewLayerId>& ids,
                         const MathRasterCache::ResidentEquationLayers& resident, QString& memo) {
    for (std::size_t k = 0; k < ids.size() && k < resident.layers.size(); ++k) {
        const auto& layer = resident.layers[k];
        if (!layer || layer->frames.size() != 1)
            continue;
        const EquationPreviewCoverage coverage(layer, &layer->frames.front());
        const auto& id = ids[k];
        switch (id.role) {
        case EquationPreviewLayerRole::TransitionFrame:
            in.transitionFrames[{id.interval, id.frame}] = coverage;
            break;
        case EquationPreviewLayerRole::ActionBase:
            in.actionBases[id.interval] = coverage;
            break;
        case EquationPreviewLayerRole::ActionAccent:
            in.actionAccents[{id.interval, id.frame}] = coverage;
            break;
        }
        memo += QStringLiteral("|%1:%2:%3@%4")
                    .arg(static_cast<int>(id.role))
                    .arg(id.interval)
                    .arg(id.frame)
                    .arg(reinterpret_cast<quintptr>(layer.get()));
    }
}

// 先読みの束の数の上限 (広い先読みはしない。今の区間の残りと次の区間だけ)。
constexpr std::size_t kEquationPrefetchBundles = 240;

} // namespace

EquationCompileResult<EquationSequenceSpec>
MvmController::compiledEquationSequence(const project::TimelineClip& clip) const {
    const QString id = QString::fromStdString(clip.id);
    // 今の data の compile だけを使う (data が変わったら前の成功を正にしない)。
    if (const auto found = equationCompiles_.constFind(id);
        found != equationCompiles_.constEnd() && found->data == clip.equationSequence)
        return found->result;
    auto result = compileEquationSequence(clip.equationSequence);
    equationCompiles_.insert(id, {clip.equationSequence, result});
    return result;
}

math::MathRenderSpec MvmController::equationStateStaticSpec(const project::EquationState& state) {
    return mathRenderSpecFor(state.equation);
}

std::shared_ptr<const preview::PreviewStillImage> MvmController::equationTransparentOutput() const {
    if (!equationTransparent_ || equationTransparent_->width != project_.outputWidth ||
        equationTransparent_->height != project_.outputHeight) {
        auto image = std::make_shared<preview::PreviewStillImage>();
        image->width = project_.outputWidth;
        image->height = project_.outputHeight;
        image->rgba.assign(static_cast<std::size_t>(project_.outputWidth) *
                               static_cast<std::size_t>(project_.outputHeight) * 4U,
                           0);
        equationTransparent_ = std::move(image);
    }
    return equationTransparent_;
}

std::shared_ptr<const preview::PreviewStillAnimation>
MvmController::equationSequencePreviewAnimation(int clipIndex, std::int64_t outputFrame) const {
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    const QString clipId = QString::fromStdString(clip.id);
    const auto none = [&] {
        equationPreviewAnimations_.remove(clipId);
        return nullptr;
    };
    if (!mathRasters_ || clip.kind != project::TimelineClipKind::EquationSequence)
        return none();
    EquationPreviewInputs in;
    in.clip = clip;
    in.timelineFpsNum = project_.timelineFpsNum;
    in.timelineFpsDen = project_.timelineFpsDen;
    in.outputWidth = project_.outputWidth;
    in.outputHeight = project_.outputHeight;
    const auto compiled = compiledEquationSequence(clip);
    in.spec = compiled.value;
    QString memo = QStringLiteral("%1/%2|%3x%4|%5|%6-%7|c%8")
                       .arg(project_.timelineFpsNum)
                       .arg(project_.timelineFpsDen)
                       .arg(project_.outputWidth)
                       .arg(project_.outputHeight)
                       .arg(clip.timelineStartFrame)
                       .arg(clip.sourceInFrame)
                       .arg(clip.sourceOutFrame)
                       .arg(static_cast<int>(compiled.failure));

    // 各状態の今の静止 (通常の Math と同じ key・色)。描けていない状態は代用もしない (別の式の
    // 静止や前に描けた静止で埋めない)。
    std::vector<std::string> staticKeys;
    for (const auto& state : clip.equationSequence.states) {
        const auto spec = equationStateStaticSpec(state);
        const auto entry = mathRasters_->request(spec);
        const QString key = mathRasters_->keyFor(spec);
        staticKeys.push_back(key.toStdString());
        math::MathComposeStyle style;
        if (entry.state != MathRasterCache::State::Ready || !entry.mask || key.isEmpty() ||
            !mathComposeStyleFor(state.equation, style)) {
            in.statics.emplace_back();
            memo += QStringLiteral("|S-");
            continue;
        }
        auto coverage = equationStaticCoverage_.value(key);
        if (!coverage) {
            std::vector<std::uint8_t> alpha(static_cast<std::size_t>(entry.mask->width) *
                                            static_cast<std::size_t>(entry.mask->height));
            for (std::size_t at = 0; at < alpha.size() && at * 4U + 3U < entry.mask->rgba.size();
                 ++at)
                alpha[at] = entry.mask->rgba[at * 4U + 3U];
            coverage = std::make_shared<const std::vector<std::uint8_t>>(std::move(alpha));
            equationStaticCoverage_.insert(key, coverage);
        }
        in.statics.push_back(EquationPreviewStatic{entry.mask->width, entry.mask->height, coverage,
                                                   style.colorArgb});
        memo += QStringLiteral("|S%1#%2").arg(key).arg(style.colorArgb);
    }

    // 今の spec の disk の artifact (Ready で、今の静止の key で検証したもの) と層。
    std::string renderError;
    const auto renderSpec =
        in.spec ? equationSequenceRenderSpecFor(*in.spec, renderError) : std::nullopt;
    if (renderSpec &&
        mathRasters_->requestEquationSequence(*renderSpec).state == MathRasterCache::State::Ready) {
        const auto artifact = mathRasters_->equationSequenceArtifactOf(*renderSpec);
        // 静止が変わった (key が違う) artifact は使わない。
        if (artifact && artifact->stateStaticKeys == staticKeys) {
            in.artifactReady = true;
            for (const auto& item : artifact->transitions) {
                EquationPreviewTransitionArtifact out{
                    item.width,   item.height, item.sourceX, item.sourceY, item.targetX,
                    item.targetY, {}};
                for (const auto& frame : item.frames)
                    out.colors.push_back(frame.colorArgb); // provenance の色
                in.transitions.push_back(std::move(out));
            }
            for (const auto& item : artifact->actions) {
                EquationPreviewActionArtifact out{item.width,   item.height,         item.staticX,
                                                  item.staticY, item.base.colorArgb, {}};
                for (const auto& frame : item.accent)
                    out.accentColors.push_back(frame.colorArgb);
                in.actions.push_back(std::move(out));
            }
            memo += QStringLiteral("|Q") + mathRasters_->equationSequenceKeyFor(*renderSpec);
            std::string timeError;
            const auto time =
                equationPreviewTimeAt(clip, project_.timelineFpsNum, project_.timelineFpsDen,
                                      &*in.spec, outputFrame, timeError);
            if (time) {
                // 今の frame の束を最優先で読む。揃ったときだけ先読みする (先読みは何も追い出さず、
                // 今の frame の読み込みを待たせない)。
                const auto current = equationPreviewCurrentLayers(time->lookup);
                bool currentReady = current.empty();
                if (!current.empty()) {
                    const auto resident = mathRasters_->residentEquationLayers(
                        *renderSpec, equationLayerRefs(current), true);
                    if (resident.state == MathRasterCache::Residency::Resident) {
                        placeEquationLayers(in, current, resident, memo);
                        currentReady = true;
                    }
                }
                if (currentReady)
                    for (const auto& bundle :
                         equationPreviewUpcomingLayers(*in.spec, *time, kEquationPrefetchBundles)) {
                        const auto resident = mathRasters_->residentEquationLayers(
                            *renderSpec, equationLayerRefs(bundle), false);
                        if (resident.state == MathRasterCache::Residency::Resident)
                            placeEquationLayers(in, bundle, resident, memo);
                    }
            }
        }
    }

    if (const auto found = equationPreviewAnimations_.constFind(clipId);
        found != equationPreviewAnimations_.constEnd() && found->memo == memo)
        return found->animation;
    auto model = std::make_shared<const EquationPreviewModel>(std::move(in));
    for (const auto& diagnostic : model->diagnostics())
        qWarning("Equation Sequence の preview: %s", diagnostic.c_str());
    if (!model->hasContent())
        return none();
    auto animation = std::make_shared<EquationSequencePreviewAnimation>(
        std::move(model), equationPreviewObserverForTest_);
    equationPreviewAnimations_.insert(clipId, {memo, animation});
    return animation;
}

MvmController::EquationSequencePreviewStatus
MvmController::equationSequencePreviewStatus(const QString& clipId, qint64 outputFrame) const {
    EquationSequencePreviewStatus status;
    const auto found = std::find_if(
        project_.timelineClips.begin(), project_.timelineClips.end(), [&](const auto& clip) {
            return clip.kind == project::TimelineClipKind::EquationSequence &&
                   QString::fromStdString(clip.id) == clipId;
        });
    if (found == project_.timelineClips.end() || !mathRasters_)
        return status;
    // 問い合わせは読むだけ: 静止・sequence の描画を要求せず、cache の record も作らない
    // (再生位置に無い選択中の clip を UI が問い合わせても描画を始めない)。描画の要求は見えている
    // clip の合成 (equationSequencePreviewAnimation) と requestMathRenders だけが行う。
    status.found = true;
    status.backend = mathRasters_->backendState();
    // compile も memo を書き換えない (data が同じ memo があれば使い、無ければその場で compile)。
    const auto memo = equationCompiles_.constFind(clipId);
    const auto compiled =
        memo != equationCompiles_.constEnd() && memo->data == found->equationSequence
            ? memo->result
            : compileEquationSequence(found->equationSequence);
    status.compile = compiled.failure;
    std::string error;
    status.time =
        equationPreviewTimeAt(*found, project_.timelineFpsNum, project_.timelineFpsDen,
                              compiled.value ? &*compiled.value : nullptr, outputFrame, error);
    for (const auto& state : found->equationSequence.states) {
        const auto entry = mathRasters_->entryOf(equationStateStaticSpec(state));
        status.staticsReady.push_back(entry.state == MathRasterCache::State::Ready && entry.mask);
    }
    // 見せるものは今の合成の animation の model が決める (animation が無ければ None)。
    if (const auto animation = equationPreviewAnimations_.constFind(clipId);
        animation != equationPreviewAnimations_.constEnd())
        if (const auto* sequence =
                dynamic_cast<const EquationSequencePreviewAnimation*>(animation->animation.get()))
            status.shown = sequence->model().shownAt(outputFrame);
    if (!compiled.value)
        return status;
    const auto renderSpec = equationSequenceRenderSpecFor(*compiled.value, error);
    if (!renderSpec)
        return status;
    status.sequenceKey = mathRasters_->equationSequenceKeyFor(*renderSpec);
    const auto entry = mathRasters_->equationSequenceEntryOf(*renderSpec);
    status.disk = entry.state;
    status.diskFailure = entry.backendFailure;
    status.diskMessage = entry.message;
    if (entry.state == MathRasterCache::State::Ready && status.time) {
        const auto current = equationPreviewCurrentLayers(status.time->lookup);
        if (!current.empty()) {
            const auto residency =
                mathRasters_->equationLayersResidencyOf(*renderSpec, equationLayerRefs(current));
            status.residency = residency.state;
            status.residencyMessage = residency.message;
        }
    }
    return status;
}

QVariantMap
MvmController::mathTransformStatus(const project::TimelineTransition& transition) const {
    if (transition.kind != project::TransitionKind::MathTransform || !mathRasters_)
        return {};
    const int outgoing = indexOfClipId(project_.timelineClips, transition.outgoingClipId);
    const int incoming = indexOfClipId(project_.timelineClips, transition.incomingClipId);
    if (outgoing < 0 || incoming < 0)
        return {};
    const auto spec =
        mathTransformSpecFor(transition, project_.timelineClips[static_cast<std::size_t>(outgoing)],
                             project_.timelineClips[static_cast<std::size_t>(incoming)]);
    if (!spec)
        return {};
    // state は disk の変形 (書き出しが使う) の状態。preview の memory に置けたかは
    // transformPreview が別に示す (memory に収まらなくても disk の変形は ready のまま)。
    QString state;
    QString message;
    QString log;
    QString unavailableReason;
    std::pair<QString, QString> preview;
    switch (mathRasters_->backendState()) {
    case MathRasterCache::BackendState::Checking:
        state = QStringLiteral("checking");
        break;
    case MathRasterCache::BackendState::Unavailable:
        state = QStringLiteral("unavailable");
        unavailableReason =
            mathRasters_->authorized() ? QStringLiteral("backend") : QStringLiteral("authority");
        message = mathRasters_->backendMessage();
        break;
    case MathRasterCache::BackendState::Available: {
        const auto entry = mathRasters_->requestTransform(*spec);
        message = entry.message;
        log = entry.log;
        switch (entry.state) {
        case MathRasterCache::State::Pending:
            state = QStringLiteral("rendering");
            break;
        case MathRasterCache::State::Failed:
            state = QStringLiteral("error");
            break;
        case MathRasterCache::State::Unavailable:
            state = QStringLiteral("unavailable");
            unavailableReason = QStringLiteral("backend");
            break;
        case MathRasterCache::State::Ready: {
            // disk の artifact は Ready のまま。出力 raster に置けないことは preview 側の理由で、
            // preview は cut で見せる (書き出しは P2-6 が同じ中立な検査を自分で行う)。
            state = QStringLiteral("ready");
            QString placementError;
            if (!mathTransformPreviewInputs(transition, &placementError) &&
                !placementError.isEmpty()) {
                preview = {QStringLiteral("placement"), placementError};
                break;
            }
            const auto residency = mathRasters_->transformResidencyOf(*spec);
            switch (residency.state) {
            case MathRasterCache::Residency::Resident:
                preview.first = QStringLiteral("ready");
                break;
            case MathRasterCache::Residency::Loading:
                preview.first = QStringLiteral("loading");
                break;
            case MathRasterCache::Residency::OverBudget:
                preview = {QStringLiteral("memory"), residency.message};
                break;
            case MathRasterCache::Residency::NotReady:
            case MathRasterCache::Residency::Failed:
                break;
            }
            break;
        }
        }
        break;
    }
    }
    return {
        {QStringLiteral("transformState"), state},
        {QStringLiteral("transformMessage"), message},
        {QStringLiteral("transformLog"), log},
        {QStringLiteral("transformUnavailableReason"), unavailableReason},
        {QStringLiteral("transformCanRetry"),
         mathRasters_->authorized() && !shutdownStarted_ &&
             mathRasters_->backendState() != MathRasterCache::BackendState::Checking},
        {QStringLiteral("transformToolchain"), mathRasters_->toolchainText()},
        // preview で変形を使えるか (書き出しの可否とは別。disk が ready のときだけ)。
        //   "" 未要求 (前・後ろの clip が見える frame の合成が要求する) / loading / ready /
        //   memory (memory の上限に収まらない) / placement (artifact が出力 raster に収まらない)。
        // memory・placement の間は cut で見せ、理由は transformPreviewMessage。
        {QStringLiteral("transformPreview"), preview.first},
        {QStringLiteral("transformPreviewMessage"), preview.second}};
}

std::pair<QString, QString> MvmController::mathWriteState(const project::TimelineClip& clip) const {
    const auto write = mathSequenceSpecFor(clip);
    if (!write)
        return {QStringLiteral("none"), {}};
    if (!mathRasters_)
        return {QStringLiteral("unavailable"), {}};
    switch (mathRasters_->backendState()) {
    case MathRasterCache::BackendState::Checking:
        return {QStringLiteral("checking"), {}};
    case MathRasterCache::BackendState::Unavailable:
        return {QStringLiteral("unavailable"), mathRasters_->backendMessage()};
    case MathRasterCache::BackendState::Available:
        break;
    }
    const auto sequence = mathRasters_->requestSequence(*write);
    switch (sequence.state) {
    case MathRasterCache::State::Pending:
        return {QStringLiteral("rendering"), {}};
    case MathRasterCache::State::Failed:
        return {QStringLiteral("error"), sequence.message};
    case MathRasterCache::State::Unavailable:
        return {QStringLiteral("unavailable"), sequence.message};
    case MathRasterCache::State::Ready:
        break;
    }
    const auto staticEntry = mathRasters_->request(mathRenderSpecFor(clip.math));
    if (staticEntry.state == MathRasterCache::State::Ready && staticEntry.mask &&
        (sequence.width != staticEntry.mask->width || sequence.height != staticEntry.mask->height))
        return {QStringLiteral("error"),
                QStringLiteral("Write の連番の大きさが静止の描画と違います")};
    return {QStringLiteral("ready"), {}};
}

std::shared_ptr<const preview::PreviewStillImage>
MvmController::mathStillImage(int clipIndex, bool& pending) const {
    pending = false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    const project::MathClipData data = effectiveMathData(clip);
    const QString clipId = QString::fromStdString(clip.id);
    const auto spec = mathRenderSpecFor(data);
    const auto entry = mathRasters_->request(spec);
    std::shared_ptr<const media::StillImage> mask;
    QString maskKey;
    if (entry.state == MathRasterCache::State::Ready && entry.mask) {
        maskKey = mathRasters_->keyFor(spec);
        mathLastGood_.insert(clipId, {maskKey, entry.mask});
        mask = entry.mask;
    } else if (const auto found = mathLastGood_.constFind(clipId);
               found != mathLastGood_.constEnd()) {
        // 描き直し中・失敗中は最後に描けた画素を出し続ける (書き出しには使わない)。
        mask = found->mask;
        maskKey = found->key;
    }
    if (!mask) {
        pending = true;
        return nullptr;
    }
    const QString memo =
        maskKey + QLatin1Char('|') + QString::fromStdString(data.color) + QLatin1Char('|') +
        QString::fromStdString(data.backgroundColor) +
        QStringLiteral("|%1x%2").arg(project_.outputWidth).arg(project_.outputHeight);
    if (const auto found = mathStillImages_.constFind(clipId);
        found != mathStillImages_.constEnd() && found->memo == memo)
        return found->image;
    auto composed = composeMathClipRaster(*mask, data, project_.outputWidth, project_.outputHeight);
    if (!composed.success) {
        // 出力より大きい式など。状態は mathClipData が示す。preview からは外す。
        pending = true;
        return nullptr;
    }
    auto still = std::make_shared<preview::PreviewStillImage>();
    still->width = composed.width;
    still->height = composed.height;
    still->rgba = std::move(composed.rgba);
    mathStillImages_.insert(clipId, {memo, still});
    return still;
}

namespace {

// Write の尺の上限 (clip の素材 frame) は clip の尺。描画の方式による上限は Project の値に
// 持ち込まず、描画の状態 (writeState) が未対応として示す。
std::int64_t mathIntroMaximumFrames(const project::TimelineClip& clip) {
    return clip.sourceOutFrame - clip.sourceInFrame;
}

// clip の素材 frame の数を秒にする (数式 clip の素材 fps は置いたときの timeline の fps)。
double mathIntroSeconds(const project::TimelineClip& clip, std::int64_t frames) {
    return clip.sourceFpsNum > 0
               ? static_cast<double>(frames) * static_cast<double>(clip.sourceFpsDen) /
                     static_cast<double>(clip.sourceFpsNum)
               : 0.0;
}

// 秒を clip の素材 frame にし、1 から上限までに収める。
std::int64_t mathIntroFramesForSeconds(const project::TimelineClip& clip, double seconds) {
    const double frames = seconds * static_cast<double>(clip.sourceFpsNum) /
                          static_cast<double>(std::max<std::int64_t>(1, clip.sourceFpsDen));
    const auto rounded = std::isfinite(frames) ? std::llround(frames) : 1LL;
    return std::clamp<std::int64_t>(rounded, 1,
                                    std::max<std::int64_t>(1, mathIntroMaximumFrames(clip)));
}

} // namespace

QVariantMap MvmController::mathClipData(const QString& clipId) const {
    const auto found = std::find_if(project_.timelineClips.begin(), project_.timelineClips.end(),
                                    [&](const auto& clip) {
                                        return clip.kind == project::TimelineClipKind::Math &&
                                               QString::fromStdString(clip.id) == clipId;
                                    });
    if (found == project_.timelineClips.end() || !mathRasters_)
        return {};
    const project::MathClipData data = effectiveMathData(*found);
    const bool hasPrevious = mathLastGood_.contains(clipId);
    const auto write = mathWriteState(*found);
    std::pair<QString, QString> writePreview;
    if (const auto spec = mathSequenceSpecFor(*found);
        spec && write.first == QStringLiteral("ready")) {
        const auto residency = mathRasters_->residencyOf(*spec);
        switch (residency.state) {
        case MathRasterCache::Residency::Resident:
            writePreview.first = QStringLiteral("ready");
            break;
        case MathRasterCache::Residency::Loading:
            writePreview.first = QStringLiteral("loading");
            break;
        case MathRasterCache::Residency::OverBudget:
            writePreview = {QStringLiteral("memory"), residency.message};
            break;
        case MathRasterCache::Residency::NotReady:
        case MathRasterCache::Residency::Failed:
            break;
        }
    }
    QString state;
    QString message;
    QString log;
    QString unavailableReason;
    switch (mathRasters_->backendState()) {
    case MathRasterCache::BackendState::Checking:
        state = QStringLiteral("checking");
        break;
    case MathRasterCache::BackendState::Unavailable:
        state = QStringLiteral("unavailable");
        unavailableReason =
            mathRasters_->authorized() ? QStringLiteral("backend") : QStringLiteral("authority");
        message = mathRasters_->backendMessage();
        break;
    case MathRasterCache::BackendState::Available: {
        const auto entry = mathRasters_->request(mathRenderSpecFor(data));
        message = entry.message;
        log = entry.log;
        switch (entry.state) {
        case MathRasterCache::State::Pending:
            state = hasPrevious ? QStringLiteral("stale") : QStringLiteral("rendering");
            break;
        case MathRasterCache::State::Ready:
            state = QStringLiteral("ready");
            if (entry.mask && (entry.mask->width > project_.outputWidth ||
                               entry.mask->height > project_.outputHeight)) {
                state = QStringLiteral("error");
                message =
                    QStringLiteral("数式が出力サイズを超えています。文字サイズを下げてください");
            }
            break;
        case MathRasterCache::State::Failed:
            state = QStringLiteral("error");
            break;
        case MathRasterCache::State::Unavailable:
            state = QStringLiteral("unavailable");
            unavailableReason = QStringLiteral("backend");
            break;
        }
        break;
    }
    }
    return {{QStringLiteral("clipId"), clipId},
            {QStringLiteral("source"), QString::fromStdString(data.source)},
            {QStringLiteral("fontSize"), data.fontSize},
            {QStringLiteral("color"), QString::fromStdString(data.color)},
            {QStringLiteral("backgroundColor"), QString::fromStdString(data.backgroundColor)},
            {QStringLiteral("state"), state},
            {QStringLiteral("unavailableReason"), unavailableReason},
            {QStringLiteral("canRetry"),
             mathRasters_->authorized() && !shutdownStarted_ &&
                 mathRasters_->backendState() != MathRasterCache::BackendState::Checking},
            // 準備中・描き直し中・失敗中で、前に描けた画素を preview に出しているか。
            {QStringLiteral("showingPrevious"), state != QStringLiteral("ready") && hasPrevious},
            {QStringLiteral("message"), message},
            {QStringLiteral("log"), log},
            {QStringLiteral("toolchain"), mathRasters_->toolchainText()},
            // Write (clip の先頭で式を書く)。尺は clip の素材 frame で、秒は表示用。
            {QStringLiteral("intro"),
             QString::fromLatin1(project::mathIntroKindName(found->mathAnimation.intro))},
            {QStringLiteral("introFrames"), static_cast<qint64>(found->mathAnimation.introFrames)},
            {QStringLiteral("introSeconds"),
             mathIntroSeconds(*found, found->mathAnimation.introFrames)},
            {QStringLiteral("introMaxSeconds"),
             mathIntroSeconds(*found, mathIntroMaximumFrames(*found))},
            {QStringLiteral("writeState"), write.first},
            {QStringLiteral("writeMessage"), write.second},
            // preview 用の mask を memory に置けたか (書き出しの可否とは別)。"" は未要求。
            {QStringLiteral("writePreview"), writePreview.first},
            {QStringLiteral("writePreviewMessage"), writePreview.second}};
}

QVariantMap MvmController::selectedMathClip() const {
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return {};
    return mathClipData(QString::fromStdString(
        project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id));
}

namespace {

// QML から来た数式の値を MathClipData へ反映する。確定と preview の両方が使う。
void applyMathValues(project::MathClipData& data, const QVariantMap& values) {
    if (values.contains(QStringLiteral("source")))
        data.source = values.value(QStringLiteral("source")).toString().trimmed().toStdString();
    if (values.contains(QStringLiteral("fontSize")))
        data.fontSize = values.value(QStringLiteral("fontSize")).toInt();
    if (values.contains(QStringLiteral("color")))
        data.color = values.value(QStringLiteral("color")).toString().toStdString();
    if (values.contains(QStringLiteral("backgroundColor")))
        data.backgroundColor =
            values.value(QStringLiteral("backgroundColor")).toString().toStdString();
}

std::string mathClipName(const project::MathClipData& data) {
    return QString::fromStdString(data.source).simplified().left(32).toStdString();
}

} // namespace

bool MvmController::createMathClip(const QString& source) {
    if (busy_ || source.trimmed().isEmpty() || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Math;
    clip.id = newClipId();
    clip.sourceFpsNum = candidate.timelineFpsNum;
    clip.sourceFpsDen = candidate.timelineFpsDen;
    clip.sourceFrameCount =
        project::defaultStillClipFrames(candidate.timelineFpsNum, candidate.timelineFpsDen);
    clip.sourceOutFrame = clip.sourceFrameCount;
    clip.math.source = source.trimmed().toStdString();
    clip.math.fontSize = std::min(clip.math.fontSize, candidate.outputHeight);
    clip.name = mathClipName(clip.math);
    std::string error;
    if (!project::validateMathClipData(clip.math, candidate.outputHeight, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    const auto placed = project::placeStillClipAt(candidate, std::move(clip), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("数式 clip を作成できません: ")))
        return false;
    Q_EMIT stateChanged();
    refreshTextPreview();
    return selectClip(placed.selectedIndex);
}

QObject* MvmController::equationEditor() const {
    return equationEditor_.get();
}

bool MvmController::createEquationSequenceClip(const QString& source) {
    if (busy_ || source.trimmed().isEmpty() || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::EquationSequence;
    clip.id = newClipId();
    clip.name = "数式 sequence";
    // 内部の時間は作成時の Project の FPS (P3-1)。最初の状態は数式 clip と同じ既定の尺。
    clip.sourceFpsNum = candidate.timelineFpsNum;
    clip.sourceFpsDen = candidate.timelineFpsDen;
    clip.equationSequence = newEquationSequenceData(
        source.trimmed().toStdString(),
        project::defaultStillClipFrames(candidate.timelineFpsNum, candidate.timelineFpsDen),
        newClipId);
    auto& equation = clip.equationSequence.states.front().equation;
    equation.fontSize = std::min(equation.fontSize, candidate.outputHeight);
    std::string error;
    std::vector<project::EquationInterval> intervals;
    if (!project::validateEquationSequence(clip.equationSequence, candidate.outputHeight, error) ||
        !project::equationIntervals(clip.equationSequence, intervals, clip.sourceFrameCount,
                                    error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    clip.sourceOutFrame = clip.sourceFrameCount;
    const auto placed = project::placeStillClipAt(candidate, std::move(clip), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate),
                           QStringLiteral("数式 sequence を作成できません: ")))
        return false;
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return selectClip(placed.selectedIndex);
}

bool MvmController::updateMathClip(const QString& clipId, const QVariantMap& values) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto id = clipId.toStdString();
    const auto found = std::find_if(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == candidate.timelineClips.end() || found->kind != project::TimelineClipKind::Math) {
        setStatus(QStringLiteral("編集する数式 clip がありません"));
        return false;
    }
    applyMathValues(found->math, values);
    // Write: "intro" は "none" / "write"、尺は "introSeconds" (clip の尺と上限に収める)。
    if (values.contains(QStringLiteral("intro"))) {
        project::MathIntroKind intro = project::MathIntroKind::None;
        if (!project::parseMathIntroKind(
                values.value(QStringLiteral("intro")).toString().toStdString(), intro)) {
            setStatus(QStringLiteral("数式の intro の種類が不正です"));
            return false;
        }
        if (intro == project::MathIntroKind::None) {
            found->mathAnimation = {};
        } else if (found->mathAnimation.intro == project::MathIntroKind::None) {
            // 既定は 1 秒 (clip が短ければ clip の尺)。
            found->mathAnimation = {intro, mathIntroFramesForSeconds(*found, 1.0)};
        }
    }
    if (values.contains(QStringLiteral("introSeconds")) &&
        found->mathAnimation.intro != project::MathIntroKind::None)
        found->mathAnimation.introFrames = mathIntroFramesForSeconds(
            *found, values.value(QStringLiteral("introSeconds")).toDouble());
    // 値の形だけを確かめる。描けるかどうかでは確定を拒否しない (描けない式も Project の正)。
    std::string error;
    if (!project::validateMathClipData(found->math, candidate.outputHeight, error) ||
        !project::validateMathClipAnimation(found->mathAnimation,
                                            found->sourceOutFrame - found->sourceInFrame, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    found->name = mathClipName(found->math);
    mathPreviewOverride_.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("数式 clip を更新できません: ")))
        return false;
    // 値が変わらなかった (commit が何もしない) 場合も、preview の上書きを外した状態へ戻す。
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return true;
}

bool MvmController::previewMathClip(const QString& clipId, const QVariantMap& values) {
    if (busy_ || playing_)
        return false;
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0 || project_.timelineClips[static_cast<std::size_t>(index)].kind !=
                         project::TimelineClipKind::Math)
        return false;
    project::MathClipData data =
        effectiveMathData(project_.timelineClips[static_cast<std::size_t>(index)]);
    applyMathValues(data, values);
    std::string error;
    if (!project::validateMathClipData(data, project_.outputHeight, error))
        return false;
    mathPreviewOverride_ = std::make_pair(clipId.toStdString(), std::move(data));
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return true;
}

void MvmController::cancelMathPreview() {
    if (!mathPreviewOverride_)
        return;
    mathPreviewOverride_.reset();
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
}

void MvmController::retryMathRendering() {
    if (!mathRasters_)
        return;
    // backend を確かめ直し、失敗を忘れる。終わったら entryChanged (空) ですべての数式を
    // 要求し直す。描けている式は disk の結果を使い続ける (強制の描き直しではない)。
    mathRasters_->startPreflight();
    Q_EMIT stateChanged();
}

void MvmController::setMathPreflightForTest(MathRasterCache::PreflightFunction preflight) {
    mathRasters_->setPreflight(std::move(preflight));
    mathRasters_->startPreflight();
}

QUrl MvmController::textRasterUrl(int index) {
    QString rasterError;
    const QImage* image = textRasterImage(index, rasterError);
    if (!image) {
        if (index >= 0 && index < static_cast<int>(project_.timelineClips.size()) &&
            project_.timelineClips[static_cast<std::size_t>(index)].kind ==
                project::TimelineClipKind::Text)
            setStatus(rasterError);
        return {};
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    // PNG は Image が URL で cache するので、ドラッグ中の preview ごとに別の名前にする。
    const QString key = QString::number(currentRevision_) + u'-' +
                        QString::number(textPreviewSerial_) + u'-' +
                        QString::fromStdString(clip.id);
    if (textRasterUrls_.contains(key))
        return textRasterUrls_.value(key);
    if (!textRasterDirectory_)
        textRasterDirectory_ = std::make_unique<QTemporaryDir>();
    if (!textRasterDirectory_->isValid()) {
        setStatus(QStringLiteral("文字画像の一時 directory を作成できません"));
        return {};
    }
    const QString path = textRasterDirectory_->filePath(key + QStringLiteral(".png"));
    if (!image->save(path, "PNG")) {
        setStatus(QStringLiteral("文字画像を保存できません"));
        return {};
    }
    const QUrl url = QUrl::fromLocalFile(path);
    textRasterUrls_.insert(key, url);
    return url;
}

void MvmController::setTextOverlayClip(const QString& clipId) {
    if (textOverlayClipId_ == clipId)
        return;
    textOverlayClipId_ = clipId;
    Q_EMIT stateChanged();
    // 再生中は次の tick の handOffPlaybackSources が組み直す。
    // ドラッグを離した直後は位置の保存で seek 中なので、直接 submit すると拒否され、
    // 文字を外したままの composition が残って文字が消えていた。refreshTextPreview は
    // seek の完了を待って組み直す。
    refreshTextPreview();
}

double MvmController::textClipOpacity(int index) const {
    const auto mapped = mapTimelinePreviewFrame(project_, previewPlan(), playheadFrame_);
    for (const auto& text : mapped.stillLayers)
        if (text.clipIndex == index)
            return std::clamp(text.opacity, 0.0, 1.0);
    return 1.0;
}

bool MvmController::textClipVisible(int index) const {
    return clipVisibleAtPlayhead(index) &&
           project_.timelineClips[static_cast<std::size_t>(index)].kind ==
               project::TimelineClipKind::Text;
}

bool MvmController::clipVisibleAtPlayhead(int clipIndex) const {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size()))
        return false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    if (!clip.enabled || clip.track.kind != project::TrackKind::Video ||
        !project::isTrackOutputEnabled(project_, clip.track))
        return false;
    const auto duration = project::timelineClipDuration(project_, clip);
    return duration.success && playheadFrame_ >= clip.timelineStartFrame &&
           playheadFrame_ < clip.timelineStartFrame + duration.frame;
}

bool MvmController::textClipHasMotion(const QString& clipId) const {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0 || project_.timelineClips[static_cast<std::size_t>(index)].kind !=
                         project::TimelineClipKind::Text)
        return false;
    const auto effects = effectsForPreview(index);
    const project::ClipEffects defaults;
    for (const auto& channel : project::effectChannels())
        if (channel.kind != project::ClipKeyKind::Opacity &&
            !project::isAudioEffectChannel(channel.kind) &&
            (effects.*channel.base != defaults.*channel.base || !(effects.*channel.keys).empty()))
            return true;
    return false;
}

project::ClipVisualGeometry MvmController::visualGeometryOf(int clipIndex) const {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size()))
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    if (clip.kind == project::TimelineClipKind::Audio)
        return {};
    const auto* item = project::findMediaItem(project_, clip.mediaItemId);
    if (!item && clip.kind != project::TimelineClipKind::Text &&
        clip.kind != project::TimelineClipKind::Manim)
        return {};
    const auto effects = project::evaluateClipEffects(effectsForPreview(clipIndex),
                                                      playheadFrame_ - clip.timelineStartFrame);
    auto geometry = project::clipVisualGeometry(effects, item ? item->width : project_.outputWidth,
                                                item ? item->height : project_.outputHeight,
                                                project_.outputWidth, project_.outputHeight);
    if (clip.kind == project::TimelineClipKind::Text && geometry.valid) {
        const auto mapped = project::mapClipEffects(effects);
        const QRectF crop(mapped.sourceRect.x * project_.outputWidth,
                          mapped.sourceRect.y * project_.outputHeight,
                          mapped.sourceRect.width * project_.outputWidth,
                          mapped.sourceRect.height * project_.outputHeight);
        const QRectF visible = QRectF(textRasterBounds(clipIndex)).intersected(crop);
        if (visible.isEmpty())
            return {};
        const double sx = geometry.width / crop.width(), sy = geometry.height / crop.height();
        geometry.x += (visible.x() - crop.x()) * sx;
        geometry.y += (visible.y() - crop.y()) * sy;
        geometry.width = visible.width() * sx;
        geometry.height = visible.height() * sy;
    }
    return geometry;
}

QString MvmController::transformClipId() const {
    // リンクした音声を含む選択でも、映像が 1 つならその映像を動かす。
    QString found;
    for (const auto& id : selectedClipIds_) {
        const int index = indexOfClipId(project_.timelineClips, id);
        if (index < 0)
            continue;
        const auto kind = project_.timelineClips[static_cast<std::size_t>(index)].kind;
        if (kind == project::TimelineClipKind::Audio)
            continue;
        if (!found.isEmpty() || (kind == project::TimelineClipKind::Text &&
                                 !textClipHasMotion(QString::fromStdString(id))))
            return {};
        found = QString::fromStdString(id);
    }
    return found;
}

QVariantMap MvmController::clipVisualGeometry(const QString& clipId) const {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index >= 0 &&
        project_.timelineClips[static_cast<std::size_t>(index)].kind ==
            project::TimelineClipKind::Text &&
        !textClipHasMotion(clipId))
        return {};

    const auto geometry = visualGeometryOf(index);
    if (!geometry.valid)
        return {};
    return {{QStringLiteral("x"), geometry.x},
            {QStringLiteral("y"), geometry.y},
            {QStringLiteral("width"), geometry.width},
            {QStringLiteral("height"), geometry.height},
            {QStringLiteral("pivotX"), geometry.pivotX},
            {QStringLiteral("pivotY"), geometry.pivotY},
            {QStringLiteral("rotation"), geometry.rotationDegrees},
            {QStringLiteral("visible"), clipVisibleAtPlayhead(index)}};
}

namespace {

// 回転した矩形 (pivot 中心に時計回り) の中に点があるか。点を逆に回して比べる。
bool rotatedRectContains(const project::ClipVisualGeometry& geometry, double x, double y) {
    const double radians = -geometry.rotationDegrees * 3.14159265358979323846 / 180.0;
    const double dx = x - geometry.pivotX;
    const double dy = y - geometry.pivotY;
    const double localX = geometry.pivotX + std::cos(radians) * dx - std::sin(radians) * dy;
    const double localY = geometry.pivotY + std::sin(radians) * dx + std::cos(radians) * dy;
    return localX >= geometry.x && localX < geometry.x + geometry.width && localY >= geometry.y &&
           localY < geometry.y + geometry.height;
}

// 回転した矩形の外接矩形。
QVariantMap rotatedBounds(const project::ClipVisualGeometry& geometry) {
    const double radians = geometry.rotationDegrees * 3.14159265358979323846 / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    double left = std::numeric_limits<double>::infinity();
    double top = left;
    double right = -left;
    double bottom = -left;
    for (const double cornerX : {geometry.x, geometry.x + geometry.width}) {
        for (const double cornerY : {geometry.y, geometry.y + geometry.height}) {
            const double dx = cornerX - geometry.pivotX;
            const double dy = cornerY - geometry.pivotY;
            const double x = geometry.pivotX + cosine * dx - sine * dy;
            const double y = geometry.pivotY + sine * dx + cosine * dy;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    return {{QStringLiteral("x"), left},
            {QStringLiteral("y"), top},
            {QStringLiteral("width"), right - left},
            {QStringLiteral("height"), bottom - top}};
}

} // namespace

QString MvmController::visualClipAt(double x, double y) {
    for (int track = static_cast<int>(project_.videoTracks.size()) - 1; track >= 0; --track) {
        for (int index = 0; index < static_cast<int>(project_.timelineClips.size()); ++index) {
            const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
            if (clip.track.kind != project::TrackKind::Video || clip.track.index != track ||
                !clipVisibleAtPlayhead(index))
                continue;
            const bool hit = clip.kind == project::TimelineClipKind::Text &&
                                     !textClipHasMotion(QString::fromStdString(clip.id))
                                 ? textRasterBounds(index).contains(static_cast<int>(std::floor(x)),
                                                                    static_cast<int>(std::floor(y)))
                                 : [&] {
                                       const auto geometry = visualGeometryOf(index);
                                       return geometry.valid && rotatedRectContains(geometry, x, y);
                                   }();
            if (hit)
                return QString::fromStdString(clip.id);
        }
    }
    return {};
}

QVariantList MvmController::previewSnapRects(const QString& excludeClipId) {
    QVariantList rects;
    for (int index = 0; index < static_cast<int>(project_.timelineClips.size()); ++index) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
        if (QString::fromStdString(clip.id) == excludeClipId || !clipVisibleAtPlayhead(index))
            continue;
        if (clip.kind == project::TimelineClipKind::Text &&
            !textClipHasMotion(QString::fromStdString(clip.id))) {
            const QRect bounds = textRasterBounds(index);
            if (!bounds.isEmpty())
                rects.push_back(QVariantMap{{QStringLiteral("x"), bounds.x()},
                                            {QStringLiteral("y"), bounds.y()},
                                            {QStringLiteral("width"), bounds.width()},
                                            {QStringLiteral("height"), bounds.height()}});
            continue;
        }
        const auto geometry = visualGeometryOf(index);
        if (geometry.valid)
            rects.push_back(rotatedBounds(geometry));
    }
    return rects;
}

QVariantMap MvmController::effectsForVisualRect(const QString& clipId, double x, double y,
                                                double width, double height) const {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (!visualGeometryOf(index).valid)
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    const auto* item = project::findMediaItem(project_, clip.mediaItemId);
    auto effects = project::evaluateClipEffects(effectsForPreview(index),
                                                playheadFrame_ - clip.timelineStartFrame);
    const int sourceWidth = item ? item->width : project_.outputWidth;
    const int sourceHeight = item ? item->height : project_.outputHeight;
    if (clip.kind == project::TimelineClipKind::Text) {
        const auto visible = visualGeometryOf(index);
        const auto full = project::clipVisualGeometry(effects, sourceWidth, sourceHeight,
                                                      project_.outputWidth, project_.outputHeight);
        if (!visible.valid || !full.valid)
            return {};
        const double sx = width / visible.width, sy = height / visible.height;
        x -= (visible.x - full.x) * sx;
        y -= (visible.y - full.y) * sy;
        width = full.width * sx;
        height = full.height * sy;
    }
    if (!project::effectsForVisualRect(effects, sourceWidth, sourceHeight, project_.outputWidth,
                                       project_.outputHeight, x, y, width, height))
        return {};
    return {{QStringLiteral("positionX"), effects.positionXPercent},
            {QStringLiteral("positionY"), effects.positionYPercent},
            {QStringLiteral("scaleX"), effects.scaleXPercent},
            {QStringLiteral("scaleY"), effects.scaleYPercent}};
}

QString MvmController::textClipAt(int x, int y) {
    for (int track = static_cast<int>(project_.videoTracks.size()) - 1; track >= 0; --track) {
        for (int index = 0; index < static_cast<int>(project_.timelineClips.size()); ++index) {
            const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
            if (clip.kind != project::TimelineClipKind::Text || clip.track.index != track ||
                !clipVisibleAtPlayhead(index))
                continue;
            // 字形の画素ではなく描画範囲の矩形で当てる。字間や字の内側を掴んでも
            // 動かせるようにするため (Premiere の選択枠と同じ扱い)。
            const auto geometry = visualGeometryOf(index);
            if (geometry.valid && rotatedRectContains(geometry, x, y))
                return QString::fromStdString(clip.id);
        }
    }
    return {};
}

bool MvmController::selectClip(int index) {
    if (index < 0 || index >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    const project::TimelineClip& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    setTimelineSelection({clip.id});
    setCurrentClipSelection(index);
    // 再生位置はルーラーの操作でだけ動かす。seekTimelineFrame は再生位置の clip を current に
    // 選び直すので使わず、選択を保ったまま今の位置の preview だけを作り直す (追加・削除で
    // clip の並びが変わっていることがある)。
    QString error;
    if (!refreshPreviewAtPlayhead(error)) {
        setStatus(QStringLiteral("Previewを更新できません: ") + error);
        return false;
    }
    return true;
}

bool MvmController::refreshPreviewAtPlayhead(QString& error) {
    if (!previewEngine_)
        return true;
    const auto status = previewEngine_->status();
    switch (status.state) {
    case preview::PreviewEngineState::ReadyPaused:
        break;
    case preview::PreviewEngineState::Seeking:
    case preview::PreviewEngineState::Playing:
    case preview::PreviewEngineState::WaitingForRenderDevice:
        // いずれ ReadyPaused になる一時的な状態。要求は捨てずに保留し、ReadyPaused になったら
        // その時点の Project (と drag 中の override) で 1 回だけ作り直す。途中の状態は出さない。
        previewRefreshPending_ = true;
        return true;
    case preview::PreviewEngineState::Error:
    case preview::PreviewEngineState::ShuttingDown:
    case preview::PreviewEngineState::Shutdown:
    case preview::PreviewEngineState::Uninitialized:
        // 待っても ReadyPaused にならない。保留せずに失敗として返す (fail-closed)。
        previewRefreshPending_ = false;
        error = status.state == preview::PreviewEngineState::Error && status.lastError
                    ? previewErrorText(*status.lastError)
                    : QStringLiteral("Previewが使える状態ではありません");
        return false;
    }
    previewRefreshPending_ = false;
    if (!syncPreviewSourcesAt(playheadFrame_, error))
        return false;
    currentSource_.reset();
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == currentClipIndex_)
            currentSource_ = slot.source;
    }
    return true;
}

bool MvmController::previewPresentedLatest() const {
    if (!previewEngine_ || previewRefreshPending_)
        return false;
    const auto status = previewEngine_->status();
    return status.state == preview::PreviewEngineState::ReadyPaused &&
           status.latestAcceptedDesiredComposition &&
           status.lastPresentedComposition == status.latestAcceptedDesiredComposition;
}

std::optional<QRectF> MvmController::submittedLayerDestination(const QString& clipId) const {
    if (!submittedComposition_)
        return std::nullopt;
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (QString::fromStdString(slot.clipId) != clipId)
            continue;
        for (const auto& layer : submittedComposition_->layers) {
            if (layer.source == slot.source)
                return QRectF(layer.destination.x, layer.destination.y, layer.destination.width,
                              layer.destination.height);
        }
    }
    return std::nullopt;
}

std::vector<float> MvmController::submittedLayerOpacities() const {
    std::vector<float> opacities;
    if (submittedComposition_) {
        for (const auto& layer : submittedComposition_->layers)
            opacities.push_back(layer.opacity);
    }
    return opacities;
}

bool MvmController::selectTimelineClip(const QString& clipId, bool linked) {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    setTimelineSelection({clipId.toStdString()}, linked);
    setCurrentClipSelection(index);
    // 再生位置はルーラーの操作でだけ動かす。
    return true;
}

bool MvmController::toggleTimelineClipSelection(const QString& clipId) {
    const std::string id = clipId.toStdString();
    const int index = indexOfClipId(project_.timelineClips, id);
    if (index < 0) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }

    const auto& clicked = project_.timelineClips[static_cast<std::size_t>(index)];
    std::vector<std::string> next = selectedClipIds_;
    const bool selected = std::find(next.begin(), next.end(), id) != next.end();
    if (selected) {
        next.erase(std::remove_if(
                       next.begin(), next.end(),
                       [&](const std::string& selectedId) {
                           if (selectedId == id)
                               return true;
                           if (clicked.linkGroupId.empty())
                               return false;
                           const int selectedIndex =
                               indexOfClipId(project_.timelineClips, selectedId);
                           return selectedIndex >= 0 &&
                                  project_.timelineClips[static_cast<std::size_t>(selectedIndex)]
                                          .linkGroupId == clicked.linkGroupId;
                       }),
                   next.end());
    } else {
        next.push_back(id);
    }

    setTimelineSelection(next);
    const int currentIndex =
        selected ? (selectedClipIds_.empty()
                        ? -1
                        : indexOfClipId(project_.timelineClips, selectedClipIds_.front()))
                 : index;
    setCurrentClipSelection(currentIndex);
    setStatus(selectedClipIds_.empty() ? QStringLiteral("clipの選択を解除しました")
                                       : QString::number(selectedClipIds_.size()) +
                                             QStringLiteral("個のclipを選択しました"));
    return true;
}

bool MvmController::selectTimelineClips(const QStringList& clipIds) {
    std::vector<std::string> selectedIds;
    selectedIds.reserve(static_cast<std::size_t>(clipIds.size()));
    for (const auto& clipId : clipIds) {
        const std::string id = clipId.toStdString();
        if (indexOfClipId(project_.timelineClips, id) < 0) {
            setStatus(QStringLiteral("矩形選択に存在しないclipが含まれています"));
            return false;
        }
        if (std::find(selectedIds.begin(), selectedIds.end(), id) == selectedIds.end())
            selectedIds.push_back(id);
    }
    setTimelineSelection(selectedIds);
    if (selectedIds.empty()) {
        setCurrentClipSelection(-1);
        setStatus(QStringLiteral("clipの選択を解除しました"));
        return true;
    }
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, selectedIds.front()));
    setStatus(QString::number(selectedClipIds_.size()) + QStringLiteral("個のclipを選択しました"));
    return true;
}

bool MvmController::selectAllClips() {
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("選択できるclipがありません"));
        return false;
    }
    std::vector<std::string> ids;
    ids.reserve(project_.timelineClips.size());
    for (const auto& clip : project_.timelineClips)
        ids.push_back(clip.id);
    const std::string current = currentClipId();
    setTimelineSelection(ids, false);
    setCurrentClipSelection(
        indexOfClipId(project_.timelineClips, current.empty() ? ids.front() : current));
    setStatus(QString::number(selectedClipIds_.size()) + QStringLiteral("個のclipを選択しました"));
    return true;
}

void MvmController::beginScrub() {
    if (navigationTimelineFrames() == 0)
        return;
    pauseTimeline();
    scrubbing_ = true;
    scrubPending_ = false;
    stopScrubAudio();
    // Premiere と同じく drag 位置の音を短い断片で鳴らす。鳴らす clip が無ければ WASAPI を開かない。
    if (!project_.timelineClips.empty() && hasShuttleAudibleClip(project_)) {
        auto scrubAudio = std::make_unique<ScrubAudioPlayback>();
        std::string audioError;
        if (scrubAudio->start(project_, static_cast<float>(masterVolume_), audioError)) {
            scrubAudio_ = std::move(scrubAudio);
        } else {
            // 音声 device の障害で映像の scrub まで止めない。無音であることは status に残す。
            setStatus(QStringLiteral("scrub音声を開始できないため無音です: ") +
                      QString::fromStdString(audioError));
        }
    }
    scrubTimer_.start();
}

void MvmController::stopScrubAudio() {
    if (!scrubAudio_)
        return;
    scrubAudio_->stop();
    scrubAudio_.reset();
}

audio::WasapiSnapshot MvmController::scrubAudioSnapshotForTest() const {
    return scrubAudio_ ? scrubAudio_->sinkSnapshot() : audio::WasapiSnapshot{};
}

bool MvmController::refreshScrubAudioMix(int index, double gainDb, double pan) {
    if (!scrubAudio_)
        return true;
    // grain は mix 済みの PCM を持つため、確定前の値も含めて作り直す。
    auto mixedProject = project_;
    mixedProject.audioTracks[static_cast<std::size_t>(index)].mixerGainDb = gainDb;
    mixedProject.audioTracks[static_cast<std::size_t>(index)].mixerPan = pan;
    stopScrubAudio();
    auto scrub = std::make_unique<ScrubAudioPlayback>();
    std::string error;
    if (!scrub->start(mixedProject, static_cast<float>(masterVolume_), error)) {
        setStatus(QStringLiteral("ミキサー変更後のスクラブ音声を開始できません: ") +
                  QString::fromStdString(error));
        return false;
    }
    scrub->setTarget(playheadFrame_);
    scrubAudio_ = std::move(scrub);
    return true;
}

void MvmController::scrubToFrame(qint64 frame) {
    if (!scrubbing_)
        return;
    const qint64 clamped = navigationTimelineFrames() > 0
                               ? std::clamp<qint64>(frame, 0, navigationTimelineFrames() - 1)
                               : 0;
    if (playheadFrame_ != clamped) {
        // 表示用の playhead は即座に動かす。preview の追従は coalesce する。
        playheadFrame_ = clamped;
        Q_EMIT stateChanged();
    }
    scrubTargetFrame_ = clamped;
    scrubPending_ = true;
    if (scrubAudio_)
        scrubAudio_->setTarget(clamped);
}

void MvmController::endScrub() {
    if (!scrubbing_)
        return;
    scrubbing_ = false;
    stopScrubAudio();
    // 最後の位置は必ず反映する。drag の途中で落とした frame を最終位置にしない。
    // ここで seek が Seeking 中に弾かれても timer が引き継いで反映する。
    if (scrubPending_ && seekTimelineFrame(scrubTargetFrame_))
        scrubPending_ = false;
    if (!scrubPending_)
        scrubTimer_.stop();
}

bool MvmController::seekTimelineFrame(qint64 frame) {
    if (!pauseTimeline())
        return false;
    if (previewEffectsOverride_) {
        previewEffectsOverride_.reset();
        previewEffectsClipIndex_ = -1;
    }
    const qint64 clamped =
        std::clamp<qint64>(frame, 0, std::max<qint64>(0, navigationTimelineFrames() - 1));
    playheadFrame_ = clamped;
    if (totalTimelineFrames_ == 0) {
        currentClipIndex_ = -1;
        currentClipName_.clear();
        currentClipPath_.clear();
        Q_EMIT stateChanged();
        return true;
    }
    if (scrubPending_ && !scrubbing_)
        scrubTargetFrame_ = clamped;
    int index = -1;
    // 再生位置とエフェクトの編集対象は独立。選択が残る限り対象を維持する。
    if (!selectedClipIds_.empty()) {
        const auto current = currentClipId();
        index = indexOfClipId(project_.timelineClips,
                              std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                        current) != selectedClipIds_.end()
                                  ? current
                                  : selectedClipIds_.front());
    }
    if (index < 0) {
        const auto* selected = topVideoClipAt(project_, clamped);
        if (selected)
            index = static_cast<int>(selected - project_.timelineClips.data());
    }
    Q_EMIT stateChanged();
    const auto previewStatus = previewEngine_->status();
    if (previewStatus.state == preview::PreviewEngineState::Error) {
        const QString detail = previewStatus.lastError
                                   ? previewErrorText(*previewStatus.lastError)
                                   : QStringLiteral("原因を取得できませんでした");
        setStatus(QStringLiteral("Preview error: ") + detail);
        return false;
    }
    if (previewStatus.state != preview::PreviewEngineState::ReadyPaused) {
        setStatus(QStringLiteral("Previewがseek可能になるまで待ってください"));
        return false;
    }
    QString error;
    if (!syncPreviewSourcesAt(clamped, error)) {
        setStatus(QStringLiteral("Previewをseekできません: ") + error);
        return false;
    }
    currentClipIndex_ = index;
    if (index >= 0) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
        currentClipName_ = QString::fromStdString(clip.name);
        currentClipPath_ = fromPath(clip.mediaPath);
        currentSource_.reset();
        for (const auto& [trackIndex, slot] : trackSources_) {
            if (slot.clipIndex == index)
                currentSource_ = slot.source;
        }
        setStatus(currentClipName_ + QStringLiteral(" を表示しています"));
    } else {
        currentSource_.reset();
        currentClipName_.clear();
        currentClipPath_.clear();
        setStatus(QStringLiteral("timeline gapを表示しています"));
    }
    if (scrubPending_ && !scrubbing_) {
        scrubPending_ = false;
        scrubTimer_.stop();
    }
    return true;
}

bool MvmController::prepareTimelineFrameForPlayback(int clipIndex, std::int64_t timelineFrame) {
    (void)clipIndex;
    QString error;
    if (!syncPreviewSourcesAt(timelineFrame, error)) {
        setStatus(QStringLiteral("Previewを準備できません: ") + error);
        return false;
    }
    return true;
}

bool MvmController::queuePreparedPlayback(int clipIndex, std::int64_t timelineFrame) {
    if (!prepareTimelineFrameForPlayback(clipIndex, timelineFrame))
        return false;
    // 映像の無い区間でも engine を再生する。時計だけを進めると音声が鳴らない。
    pendingPlaybackStart_ = true;
    pendingPlaybackClipIndex_ = clipIndex;
    pendingPlaybackBaseFrame_ = timelineFrame;
    playing_ = true;
    statusText_ = QStringLiteral("再生開始のためseekしています");
    Q_EMIT stateChanged();
    return true;
}

void MvmController::startPendingPlayback() {
    if (!pendingPlaybackStart_)
        return;
    const int clipIndex = pendingPlaybackClipIndex_;
    const std::int64_t baseFrame = pendingPlaybackBaseFrame_;
    const auto played = previewEngine_->play();
    if (!played) {
        stopPlaybackWithError(QStringLiteral("Previewを再生できません: ") +
                              previewErrorText(played.error()));
        return;
    }
    pendingPlaybackStart_ = false;
    pendingPlaybackClipIndex_ = -1;
    playbackClipIndex_ = clipIndex;
    playbackBaseFrame_ = baseFrame;
    playbackClock_.restart();
    playbackTimer_.start();
    playbackPreparationFailure_.clear();
    statusText_ = QStringLiteral("timelineを再生しています");
    Q_EMIT stateChanged();
}

bool MvmController::playTimeline() {
    if (shuttleRate_ != 0 && !pauseTimeline())
        return false;
    if (busy_) {
        setStatus(QStringLiteral("処理中はtimelineを再生できません"));
        return false;
    }
    if (playing_)
        return true;
    if (previewEffectsOverride_)
        cancelEffectPreview();
    // 枠が空くのを待っている境界からは、ここで改めて再生を始める (待った後の再開と二重にしない)。
    pendingSlotRebuildFrame_.reset();
    // drag 中に再生を始めたら scrub の断片と通常再生が二重に鳴らないようにする。
    stopScrubAudio();
    if (totalTimelineFrames_ == 0) {
        setStatus(QStringLiteral("再生するクリップまたは字幕がありません"));
        return false;
    }
    if (!timelinePreviewCompatible(project_)) {
        setStatus(timelineFpsText() +
                  QStringLiteral(" ではないclipを含むためtimeline再生は未対応です。"
                                 "編集・保存・Exportは可能です"));
        return false;
    }
    if (playheadFrame_ >= totalTimelineFrames_) {
        setStatus(QStringLiteral("timeline終端です。再生位置をseekしてください"));
        return false;
    }

    const auto previewState = previewEngine_->status().state;
    if (previewState != preview::PreviewEngineState::ReadyPaused) {
        setStatus(QStringLiteral("Previewが再生可能な状態ではありません: ") +
                  previewStateText(previewState));
        return false;
    }
    const auto* selected = topVideoClipAt(project_, playheadFrame_);
    const int clipIndex =
        selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
    return queuePreparedPlayback(clipIndex, playheadFrame_);
}

void MvmController::stopPlaybackWithError(QString error) {
    playbackTimer_.stop();
    playbackClock_.invalidate();
    if (previewEngine_ && previewEngine_->status().state == preview::PreviewEngineState::Playing) {
        const auto paused = previewEngine_->pause();
        if (!paused)
            error +=
                QStringLiteral("\nPreviewも停止できません: ") + previewErrorText(paused.error());
    }
    playing_ = false;
    shuttleRate_ = 0;
    pendingPlaybackStart_ = false;
    playbackClipIndex_ = -1;
    pendingPlaybackClipIndex_ = -1;
    pendingCapacityRebuildFrame_.reset();
    pendingSlotRebuildFrame_.reset();
    retirePreparedPlaybackSources();
    setStatus(std::move(error));
}

void MvmController::retirePreparedPlaybackSources() {
    cancelSourcePreparations();
    for (const auto& source : preparedVideoSources_)
        if (!previewEngine_->removeSource(source.source))
            retiredSources_.push_back(source.source);
    for (const auto& source : preparedAudioSources_)
        if (!previewEngine_->removeSource(source.source))
            retiredSources_.push_back(source.source);
    preparedVideoSources_.clear();
    preparedAudioSources_.clear();
    playbackPreparationFailure_.clear();
    failedPreparationStart_.reset();
    playbackCapacityFailure_ = false;
}

bool MvmController::setPreviewRegistrationLimitForTest(std::size_t limit) {
    if (!previewEngine_)
        return false;
    return static_cast<bool>(
        preview::internal::PreviewRenderPort::setRegisteredVideoSourceLimitForTest(*previewEngine_,
                                                                                   limit));
}

void MvmController::cancelSourcePreparations() {
    const auto cancel = [&](preview::PreviewPreparationId id) {
        if (previewEngine_->cancelSourcePreparation(id))
            stalePreparations_.push_back(id);
    };
    for (const auto& pending : pendingVideoPreparations_)
        cancel(pending.id);
    for (const auto& pending : pendingAudioPreparations_)
        cancel(pending.id);
    pendingVideoPreparations_.clear();
    pendingAudioPreparations_.clear();
    ++playbackPreparationGeneration_;
}

void MvmController::adoptPreparationOutcome(preview::PreviewPreparationId id,
                                            preview::Result<preview::PreviewSourceId> outcome) {
    // 取り消した準備。成功していても (取り消しが間に合わなかった) 使わずに外す。
    if (const auto stale = std::find(stalePreparations_.begin(), stalePreparations_.end(), id);
        stale != stalePreparations_.end()) {
        stalePreparations_.erase(stale);
        ++playbackStalePreparationCount_;
        if (outcome && !previewEngine_->removeSource(outcome.value()))
            retiredSources_.push_back(outcome.value());
        return;
    }
    const auto video =
        std::find_if(pendingVideoPreparations_.begin(), pendingVideoPreparations_.end(),
                     [&](const auto& pending) { return pending.id == id; });
    const auto audio =
        std::find_if(pendingAudioPreparations_.begin(), pendingAudioPreparations_.end(),
                     [&](const auto& pending) { return pending.id == id; });
    const bool known =
        video != pendingVideoPreparations_.end() || audio != pendingAudioPreparations_.end();
    const std::uint64_t generation = video != pendingVideoPreparations_.end()   ? video->generation
                                     : audio != pendingAudioPreparations_.end() ? audio->generation
                                                                                : 0;
    const std::int64_t boundary = video != pendingVideoPreparations_.end()   ? video->boundary
                                  : audio != pendingAudioPreparations_.end() ? audio->boundary
                                                                             : 0;
    std::optional<TrackPreviewSource> videoEntry;
    std::optional<AudioPreviewSource> audioEntry;
    if (video != pendingVideoPreparations_.end()) {
        videoEntry = video->entry;
        pendingVideoPreparations_.erase(video);
    } else if (audio != pendingAudioPreparations_.end()) {
        audioEntry = audio->entry;
        pendingAudioPreparations_.erase(audio);
    }
    if (!outcome) {
        // 古くなった準備 (取り消し・pause / seek) は失敗として覚えない。境界までに準備し直す。
        if (!known || outcome.error().code == preview::PreviewErrorCode::PreparationStale)
            return;
        playbackCapacityFailure_ =
            outcome.error().code == preview::PreviewErrorCode::RegistrationCapacityExceeded;
        playbackPreparationFailure_ = previewErrorText(outcome.error());
        failedPreparationStart_ = boundary;
        ++playbackPreparationFailureCount_;
        return;
    }
    // 取り消した後に届いた完了、要求の後に再生や Project が変わった準備は使わない。
    if (!known || generation != playbackPreparationGeneration_) {
        ++playbackStalePreparationCount_;
        if (!previewEngine_->removeSource(outcome.value()))
            retiredSources_.push_back(outcome.value());
        return;
    }
    if (videoEntry) {
        videoEntry->source = outcome.value();
        preparedVideoSources_.push_back(std::move(*videoEntry));
    } else {
        audioEntry->source = outcome.value();
        preparedAudioSources_.push_back(std::move(*audioEntry));
    }
}

void MvmController::collectSourcePreparations() {
    for (auto& completed : previewEngine_->takeCompletedSourcePreparations())
        adoptPreparationOutcome(completed.preparation, std::move(completed.source));
}

void MvmController::waitDueSourcePreparations(std::int64_t frame) {
    std::vector<preview::PreviewPreparationId> due;
    for (const auto& pending : pendingVideoPreparations_)
        if (pending.boundary <= frame)
            due.push_back(pending.id);
    for (const auto& pending : pendingAudioPreparations_)
        if (pending.boundary <= frame)
            due.push_back(pending.id);
    if (due.empty())
        return;
    // 先読みの幅 (2 秒) の間に open / seek が終わらなかった。境界ではこの source が要るので待つ。
    ++playbackPreparationWaitCount_;
    const auto began = std::chrono::steady_clock::now();
    for (const auto id : due)
        adoptPreparationOutcome(id, previewEngine_->waitSourcePreparation(id));
    playbackMaxPreparationMs_ =
        std::max(playbackMaxPreparationMs_,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                     .count());
}

void MvmController::blockNextSourcePreparationForTest(int milliseconds) {
    if (previewEngine_)
        preview::internal::PreviewRenderPort::blockNextSourcePreparationForTest(*previewEngine_,
                                                                                milliseconds);
}

void MvmController::holdSourcePreparationsForTest(bool held) {
    if (previewEngine_)
        preview::internal::PreviewRenderPort::holdSourcePreparationsForTest(*previewEngine_, held);
}

std::uint64_t MvmController::publishedPreviewSourceCountForTest() const {
    return previewEngine_
               ? preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_)
                     .publishedSourceCount
               : 0;
}

std::uint64_t MvmController::engineStaleSourcePreparationCountForTest() const {
    return previewEngine_
               ? preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_)
                     .staleSourcePreparationRejectCount
               : 0;
}

bool MvmController::disablePreviewAudioSourcesForTest() {
    if (!previewEngine_)
        return false;
    return static_cast<bool>(
        preview::internal::PreviewRenderPort::disableAudioSourcesForTest(*previewEngine_));
}

std::vector<std::int64_t> MvmController::presentedFrameHistoryForTest() const {
    if (!previewEngine_)
        return {};
    return preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_)
        .recentPresentedOutputFrames;
}

std::vector<std::pair<std::int64_t, float>>
MvmController::presentedOverlayOpacityHistoryForTest() const {
    if (!previewEngine_)
        return {};
    const auto diagnostics =
        preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_);
    std::vector<std::pair<std::int64_t, float>> history;
    for (std::size_t index = 0; index < diagnostics.recentPresentedOutputFrames.size(); ++index)
        history.emplace_back(diagnostics.recentPresentedOutputFrames[index],
                             diagnostics.recentPresentedLayerCounts[index] > 1
                                 ? diagnostics.recentPresentedTopLayerOpacities[index]
                                 : -1.0F);
    return history;
}

std::vector<MvmController::PresentedFrameForTest> MvmController::presentedFramesForTest() const {
    if (!previewEngine_)
        return {};
    const auto diagnostics =
        preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_);
    std::vector<PresentedFrameForTest> frames;
    for (std::size_t index = 0; index < diagnostics.recentPresentedOutputFrames.size(); ++index)
        frames.push_back({diagnostics.recentPresentedOutputFrames[index],
                          diagnostics.recentPresentedLayerCounts[index],
                          diagnostics.recentPresentedBaseSourceFrames[index]});
    return frames;
}

float MvmController::audioEndpointVolumeForTest() const {
    if (!previewEngine_)
        return -1.0F;
    return preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_)
        .audioEndpointVolume;
}

MvmController::PresentedFrameForTest MvmController::lastPresentedFrameForTest() const {
    const auto frames = presentedFramesForTest();
    return frames.empty() ? PresentedFrameForTest{} : frames.back();
}

std::vector<std::int64_t> MvmController::unpairedFrameHistoryForTest() const {
    if (!previewEngine_)
        return {};
    return preview::internal::PreviewRenderPort::runtimeDiagnostics(*previewEngine_)
        .recentUnpairedOutputFrames;
}

bool MvmController::preparePlaybackSourcesAt(std::int64_t frame, bool& needsHandOff,
                                             QString& reason) {
    needsHandOff = false;
    playbackCapacityFailure_ = false;
    const auto video = mapTimelinePreviewFrame(project_, previewPlan(), frame);
    const auto audio = mapTimelinePreviewAudio(project_, previewPlan(), frame);
    if (!video.success || !audio.success) {
        reason = QString::fromStdString(video.success ? audio.error : video.error);
        return false;
    }
    std::set<std::uint64_t> usedVideoSources;
    std::set<std::uint64_t> usedVideoPreparations;
    for (const auto& layer : video.layers) {
        const auto covers = [&](const TrackPreviewSource& entry) {
            return !usedVideoSources.contains(entry.source.value) &&
                   previewVideoMappingCovers(project_, entry.mapping, layer.renderClip);
        };
        const auto active = std::find_if(trackSources_.begin(), trackSources_.end(),
                                         [&](const auto& entry) { return covers(entry.second); });
        const auto prepared =
            std::find_if(preparedVideoSources_.begin(), preparedVideoSources_.end(), covers);
        if (active != trackSources_.end()) {
            usedVideoSources.insert(active->second.source.value);
            continue;
        }
        needsHandOff = true;
        if (prepared != preparedVideoSources_.end()) {
            usedVideoSources.insert(prepared->source.value);
            continue;
        }
        const auto pending = std::find_if(
            pendingVideoPreparations_.begin(), pendingVideoPreparations_.end(),
            [&](const PendingVideoPreparation& entry) {
                return !usedVideoPreparations.contains(entry.id.value) &&
                       previewVideoMappingCovers(project_, entry.entry.mapping, layer.renderClip);
            });
        if (pending != pendingVideoPreparations_.end()) {
            usedVideoPreparations.insert(pending->id.value);
            continue;
        }
        const auto& clip = layer.renderClip;
        if (!std::filesystem::is_regular_file(clip.mediaPath)) {
            reason = QString::fromStdString(clip.name) + QStringLiteral(" のファイルがありません");
            return false;
        }
        // open / seek は engine の準備用の thread が行う。ここで測るのは control thread が
        // 要求に使った時間だけ (完了を待たない)。
        const auto began = std::chrono::steady_clock::now();
        const auto requested =
            previewEngine_->requestSourcePreparation(previewVideoDescriptorOf(project_, clip));
        playbackMaxPreparationMs_ = std::max(
            playbackMaxPreparationMs_,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                .count());
        if (!requested) {
            playbackCapacityFailure_ =
                requested.error().code == preview::PreviewErrorCode::RegistrationCapacityExceeded;
            reason = previewErrorText(requested.error());
            return false;
        }
        pendingVideoPreparations_.push_back(
            {requested.value(),
             {{}, layer.clipId, layer.clipIndex, previewVideoMappingOf(clip)},
             frame,
             playbackPreparationGeneration_});
        usedVideoPreparations.insert(requested.value().value);
    }
    std::vector<AudioSourceIdentity> identities;
    if (!audioIdentitiesFor(audio, identities, reason))
        return false;
    for (std::size_t index = 0; index < identities.size(); ++index) {
        const auto matches = [&](const AudioPreviewSource& entry) {
            return entry.identity == identities[index];
        };
        if (std::any_of(audioSources_.begin(), audioSources_.end(), matches))
            continue;
        needsHandOff = true;
        if (std::any_of(preparedAudioSources_.begin(), preparedAudioSources_.end(), matches))
            continue;
        if (std::any_of(pendingAudioPreparations_.begin(), pendingAudioPreparations_.end(),
                        [&](const PendingAudioPreparation& entry) { return matches(entry.entry); }))
            continue;
        preview::PreviewSourceDescriptor descriptor;
        if (!audioDescriptorFor(audio.layers[index], descriptor, reason))
            return false;
        const auto began = std::chrono::steady_clock::now();
        const auto requested = previewEngine_->requestSourcePreparation(descriptor);
        playbackMaxPreparationMs_ = std::max(
            playbackMaxPreparationMs_,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                .count());
        if (!requested) {
            playbackCapacityFailure_ =
                requested.error().code == preview::PreviewErrorCode::RegistrationCapacityExceeded;
            reason = previewErrorText(requested.error());
            return false;
        }
        pendingAudioPreparations_.push_back({requested.value(),
                                             {{},
                                              identities[index],
                                              descriptor,
                                              audio.layers[index].clipId,
                                              audio.layers[index].clipIndex},
                                             frame,
                                             playbackPreparationGeneration_});
    }
    playbackMaxPreparedSourceCount_ =
        std::max(playbackMaxPreparedSourceCount_,
                 preparedVideoSources_.size() + preparedAudioSources_.size() +
                     pendingVideoPreparations_.size() + pendingAudioPreparations_.size());
    return true;
}

bool MvmController::prepareUpcomingPlaybackSources(std::int64_t frame, QString& reason) {
    const auto& plan = previewPlan();
    if (!plan.success) {
        reason = QString::fromStdString(plan.error);
        return false;
    }
    const std::int64_t lead =
        (2 * project_.timelineFpsNum + project_.timelineFpsDen - 1) / project_.timelineFpsDen;
    const std::int64_t horizon = std::min(totalTimelineFrames_, frame + lead);
    std::set<std::int64_t> starts;
    for (const auto& entry : plan.video)
        if (entry.start > frame && entry.start <= horizon)
            starts.insert(entry.start);
    for (const auto& entry : plan.audio)
        if (entry.start > frame && entry.start <= horizon)
            starts.insert(entry.start);
    // 準備するのは source 集合が次に変わる境界 1 つだけにする。先読み幅の中の境界を全部準備すると、
    // 短い clip が続く timeline では使う前の source で engine の登録上限を踏み、境界で Preview の
    // 組み直しへ戻ってしまう。その境界を越えて引き継いだ後の tick で、さらに次を準備する。
    for (const auto start : starts) {
        // 失敗した境界は越えるまで準備し直さない。seek の待ちを毎 tick 繰り返すと、壊れた素材で
        // 境界の手前から再生が引っ掛かり続ける。境界では引き継ぎに失敗して組み直しへ回る。
        if (failedPreparationStart_ && *failedPreparationStart_ == start) {
            reason = playbackPreparationFailure_;
            return false;
        }
        bool needsHandOff = false;
        if (!preparePlaybackSourcesAt(start, needsHandOff, reason)) {
            // 引き継いだ直後は、旧 source が新しい composition の提示まで登録枠を使い続ける
            // (8 layer の cut なら旧 8 + 新 8)。取り消した準備も、engine が受け取るまで枠を
            // 使い続ける。この間の登録上限は一時的な不足なので失敗として覚えず、枠が返った後の
            // tick で準備し直す。登録上限の検査は open の前に行われるので、準備し直しても待たない。
            if (playbackCapacityFailure_ &&
                (!retiredSources_.empty() || !stalePreparations_.empty()))
                return false;
            failedPreparationStart_ = start;
            ++playbackPreparationFailureCount_;
            return false;
        }
        if (needsHandOff)
            return true;
    }
    return true;
}

bool MvmController::handOffPlaybackSources(std::int64_t frame, QString& reason) {
    // 完了した準備を受け取り、この境界までに終わらなかったものだけを待つ。
    collectSourcePreparations();
    waitDueSourcePreparations(frame);
    const auto mappedFrame = mapTimelinePreviewFrame(project_, previewPlan(), frame);
    if (!mappedFrame.success) {
        reason = QString::fromStdString(mappedFrame.error);
        return false;
    }
    std::map<int, TrackPreviewSource> sources;
    std::set<std::uint64_t> usedVideoSources;
    for (const auto& layer : mappedFrame.layers) {
        const auto active =
            std::find_if(trackSources_.begin(), trackSources_.end(), [&](const auto& entry) {
                return !usedVideoSources.contains(entry.second.source.value) &&
                       previewVideoMappingCovers(project_, entry.second.mapping, layer.renderClip);
            });
        const auto prepared = std::find_if(
            preparedVideoSources_.begin(), preparedVideoSources_.end(), [&](const auto& entry) {
                return !usedVideoSources.contains(entry.source.value) &&
                       previewVideoMappingCovers(project_, entry.mapping, layer.renderClip);
            });
        if (active == trackSources_.end() && prepared == preparedVideoSources_.end()) {
            reason = QString::fromStdString(layer.renderClip.name) +
                     QStringLiteral(" のvideo sourceを準備できませんでした");
            return false;
        }
        auto selected = active != trackSources_.end() ? active->second : *prepared;
        selected.clipId = layer.clipId;
        selected.clipIndex = layer.clipIndex;
        sources[layer.slot] = std::move(selected);
        usedVideoSources.insert(sources[layer.slot].source.value);
    }
    const auto audioMapping = mapTimelinePreviewAudio(project_, previewPlan(), frame);
    if (!audioMapping.success) {
        reason = QString::fromStdString(audioMapping.error);
        return false;
    }
    std::vector<AudioSourceIdentity> desired;
    if (!audioIdentitiesFor(audioMapping, desired, reason))
        return false;
    std::vector<AudioPreviewSource> audio;
    for (std::size_t index = 0; index < desired.size(); ++index) {
        const auto active =
            std::find_if(audioSources_.begin(), audioSources_.end(),
                         [&](const auto& entry) { return entry.identity == desired[index]; });
        const auto prepared =
            std::find_if(preparedAudioSources_.begin(), preparedAudioSources_.end(),
                         [&](const auto& entry) { return entry.identity == desired[index]; });
        if (active == audioSources_.end() && prepared == preparedAudioSources_.end()) {
            reason = QString::fromStdString(audioMapping.layers[index].segment.clip.name) +
                     QStringLiteral(" のaudio sourceを準備できませんでした");
            return false;
        }
        auto selected = active != audioSources_.end() ? *active : *prepared;
        selected.clipId = audioMapping.layers[index].clipId;
        selected.clipIndex = audioMapping.layers[index].clipIndex;
        audio.push_back(std::move(selected));
    }
    // 文字 clip は再生中にも出入りする。video / audio が変わらなくても composition は
    // 毎回組み、前回と違うときだけ出し直す (同じ文字は同じ画像 instance なので安い)。
    // 引き継いだ clip の effect もここで反映する。
    preview::PreviewFrameRequest unusedRequest;
    const auto composition = previewCompositionFor(mappedFrame, sources, unusedRequest, reason);
    if (!composition)
        return false;
    refreshSelectedMathTransformStatus();
    if (!submittedComposition_ || submittedComposition_->layers != composition->layers) {
        auto scheduled = std::make_shared<preview::CompositionSnapshot>(*composition);
        // engine は activation の frame に届くまで前に提示した composition を使い続け、保留は
        // 最新の 1 つしか持たない。engine の提示はこの tick の frame より遅れているので、
        // トランジションや fade で毎 tick 変わる不透明度を毎回この frame から有効にすると、
        // 届く前に次の tick で上書きされ続けて区間の終わりまで反映されない。重ねる source が
        // 変わらない出し直しは、重ね方が変わった frame の activation を引き継ぐ。
        scheduled->activationOutputFrame =
            submittedComposition_ && sameLayerSources(*submittedComposition_, *composition)
                ? submittedComposition_->activationOutputFrame
                : frame;
        const auto submitted = previewEngine_->submitComposition(scheduled);
        if (!submitted) {
            reason = previewErrorText(submitted.error());
            return false;
        }
        submittedComposition_ = std::move(scheduled);
    }
    const auto selectedVideo = [&](preview::PreviewSourceId source) {
        return std::any_of(sources.begin(), sources.end(),
                           [&](const auto& entry) { return entry.second.source == source; });
    };
    const auto selectedAudio = [&](preview::PreviewSourceId source) {
        return std::any_of(audio.begin(), audio.end(),
                           [&](const auto& entry) { return entry.source == source; });
    };
    for (const auto& [slot, source] : trackSources_)
        if (!selectedVideo(source.source))
            retiredSources_.push_back(source.source);
    for (const auto& source : audioSources_)
        if (!selectedAudio(source.source))
            retiredSources_.push_back(source.source);
    std::erase_if(preparedVideoSources_,
                  [&](const auto& entry) { return selectedVideo(entry.source); });
    std::erase_if(preparedAudioSources_,
                  [&](const auto& entry) { return selectedAudio(entry.source); });
    trackSources_ = std::move(sources);
    audioSources_ = std::move(audio);
    currentSource_.reset();
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == currentClipIndex_)
            currentSource_ = slot.source;
    }
    return true;
}

void MvmController::advanceTimelinePlayback() {
    if (!playing_ || !playbackClock_.isValid())
        return;
    const auto mapped = timelineFrameFromElapsed(playbackBaseFrame_, playbackClock_.nsecsElapsed(),
                                                 project_.timelineFpsNum, project_.timelineFpsDen);
    if (!mapped.success) {
        stopPlaybackWithError(QString::fromStdString(mapped.error));
        return;
    }
    const std::int64_t frame = std::min(mapped.frame, totalTimelineFrames_);
    if (frame >= totalTimelineFrames_) {
        playbackTimer_.stop();
        playbackClock_.invalidate();
        previewEngine_->pause();
        playheadFrame_ = totalTimelineFrames_;
        playing_ = false;
        shuttleRate_ = 0;
        playbackClipIndex_ = -1;
        retirePreparedPlaybackSources();
        statusText_ = QStringLiteral("timeline終端まで再生しました");
        Q_EMIT stateChanged();
        return;
    }
    QString handOffFailure;
    if (handOffPlaybackSources(frame, handOffFailure)) {
        if (failedPreparationStart_ && frame >= *failedPreparationStart_) {
            playbackPreparationFailure_.clear();
            failedPreparationStart_.reset();
            playbackCapacityFailure_ = false;
        }
        // 先読みは引き継ぎの後に行う。境界の tick で先に行うと、まだ引き継いでいない境界の source
        // に 加えてその次の境界まで準備してしまう。旧 source の削除は状態の poll (100ms) を待たず
        // ここでも行い、次の境界の準備に登録枠を早く返す。
        removeRetiredSources(previewEngine_->status());
        QString preparationFailure;
        if (!prepareUpcomingPlaybackSources(frame, preparationFailure))
            playbackPreparationFailure_ = preparationFailure;
        else
            playbackPreparationFailure_.clear();
        if (playheadFrame_ != frame) {
            playheadFrame_ = frame;
            Q_EMIT stateChanged();
        }
        return;
    }

    if (!playbackPreparationFailure_.isEmpty())
        handOffFailure += QStringLiteral("; 先読み: ") + playbackPreparationFailure_;
    lastPlaybackRebuildReason_ = handOffFailure;
    ++playbackRebuildCount_;
    const bool capacityFailure = playbackCapacityFailure_;
    retirePreparedPlaybackSources();
    // 取り消した準備・削除待ちの旧 source が枠を持っているだけなら、枠は待てば返る。engine を
    // 作り直すと、取り消しの効かない段 (decoder の seek の途中など) にいる準備の thread を
    // control thread で join することになる。
    const bool transientCapacity =
        capacityFailure && (!stalePreparations_.empty() || !retiredSources_.empty());
    playbackTimer_.stop();
    playbackClock_.invalidate();
    const auto paused = previewEngine_->pause();
    if (!paused) {
        stopPlaybackWithError(QStringLiteral("clip境界でPreviewを停止できません: ") +
                              previewErrorText(paused.error()));
        return;
    }
    playheadFrame_ = frame;
    if (transientCapacity) {
        ++playbackSlotWaitCount_;
        playing_ = false;
        pendingSlotRebuildFrame_ = frame;
        statusText_ = QStringLiteral("登録枠が空くのを待って再生を続けます: ") + handOffFailure;
        Q_EMIT stateChanged();
        return;
    }
    if (capacityFailure) {
        ++playbackCapacityResetCount_;
        if (!resetPreviewEngine()) {
            stopPlaybackWithError(QStringLiteral("登録上限でPreviewを再初期化できません: ") +
                                  statusText_);
            return;
        }
        playbackTimer_.stop();
        playing_ = false;
        pendingCapacityRebuildFrame_ = frame;
        statusText_ = QStringLiteral("登録上限でPreviewを組み直しています: ") + handOffFailure;
        Q_EMIT stateChanged();
        return;
    }
    const auto* selected = topVideoClipAt(project_, frame);
    const int nextClip = selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
    if (!queuePreparedPlayback(nextClip, frame)) {
        stopPlaybackWithError(QStringLiteral("次のclipへ切り替えられません: ") + handOffFailure +
                              QStringLiteral("; ") + statusText_);
        return;
    }
    // 引き継げずに組み直したことと、その理由を残す。境界で一瞬止まる原因の手がかりになる。
    statusText_ = QStringLiteral("clip境界でPreviewを組み直しています: ") + handOffFailure;
    Q_EMIT stateChanged();
}

bool MvmController::cancelPendingPlaybackForPause() {
    if (!pendingPlaybackStart_)
        return false;
    pendingPlaybackStart_ = false;
    pendingPlaybackClipIndex_ = -1;
    playing_ = false;
    playbackClipIndex_ = -1;
    statusText_ = QStringLiteral("timelineを一時停止しました");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::pauseTimeline() {
    if (shuttleRate_ != 0 && !shuttleSeeking_) {
        const bool timedShuttle = shuttleTimer_.isActive();
        // 最後の tick から今までに進んだ分を playhead へ確定させてから止める。
        // tick の位置のまま止めると、停止位置が最大 1 tick 分 (16x で約 640ms) 古くなる。
        // audio clock を読むので、音声を止める前に行う。
        // 確定できなくても transport は必ず止め、古い位置で止まったことを status に残す。
        QString clockError;
        if (timedShuttle) {
            std::int64_t frame = 0;
            if (shuttleFrameFromClock(frame, clockError))
                playheadFrame_ = frame;
            else if (clockError.isEmpty())
                clockError = QStringLiteral("原因を取得できませんでした");
        }
        shuttleTimer_.stop();
        shuttleClock_.invalidate();
        if (shuttleAudio_) {
            shuttleAudio_->stop();
            shuttleAudio_.reset();
        }
        shuttleRate_ = 0;
        if (!playing_) {
            if (timedShuttle) {
                scrubTargetFrame_ = playheadFrame_;
                scrubPending_ = true;
                scrubTimer_.start();
            }
            setStatus(clockError.isEmpty()
                          ? QStringLiteral("シャトルを停止しました")
                          : QStringLiteral("シャトル停止位置を確定できません: ") + clockError);
            return true;
        }
    }
    if (pendingSlotRebuildFrame_) {
        pendingSlotRebuildFrame_.reset();
        statusText_ = QStringLiteral("timelineを一時停止しました");
        Q_EMIT stateChanged();
    }
    if (!playing_)
        return true;
    if (cancelPendingPlaybackForPause())
        return true;
    advanceTimelinePlayback();
    if (!playing_)
        return true;
    if (cancelPendingPlaybackForPause())
        return true;
    playbackTimer_.stop();
    const auto paused = previewEngine_->pause();
    if (!paused) {
        stopPlaybackWithError(QStringLiteral("timelineを一時停止できません: ") +
                              previewErrorText(paused.error()));
        return false;
    }
    playbackClock_.invalidate();
    playing_ = false;
    playbackClipIndex_ = -1;
    retirePreparedPlaybackSources();
    statusText_ = QStringLiteral("timelineを一時停止しました");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::shuttleLeft() {
    return changeShuttleRate(-1);
}

bool MvmController::shuttleRight() {
    return changeShuttleRate(1);
}

bool MvmController::changeShuttleRate(int direction) {
    if (busy_ || totalTimelineFrames_ <= 0 || direction == 0)
        return false;
    if (shuttleTimer_.isActive())
        advanceTimelineShuttle();
    const int current = shuttleRate_ != 0 ? shuttleRate_ : (playing_ ? 1 : 0);
    const auto next = nextShuttleRate(current, direction);
    if (!next)
        return false;
    if (!pauseTimeline())
        return false;
    if (*next == 0)
        return true;
    scrubPending_ = false;
    if (!scrubbing_)
        scrubTimer_.stop();
    stopScrubAudio();
    if (*next == 1) {
        if (!playTimeline())
            return false;
        shuttleRate_ = 1;
        Q_EMIT stateChanged();
        return true;
    }
    shuttleRate_ = *next;
    shuttleBaseFrame_ = std::clamp<std::int64_t>(playheadFrame_, 0, totalTimelineFrames_ - 1);
    shuttleAudioFailure_.clear();
    // 鳴らす clip が無ければ WASAPI を開かず、timer clock だけで動かす。
    if (std::abs(*next) <= 4 && hasShuttleAudibleClip(project_)) {
        auto shuttleAudio = std::make_unique<ShuttleAudioPlayback>();
        std::string audioError;
        if (shuttleAudio->start(project_, *next, shuttleBaseFrame_,
                                static_cast<float>(masterVolume_), audioError)) {
            shuttleAudio_ = std::move(shuttleAudio);
        } else {
            // 音声 device の障害で映像の transport 操作まで止めない。
            // 無音で続けていることは status に出し続ける。
            shuttleAudioFailure_ = QString::fromStdString(audioError);
        }
    }
    shuttleClock_.restart();
    shuttleTimer_.start();
    setStatus(shuttleStatusText());
    return true;
}

void MvmController::advanceTimelineShuttle() {
    if (!shuttleTimer_.isActive() || !shuttleClock_.isValid())
        return;
    if (shuttleAudio_ && !shuttleAudio_->error().empty()) {
        const QString error = QString::fromStdString(shuttleAudio_->error());
        pauseTimeline();
        setStatus(QStringLiteral("シャトル音声を再生できません: ") + error);
        return;
    }
    std::int64_t frame = 0;
    QString clockError;
    if (!shuttleFrameFromClock(frame, clockError)) {
        // 停止時にもう一度 clock を読み、確定できなかったことを pauseTimeline が status へ出す。
        pauseTimeline();
        return;
    }
    shuttleSeeking_ = true;
    const bool updated = seekTimelineFrame(frame);
    shuttleSeeking_ = false;
    if (!updated) {
        if (previewEngine_->status().state == preview::PreviewEngineState::Error) {
            const QString failure = statusText_;
            pauseTimeline();
            setStatus(failure);
        }
        return;
    }
    if ((shuttleRate_ < 0 && frame == 0) ||
        (shuttleRate_ > 0 && frame == totalTimelineFrames_ - 1)) {
        pauseTimeline();
        return;
    }
    statusText_ = shuttleStatusText();
    Q_EMIT stateChanged();
}

bool MvmController::shuttleFrameFromClock(std::int64_t& frame, QString& error) const {
    const std::int64_t playedSamples = shuttleAudio_ ? shuttleAudio_->elapsedSamples() : 0;
    const std::int64_t elapsedNs =
        shuttleAudio_ ? (playedSamples / audio::kInternalSampleRate) * 1'000'000'000LL +
                            (playedSamples % audio::kInternalSampleRate) * 1'000'000'000LL /
                                audio::kInternalSampleRate
                      : shuttleClock_.nsecsElapsed();
    const auto mapped = timelineShuttleFrameFromElapsed(
        shuttleBaseFrame_, elapsedNs, project_.timelineFpsNum, project_.timelineFpsDen,
        shuttleRate_, totalTimelineFrames_ - 1);
    if (!mapped.success) {
        error = QString::fromStdString(mapped.error);
        return false;
    }
    frame = mapped.frame;
    return true;
}

QString MvmController::shuttleStatusText() const {
    const QString audio =
        shuttleAudio_ ? QStringLiteral("（音声あり）")
        : shuttleAudioFailure_.isEmpty()
            ? QStringLiteral("（音声なし）")
            : QStringLiteral("（音声を開始できないため無音: %1）").arg(shuttleAudioFailure_);
    return QStringLiteral("シャトル %1 倍速%2").arg(shuttleRate_).arg(audio);
}

bool MvmController::stepTimelineFrames(int delta) {
    if (busy_ || totalTimelineFrames_ <= 0 || delta == 0)
        return false;
    // 再生・シャトル中は止めてから、止まった位置を基準に動かす。
    if (!pauseTimeline())
        return false;
    const auto lastFrame = totalTimelineFrames_ - 1;
    const auto baseFrame = std::clamp<std::int64_t>(playheadFrame_, 0, totalTimelineFrames_);
    const auto amount = static_cast<std::int64_t>(delta);
    const auto target =
        amount > 0 ? baseFrame + std::min(amount, std::max<std::int64_t>(0, lastFrame - baseFrame))
                   : baseFrame - std::min(-amount, baseFrame);
    return seekTimelineFrame(std::min(target, lastFrame));
}

bool MvmController::jumpToEditPoint(int direction) {
    if (busy_ || navigationTimelineFrames() <= 0)
        return false;
    // 再生・シャトル中は止めてから、止まった位置を基準に動かす。
    if (!pauseTimeline())
        return false;
    const auto lastFrame = navigationTimelineFrames() - 1;
    if (playheadFrame_ == totalTimelineFrames_ && direction < 0 &&
        navigationTimelineFrames() == totalTimelineFrames_)
        return seekTimelineFrame(lastFrame);
    const auto point = adjacentTimelineEditPoint(
        project_, std::clamp<std::int64_t>(playheadFrame_, 0, lastFrame), direction, lastFrame);
    if (!point.success) {
        setStatus(QString::fromStdString(point.error));
        return false;
    }
    return seekTimelineFrame(point.frame);
}

qint64 MvmController::navigationTimelineFrames() const {
    qint64 extent = totalTimelineFrames_;
    for (const auto frame : project_.timelineMarkers)
        extent = std::max(extent, frame + 1);
    if (project_.inFrame)
        extent = std::max(extent, *project_.inFrame + 1);
    if (project_.outFrame)
        extent = std::max(extent, *project_.outFrame + 1);
    return extent;
}

bool MvmController::applyMathTransform() {
    if (busy_)
        return false;
    if (selectedEditOutgoing_.empty()) {
        setStatus(QStringLiteral("数式の変形を置く編集点が選択されていません"));
        return false;
    }
    const std::string outgoing = selectedEditOutgoing_;
    const std::string incoming = selectedEditIncoming_;
    // 求める長さは Blend の既定と同じ。cut からの配置・縮め方・条件は model が決める。
    const std::int64_t requestedFrames =
        project::defaultTransitionFrames(project_.timelineFpsNum, project_.timelineFpsDen);
    project::TransitionEditResult placed;
    const bool applied = applyTimelineEdit(
        [&](project::Project& candidate) {
            placed = project::applyMathTransformTransition(candidate, outgoing, incoming,
                                                           requestedFrames, newClipId);
            project::TimelineEditResult result;
            result.success = placed.success;
            result.error = placed.error;
            return result;
        },
        std::string{}, QString());
    if (!applied) {
        // applyTimelineEdit は model の理由をそのまま status に出している。編集点の表示にも残す。
        mathTransformRejection_ = {outgoing, incoming, {}, currentRevision_, statusText_};
        Q_EMIT stateChanged();
        return false;
    }
    mathTransformRejection_ = {};
    selectedEditOutgoing_.clear();
    selectedEditIncoming_.clear();
    selectedTransitionId_ = placed.transitionId;
    QString status =
        QString::number(placed.frames) + QStringLiteral("フレームの数式の変形を作成しました");
    if (placed.frames < requestedFrames)
        status +=
            QStringLiteral("。後ろの数式 clip の尺・区間の見た目の条件に合わせて短くしました");
    setStatus(status);
    notifyTimelineTransitions();
    Q_EMIT stateChanged();
    return true;
}

void MvmController::refreshSelectedMathTransformStatus() {
    // 変形を選んでいるときだけ、描画と memory の状態の key を差し替える (長さの上限などは
    // 編集でしか変わらないので、再生の tick ごとには計算し直さない)。
    if (selectedTransitionId_.empty() ||
        shownSelectedTransition_.value(QStringLiteral("kind")).toString() !=
            QLatin1String(project::transitionKindName(project::TransitionKind::MathTransform)))
        return;
    const auto found = std::find_if(
        project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
        [&](const auto& transition) { return transition.id == selectedTransitionId_; });
    if (found == project_.timelineTransitions.end())
        return;
    auto updated = shownSelectedTransition_;
    const auto status = mathTransformStatus(*found);
    for (auto it = status.cbegin(); it != status.cend(); ++it)
        updated.insert(it.key(), it.value());
    if (updated == shownSelectedTransition_)
        return;
    shownSelectedTransition_ = std::move(updated);
    Q_EMIT selectedTransitionChanged();
}

bool MvmController::editEquationSequenceData(
    const std::string& clipId,
    const std::function<bool(project::EquationSequenceClipData&, std::string&)>& edit) {
    if (busy_ || !pauseTimeline())
        return false;
    auto candidate = project_;
    const auto result = project::editEquationSequence(candidate, clipId, edit);
    if (!result.success) {
        setStatus(QString::fromStdString(result.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate),
                           QStringLiteral("数式 sequence を更新できません: ")))
        return false;
    // 数式 clip の確定と同じく、今の Project の key で描画を要求し直し preview を組み直す。
    // 前の key の animation は P3-4 の retainOnly / memo が捨てる。再生位置は動かさない。
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return true;
}

GraphPreviewCache::Status MvmController::graphPreviewStatus(const std::string& clipId,
                                                            std::int64_t outputFrame) const {
    GraphPreviewCache::Status status;
    status.clipId = clipId;
    status.projectGeneration = projectGeneration_;
    status.projectRevision = currentRevision_;
    const auto found = std::find_if(
        project_.timelineClips.begin(), project_.timelineClips.end(), [&](const auto& clip) {
            return clip.id == clipId && clip.kind == project::TimelineClipKind::Graph;
        });
    if (found == project_.timelineClips.end())
        return status;
    if (!found->enabled) {
        status.reason = GraphPreviewCache::Reason::DisabledClip;
        return status;
    }
    if (!project::isTrackOutputEnabled(project_, found->track)) {
        status.reason = GraphPreviewCache::Reason::HiddenTrack;
        return status;
    }
    if (std::any_of(project_.timelineTransitions.begin(), project_.timelineTransitions.end(),
                    [&](const auto& transition) {
                        return transition.outgoingClipId == clipId ||
                               transition.incomingClipId == clipId;
                    })) {
        status.reason = GraphPreviewCache::Reason::UnsupportedTransition;
        return status;
    }
    auto compiled = compileGraphRender(found->graph, found->sourceFrameCount, project_.outputWidth,
                                       project_.outputHeight);
    if (const auto* error = std::get_if<graph::Error>(&compiled)) {
        status.compileValid = false;
        status.reason = GraphPreviewCache::reasonFor(error->failure);
        status.message = QString::fromStdString(error->message);
        return status;
    }
    std::string error;
    const auto frame =
        project::evaluateGraphClip(*found, {project_.timelineFpsNum, project_.timelineFpsDen},
                                   outputFrame - found->timelineStartFrame, error);
    if (!frame) {
        status.reason = GraphPreviewCache::Reason::InvalidGraph;
        return status;
    }
    auto ready =
        graphRasters_->status(std::get<graph::GraphRenderSpec>(compiled), frame->sourceFrame);
    ready.clipId = clipId;
    ready.projectGeneration = projectGeneration_;
    ready.projectRevision = currentRevision_;
    return ready;
}

bool MvmController::createGraphClip(std::int64_t start, std::optional<project::TrackRef> track) {
    if (busy_ || !pauseTimeline())
        return false;
    auto candidate = project_;
    const auto result =
        track ? project::addGraph(candidate, newClipId(), {newClipId()}, "グラフ", *track, start)
              : project::placeNewGraph(candidate, newClipId(), {newClipId()}, "グラフ", start);
    if (!result.success) {
        setStatus(QString::fromStdString(result.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Graph を作成できません: ")))
        return false;
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return selectClip(result.selectedIndex);
}

bool MvmController::createGraphClipFromUi() {
    // 選択中の clip の track は使わない。再生ヘッドがその clip の中なら重なり、音声 clip なら
    // 種別が違って拒否される。文字・数式と同じ空き映像 track の規則で置く。
    return createGraphClip(playheadFrame_);
}

QVariantMap MvmController::selectedGraphClip() const {
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    if (clip.kind != project::TimelineClipKind::Graph)
        return {};
    const auto& data = clip.graph;
    QVariantList functions;
    for (const auto& f : data.functions) {
        QVariantMap row{{"id", QString::fromStdString(f.id.value)},
                        {"expression", QString::fromStdString(f.expression)},
                        {"label", QString::fromStdString(f.label)},
                        {"color", QString::fromStdString(f.color)},
                        {"strokeWidth", f.strokeWidth},
                        {"domainMin", f.domainMin ? QString::number(*f.domainMin, 'g', 17) : QString()},
                        {"domainMax", f.domainMax ? QString::number(*f.domainMax, 'g', 17) : QString()}};
        functions.push_back(row);
    }
    return {{"clipId", QString::fromStdString(clip.id)},
            {"generation", QString::number(projectGeneration_)},
            {"revision", QString::number(currentRevision_)},
            {"xMin", data.viewport.xMin}, {"xMax", data.viewport.xMax},
            {"yMin", data.viewport.yMin}, {"yMax", data.viewport.yMax},
            {"showAxes", data.axes.showAxes}, {"showGrid", data.axes.showGrid},
            {"xLabel", QString::fromStdString(data.axes.xLabel)},
            {"yLabel", QString::fromStdString(data.axes.yLabel)},
            {"functions", functions}, {"draw", data.intro.kind == project::GraphIntroKind::Draw},
            {"frames", static_cast<qint64>(data.intro.frames)},
            {"sourceFrames", static_cast<qint64>(clip.sourceFrameCount)}};
}

QVariantMap MvmController::editGraphFromUi(const QVariantMap& authority,
                                          const QString& functionId,
                                          const QString& operation,
                                          const QVariantMap& values) {
    const auto current = selectedGraphClip();
    const auto reject = [](const QString& message) -> QVariantMap {
        return {{"ok", false}, {"message", message}};
    };
    if (current.isEmpty() || authority.value("clipId") != current.value("clipId") ||
        authority.value("generation") != current.value("generation") ||
        authority.value("revision") != current.value("revision"))
        return reject(QStringLiteral("編集対象が変わりました。欄を戻してから編集してください"));
    const auto clipId = current.value("clipId").toString().toStdString();
    const auto length = current.value("sourceFrames").toLongLong();
    QString message;
    const bool ok = editGraphData(clipId, [&](auto& data, std::string& error) {
        const auto fail = [&](const QString& text) {
            message = text;
            error = text.toStdString();
            return false;
        };
        const auto number = [&](const QString& key, double& destination) {
            bool valid = false;
            const auto text = values.value(key).toString().trimmed();
            const double value = text.toDouble(&valid);
            if (!valid || !std::isfinite(value))
                return false;
            destination = value;
            return true;
        };
        if (operation == "add") {
            project::GraphFunction f;
            f.id = {newClipId()};
            if (!project::addGraphFunction(data, f, length))
                return fail(QStringLiteral("関数は 1〜3 個です"));
        } else if (operation == "delete") {
            if (!project::deleteGraphFunction(data, {functionId.toStdString()}, length))
                return fail(QStringLiteral("最後の関数、または存在しない関数は削除できません"));
        } else if (operation == "move") {
            if (!project::moveGraphFunction(data, {functionId.toStdString()},
                                            values.value("index").toUInt(), length))
                return fail(QStringLiteral("関数の移動先が不正です"));
        } else if (operation == "field") {
            auto function = std::find_if(data.functions.begin(), data.functions.end(),
                                         [&](const auto& f) { return f.id.value == functionId.toStdString(); });
            if (!functionId.isEmpty() && function == data.functions.end())
                return fail(QStringLiteral("編集する関数がありません"));
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                const auto& key = it.key();
                if (key == "xMin" || key == "xMax" || key == "yMin" || key == "yMax") {
                    double* destination = key == "xMin" ? &data.viewport.xMin :
                                          key == "xMax" ? &data.viewport.xMax :
                                          key == "yMin" ? &data.viewport.yMin : &data.viewport.yMax;
                    if (!number(key, *destination))
                        return fail(QStringLiteral("有限の数値を入力してください"));
                } else if (key == "xLabel") data.axes.xLabel = it.value().toString().toStdString();
                else if (key == "yLabel") data.axes.yLabel = it.value().toString().toStdString();
                else if (key == "showAxes") data.axes.showAxes = it.value().toBool();
                else if (key == "showGrid") data.axes.showGrid = it.value().toBool();
                else if (key == "draw") {
                    data.intro = it.value().toBool() ? project::GraphIntro{project::GraphIntroKind::Draw, 1}
                                                    : project::GraphIntro{};
                } else if (key == "frames") {
                    bool valid = false;
                    const auto frames = it.value().toString().toLongLong(&valid);
                    if (!valid || data.intro.kind != project::GraphIntroKind::Draw)
                        return fail(QStringLiteral("Draw の整数 frame 数を入力してください"));
                    data.intro.frames = frames;
                } else if (function == data.functions.end()) {
                    return fail(QStringLiteral("関数を選択してください"));
                } else if (key == "expression") function->expression = it.value().toString().toStdString();
                else if (key == "label") function->label = it.value().toString().toStdString();
                else if (key == "color") function->color = it.value().toString().toUpper().toStdString();
                else if (key == "strokeWidth") {
                    if (!number(key, function->strokeWidth))
                        return fail(QStringLiteral("有限の線幅を入力してください"));
                } else if (key == "domainMin" || key == "domainMax") {
                    auto& bound = key == "domainMin" ? function->domainMin : function->domainMax;
                    if (it.value().toString().trimmed().isEmpty()) bound.reset();
                    else {
                        double value = 0;
                        if (!number(key, value))
                            return fail(QStringLiteral("範囲は有限の数値、または空欄にしてください"));
                        bound = value;
                    }
                } else return fail(QStringLiteral("未知の Graph 編集項目です"));
            }
        } else return fail(QStringLiteral("未知の Graph 操作です"));
        return true;
    });
    return {{"ok", ok}, {"message", ok ? QString() : message.isEmpty() ? statusText_ : message}};
}

QVariantMap MvmController::graphStatusFromUi(const QString& clipId) const {
    const auto status = graphPreviewStatus(clipId.toStdString(), playheadFrame_);
    const bool ready = status.compileValid && status.validatedSnapshot && status.frameAvailable &&
                       !status.transparentFallback && status.reason == GraphPreviewCache::Reason::None;
    // enum の値に対応する表示。renderer の本文を照合して状態を推定しない。
    const QStringList captions{
        QStringLiteral("表示準備中"), QStringLiteral("グラフの構造または表示範囲が不正です"),
        QStringLiteral("式の構文が不正です。式を修正してください"),
        QStringLiteral("未対応の識別子です。対応する関数名に修正してください"),
        QStringLiteral("定義域に描画できる有限の線がありません"),
        QStringLiteral("renderer が利用できません。Manim の設定を確認してください"),
        QStringLiteral("描画中"), QStringLiteral("現在の成果物がありません"),
        QStringLiteral("成果物が破損しています"), QStringLiteral("成果物の生成元が一致しません"),
        QStringLiteral("現在の frame が未読込です"), QStringLiteral("メモリ上限を超えました"),
        QStringLiteral("描画を取り消しました"), QStringLiteral("新しい編集の描画を待っています"),
        QStringLiteral("グラフの transition は未対応です"), QStringLiteral("描画に失敗しました"),
        QStringLiteral("TeX ラベルを描画できません。ラベルを修正してください"),
        QStringLiteral("成果物を公開できません"), QStringLiteral("描画量の上限を超えました"),
        QStringLiteral("frame を読み込み中"), QStringLiteral("clip が無効です"),
        QStringLiteral("track の出力が無効です")};
    QString caption = captions.value(static_cast<int>(status.reason));
    if (status.backendCheckInProgress)
        caption = QStringLiteral("renderer を確認中");
    return {{"reason", static_cast<int>(status.reason)}, {"job", static_cast<int>(status.job)},
            {"ready", ready}, {"caption", caption}, {"message", status.message}, {"key", status.key},
            {"sourceFrame", static_cast<qint64>(status.sourceFrame)}};
}

bool MvmController::editGraphData(
    const std::string& clipId,
    const std::function<bool(project::GraphClipData&, std::string&)>& edit) {
    if (busy_ || !pauseTimeline())
        return false;
    auto candidate = project_;
    const auto result = project::editGraph(candidate, clipId, edit);
    if (!result.success) {
        setStatus(QString::fromStdString(result.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Graph を更新できません: ")))
        return false;
    requestMathRenders();
    Q_EMIT stateChanged();
    refreshTextPreview();
    return true;
}

bool MvmController::setGraphDuration(const std::string& clipId, std::int64_t frames) {
    if (busy_ || !pauseTimeline())
        return false;
    auto candidate = project_;
    const auto result = project::setGraphSourceDuration(candidate, clipId, frames);
    if (!result.success) {
        setStatus(QString::fromStdString(result.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Graph の尺を更新できません: ")))
        return false;
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::undoLastEdit() {
    return stepEditHistory(undoHistory_, redoHistory_, false);
}

bool MvmController::redoLastEdit() {
    return stepEditHistory(redoHistory_, undoHistory_, true);
}

bool MvmController::stepEditHistory(std::vector<UndoEntry>& from, std::vector<UndoEntry>& to,
                                    bool redo) {
    const QString failurePrefix =
        redo ? QStringLiteral("編集をやり直せません: ") : QStringLiteral("編集を元に戻せません: ");
    if (busy_)
        return false;
    if (!projectLockHeld_) {
        setStatus(failurePrefix + QStringLiteral("Projectを排他できません"));
        return false;
    }
    // 戻す編集が無い・直列化できない場合は再生を止める前に返す。現在の状態 (再生位置を含む) を
    // 履歴へ積むのは止めた後 (止めると再生位置が確定する)。
    if (from.empty()) {
        setStatus(redo ? QStringLiteral("やり直せる編集がありません")
                       : QStringLiteral("元に戻せる編集がありません"));
        return false;
    }
    {
        const auto serialized = project::serializeProjectJson(from.back().project, projectPath_);
        if (!serialized.success) {
            setStatus(failurePrefix + QString::fromStdString(serialized.error));
            return false;
        }
    }
    if (!pauseTimeline())
        return false;
    UndoEntry& entry = from.back();

    // 戻した先から逆向きに辿れるよう、いまの状態を反対側の履歴へ積む。どちらも複製せずに移す。
    // currentClipId() は project_ を読むので、移す前に取る。
    std::string currentId = currentClipId();
    UndoEntry current{std::move(project_), selectedClipIds_, std::move(currentId), playheadFrame_,
                      currentRevision_};
    current.selectedSubtitleId = selectedSubtitleId_;
    current.selectedSubtitleIds = selectedSubtitleIds_;
    current.bytes = project::approximateProjectBytes(current.project);
    const std::vector<std::string> previousSelection = entry.selectedClipIds;
    const std::string previousCurrentClipId = entry.currentClipId;
    selectedSubtitleId_ = entry.selectedSubtitleId;
    // refreshTimelineModel が、戻した Project に無い字幕を選択から外す。
    selectedSubtitleIds_ = entry.selectedSubtitleIds;
    project_ = std::move(entry.project);
    refreshAudioInputAuthority(true);
    playheadFrame_ = entry.playheadFrame;
    currentRevision_ = entry.revision;
    from.pop_back();
    to.push_back(std::move(current));
    trimEditHistory();

    selectedClipIds_.clear();
    for (const auto& id : previousSelection) {
        if (indexOfClipId(project_.timelineClips, id) >= 0)
            selectedClipIds_.push_back(id);
    }
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, previousCurrentClipId));
    refreshTimelineModel();
    scheduleRecoveryAutosave();

    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    if (!resetPreviewEngine()) {
        const QString resetFailure = statusText_;
        setStatus((redo ? QStringLiteral("編集はやり直しましたが、")
                        : QStringLiteral("編集は元に戻しましたが、")) +
                  resetFailure);
        return true;
    }
    setStatus(redo ? QStringLiteral("編集をやり直しました")
                   : QStringLiteral("編集を元に戻しました"));
    return true;
}

bool MvmController::adoptProject(project::Project loaded, std::filesystem::path path,
                                 QString successStatus) {
    settleRecoveryWrite();
    if (!pauseTimeline())
        return false;
    cancelAudioAdjustment();
    audioFileCache_.clear();
    audioContentDirty_ = true;
    project_ = std::move(loaded);
    if (graphRasters_)
        graphRasters_->resetSession();
    refreshAudioInputAuthority(false);
    ++projectGeneration_;
    selectedSubtitleId_.clear();
    selectedSubtitleIds_.clear();
    audioMixerBuses_.clear();
    audioMixerPeaks_.clear();
    projectPath_ = std::move(path);
    syncMathCacheAuthority();
    savedProject_ = project_;
    if (!rememberCanonicalBase())
        statusText_ = QStringLiteral("Project fileの基準hashを記録できません");
    currentRevision_ = 0;
    savedRevision_ = 0;
    nextRevision_ = 1;
    clearEditHistory();
    selectedClipIds_.clear();
    currentClipIndex_ = -1;
    currentClipName_.clear();
    currentClipPath_.clear();
    playheadFrame_ = 0;
    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    recoveryProject_.reset();
    recoveryRevision_ = 0;
    refreshTimelineModel();
    // fps が変わりうるので engine を作り直す。output rate は initialize でしか決まらない。
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("Projectは切り替えましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    setStatus(std::move(successStatus));
    detectRecovery();
    if (!recoveryAvailable() && !recoveryCorrupt_ && !recoveryForeign_) {
        const project::Project beforeRestore = project_;
        restoreFirstManimClip();
        if (project_ != beforeRestore && currentRevision_ == savedRevision_) {
            currentRevision_ = nextRevision_++;
            scheduleRecoveryAutosave();
        }
        // Project切り替え時のasset同期は利用者の編集ではないため、Undo履歴へ残さない。
        clearEditHistory();
    }
    Q_EMIT stateChanged();
    return true;
}


bool MvmController::setTimelineFrameRate(int fpsNum, int fpsDen) {
    return setProjectVideoSettings(project_.outputWidth, project_.outputHeight, fpsNum, fpsDen);
}

QVariantMap MvmController::projectSettingsForClip(const QString& clipId) const {
    QVariantMap settings;
    settings.insert(QStringLiteral("valid"), false);
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0) {
        settings.insert(QStringLiteral("error"), QStringLiteral("対象のclipがありません"));
        return settings;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    if (clip.kind == project::TimelineClipKind::Audio) {
        settings.insert(QStringLiteral("error"),
                        QStringLiteral("音声clipには映像のProject設定がありません"));
        return settings;
    }

    const ProbedMedia media = probeMedia(clip.mediaPath);
    if (!media.success) {
        settings.insert(QStringLiteral("error"), media.error);
        return settings;
    }
    if (!project::isConfigurableTimelineFrameRate(media.fpsNum, media.fpsDen)) {
        settings.insert(
            QStringLiteral("error"),
            QStringLiteral("素材のframe rate（%1/%2 fps）はProject設定で対応していません")
                .arg(media.fpsNum)
                .arg(media.fpsDen));
        return settings;
    }

    // Projectは正方画素だけを持つ。anamorphic素材はSARを横幅へ反映し、
    // yuv420pで書き出せる最も近い偶数pixelへ丸める。
    const double squarePixelWidth = static_cast<double>(media.width) * media.sarNum / media.sarDen;
    const int outputWidth = static_cast<int>(std::llround(squarePixelWidth / 2.0)) * 2;
    const int outputHeight = (media.height + 1) / 2 * 2;
    if (!project::isValidProjectOutputSize(outputWidth, outputHeight)) {
        settings.insert(QStringLiteral("error"),
                        QStringLiteral("素材から導出した出力解像度が対応範囲外です: %1×%2")
                            .arg(outputWidth)
                            .arg(outputHeight));
        return settings;
    }

    QString sourceText =
        QString::number(media.width) + QStringLiteral("×") + QString::number(media.height);
    if (media.sarNum != 1 || media.sarDen != 1)
        sourceText += QStringLiteral(" / SAR ") + QString::number(media.sarNum) +
                      QStringLiteral(":") + QString::number(media.sarDen);
    sourceText +=
        QStringLiteral(" / ") +
        QString::number(static_cast<double>(media.fpsNum) / static_cast<double>(media.fpsDen), 'f',
                        media.fpsDen == 1 ? 0 : 2) +
        QStringLiteral(" fps");

    settings.insert(QStringLiteral("valid"), true);
    settings.insert(QStringLiteral("clipName"), QString::fromStdString(clip.name));
    settings.insert(QStringLiteral("sourceText"), sourceText);
    settings.insert(QStringLiteral("width"), outputWidth);
    settings.insert(QStringLiteral("height"), outputHeight);
    settings.insert(QStringLiteral("fpsNum"), static_cast<qlonglong>(media.fpsNum));
    settings.insert(QStringLiteral("fpsDen"), static_cast<qlonglong>(media.fpsDen));
    settings.insert(QStringLiteral("changes"), project_.outputWidth != outputWidth ||
                                                   project_.outputHeight != outputHeight ||
                                                   project_.timelineFpsNum != media.fpsNum ||
                                                   project_.timelineFpsDen != media.fpsDen);
    return settings;
}

bool MvmController::setProjectVideoSettings(int width, int height, int fpsNum, int fpsDen) {
    if (busy_)
        return false;
    // 変更の無い設定・不正な値は再生を止める前に返す。再生位置の換算は止めた後に行う
    // (止めると再生位置が確定する)。
    if (project_.outputWidth == width && project_.outputHeight == height &&
        project_.timelineFpsNum == fpsNum && project_.timelineFpsDen == fpsDen) {
        setStatus(QStringLiteral("Project設定は変更されていません"));
        return true;
    }
    {
        project::Project probe = project_;
        const auto valid = project::setProjectVideoSettings(probe, width, height, fpsNum, fpsDen);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
    }
    if (!pauseTimeline())
        return false;

    const auto convertedPlayhead = project::sourceBoundaryToTimelineBoundary(
        playheadFrame_, project_.timelineFpsNum, project_.timelineFpsDen, fpsNum, fpsDen);
    if (!convertedPlayhead.success) {
        setStatus(QStringLiteral("再生位置を新しいframe rateへ変換できません"));
        return false;
    }
    project::Project candidate = project_;
    const auto changed = project::setProjectVideoSettings(candidate, width, height, fpsNum, fpsDen);
    if (!changed.success) {
        setStatus(QString::fromStdString(changed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    playheadFrame_ = std::clamp<std::int64_t>(convertedPlayhead.frame, 0,
                                              std::max<std::int64_t>(0, totalTimelineFrames_ - 1));
    // engine の output rate は initialize でしか決まらないので作り直す。
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("frame rateは変更しましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    Q_EMIT stateChanged();
    QString status = QStringLiteral("Project設定を ") + QString::number(project_.outputWidth) +
                     QStringLiteral("×") + QString::number(project_.outputHeight) +
                     QStringLiteral(" / ") + timelineFpsText() + QStringLiteral(" にしました");
    if (!frameRateMeasured())
        status += QStringLiteral("（このrateのpreviewは未計測です）");
    setStatus(std::move(status));
    return true;
}

void MvmController::shutdown() {
    cancelAudioAdjustment();
    transcriptionCancel_.store(true);
    if (transcriptionThread_.joinable())
        transcriptionThread_.join();

    if (shutdownStarted_)
        return;
    shutdownStarted_ = true;
    // 描画中の Manim / LaTeX を process ごと止め、worker が終わるまで待つ。
    if (graphRasters_)
        graphRasters_->shutdown();
    if (mathRasters_)
        mathRasters_->shutdown();
    // shutdown 後に素材の stat・内容 hash を始めない。cancel が再開した poll もここで止める。
    audioWatchFallbackTimer_.stop();
    audioAdjustmentTimer_.stop();
    if (const auto watched = audioFileWatcher_.files(); !watched.isEmpty())
        audioFileWatcher_.removePaths(watched);
    if (projectLockHeld_ && dirty())
        startRecoveryWrite(true);
    settleRecoveryWrite();
    exportCancelRequested_.store(true, std::memory_order_release);
    if (exportThread_.joinable())
        exportThread_.join();
    exporting_ = false;
    exportCancelling_ = false;
    busy_ = false;
    playbackTimer_.stop();
    shuttleTimer_.stop();
    if (shuttleAudio_) {
        shuttleAudio_->stop();
        shuttleAudio_.reset();
    }
    stopScrubAudio();
    scrubTimer_.stop();
    slipPreviewTimer_.stop();
    slipPreview_.reset();
    meterTimer_.stop();
    playing_ = false;
    shuttleRate_ = 0;
    stateTimer_.stop();
    if (previewEngine_)
        previewEngine_->requestShutdown();
    if (previewSurface_)
        previewSurface_->setEngine({});
    releaseProjectLock();
}

} // namespace mvm::app
