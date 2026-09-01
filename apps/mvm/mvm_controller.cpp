#include "mvm_controller.h"

#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/timeline_export.h"
#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "timeline_clip_model.h"

#include <algorithm>
#include <numeric>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>

namespace mvm::app {
namespace {

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString previewErrorText(const preview::PreviewError& error) {
    return QString::fromStdString(error.detail);
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

struct ProbedMedia {
    bool success = false;
    std::int64_t fpsNum = 0;
    std::int64_t fpsDen = 1;
    std::int64_t frameCount = 0;
    QString error;
};

ProbedMedia probeMedia(const std::filesystem::path& path) {
    ProbedMedia result;
    const auto text = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(utf8.c_str(), &probe) != 0 || !probe.ok) {
        result.error = QStringLiteral("素材を解析できません: ") +
                       QString::fromUtf8(probe.error[0] ? probe.error : utf8.c_str());
        return result;
    }
    if (!probe.has_video || probe.is_unbounded_length || probe.frame_count <= 0 ||
        probe.fps_num <= 0 || probe.fps_den <= 0) {
        result.error = QStringLiteral("有限尺と有効なFPSを持つ動画ではありません");
        return result;
    }
    const auto divisor = std::gcd(probe.fps_num, probe.fps_den);
    result.fpsNum = probe.fps_num / divisor;
    result.fpsDen = probe.fps_den / divisor;
    result.frameCount = probe.frame_count;
    result.success = true;
    return result;
}

std::string newClipId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
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

int indexOfClipId(const std::vector<project::TimelineClip>& clips, const std::string& clipId) {
    for (std::size_t index = 0; index < clips.size(); ++index)
        if (clips[index].id == clipId)
            return static_cast<int>(index);
    return -1;
}

} // namespace

MvmController::MvmController(std::filesystem::path projectPath,
                             std::filesystem::path manimExecutablePath, project::Project project,
                             QObject* parent)
    : QObject(parent), projectPath_(std::move(projectPath)),
      manimExecutablePath_(std::move(manimExecutablePath)), project_(std::move(project)),
      previewEngine_(std::make_shared<preview::PreviewEngine>()),
      dispatcher_(std::make_shared<QtEventDispatcher>(this)),
      timelineModel_(std::make_unique<TimelineClipModel>()) {
    refreshTimelineModel();
    initializePreviewEngine(QStringLiteral("Preview初期化に失敗しました: "));
    restoreFirstManimClip();

    playbackTimer_.setInterval(16);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this, &MvmController::advanceTimelinePlayback);

    stateTimer_.setInterval(100);
    connect(&stateTimer_, &QTimer::timeout, this, &MvmController::pollPreviewState);
    stateTimer_.start();
}

MvmController::~MvmController() {
    shutdown();
}

void MvmController::attachPreview(PreviewEngineRhiItem* surface) {
    previewSurface_ = surface;
    if (previewSurface_)
        previewSurface_->setEngine(previewEngine_);
}

QString MvmController::projectPath() const {
    return fromPath(projectPath_);
}

const project::ClipEffects& MvmController::currentEffects() const {
    static const project::ClipEffects defaults;
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return defaults;
    return project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects;
}

double MvmController::effectPositionX() const {
    return currentEffects().positionXPercent;
}

double MvmController::effectPositionY() const {
    return currentEffects().positionYPercent;
}

double MvmController::effectScale() const {
    return currentEffects().scalePercent;
}

double MvmController::effectRotation() const {
    return currentEffects().rotationDegrees;
}

double MvmController::effectOpacity() const {
    return currentEffects().opacityPercent;
}

double MvmController::effectCropLeft() const {
    return currentEffects().cropLeftPercent;
}

double MvmController::effectCropTop() const {
    return currentEffects().cropTopPercent;
}

double MvmController::effectCropRight() const {
    return currentEffects().cropRightPercent;
}

double MvmController::effectCropBottom() const {
    return currentEffects().cropBottomPercent;
}

qint64 MvmController::effectFadeIn() const {
    return currentEffects().fadeInFrames;
}

qint64 MvmController::effectFadeOut() const {
    return currentEffects().fadeOutFrames;
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
    config.output.frameRate = {60, 1};
    const auto initialized = previewEngine_->initialize(config, dispatcher_);
    if (!initialized) {
        statusText_ = failurePrefix + previewErrorText(initialized.error());
        return false;
    }
    return true;
}

