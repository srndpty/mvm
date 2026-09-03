#include "mvm_controller.h"

#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/timeline_export.h"
#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "core/checked_output_timebase.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "project/clip_effects.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "timeline_clip_model.h"
#include "track_model.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>
#include <QVariantMap>

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

// audio 素材には映像 fps が無い。Project timeline の fps を素材の frame domain と
// して採用し、尺だけを duration から求める。frame 算術を 1 種類に保つための選択で
// あり、素材側に fps があると主張しているわけではない。
ProbedMedia probeAudioMedia(const std::filesystem::path& path, std::int64_t timelineFpsNum,
                            std::int64_t timelineFpsDen) {
    ProbedMedia result;
    const auto text = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(text.data()), text.size());
    MvmMltProbeResult probe{};
    if (mvm_mlt_probe_file(utf8.c_str(), &probe) != 0 || !probe.ok) {
        result.error = QStringLiteral("素材を解析できません: ") +
                       QString::fromUtf8(probe.error[0] ? probe.error : utf8.c_str());
        return result;
    }
    if (!probe.has_audio) {
        result.error = QStringLiteral("音声トラックがありません");
        return result;
    }
    if (probe.is_unbounded_length || !(probe.duration_sec > 0.0)) {
        result.error = QStringLiteral("有限の尺を持つ音声素材ではありません");
        return result;
    }
    const auto frames = audioSourceFrameCount(probe.duration_sec, timelineFpsNum, timelineFpsDen);
    if (!frames.success) {
        result.error = QString::fromStdString(frames.error);
        return result;
    }
    result.fpsNum = timelineFpsNum;
    result.fpsDen = timelineFpsDen;
    result.frameCount = frames.frameCount;
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

// timeline 上で最も上の video track に載っている clip。inspector の対象を決める。
const project::TimelineClip* topVideoClipAt(const project::Project& project,
                                            std::int64_t timelineFrame) {
    const auto active = project::activeClipsAt(project, project::TrackKind::Video, timelineFrame);
    for (auto entry = active.rbegin(); entry != active.rend(); ++entry)
        if (*entry)
            return *entry;
    return nullptr;
}

double linearToDb(float linear, double silenceDb) {
    if (!(linear > 0.0F))
        return silenceDb;
    const double db = 20.0 * std::log10(static_cast<double>(linear));
    return db < silenceDb ? silenceDb : db;
}

} // namespace

MvmController::MvmController(std::filesystem::path projectPath,
                             std::filesystem::path manimExecutablePath, project::Project project,
                             QObject* parent)
    : QObject(parent), projectPath_(std::move(projectPath)),
      manimExecutablePath_(std::move(manimExecutablePath)), project_(std::move(project)),
      previewEngine_(std::make_shared<preview::PreviewEngine>()),
      dispatcher_(std::make_shared<QtEventDispatcher>(this)),
      timelineModel_(std::make_unique<TimelineClipModel>()),
      videoTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Video)),
      audioTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Audio)) {
    refreshTimelineModel();
    initializePreviewEngine(QStringLiteral("Preview初期化に失敗しました: "));
    restoreFirstManimClip();

    playbackTimer_.setInterval(16);
    playbackTimer_.setTimerType(Qt::PreciseTimer);
    connect(&playbackTimer_, &QTimer::timeout, this, &MvmController::advanceTimelinePlayback);

    stateTimer_.setInterval(100);
    connect(&stateTimer_, &QTimer::timeout, this, &MvmController::pollPreviewState);
    stateTimer_.start();

    // scrub は drag の 1 移動ごとに seek せず、最新位置だけを一定間隔で処理する。
    // seek は engine の Seeking state を挟むため、coalesce しないと詰まる。
    scrubTimer_.setInterval(40);
    connect(&scrubTimer_, &QTimer::timeout, this, [this] {
        if (!scrubPending_) {
            // drag 終了後は、最後の位置を反映し終えてから timer を止める。
            if (!scrubbing_)
                scrubTimer_.stop();
            return;
        }
        // seek が Seeking 中で弾かれた場合は pending のままにする。
        // ここで落とすと、drag が止まった位置の preview が更新されないまま残る。
        if (seekTimelineFrame(scrubTargetFrame_))
            scrubPending_ = false;
    });

    meterTimer_.setInterval(50);
    connect(&meterTimer_, &QTimer::timeout, this, &MvmController::pollAudioMeter);
    meterTimer_.start();
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
    if (previewEffectsOverride_ && previewEffectsClipIndex_ == currentClipIndex_)
        return *previewEffectsOverride_;
    return project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects;
}