bool MvmController::resetPreviewEngine() {
    auto replacement = std::make_shared<preview::PreviewEngine>();
    const auto previous = previewEngine_;
    previewEngine_ = replacement;
    if (!initializePreviewEngine(QStringLiteral("Preview再初期化に失敗しました: "))) {
        previewEngine_ = previous;
        Q_EMIT stateChanged();
        return false;
    }

    currentSource_.reset();
    staleSource_.reset();
    trackSources_ = {};
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

QAbstractItemModel* MvmController::timelineModel() const {
    return timelineModel_.get();
}

QString MvmController::currentTimeText() const {
    const qint64 frames = std::max<qint64>(0, playheadFrame_);
    const qint64 totalSeconds = frames / 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frames % 60, 2, 10, QLatin1Char('0'));
}

bool MvmController::canPlay() const {
    return timelineCanPlay(project_, busy_, playing_, playheadFrame_, totalTimelineFrames_);
}

void MvmController::refreshTimelineModel() {
    if (timelineModel_)
        timelineModel_->setProject(project_);
    const auto timeline = project::validateTimeline(project_);
    totalTimelineFrames_ = timeline.success ? timeline.totalFrames : 0;
    if (totalTimelineFrames_ == 0)
        playheadFrame_ = 0;
    else
        playheadFrame_ = std::clamp<std::int64_t>(playheadFrame_, 0, totalTimelineFrames_ - 1);
}

bool MvmController::saveProject(project::Project candidate, const QString& failurePrefix) {
    const project::ProjectIoResult saved =
        project::saveProjectJsonTransaction(project_, std::move(candidate), projectPath_);
    if (!saved.success) {
        setStatus(failurePrefix + QString::fromStdString(saved.error));
        return false;
    }
    refreshTimelineModel();
    return true;
}

void MvmController::setCurrentClipSelection(int index) {
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
    for (const auto& slot : trackSources_)
        if (slot && slot->clipIndex == index)
            currentSource_ = slot->source;
    Q_EMIT stateChanged();
}

bool MvmController::refreshPreviewAfterSavedEdit(const std::string& selectedClipId,
                                                 const QString& successStatus) {
    const qint64 frame = playheadFrame_;
    const bool refreshed = seekTimelineFrame(frame);
    const QString previewFailure = statusText_;
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, selectedClipId));
    if (!refreshed) {
        setStatus(QStringLiteral("編集は保存されましたが、Previewの更新に失敗しました: ") +
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
        const auto placed = project::appendManimTimelineClipAt(
            candidate, asset, newClipId(), media.fpsNum, media.fpsDen, media.frameCount,
            playheadFrame_);
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
        const auto valid = project::validateTimeline(candidate);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
    }
    return saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: "));
}

void MvmController::pollPreviewState() {
    if (!previewEngine_)
        return;
    const auto status = previewEngine_->status();
    if (status.lastPresentedComposition == status.latestAcceptedDesiredComposition &&
        !retiredSources_.empty()) {
        const auto pendingRetirement = std::move(retiredSources_);
        retiredSources_.clear();
        for (const auto source : pendingRetirement) {
            const auto removed = previewEngine_->removeSource(source);
            if (!removed)
                retiredSources_.push_back(source);
        }
    }
    const bool ready = status.state == preview::PreviewEngineState::ReadyPaused ||
                       status.state == preview::PreviewEngineState::Playing;
    if (previewReady_ != ready) {
        previewReady_ = ready;
        if (ready && !busy_ && currentClipPath_.isEmpty() && !hasManimAsset())
            statusText_ = QStringLiteral("Add Manim Clipを選択してください");
        Q_EMIT stateChanged();
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
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: ") + compositionError);
        return false;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    const std::int64_t timelineFrame =
        clip.timelineStartFrame + (sourceFrame - clip.sourceInFrame);
    if (!syncPreviewSourcesAt(timelineFrame, compositionError)) {
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: ") + compositionError);
        return false;
    }
    currentClipName_ = clipName;
    currentClipPath_ = fromPath(videoPath);
    currentClipIndex_ = clipIndex;
    currentSource_.reset();
    for (const auto& slot : trackSources_)
        if (slot && slot->clipIndex == currentClipIndex_)
            currentSource_ = slot->source;
    statusText_ = clipName + QStringLiteral(" を表示しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error) {
    const auto mappedFrame = mapTimelinePreviewFrame(project_, timelineFrame);
    if (!mappedFrame.success) {
        error = QString::fromStdString(mappedFrame.error);
        return false;
    }
    std::array<const project::TimelineClip*, 2> active{};
    for (const auto& mapping : mappedFrame.layers)
        active[static_cast<std::size_t>(mapping.track)] =
            &project_.timelineClips[static_cast<std::size_t>(mapping.clipIndex)];
    auto candidateSources = trackSources_;
    std::vector<preview::PreviewSourceId> newlyAdded;

    for (std::size_t track = 0; track < active.size(); ++track) {
        const project::TimelineClip* clip = active[track];
        if (!clip) {
            candidateSources[track].reset();
            continue;
        }
        const int clipIndex = static_cast<int>(clip - project_.timelineClips.data());
        if (candidateSources[track] && candidateSources[track]->clipId == clip->id)
            continue;
        if (!std::filesystem::is_regular_file(clip->mediaPath)) {
            error = QString::fromStdString(clip->name) + QStringLiteral(" のファイルがありません: ") +
                    fromPath(clip->mediaPath);
            return false;
        }
        if (!project::sourceRateMatchesTimelineRate(project_, *clip)) {
            error = QString::fromStdString(clip->name) +
                    QStringLiteral(" は60fpsではないためPreview未対応です");
            return false;
        }
        preview::PreviewSourceDescriptor descriptor;
        descriptor.mediaPath = clip->mediaPath;
        descriptor.videoEnabled = true;
        const auto added = previewEngine_->addSource(descriptor);
        if (!added) {
            error = previewErrorText(added.error());
            for (const auto source : newlyAdded)
                previewEngine_->removeSource(source);
            return false;
        }
        newlyAdded.push_back(added.value());
        candidateSources[track] = TrackPreviewSource{added.value(), clip->id, clipIndex};
    }

    auto composition = std::make_shared<preview::CompositionSnapshot>();
    preview::PreviewFrameRequest request;
    request.outputFrameNumber = timelineFrame;
    for (std::size_t track = 0; track < active.size(); ++track) {
        const project::TimelineClip* clip = active[track];
        if (!clip || !candidateSources[track])
            continue;
        preview::PreviewCompositionLayer layer;
        layer.source = candidateSources[track]->source;
        if (!project::clipEffectsAreDefault(clip->effects)) {
            const auto mapped = project::mapClipEffects(clip->effects);
            layer.destination = {static_cast<float>(mapped.destinationRect.x),
                                 static_cast<float>(mapped.destinationRect.y),
                                 static_cast<float>(mapped.destinationRect.width),
                                 static_cast<float>(mapped.destinationRect.height)};
            layer.sourceRect = {static_cast<float>(mapped.sourceRect.x),
                                static_cast<float>(mapped.sourceRect.y),
                                static_cast<float>(mapped.sourceRect.width),
                                static_cast<float>(mapped.sourceRect.height)};
            layer.opacity = static_cast<float>(mapped.baseOpacity);
            layer.effectsEnabled = true;
            layer.rotationDegrees = static_cast<float>(mapped.rotationDegrees);
            layer.sourceInFrame = clip->sourceInFrame;
            layer.sourceDurationFrames = clip->sourceOutFrame - clip->sourceInFrame;
            layer.fadeInFrames = mapped.fadeInFrames;
            layer.fadeOutFrames = mapped.fadeOutFrames;
        }
        // activeClipsAtはV1,V2の順なので、この挿入順がbottom/top authorityになる。
        composition->layers.push_back(layer);
        const auto& layerMapping = *std::find_if(
            mappedFrame.layers.begin(), mappedFrame.layers.end(), [track](const auto& mapping) {
                return static_cast<std::size_t>(mapping.track) == track;
            });
        request.sources.push_back(
            {candidateSources[track]->source, layerMapping.sourceFrameNumber});
    }
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted) {
        error = previewErrorText(submitted.error());
        for (const auto source : newlyAdded)
            previewEngine_->removeSource(source);
        return false;
    }
    const auto sought = previewEngine_->seekFrameRequest(request);
    if (!sought) {
        error = previewErrorText(sought.error());
        return false;
    }
    for (std::size_t track = 0; track < trackSources_.size(); ++track) {
        if (trackSources_[track] &&
            (!candidateSources[track] ||
             trackSources_[track]->source != candidateSources[track]->source))
            retiredSources_.push_back(trackSources_[track]->source);
    }
    trackSources_ = std::move(candidateSources);
    currentSource_.reset();
    for (const auto& slot : trackSources_)
        if (slot && slot->clipIndex == currentClipIndex_)
            currentSource_ = slot->source;
    error.clear();
    return true;
}

bool MvmController::submitClipComposition(preview::PreviewSourceId source,
                                          const project::TimelineClip& clip, QString& error) {
    auto composition = std::make_shared<preview::CompositionSnapshot>();
    preview::PreviewCompositionLayer layer;
    layer.source = source;
    if (!project::clipEffectsAreDefault(clip.effects)) {
        const auto mapped = project::mapClipEffects(clip.effects);
        layer.destination = {static_cast<float>(mapped.destinationRect.x),
                             static_cast<float>(mapped.destinationRect.y),
                             static_cast<float>(mapped.destinationRect.width),
                             static_cast<float>(mapped.destinationRect.height)};
        layer.sourceRect = {static_cast<float>(mapped.sourceRect.x),
                            static_cast<float>(mapped.sourceRect.y),
                            static_cast<float>(mapped.sourceRect.width),
                            static_cast<float>(mapped.sourceRect.height)};
        layer.opacity = static_cast<float>(mapped.baseOpacity);
        layer.effectsEnabled = true;
        layer.rotationDegrees = static_cast<float>(mapped.rotationDegrees);
        layer.sourceInFrame = clip.sourceInFrame;
        layer.sourceDurationFrames = clip.sourceOutFrame - clip.sourceInFrame;
        layer.fadeInFrames = mapped.fadeInFrames;
        layer.fadeOutFrames = mapped.fadeOutFrames;
    }
    composition->layers.push_back(layer);
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted) {
        error = previewErrorText(submitted.error());
        return false;
    }
    error.clear();
    return true;
}