project::ClipEffects MvmController::effectsForPreview(int clipIndex) const {
    if (previewEffectsOverride_ && previewEffectsClipIndex_ == clipIndex)
        return *previewEffectsOverride_;
    return project_.timelineClips[static_cast<std::size_t>(clipIndex)].effects;
}

bool MvmController::applyEffectKey(project::ClipEffects& effects, const QString& key,
                                   double value) {
    if (key == QStringLiteral("positionX"))
        effects.positionXPercent = value;
    else if (key == QStringLiteral("positionY"))
        effects.positionYPercent = value;
    else if (key == QStringLiteral("scale"))
        effects.scalePercent = value;
    else if (key == QStringLiteral("rotation"))
        effects.rotationDegrees = value;
    else if (key == QStringLiteral("opacity"))
        effects.opacityPercent = value;
    else if (key == QStringLiteral("cropLeft"))
        effects.cropLeftPercent = value;
    else if (key == QStringLiteral("cropTop"))
        effects.cropTopPercent = value;
    else if (key == QStringLiteral("cropRight"))
        effects.cropRightPercent = value;
    else if (key == QStringLiteral("cropBottom"))
        effects.cropBottomPercent = value;
    else if (key == QStringLiteral("fadeIn"))
        effects.fadeInFrames = static_cast<std::int64_t>(std::llround(value));
    else if (key == QStringLiteral("fadeOut"))
        effects.fadeOutFrames = static_cast<std::int64_t>(std::llround(value));
    else
        return false;
    return true;
}

bool MvmController::frameRateMeasured() const {
    return project::isMeasuredTimelineFrameRate(project_.timelineFpsNum, project_.timelineFpsDen);
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
    config.output.frameRate = {static_cast<std::uint32_t>(project_.timelineFpsNum),
                               static_cast<std::uint32_t>(project_.timelineFpsDen)};
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
    trackSources_.clear();
    audioSource_.reset();
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

QAbstractItemModel* MvmController::videoTrackModel() const {
    return videoTrackModel_.get();
}

QAbstractItemModel* MvmController::audioTrackModel() const {
    return audioTrackModel_.get();
}

QString MvmController::timelineFpsText() const {
    if (project_.timelineFpsDen == 1)
        return QString::number(project_.timelineFpsNum) + QStringLiteral(" fps");
    const double value = static_cast<double>(project_.timelineFpsNum) /
                         static_cast<double>(project_.timelineFpsDen);
    return QString::number(value, 'f', 2) + QStringLiteral(" fps");
}

QVariantList MvmController::supportedFrameRates() const {
    QVariantList rates;
    for (const auto& rate : project::configurableTimelineFrameRates()) {
        QVariantMap entry;
        entry[QStringLiteral("num")] = static_cast<int>(rate.numerator);
        entry[QStringLiteral("den")] = static_cast<int>(rate.denominator);
        const double value =
            static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator);
        entry[QStringLiteral("label")] = rate.denominator == 1
                                             ? QString::number(rate.numerator)
                                             : QString::number(value, 'f', 2);
        rates.append(entry);
    }
    return rates;
}