bool MvmController::refreshCurrentClipEffectsPreview(QString& error) {
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size())) {
        error = QStringLiteral("選択clipがありません");
        return false;
    }
    return syncPreviewSourcesAt(playheadFrame_, error);
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
    request.width = 640;
    request.height = 360;
    request.fps = 60;
    const ManimClipGenerationResult generated = mvm::app::generateManimClip(project_, request);

    busy_ = false;
    if (!generated.success) {
        QString detail = QString::fromStdString(generated.error);
        if (!generated.stderrText.empty())
            detail += QStringLiteral("\n") + QString::fromStdString(generated.stderrText);
        setStatus(detail);
        return false;
    }

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
    const project::TimelineEditResult placed = project::appendManimTimelineClipAt(
        candidate, candidate.manimAssets.front(), newClipId(), media.fpsNum, media.fpsDen,
        media.frameCount, playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    Q_EMIT stateChanged();
    return selectClip(placed.selectedIndex);
}

bool MvmController::addVideoClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return false;
    }
    const QString localFile = fileUrl.toLocalFile();
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile()) {
        setStatus(QStringLiteral("存在する動画ファイルを選択してください"));
        return false;
    }

    const std::filesystem::path mediaPath(localFile.toStdWString());
    const ProbedMedia media = probeMedia(mediaPath);
    if (!media.success) {
        setStatus(media.error);
        return false;
    }
    project::Project candidate = project_;
    const project::TimelineClip clip{project::TimelineClipKind::Video,
                                     mediaPath,
                                     info.fileName().toStdString(),
                                     newClipId(),
                                     media.fpsNum,
                                     media.fpsDen,
                                     media.frameCount,
                                     0,
                                     media.frameCount,
                                     0,
                                     {}};
    const auto placed = project::appendVideoTimelineClip(candidate, clip);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }

    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    const int index = placed.selectedIndex;
    Q_EMIT stateChanged();
    return selectClip(index);
}

bool MvmController::selectClip(int index) {
    if (index < 0 || index >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    setCurrentClipSelection(index);
    const project::TimelineClip& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    return seekTimelineFrame(clip.timelineStartFrame);
}

bool MvmController::selectTimelineClip(const QString& clipId, qint64 frame) {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0) {
        setStatus(QStringLiteral("選択したclipがありません"));
        return false;
    }
    setCurrentClipSelection(index);
    return seekTimelineFrame(frame);
}

bool MvmController::seekTimelineFrame(qint64 frame) {
    if (!pauseTimeline())
        return false;
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("seekするclipがありません"));
        return false;
    }
    const qint64 clamped = std::clamp<qint64>(frame, 0, totalTimelineFrames_ - 1);
    playheadFrame_ = clamped;
    const auto active = project::activeClipsAt(project_, clamped);
    int index = -1;
    // 選択clipがactiveなら維持し、それ以外はV2を優先してinspector対象にする。
    for (const auto* clip : active)
        if (clip && static_cast<int>(clip - project_.timelineClips.data()) == currentClipIndex_)
            index = currentClipIndex_;
    if (index < 0) {
        const auto* selected = active[1] ? active[1] : active[0];
        if (selected)
            index = static_cast<int>(selected - project_.timelineClips.data());
    }
    Q_EMIT stateChanged();
    if (previewEngine_->status().state != preview::PreviewEngineState::ReadyPaused) {
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
        for (const auto& slot : trackSources_)
            if (slot && slot->clipIndex == index)
                currentSource_ = slot->source;
        setStatus(currentClipName_ + QStringLiteral(" を表示しています"));
    } else {
        currentSource_.reset();
        currentClipName_.clear();
        currentClipPath_.clear();
        setStatus(QStringLiteral("timeline gapを表示しています"));
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
    statusText_ = QStringLiteral("timelineを再生しています");
    Q_EMIT stateChanged();
}