QString MvmController::currentTimeText() const {
    const qint64 frames = std::max<qint64>(0, playheadFrame_);
    // non-drop-frame timecode。29.97 等は 30 として数え、実時間とはずれる。
    const qint64 nominalFps = std::max<qint64>(
        1, static_cast<qint64>(std::llround(static_cast<double>(project_.timelineFpsNum) /
                                            static_cast<double>(project_.timelineFpsDen))));
    const qint64 totalSeconds = frames / nominalFps;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(frames % nominalFps, 2, 10, QLatin1Char('0'));
}

bool MvmController::canPlay() const {
    return timelineCanPlay(project_, busy_, playing_, playheadFrame_, totalTimelineFrames_);
}

void MvmController::refreshTimelineModel() {
    if (timelineModel_)
        timelineModel_->setProject(project_);
    if (videoTrackModel_)
        videoTrackModel_->setProject(project_);
    if (audioTrackModel_)
        audioTrackModel_->setProject(project_);
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
        // Manim clip は overlay として扱うため、上側の video track を使う。
        const int overlayTrack =
            candidate.videoTracks.size() > 1 ? 1 : 0;
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
        const auto valid = project::validateTimeline(candidate);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
    }
    return saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: "));
}

void MvmController::pollAudioMeter() {
    if (!previewEngine_)
        return;
    const auto telemetry = previewEngine_->telemetry();
    const double left = linearToDb(telemetry.audioMeterPeakLeft, kMeterSilenceDb);
    const double right = linearToDb(telemetry.audioMeterPeakRight, kMeterSilenceDb);
    if (std::abs(left - audioMeterDbLeft_) < 0.05 && std::abs(right - audioMeterDbRight_) < 0.05)
        return;
    audioMeterDbLeft_ = left;
    audioMeterDbRight_ = right;
    Q_EMIT meterChanged();
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
        // preview が使えるようになったら playhead 位置の frame を出す。
        // Project を開いた直後や engine を作り直した直後に黒画面のままにしない。
        // 判定は「source が 1 つも載っていないか」で行う。選択 clip の有無で見ると、
        // engine reset 直後に seek が弾かれて選択だけ残った状態を拾えない。
        const bool showInitialFrame = ready && !busy_ && !pendingVideoPath_ &&
                                      trackSources_.empty() && !audioSource_ &&
                                      !project_.timelineClips.empty();
        Q_EMIT stateChanged();
        if (showInitialFrame) {
            seekTimelineFrame(playheadFrame_);
            return;
        }
        if (ready && !busy_ && currentClipPath_.isEmpty() && !hasManimAsset())
            statusText_ = QStringLiteral("素材を追加してください");
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
        setStatus(QStringLiteral("生成videoをcompositionへ追加できません: 対象clipがありません"));
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
    for (const auto& [trackIndex, slot] : trackSources_) {
        if (slot.clipIndex == currentClipIndex_)
            currentSource_ = slot.source;
    }
    statusText_ = clipName + QStringLiteral(" を表示しています");
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::syncAudioSourceFor(std::int64_t timelineFrame, QString& error) {
    const auto mapped = mapTimelinePreviewAudio(project_, timelineFrame);
    if (!mapped.success) {
        error = QString::fromStdString(mapped.error);
        return false;
    }

    std::optional<AudioSourceIdentity> desired;
    if (mapped.hasAudio) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(mapped.clipIndex)];
        desired = AudioSourceIdentity{clip.id,
                                      clip.sourceInFrame,
                                      clip.timelineStartFrame,
                                      clip.sourceFpsNum,
                                      clip.sourceFpsDen,
                                      project_.timelineFpsNum,
                                      project_.timelineFpsDen};
    }
    // engine は addSource 時点の offset を保持する。identity のどれか 1 つでも
    // 変わったら、既存 source を再利用してはいけない。
    if (audioSource_ && desired && audioSource_->identity == *desired) {
        audioSource_->clipIndex = mapped.clipIndex;
        return true;
    }

    // engine は active audio source を 1 件しか受理しない。切り替えは
    // 「外してから足す」しかないので、遅延 retire は使わない。
    if (audioSource_) {
        const auto removed = previewEngine_->removeSource(audioSource_->source);
        if (!removed) {
            error = previewErrorText(removed.error());
            return false;
        }
        audioSource_.reset();
    }
    if (!desired)
        return true;

    const auto& clip = project_.timelineClips[static_cast<std::size_t>(mapped.clipIndex)];
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

    preview::PreviewSourceDescriptor descriptor;
    descriptor.mediaPath = clip.mediaPath;
    descriptor.audioEnabled = true;
    descriptor.audioSampleOffset = offset.sampleOffset;
    const auto added = previewEngine_->addSource(descriptor);
    if (!added) {
        error = previewErrorText(added.error());
        return false;
    }
    audioSource_ = AudioPreviewSource{added.value(), *desired, mapped.clipIndex};
    return true;
}