bool MvmController::playTimeline() {
    if (busy_) {
        setStatus(QStringLiteral("処理中はtimelineを再生できません"));
        return false;
    }
    if (playing_)
        return true;
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("再生するclipがありません"));
        return false;
    }
    if (!timelinePreviewCompatible(project_)) {
        setStatus(QStringLiteral(
            "60fpsではないclipを含むためtimeline再生は未対応です。編集・保存・Exportは可能です"));
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
    const auto active = project::activeClipsAt(project_, playheadFrame_);
    const auto* selected = active[1] ? active[1] : active[0];
    const int clipIndex = selected
                              ? static_cast<int>(selected - project_.timelineClips.data())
                              : -1;
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
    pendingPlaybackStart_ = false;
    playbackClipIndex_ = -1;
    pendingPlaybackClipIndex_ = -1;
    setStatus(std::move(error));
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
        playbackClipIndex_ = -1;
        statusText_ = QStringLiteral("timeline終端まで再生しました");
        Q_EMIT stateChanged();
        return;
    }
    const auto active = project::activeClipsAt(project_, frame);
    bool sameSourceSet = true;
    for (std::size_t track = 0; track < active.size(); ++track) {
        const std::string desired = active[track] ? active[track]->id : std::string{};
        const std::string installed = trackSources_[track] ? trackSources_[track]->clipId
                                                          : std::string{};
        sameSourceSet = sameSourceSet && desired == installed;
    }
    if (sameSourceSet) {
        if (playheadFrame_ != frame) {
            playheadFrame_ = frame;
            Q_EMIT stateChanged();
        }
        return;
    }

    playbackTimer_.stop();
    playbackClock_.invalidate();
    const auto paused = previewEngine_->pause();
    if (!paused) {
        stopPlaybackWithError(QStringLiteral("clip境界でPreviewを停止できません: ") +
                              previewErrorText(paused.error()));
        return;
    }
    playheadFrame_ = frame;
    const auto* selected = active[1] ? active[1] : active[0];
    const int nextClip = selected
                             ? static_cast<int>(selected - project_.timelineClips.data())
                             : -1;
    if (!queuePreparedPlayback(nextClip, frame)) {
        stopPlaybackWithError(QStringLiteral("次のclipへ切り替えられません: ") + statusText_);
        return;
    }
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
    statusText_ = QStringLiteral("timelineを一時停止しました");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::moveTimelineClip(const QString& clipId, int destinationTrack,
                                     qint64 timelineStartFrame) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (destinationTrack < static_cast<int>(project::VideoTrack::V1) ||
        destinationTrack > static_cast<int>(project::VideoTrack::V2)) {
        setStatus(QStringLiteral("移動先video trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto moved = project::moveClip(candidate, clipId.toStdString(),
                                         static_cast<project::VideoTrack>(destinationTrack),
                                         std::max<qint64>(0, timelineStartFrame));
    if (!moved.success) {
        setStatus(QString::fromStdString(moved.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    return refreshPreviewAfterSavedEdit(clipId.toStdString(), QStringLiteral("clipを移動しました"));
}

bool MvmController::trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else {
        setStatus(QStringLiteral("trim edgeが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto trimmed =
        project::trimTimelineClip(candidate, clipId.toStdString(), trimEdge, projectFrameDelta);
    if (!trimmed.success) {
        setStatus(QString::fromStdString(trimmed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    return refreshPreviewAfterSavedEdit(clipId.toStdString(), QStringLiteral("clipをtrimしました"));
}

bool MvmController::deleteCurrentClip() {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;

    project::Project candidate = project_;
    const project::TimelineEditResult deleted =
        project::deleteTimelineClip(candidate, currentClipIndex_);
    if (!deleted.success) {
        setStatus(QString::fromStdString(deleted.error));
        return false;
    }

    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;

    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    const bool previewReset = resetPreviewEngine();
    currentSource_.reset();
    staleSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = -1;
    Q_EMIT stateChanged();
    if (!previewReset) {
        const QString resetFailure = statusText_;
        setStatus(QStringLiteral("clipは削除しましたが、") + resetFailure);
        return true;
    }

    if (deleted.selectedIndex >= 0) {
        if (!selectClip(deleted.selectedIndex)) {
            const QString previewFailure = statusText_;
            setStatus(QStringLiteral("clipは削除しましたが、次のclipをPreviewできません: ") +
                      previewFailure);
        }
        return true;
    }

    setStatus(QStringLiteral("timelineから最後のclipを削除しました"));
    return true;
}

bool MvmController::exportTimeline(const QUrl& outputUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("書き出すclipがありません"));
        return false;
    }
    if (!outputUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの書き出し先を指定してください"));
        return false;
    }

    TimelineExportRequest request;
    request.outputPath = std::filesystem::path(outputUrl.toLocalFile().toStdWString());

    // Manim 生成と同じく GUI thread で同期実行する (M4 は非同期 job を作らない)。
    busy_ = true;
    statusText_ = QStringLiteral("書き出しています…");
    Q_EMIT stateChanged();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const TimelineExportResult exported = mvm::app::exportTimeline(project_, request);

    busy_ = false;
    if (!exported.success) {
        setStatus(QStringLiteral("書き出しに失敗しました: ") +
                  QString::fromStdString(exported.error));
        return false;
    }
    setStatus(QStringLiteral("書き出しました: ") + fromPath(exported.outputPath) +
              QStringLiteral(" (") + QString::number(exported.frameCount) +
              QStringLiteral(" frame / ") + QString::number(exported.durationSec, 'f', 2) +
              QStringLiteral(" 秒)"));
    return true;
}

bool MvmController::applyCurrentClipEffects(double positionX, double positionY, double scale,
                                            double rotation, double opacity, double cropLeft,
                                            double cropTop, double cropRight, double cropBottom,
                                            qint64 fadeInSourceFrames, qint64 fadeOutSourceFrames) {
    if (busy_ || currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("effectを適用するclipがありません"));
        return false;
    }
    if (!pauseTimeline())
        return false;

    project::Project candidate = project_;
    project::ClipEffects effects;
    effects.positionXPercent = positionX;
    effects.positionYPercent = positionY;
    effects.scalePercent = scale;
    effects.rotationDegrees = rotation;
    effects.opacityPercent = opacity;
    effects.cropLeftPercent = cropLeft;
    effects.cropTopPercent = cropTop;
    effects.cropRightPercent = cropRight;
    effects.cropBottomPercent = cropBottom;
    effects.fadeInFrames = fadeInSourceFrames;
    effects.fadeOutFrames = fadeOutSourceFrames;
    candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects = effects;
    const auto valid = project::validateTimeline(candidate);
    if (!valid.success) {
        setStatus(QString::fromStdString(valid.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("effectを保存できません: ")))
        return false;
    Q_EMIT stateChanged();

    QString previewError;
    if (!refreshCurrentClipEffectsPreview(previewError)) {
        setStatus(QStringLiteral("effectは保存されましたが、Previewの更新に失敗しました: ") +
                  previewError);
        return true;
    }
    setStatus(QStringLiteral("effectを保存してPreviewへ反映しました"));
    return true;
}

void MvmController::shutdown() {
    if (shutdownStarted_)
        return;
    shutdownStarted_ = true;
    playbackTimer_.stop();
    playing_ = false;
    stateTimer_.stop();
    if (previewEngine_)
        previewEngine_->requestShutdown();
    if (previewSurface_)
        previewSurface_->setEngine({});
}

} // namespace mvm::app