bool MvmController::syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error) {
    const auto mappedFrame = mapTimelinePreviewFrame(project_, timelineFrame);
    if (!mappedFrame.success) {
        error = QString::fromStdString(mappedFrame.error);
        return false;
    }
    // audio は composition の layer ではないので、video より先に確定させる。
    // seekFrameRequest は composition の source 集合と完全一致を要求するため、
    // audio source をここへ混ぜない。
    if (!syncAudioSourceFor(timelineFrame, error))
        return false;

    auto candidateSources = trackSources_;
    std::vector<preview::PreviewSourceId> newlyAdded;
    const auto rollback = [&] {
        for (const auto source : newlyAdded) {
            // accepted composition が参照している最中は removeSource が拒否されうる。
            // 落とさず retirement queue へ回し、pollPreviewState に再試行させる。
            if (!previewEngine_->removeSource(source))
                retiredSources_.push_back(source);
        }
    };

    std::vector<int> desiredTracks;
    for (const auto& layer : mappedFrame.layers) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layer.clipIndex)];
        desiredTracks.push_back(layer.videoTrackIndex);
        const auto existing = candidateSources.find(layer.videoTrackIndex);
        if (existing != candidateSources.end() && existing->second.clipId == clip.id) {
            existing->second.clipIndex = layer.clipIndex;
            continue;
        }
        if (!std::filesystem::is_regular_file(clip.mediaPath)) {
            error = QString::fromStdString(clip.name) +
                    QStringLiteral(" のファイルがありません: ") + fromPath(clip.mediaPath);
            rollback();
            return false;
        }
        if (!project::sourceRateMatchesTimelineRate(project_, clip)) {
            error = QString::fromStdString(clip.name) + QStringLiteral(" は ") +
                    timelineFpsText() + QStringLiteral(" ではないためPreview未対応です");
            rollback();
            return false;
        }
        preview::PreviewSourceDescriptor descriptor;
        descriptor.mediaPath = clip.mediaPath;
        descriptor.videoEnabled = true;
        const auto added = previewEngine_->addSource(descriptor);
        if (!added) {
            error = previewErrorText(added.error());
            rollback();
            return false;
        }
        newlyAdded.push_back(added.value());
        candidateSources[layer.videoTrackIndex] =
            TrackPreviewSource{added.value(), clip.id, layer.clipIndex};
    }
    for (auto entry = candidateSources.begin(); entry != candidateSources.end();) {
        if (std::find(desiredTracks.begin(), desiredTracks.end(), entry->first) ==
            desiredTracks.end())
            entry = candidateSources.erase(entry);
        else
            ++entry;
    }

    auto composition = std::make_shared<preview::CompositionSnapshot>();
    preview::PreviewFrameRequest request;
    request.outputFrameNumber = timelineFrame;
    // mappedFrame.layers は video track index の昇順であり、この挿入順が
    // bottom/top の authority になる。
    for (const auto& layerMapping : mappedFrame.layers) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layerMapping.clipIndex)];
        const auto slot = candidateSources.find(layerMapping.videoTrackIndex);
        if (slot == candidateSources.end())
            continue;
        // drag 中の override はここでだけ効かせる。Project は書き換えない。
        const project::ClipEffects effects = effectsForPreview(layerMapping.clipIndex);
        preview::PreviewCompositionLayer layer;
        layer.source = slot->second.source;
        if (!project::clipEffectsAreDefault(effects)) {
            const auto mapped = project::mapClipEffects(effects);
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
        request.sources.push_back({slot->second.source, layerMapping.sourceFrameNumber});
    }
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted) {
        error = previewErrorText(submitted.error());
        rollback();
        return false;
    }
    const auto sought = previewEngine_->seekFrameRequest(request);
    if (!sought) {
        error = previewErrorText(sought.error());
        // scrub は Seeking 中の reject を 40ms ごとに retry する。ここで rollback
        // しないと、retry のたびに source が積み上がって登録上限に達する。
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
    if (project_.timelineFpsDen != 1) {
        setStatus(QStringLiteral("Manimは整数fpsでしか生成できません。現在のProject frame rate (") +
                  timelineFpsText() +
                  QStringLiteral(") ではProjectと一致するclipを作れないため生成しません"));
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
    // Manim へ渡せる fps は整数だけである。1001 分母の Project で整数へ丸めると、
    // 生成物は 30/1 になり Project の 30000/1001 と一致せず「Preview未対応」の
    // clip ができる。丸めて対応したことにせず、ここで fail-closed にする。
    request.fps = static_cast<int>(project_.timelineFpsNum / project_.timelineFpsDen);
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
    const int overlayTrack = candidate.videoTracks.size() > 1 ? 1 : 0;
    const project::TimelineEditResult placed = project::appendManimTimelineClipAt(
        candidate, candidate.manimAssets.front(), newClipId(), media.fpsNum, media.fpsDen,
        media.frameCount, playheadFrame_,
        project::TrackRef{project::TrackKind::Video, overlayTrack});
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
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Video;
    clip.mediaPath = mediaPath;
    clip.name = info.fileName().toStdString();
    clip.id = newClipId();
    clip.sourceFpsNum = media.fpsNum;
    clip.sourceFpsDen = media.fpsDen;
    clip.sourceFrameCount = media.frameCount;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = media.frameCount;
    const auto placed = project::appendTimelineClip(
        candidate, std::move(clip), project::TrackRef{project::TrackKind::Video, 0});
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

bool MvmController::addAudioClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (project_.audioTracks.empty()) {
        setStatus(QStringLiteral("audio trackがありません。先にaudio trackを追加してください"));
        return false;
    }
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return false;
    }
    const QString localFile = fileUrl.toLocalFile();
    const QFileInfo info(localFile);
    if (!info.exists() || !info.isFile()) {
        setStatus(QStringLiteral("存在する音声ファイルを選択してください"));
        return false;
    }

    const std::filesystem::path mediaPath(localFile.toStdWString());
    const ProbedMedia media =
        probeAudioMedia(mediaPath, project_.timelineFpsNum, project_.timelineFpsDen);
    if (!media.success) {
        setStatus(media.error);
        return false;
    }
    project::Project candidate = project_;
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Audio;
    clip.mediaPath = mediaPath;
    clip.name = info.fileName().toStdString();
    clip.id = newClipId();
    clip.sourceFpsNum = media.fpsNum;
    clip.sourceFpsDen = media.fpsDen;
    clip.sourceFrameCount = media.frameCount;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = media.frameCount;
    const auto placed = project::appendTimelineClip(
        candidate, std::move(clip), project::TrackRef{project::TrackKind::Audio, 0});
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

void MvmController::beginScrub() {
    if (project_.timelineClips.empty())
        return;
    pauseTimeline();
    scrubbing_ = true;
    scrubPending_ = false;
    scrubTimer_.start();
}

void MvmController::scrubToFrame(qint64 frame) {
    if (!scrubbing_)
        return;
    const qint64 clamped =
        totalTimelineFrames_ > 0 ? std::clamp<qint64>(frame, 0, totalTimelineFrames_ - 1) : 0;
    if (playheadFrame_ != clamped) {
        // 表示用の playhead は即座に動かす。preview の追従は coalesce する。
        playheadFrame_ = clamped;
        Q_EMIT stateChanged();
    }
    scrubTargetFrame_ = clamped;
    scrubPending_ = true;
}

void MvmController::endScrub() {
    if (!scrubbing_)
        return;
    scrubbing_ = false;
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
    if (project_.timelineClips.empty()) {
        setStatus(QStringLiteral("seekするclipがありません"));
        return false;
    }
    const qint64 clamped = std::clamp<qint64>(frame, 0, totalTimelineFrames_ - 1);
    playheadFrame_ = clamped;
    const auto active = project::activeClipsAt(project_, project::TrackKind::Video, clamped);
    int index = -1;
    // 選択clipがactiveなら維持し、それ以外は最上位trackをinspector対象にする。
    for (const auto* clip : active) {
        if (clip && static_cast<int>(clip - project_.timelineClips.data()) == currentClipIndex_)
            index = currentClipIndex_;
    }
    if (index < 0) {
        const auto* selected = topVideoClipAt(project_, clamped);
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
    const auto activeVideo = project::activeClipsAt(project_, project::TrackKind::Video, frame);
    bool sameSourceSet = true;
    for (std::size_t track = 0; track < activeVideo.size(); ++track) {
        const bool muted = project_.videoTracks[track].muted;
        const std::string desired =
            (activeVideo[track] && !muted) ? activeVideo[track]->id : std::string{};
        const auto installed = trackSources_.find(static_cast<int>(track));
        const std::string current =
            installed == trackSources_.end() ? std::string{} : installed->second.clipId;
        sameSourceSet = sameSourceSet && desired == current;
    }
    const auto audioMapping = mapTimelinePreviewAudio(project_, frame);
    const std::string desiredAudio =
        (audioMapping.success && audioMapping.hasAudio) ? audioMapping.clipId : std::string{};
    const std::string currentAudio = audioSource_ ? audioSource_->identity.clipId : std::string{};
    sameSourceSet = sameSourceSet && desiredAudio == currentAudio;
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
    const auto* selected = topVideoClipAt(project_, frame);
    const int nextClip =
        selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
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

bool MvmController::moveTimelineClip(const QString& clipId, const QString& trackKind,
                                     int trackIndex, qint64 timelineStartFrame) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    project::TrackRef destination;
    if (!resolveTrackRef(trackKind, trackIndex, destination)) {
        setStatus(QStringLiteral("移動先trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto moved = project::moveClip(candidate, clipId.toStdString(), destination,
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

bool MvmController::addTrack(const QString& trackKind) {
    if (busy_)
        return false;
    project::TrackKind kind;
    if (trackKind == QStringLiteral("video"))
        kind = project::TrackKind::Video;
    else if (trackKind == QStringLiteral("audio"))
        kind = project::TrackKind::Audio;
    else {
        setStatus(QStringLiteral("追加するtrack種別が不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto added = project::addTrack(candidate, kind);
    if (!added.success) {
        setStatus(QString::fromStdString(added.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    setStatus(QStringLiteral("trackを追加しました"));
    return true;
}

bool MvmController::removeTrack(const QString& trackKind, int trackIndex) {
    if (busy_)
        return false;
    // track を消すと後続 track の index が繰り上がる。preview cache は track index を
    // key にしているので、止めてから組み直さないと stale な対応が残る。
    if (!pauseTimeline())
        return false;
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("削除するtrackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto removed = project::removeTrack(candidate, track);
    if (!removed.success) {
        setStatus(QString::fromStdString(removed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    // index の対応が変わったので cache を捨ててから現在位置で組み直す。
    trackSources_.clear();
    audioSource_.reset();
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("trackは削除しましたが、Previewを初期化できません: ") + failure);
        return true;
    }
    const std::string selectedId =
        (currentClipIndex_ >= 0 &&
         currentClipIndex_ < static_cast<int>(project_.timelineClips.size()))
            ? project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id
            : std::string{};
    currentClipIndex_ = -1;
    return refreshPreviewAfterSavedEdit(selectedId, QStringLiteral("trackを削除しました"));
}

bool MvmController::setTrackMuted(const QString& trackKind, int trackIndex, bool muted) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto changed = project::setTrackMuted(candidate, track, muted);
    if (!changed.success) {
        setStatus(QString::fromStdString(changed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    // mute は preview の layer 構成そのものを変える。現在位置で組み直す。
    QString error;
    if (!syncPreviewSourcesAt(playheadFrame_, error)) {
        setStatus(QStringLiteral("muteは保存されましたが、Previewの更新に失敗しました: ") + error);
        return true;
    }
    setStatus(muted ? QStringLiteral("trackをミュートしました")
                    : QStringLiteral("trackのミュートを解除しました"));
    return true;
}

bool MvmController::hasGapAt(const QString& trackKind, int trackIndex, qint64 frame) const {
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track))
        return false;
    return project::gapAt(project_, track, frame).found;
}

bool MvmController::rippleDeleteGap(const QString& trackKind, int trackIndex, qint64 frame) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track)) {
        setStatus(QStringLiteral("trackが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto rippled = project::rippleDeleteGap(candidate, track, frame);
    if (!rippled.success) {
        setStatus(QString::fromStdString(rippled.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    const std::string selectedId =
        (currentClipIndex_ >= 0 &&
         currentClipIndex_ < static_cast<int>(project_.timelineClips.size()))
            ? project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].id
            : std::string{};
    return refreshPreviewAfterSavedEdit(selectedId, QStringLiteral("空白をリップル削除しました"));
}

bool MvmController::adoptProject(project::Project loaded, std::filesystem::path path,
                                 QString successStatus) {
    if (!pauseTimeline())
        return false;
    project_ = std::move(loaded);
    projectPath_ = std::move(path);
    currentClipIndex_ = -1;
    currentClipName_.clear();
    currentClipPath_.clear();
    playheadFrame_ = 0;
    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    refreshTimelineModel();
    // fps が変わりうるので engine を作り直す。output rate は initialize でしか決まらない。
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("Projectは切り替えましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    restoreFirstManimClip();
    Q_EMIT stateChanged();
    setStatus(std::move(successStatus));
    return true;
}

bool MvmController::newProject(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの保存先を指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    project::Project fresh = project::createDefaultProject();
    fresh.timelineFpsNum = project_.timelineFpsNum;
    fresh.timelineFpsDen = project_.timelineFpsDen;
    const auto saved = project::saveProjectJson(fresh, path);
    if (!saved.success) {
        setStatus(QStringLiteral("新規Projectを保存できません: ") +
                  QString::fromStdString(saved.error));
        return false;
    }
    return adoptProject(std::move(fresh), path, QStringLiteral("新規Projectを作成しました"));
}

bool MvmController::openProject(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルのProjectファイルを指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    const auto loaded = project::loadProjectJson(path);
    if (!loaded.success) {
        setStatus(QStringLiteral("Projectを開けません: ") + QString::fromStdString(loaded.error));
        return false;
    }
    return adoptProject(loaded.project, path, QStringLiteral("Projectを開きました"));
}

bool MvmController::saveProjectAs(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルの保存先を指定してください"));
        return false;
    }
    const std::filesystem::path path(fileUrl.toLocalFile().toStdWString());
    const auto saved = project::saveProjectJson(project_, path);
    if (!saved.success) {
        setStatus(QStringLiteral("Projectを保存できません: ") +
                  QString::fromStdString(saved.error));
        return false;
    }
    projectPath_ = path;
    setStatus(QStringLiteral("Projectを保存しました: ") + fromPath(path));
    return true;
}

bool MvmController::setTimelineFrameRate(int fpsNum, int fpsDen) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    if (project_.timelineFpsNum == fpsNum && project_.timelineFpsDen == fpsDen)
        return true;
    project::Project candidate = project_;
    const auto changed = project::setTimelineFrameRate(candidate, fpsNum, fpsDen);
    if (!changed.success) {
        setStatus(QString::fromStdString(changed.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("Projectを保存できません: ")))
        return false;
    // engine の output rate は initialize でしか決まらないので作り直す。
    if (!resetPreviewEngine()) {
        const QString failure = statusText_;
        setStatus(QStringLiteral("frame rateは変更しましたが、Previewを初期化できません: ") +
                  failure);
        return true;
    }
    Q_EMIT stateChanged();
    QString status = QStringLiteral("timeline frame rateを ") + timelineFpsText() +
                     QStringLiteral(" にしました");
    if (!frameRateMeasured())
        status += QStringLiteral("（このrateのpreviewは未計測です）");
    setStatus(std::move(status));
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
    request.fpsNum = static_cast<int>(project_.timelineFpsNum);
    request.fpsDen = static_cast<int>(project_.timelineFpsDen);

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

bool MvmController::setEffectValue(const QString& key, double value, bool commit) {
    if (busy_ || currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size())) {
        setStatus(QStringLiteral("effectを適用するclipがありません"));
        return false;
    }
    if (!pauseTimeline())
        return false;

    const int clipIndex = currentClipIndex_;
    project::ClipEffects candidateEffects = effectsForPreview(clipIndex);
    if (!applyEffectKey(candidateEffects, key, value)) {
        setStatus(QStringLiteral("未知のeffect項目です: ") + key);
        return false;
    }

    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    std::string effectsError;
    if (!project::validateClipEffects(candidateEffects, clip.sourceOutFrame - clip.sourceInFrame,
                                      effectsError)) {
        setStatus(QString::fromStdString(effectsError));
        return false;
    }

    if (!commit) {
        // drag 中は Project を書き換えない。preview だけ override で追従させる。
        previewEffectsOverride_ = candidateEffects;
        previewEffectsClipIndex_ = clipIndex;
        Q_EMIT stateChanged();
        QString previewError;
        if (!refreshCurrentClipEffectsPreview(previewError))
            setStatus(QStringLiteral("effectのPreview更新に失敗しました: ") + previewError);
        return true;
    }

    project::Project candidate = project_;
    candidate.timelineClips[static_cast<std::size_t>(clipIndex)].effects = candidateEffects;
    const auto valid = project::validateTimeline(candidate);
    if (!valid.success) {
        setStatus(QString::fromStdString(valid.error));
        return false;
    }
    if (!saveProject(std::move(candidate), QStringLiteral("effectを保存できません: ")))
        return false;
    previewEffectsOverride_.reset();
    previewEffectsClipIndex_ = -1;
    Q_EMIT stateChanged();

    QString previewError;
    if (!refreshCurrentClipEffectsPreview(previewError)) {
        setStatus(QStringLiteral("effectのPreview更新に失敗しました: ") + previewError);
        return true;
    }
    setStatus(QStringLiteral("effectを保存してPreviewへ反映しました"));
    return true;
}

bool MvmController::cancelEffectPreview() {
    if (!previewEffectsOverride_)
        return true;
    previewEffectsOverride_.reset();
    previewEffectsClipIndex_ = -1;
    Q_EMIT stateChanged();
    QString previewError;
    if (!refreshCurrentClipEffectsPreview(previewError)) {
        setStatus(QStringLiteral("effectのPreview更新に失敗しました: ") + previewError);
        return true;
    }
    setStatus(QStringLiteral("effectの編集を取り消しました"));
    return true;
}

void MvmController::shutdown() {
    if (shutdownStarted_)
        return;
    shutdownStarted_ = true;
    playbackTimer_.stop();
    scrubTimer_.stop();
    meterTimer_.stop();
    playing_ = false;
    stateTimer_.stop();
    if (previewEngine_)
        previewEngine_->requestShutdown();
    if (previewSurface_)
        previewSurface_->setEngine({});
}

} // namespace mvm::app
