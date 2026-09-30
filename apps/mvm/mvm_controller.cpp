#include "mvm_controller.h"

#include "app/audio_source_set_transaction.h"
#include "app/manim_clip_workflow.h"
#include "app/preview/preview_engine_rhi_item.h"
#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "app/timeline_playback.h"
#include "app/timeline_preview_mapping.h"
#include "core/checked_output_timebase.h"
#include "core/export_eta.h"
#include "core/timecode.h"
#include "image_raster_cache.h"
#include "media_file_filters.h"
#include "media_import.h"
#include "project/clip_effects.h"
#include "project/path_identity.h"
#include "project/project_json.h"
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
#include <tuple>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariantMap>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mvm::app {
namespace {

QString fromPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

bool atomicSaveProject(const project::Project& project, const std::filesystem::path& path,
                       QString& error) {
    const auto saved = project::saveProjectJson(project, path);
    if (!saved.success) {
        error = QString::fromStdString(saved.error);
        return false;
    }
    return true;
}

bool revealFileInExplorer(const std::filesystem::path& path, QString& error) {
    char detail[512] = {};
    if (mvm_reveal_in_explorer(path.c_str(), detail, sizeof(detail)) == 0)
        return true;
    error = QString::fromUtf8(detail);
    return false;
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
    int width = 0;
    int height = 0;
    std::int64_t fpsNum = 0;
    std::int64_t fpsDen = 1;
    int sarNum = 1;
    int sarDen = 1;
    std::int64_t frameCount = 0;
    bool hasAudio = false;
    QString error;
};

// 素材の種別は probeMediaFile (media_import) だけが決める。ここでは用途に合うかを見る。
ProbedMedia videoFacts(const MediaImportResult& probed) {
    ProbedMedia result;
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    if (probed.item.kind != project::MediaKind::Video) {
        result.error = probed.item.kind == project::MediaKind::Image
                           ? QStringLiteral("静止画は動画として追加できません")
                           : QStringLiteral("映像の無い素材は動画として追加できません");
        return result;
    }
    result.width = probed.item.width;
    result.height = probed.item.height;
    result.fpsNum = probed.item.fpsNum;
    result.fpsDen = probed.item.fpsDen;
    result.sarNum = probed.sarNum;
    result.sarDen = probed.sarDen;
    result.frameCount = probed.item.frameCount;
    result.hasAudio = probed.hasAudio;
    result.success = true;
    return result;
}

ProbedMedia probeMedia(const std::filesystem::path& path) {
    return videoFacts(probeMediaFile(path));
}

// audio 素材には映像 fps が無い。Project timeline の fps を素材の frame domain と
// して採用し、尺だけを duration から求める。frame 算術を 1 種類に保つための選択で
// あり、素材側に fps があると主張しているわけではない。
ProbedMedia audioFacts(const MediaImportResult& probed, std::int64_t timelineFpsNum,
                       std::int64_t timelineFpsDen) {
    ProbedMedia result;
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    const bool hasAudio = probed.item.kind == project::MediaKind::Audio ||
                          (probed.item.kind == project::MediaKind::Video && probed.hasAudio);
    if (!hasAudio) {
        result.error = QStringLiteral("音声トラックがありません");
        return result;
    }
    const auto frames = audioSourceFrameCount(probed.durationSec, timelineFpsNum, timelineFpsDen);
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


// QML から渡る「リンク相手にも適用するか」をモデルの LinkMode へ写す。
project::LinkMode linkModeFor(bool linked) {
    return linked ? project::LinkMode::Linked : project::LinkMode::Single;
}

std::string newClipId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

std::optional<project::ClipSpeedDurationEdit>
speedDurationEdit(const QString& input, double speedPercent, const QString& durationText,
                  bool preservePitch, bool ripple, std::int64_t fpsNum, std::int64_t fpsDen) {
    project::ClipSpeedDurationEdit edit;
    edit.preservePitch = preservePitch;
    edit.ripple = ripple;
    if (input == QStringLiteral("speed")) {
        if (!std::isfinite(speedPercent) || speedPercent < 10.0 || speedPercent > 1000.0)
            return std::nullopt;
        const auto hundredths = static_cast<std::int64_t>(std::llround(speedPercent * 100.0));
        edit.input = project::ClipSpeedDurationEdit::Input::Speed;
        edit.speedNum = hundredths;
        edit.speedDen = 10000;
    } else if (input == QStringLiteral("duration")) {
        const auto frames = core::parseTimecode(durationText.toStdString(), fpsNum, fpsDen);
        if (!frames || *frames < 1)
            return std::nullopt;
        edit.input = project::ClipSpeedDurationEdit::Input::Duration;
        edit.durationFrames = *frames;
    } else {
        return std::nullopt;
    }
    return edit;
}

// 素材 1 つを timeline へ置くときの clip。音声を持つ動画は linkedAudio にリンク相手を持つ。
// 置き場所 (track / 開始位置) は呼び出し側が決める。
struct MediaClips {
    bool success = false;
    QString error;
    project::TimelineClip primary;
    std::optional<project::TimelineClip> linkedAudio;
};

project::TimelineClip mediaClip(project::TimelineClipKind kind, const project::MediaItem& item,
                                std::int64_t fpsNum, std::int64_t fpsDen,
                                std::int64_t frameCount) {
    project::TimelineClip clip;
    clip.kind = kind;
    // clip は素材と同じファイルを指す (validateMediaReferences の不変条件)。
    clip.mediaPath = item.mediaPath;
    clip.mediaItemId = item.id;
    clip.name = QString::fromStdWString(item.mediaPath.filename().wstring()).toStdString();
    clip.id = newClipId();
    clip.sourceFpsNum = fpsNum;
    clip.sourceFpsDen = fpsDen;
    clip.sourceFrameCount = frameCount;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = frameCount;
    return clip;
}

// プロジェクトパネルの素材 itemId から、timeline へ置く clip を作る。kind は作る clip の種別
// (動画素材の音声だけを置くなら Audio)。使う時点でファイルを調べ直し、素材の値 (種別・fps・
// 尺・解像度) を実物に合わせて candidate 上で更新してから clip を作る。登録後に消えた・
// 別の種類へ差し替わったファイルは置かず、同じ種類の別の中身 (縦横が違う画像など) なら
// 素材の値が新しくなる。prebuilt は同じファイルを判定済みの結果 (あれば調べ直さない。
// 画像の decode は重い)。
MediaClips prepareMediaClips(project::Project& candidate, const std::string& itemId,
                             project::MediaKind kind, const MediaImportResult* prebuilt = nullptr) {
    MediaClips result;
    const auto* found = project::findMediaItem(candidate, itemId);
    if (!found) {
        result.error = QStringLiteral("素材がありません");
        return result;
    }
    const auto probed = prebuilt ? *prebuilt : probeMediaFile(found->mediaPath);
    if (!probed.success) {
        result.error = QString::fromStdString(probed.error);
        return result;
    }
    // 種別が clip の用途に合うかを先に見る (合わない差し替えで素材の値を書き換えない)。
    ProbedMedia facts;
    switch (kind) {
    case project::MediaKind::Video:
        facts = videoFacts(probed);
        break;
    case project::MediaKind::Audio:
        facts = audioFacts(probed, candidate.timelineFpsNum, candidate.timelineFpsDen);
        break;
    case project::MediaKind::Image:
        facts.success = probed.item.kind == project::MediaKind::Image;
        if (!facts.success)
            facts.error = QStringLiteral("画像ではありません");
        break;
    }
    if (!facts.success) {
        result.error = facts.error;
        return result;
    }
    const auto refreshed = project::refreshMediaItem(candidate, itemId, probed.item);
    if (!refreshed.success) {
        result.error = QStringLiteral("素材の情報を更新できません: ") +
                       QString::fromStdString(refreshed.error);
        return result;
    }
    const auto& item = *project::findMediaItem(candidate, itemId);
    switch (kind) {
    case project::MediaKind::Video:
        result.primary = mediaClip(project::TimelineClipKind::Video, item, facts.fpsNum,
                                   facts.fpsDen, facts.frameCount);
        if (facts.hasAudio) {
            result.linkedAudio = mediaClip(project::TimelineClipKind::Audio, item, facts.fpsNum,
                                           facts.fpsDen, facts.frameCount);
            result.primary.linkGroupId = result.linkedAudio->linkGroupId = newClipId();
        }
        break;
    case project::MediaKind::Audio:
        result.primary = mediaClip(project::TimelineClipKind::Audio, item, facts.fpsNum,
                                   facts.fpsDen, facts.frameCount);
        break;
    case project::MediaKind::Image:
        result.primary = mediaClip(
            project::TimelineClipKind::Image, item, candidate.timelineFpsNum,
            candidate.timelineFpsDen,
            project::defaultStillClipFrames(candidate.timelineFpsNum, candidate.timelineFpsDen));
        break;
    }
    result.success = true;
    return result;
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
                             QObject* parent, ExportRunner exportRunner,
                             ExportThreadFactory exportThreadFactory, FileRevealer fileRevealer)
    : QObject(parent), projectPath_(std::move(projectPath)),
      manimExecutablePath_(std::move(manimExecutablePath)), project_(std::move(project)),
      previewEngine_(std::make_shared<preview::PreviewEngine>()),
      dispatcher_(std::make_shared<QtEventDispatcher>(this)),
      timelineModel_(std::make_unique<TimelineClipModel>()),
      videoTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Video)),
      audioTrackModel_(std::make_unique<TrackModel>(project::TrackKind::Audio)),
      mediaBinModel_(std::make_unique<MediaBinModel>()),
      exportRunner_(exportRunner ? std::move(exportRunner) : mvm::app::exportTimeline),
      exportThreadFactory_(
          exportThreadFactory
              ? std::move(exportThreadFactory)
              : [](std::function<void()> task) { return std::thread(std::move(task)); }),
      fileRevealer_(fileRevealer ? std::move(fileRevealer) : revealFileInExplorer) {
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

QString MvmController::recoveryProjectPath() const {
    return fromPath(recoveryPath());
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
    else if (key == QStringLiteral("scaleX"))
        effects.scaleXPercent = value;
    else if (key == QStringLiteral("scaleY"))
        effects.scaleYPercent = value;
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
    // engine の envelope 一致判定だけを authority にする。
    // Project 側の rate 表へ fallback すると、engine の initialize が失敗していても
    // 「60fps だから測定済み」と答えてしまい、false positive になる。
    // 構成が未確定の engine では matchesMeasuredEnvelope() が false を返す。
    return previewEngine_ && previewEngine_->capabilities().matchesMeasuredEnvelope();
}

double MvmController::effectPositionX() const {
    return currentEffects().positionXPercent;
}

double MvmController::effectPositionY() const {
    return currentEffects().positionYPercent;
}

double MvmController::effectScaleX() const {
    return currentEffects().scaleXPercent;
}

double MvmController::effectScaleY() const {
    return currentEffects().scaleYPercent;
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

void MvmController::reportExportFailure(QString message) {
    setStatus(message);
    Q_EMIT exportFailed(message);
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

void MvmController::setMasterVolume(double volume) {
    const double clamped = std::clamp(volume, 0.0, 1.0);
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
    return QString::fromStdString(core::formatTimecode(
        playheadFrame_, project_.timelineFpsNum, project_.timelineFpsDen));
}

bool MvmController::canPlay() const {
    return timelineCanPlay(project_, busy_, playing_, playheadFrame_, totalTimelineFrames_);
}

void MvmController::refreshTimelineModel() {
    textPreviewOverride_.reset();
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
    if (audioTrackModel_)
        audioTrackModel_->setProject(project_);
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

void MvmController::pushUndoEntry(UndoEntry entry) {
    undoHistory_.push_back(std::move(entry));
    constexpr std::size_t kMaximumUndoEntries = 100;
    if (undoHistory_.size() > kMaximumUndoEntries)
        undoHistory_.erase(undoHistory_.begin());
    // 新しい編集をした時点で、やり直し先の未来は無くなる。
    redoHistory_.clear();
}

void MvmController::clearEditHistory() {
    undoHistory_.clear();
    redoHistory_.clear();
}

bool MvmController::commitProjectEdit(project::Project candidate, const QString& failurePrefix) {
    if (!projectLockHeld_) {
        setStatus(failurePrefix + QStringLiteral("Projectを排他できません"));
        return false;
    }
    UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_, currentRevision_};
    const auto serialized = project::serializeProjectJson(candidate, projectPath_);
    if (!serialized.success) {
        setStatus(failurePrefix + QString::fromStdString(serialized.error));
        return false;
    }
    project_ = std::move(candidate);
    pushUndoEntry(std::move(undo));
    currentRevision_ = nextRevision_++;
    refreshTimelineModel();
    scheduleRecoveryAutosave();
    return true;
}

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
        QString ignored;
        removeRecoveryFile(ignored);
        return;
    }
    recoveryDebounceTimer_.start();
    if (!recoveryMaximumTimer_.isActive())
        recoveryMaximumTimer_.start();
}

void MvmController::writeRecoveryAutosave() {
    recoveryDebounceTimer_.stop();
    recoveryMaximumTimer_.stop();
    if (!dirty()) {
        QString ignored;
        removeRecoveryFile(ignored);
        return;
    }
    if (!projectLockHeld_ || recoveryRevision_ == currentRevision_)
        return;
    // diskを読み直すと、開いたあとの外部変更を基準hashとして記録してしまう。
    if (!canonicalBaseKnown_) {
        setStatus(
            QStringLiteral("自動復旧データを保存できません: Project fileの基準hashが不明です"));
        recoveryMaximumTimer_.start();
        return;
    }
    const auto saved = project::saveProjectRecovery(
        project_, recoveryPath(), projectPath_, savedCanonicalSha256_,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toStdString(), sessionId_);
    if (!saved.success) {
        setStatus(QStringLiteral("自動復旧データを保存できません: ") +
                  QString::fromStdString(saved.error));
        recoveryMaximumTimer_.start();
        return;
    }
    recoveryRevision_ = currentRevision_;
}

void MvmController::detectRecovery() {
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
    if (busy_ || !projectLockHeld_ || !recoveryProject_ || !pauseTimeline())
        return false;
    project_ = *recoveryProject_;
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
        const auto valid = project::validateTimeline(candidate);
        if (!valid.success) {
            setStatus(QString::fromStdString(valid.error));
            return false;
        }
    }
    return commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: "));
}

void MvmController::pollAudioMeter() {
    if (!previewEngine_)
        return;
    const auto telemetry = previewEngine_->telemetry();
    // シャトルと scrub は preview engine と別の endpoint で鳴らすので、鳴っている側の peak を出す。
    const bool ownSink = shuttleAudio_ || scrubAudio_;
    const auto own = shuttleAudio_ ? shuttleAudio_->sinkSnapshot()
                     : scrubAudio_ ? scrubAudio_->sinkSnapshot()
                                   : audio::WasapiSnapshot{};
    const double left =
        linearToDb(ownSink ? own.meterPeakLeft : telemetry.audioMeterPeakLeft, kMeterSilenceDb);
    const double right =
        linearToDb(ownSink ? own.meterPeakRight : telemetry.audioMeterPeakRight, kMeterSilenceDb);
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
                                      trackSources_.empty() && audioSources_.empty() &&
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
    if (textPreviewRefreshPending_ && playbackState == preview::PreviewEngineState::ReadyPaused)
        refreshTextPreview();
    if (previewRefreshPending_ && previewEngine_->status().state ==
                                      preview::PreviewEngineState::ReadyPaused) {
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

bool MvmController::audioDescriptorFor(int clipIndex, preview::PreviewSourceDescriptor& descriptor,
                                       QString& error) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size())) {
        error = QStringLiteral("audio clipがありません");
        return false;
    }
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
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
    descriptor.audioSampleOffset = offset.sampleOffset;
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
    descriptor.audioGainAtMediaSample =
        [clip, sampleOffset = offset.sampleOffset, timelineFpsNum, timelineFpsDen,
         timebase = timebase.value()](std::int64_t mediaSample) -> float {
        const auto timelineSample = mediaSample - sampleOffset;
        if (timelineSample < 0)
            return 0.0F;
        const auto frame = timebase.schedulerOutputFrame(timelineSample);
        if (!frame)
            return 0.0F;
        const auto local = frame.value() - clip.timelineStartFrame;
        const auto source =
            project::clipFadeSourceFrameAt(clip, timelineFpsNum, timelineFpsDen, local);
        if (!source.success)
            return 0.0F;
        return static_cast<float>(project::evaluateClipVolume(
            clip.effects, local, source.frame, clip.sourceOutFrame - clip.sourceInFrame));
    };
    return true;
}

bool MvmController::audioIdentitiesFor(const TimelinePreviewAudioMapping& mapped,
                                       std::vector<AudioSourceIdentity>& identities,
                                       QString& error) const {
    identities.clear();
    identities.reserve(mapped.layers.size());
    for (const auto& layer : mapped.layers) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layer.clipIndex)];
        const auto offset = audioPreviewSampleOffset(project_, clip);
        if (!offset.success) {
            error = QString::fromStdString(offset.error);
            return false;
        }
        identities.push_back(
            {clip.mediaPath, offset.sampleOffset, clip.effects, clip.speedNum, clip.speedDen,
             clip.preservePitch});
    }
    return true;
}

bool MvmController::applyAudioSourceFor(std::int64_t timelineFrame, AudioSwitchUndo& undo,
                                        QString& error) {
    undo = AudioSwitchUndo{};
    const auto mapped = mapTimelinePreviewAudio(project_, timelineFrame);
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
        if (!audioDescriptorFor(mapped.layers[index].clipIndex, descriptor, error))
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

std::shared_ptr<preview::CompositionSnapshot>
MvmController::previewCompositionFor(const TimelinePreviewFrameMapping& mappedFrame,
                                     const std::map<int, TrackPreviewSource>& sources,
                                     preview::PreviewFrameRequest& request, QString& error) const {
    auto composition = std::make_shared<preview::CompositionSnapshot>();
    request = preview::PreviewFrameRequest{};
    request.outputFrameNumber = mappedFrame.outputFrameNumber;
    error.clear();
    // previewLayerStack が video と文字を track 順 (背面 -> 前面) に並べる。
    // この挿入順が engine の z 順の authority になる。
    for (const auto& entry : previewLayerStack(mappedFrame)) {
        if (entry.still) {
            const auto& stillMapping = mappedFrame.stillLayers[entry.index];
            const double opacity = std::clamp(stillMapping.opacity, 0.0, 1.0);
            preview::PreviewCompositionLayer layer;
            if (stillMapping.kind == project::TimelineClipKind::Image) {
                bool pending = false;
                layer.stillImage = imageStillImage(stillMapping.clipIndex, error, pending);
                // raster を worker で生成中。できるまではこの画像を合成に入れず、
                // できたら entryChanged で組み直す。
                if (pending)
                    continue;
                if (!layer.stillImage)
                    return nullptr;
                const auto& clip =
                    project_.timelineClips[static_cast<std::size_t>(stillMapping.clipIndex)];
                const project::ClipEffects effects = effectsForPreview(stillMapping.clipIndex);
                // 画像は全画面の raster なので、位置・拡大・回転・crop は video
                // と同じ座標系で効く。
                if (!project::clipEffectsAreDefault(effects))
                    applyPreviewLayerEffects(layer, effects, opacity, 0,
                                             clip.sourceOutFrame - clip.sourceInFrame);
                else
                    layer.opacity = static_cast<float>(opacity);
                composition->layers.push_back(std::move(layer));
                continue;
            }
            // UI が重ねている (ドラッグ・編集中の) 文字は二重に描かない。
            if (QString::fromStdString(stillMapping.clipId) == textOverlayClipId_)
                continue;
            layer.stillImage = textStillImage(stillMapping.clipIndex, error);
            if (!layer.stillImage)
                return nullptr;
            // 書き出しと同じく opacity の値・key・fade を効かせる。位置や拡大などの effect は
            // 文字では検証が拒否するので、静止画 layer には最終の不透明度だけを渡す。
            layer.opacity = static_cast<float>(opacity);
            composition->layers.push_back(std::move(layer));
            continue;
        }
        const auto& layerMapping = mappedFrame.layers[entry.index];
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layerMapping.clipIndex)];
        const auto slot = sources.find(layerMapping.videoTrackIndex);
        if (slot == sources.end())
            continue;
        // drag 中の override はここでだけ効かせる。Project は書き換えない。
        const project::ClipEffects effects = effectsForPreview(layerMapping.clipIndex);
        preview::PreviewCompositionLayer layer;
        layer.source = slot->second.source;
        if (!project::clipEffectsAreDefault(effects)) {
            const auto fadeFrame = project::clipFadeSourceFrameAt(
                clip, project_.timelineFpsNum, project_.timelineFpsDen,
                mappedFrame.outputFrameNumber - clip.timelineStartFrame);
            if (!fadeFrame.success) {
                error = QString::fromStdString(fadeFrame.error);
                return nullptr;
            }
            applyPreviewLayerEffects(layer, effects,
                                     project::evaluateClipOpacity(
                                         effects,
                                         mappedFrame.outputFrameNumber - clip.timelineStartFrame,
                                         fadeFrame.frame,
                                         clip.sourceOutFrame - clip.sourceInFrame),
                                     clip.sourceInFrame, clip.sourceOutFrame - clip.sourceInFrame);
        }
        composition->layers.push_back(layer);
        request.sources.push_back({slot->second.source, layerMapping.sourceFrameNumber});
    }
    return composition;
}

bool MvmController::syncPreviewSourcesAt(std::int64_t timelineFrame, QString& error) {
    const auto mappedFrame = mapTimelinePreviewFrame(project_, timelineFrame);
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

    std::vector<int> desiredTracks;
    for (const auto& layer : mappedFrame.layers) {
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layer.clipIndex)];
        desiredTracks.push_back(layer.videoTrackIndex);
        const auto existing = candidateSources.find(layer.videoTrackIndex);
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
        candidateSources[layer.videoTrackIndex] = TrackPreviewSource{
            added.value(), clip.id, layer.clipIndex, previewVideoMappingOf(clip)};
    }
    for (auto entry = candidateSources.begin(); entry != candidateSources.end();) {
        if (std::find(desiredTracks.begin(), desiredTracks.end(), entry->first) ==
            desiredTracks.end())
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
    project::Project candidate = project_;
    // 素材を先にプロジェクトパネルへ登録し、clip はその素材から作る。
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips = prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Video);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    // 先頭素材だけ V1、以降は playhead 上の新しい Vn へ置く。
    const bool hasVideoClip = std::any_of(
        candidate.timelineClips.begin(), candidate.timelineClips.end(),
        [](const auto& entry) { return entry.kind != project::TimelineClipKind::Audio; });
    int videoTrackIndex = 0;
    if (hasVideoClip) {
        const auto addedTrack = project::addTrack(candidate, project::TrackKind::Video);
        if (!addedTrack.success) {
            setStatus(QString::fromStdString(addedTrack.error));
            return false;
        }
        videoTrackIndex = addedTrack.selectedIndex;
    }
    project::TimelineEditResult placed;
    if (clips.linkedAudio) {
        if (candidate.audioTracks.empty()) {
            const auto addedTrack = project::addTrack(candidate, project::TrackKind::Audio);
            if (!addedTrack.success) {
                setStatus(QString::fromStdString(addedTrack.error));
                return false;
            }
        }
        placed = project::placeLinkedAvPairAt(
            candidate, clips.primary, project::TrackRef{project::TrackKind::Video, videoTrackIndex},
            *clips.linkedAudio, project::TrackRef{project::TrackKind::Audio, 0}, playheadFrame_);
        // A1 が使用中なら既存 clip を壊さず、新しい audio track に同じ位置で置く。
        if (!placed.success) {
            const auto addedTrack = project::addTrack(candidate, project::TrackKind::Audio);
            if (addedTrack.success)
                placed = project::placeLinkedAvPairAt(
                    candidate, std::move(clips.primary),
                    project::TrackRef{project::TrackKind::Video, videoTrackIndex},
                    std::move(*clips.linkedAudio),
                    project::TrackRef{project::TrackKind::Audio, addedTrack.selectedIndex},
                    playheadFrame_);
        }
        if (!placed.success) {
            setStatus(QStringLiteral("リンク音声をA1へ配置できません: ") +
                      QString::fromStdString(placed.error));
            return false;
        }
    } else {
        placed = project::placeTimelineClipAt(
            candidate, std::move(clips.primary),
            project::TrackRef{project::TrackKind::Video, videoTrackIndex}, playheadFrame_);
        if (!placed.success) {
            setStatus(QString::fromStdString(placed.error));
            return false;
        }
    }

    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
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
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在する音声ファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    project::Project candidate = project_;
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips = prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Audio);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    // 動画・画像と同じく再生ヘッドの位置に置く。空いた audio track が無ければ足す。
    const auto placed =
        project::placeAudioClipAt(candidate, std::move(clips.primary), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    const int index = placed.selectedIndex;
    Q_EMIT stateChanged();
    return selectClip(index);
}

const project::MediaItem*
MvmController::registerMediaItem(project::Project& candidate,
                                 const std::filesystem::path& mediaPath, QString& error,
                                 const MediaImportResult* prebuilt) const {
    if (const auto* existing = project::findMediaItemByPath(candidate, mediaPath))
        return existing;
    auto probed = prebuilt ? *prebuilt : probeMediaFile(mediaPath);
    if (!probed.success) {
        error = QStringLiteral("素材をプロジェクトへ登録できません: ") +
                QString::fromStdString(probed.error);
        return nullptr;
    }
    const std::string id = newClipId();
    probed.item.id = id;
    probed.item.name =
        QFileInfo(QString::fromStdWString(mediaPath.wstring())).fileName().toStdString();
    const auto added = project::addMediaItem(candidate, std::move(probed.item));
    if (!added.success) {
        error = QStringLiteral("素材をプロジェクトへ登録できません: ") +
                QString::fromStdString(added.error);
        return nullptr;
    }
    return project::findMediaItem(candidate, id);
}

bool MvmController::applyMediaBinEdit(
    const std::function<project::MediaBinEditResult(project::Project&)>& edit,
    const QString& successStatus) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    const auto edited = edit(candidate);
    if (!edited.success) {
        setStatus(QString::fromStdString(edited.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    setStatus(successStatus);
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::importMediaFiles(const QList<QUrl>& fileUrls, const QString& folderId) {
    if (busy_)
        return false;
    project::Project candidate = project_;
    int imported = 0;
    int alreadyImported = 0;
    QStringList failures;
    for (const QUrl& url : fileUrls) {
        if (!url.isLocalFile()) {
            failures << url.toString() + QStringLiteral(": ローカルファイルではありません");
            continue;
        }
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() || !info.isFile()) {
            failures << info.fileName() + QStringLiteral(": ファイルがありません");
            continue;
        }
        const std::filesystem::path mediaPath(info.absoluteFilePath().toStdWString());
        if (project::findMediaItemByPath(candidate, mediaPath)) {
            ++alreadyImported;
            continue;
        }
        auto probed = probeMediaFile(mediaPath);
        if (!probed.success) {
            failures << info.fileName() + QStringLiteral(": ") +
                            QString::fromStdString(probed.error);
            continue;
        }
        probed.item.id = newClipId();
        probed.item.name = info.fileName().toStdString();
        probed.item.folderId = folderId.toStdString();
        const auto added = project::addMediaItem(candidate, std::move(probed.item));
        if (!added.success) {
            failures << info.fileName() + QStringLiteral(": ") +
                            QString::fromStdString(added.error);
            continue;
        }
        ++imported;
    }
    if (imported > 0 &&
        !commitProjectEdit(std::move(candidate), QStringLiteral("素材を読み込めません: ")))
        return false;

    QStringList parts;
    if (imported > 0)
        parts << QStringLiteral("%1 件の素材を読み込みました").arg(imported);
    if (alreadyImported > 0)
        parts << QStringLiteral("読み込み済みの %1 件は省略しました").arg(alreadyImported);
    if (!failures.isEmpty())
        parts << QStringLiteral("%1 件は読み込めません (%2)")
                     .arg(failures.size())
                     .arg(failures.join(QStringLiteral(" / ")));
    setStatus(parts.join(QStringLiteral("。")));
    if (imported > 0 && !folderId.isEmpty())
        mediaBinModel_->setExpanded(folderId, true);
    Q_EMIT stateChanged();
    // 一部が読めなくても、読めた素材は commit 済みである。false を「変更なし」の意味に保つ。
    return imported > 0;
}

QString MvmController::createMediaFolder(const QString& parentFolderId) {
    // 兄弟に限らず Project 全体で未使用の番号を振る。移動しても名前が衝突しない。
    QString name;
    for (int number = 1;; ++number) {
        name = QStringLiteral("フォルダ %1").arg(number, 2, 10, QLatin1Char('0'));
        const std::string candidateName = name.toStdString();
        if (std::none_of(project_.mediaFolders.begin(), project_.mediaFolders.end(),
                         [&](const auto& folder) { return folder.name == candidateName; }))
            break;
    }
    project::MediaFolder folder{newClipId(), name.toStdString(), parentFolderId.toStdString()};
    const QString id = QString::fromStdString(folder.id);
    const bool created = applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::addMediaFolder(candidate, std::move(folder));
        },
        QStringLiteral("フォルダを作成しました: ") + name);
    if (!created)
        return {};
    if (!parentFolderId.isEmpty())
        mediaBinModel_->setExpanded(parentFolderId, true);
    return id;
}

bool MvmController::renameMediaBinEntry(const QString& entryId, const QString& name) {
    const QString trimmed = name.trimmed();
    return applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::renameMediaBinEntry(candidate, entryId.toStdString(),
                                                trimmed.toStdString());
        },
        QStringLiteral("名前を変更しました: ") + trimmed);
}

bool MvmController::moveMediaBinEntries(const QStringList& entryIds, const QString& folderId) {
    std::vector<std::string> ids;
    for (const auto& id : entryIds)
        ids.push_back(id.toStdString());
    const bool moved = applyMediaBinEdit(
        [&](project::Project& candidate) {
            return project::moveMediaBinEntries(candidate, ids, folderId.toStdString());
        },
        QStringLiteral("%1 件を移動しました").arg(entryIds.size()));
    if (moved && !folderId.isEmpty())
        mediaBinModel_->setExpanded(folderId, true);
    return moved;
}

namespace {

std::vector<std::string> toStdIds(const QStringList& ids) {
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(ids.size()));
    for (const auto& id : ids)
        result.push_back(id.toStdString());
    return result;
}

} // namespace

int MvmController::mediaBinRemovalClipCount(const QStringList& entryIds) {
    const auto plan = project::planMediaBinRemoval(project_, toStdIds(entryIds));
    if (!plan.success) {
        setStatus(QString::fromStdString(plan.error));
        return -1;
    }
    return static_cast<int>(plan.clipIds.size());
}

bool MvmController::removeMediaBinEntries(const QStringList& entryIds) {
    if (busy_)
        return false;
    const auto ids = toStdIds(entryIds);
    const auto plan = project::planMediaBinRemoval(project_, ids);
    if (!plan.success) {
        setStatus(QString::fromStdString(plan.error));
        return false;
    }
    // 再生中の source が削除する clip を掴んだまま進まないよう、先に止める。
    if (!plan.clipIds.empty() && !pauseTimeline())
        return false;
    std::size_t removedClips = 0;
    const bool removed = applyMediaBinEdit(
        [&](project::Project& candidate) {
            auto result = project::removeMediaBinEntries(candidate, ids);
            removedClips = result.removedClipIds.size();
            return result;
        },
        QStringLiteral("%1 件を削除しました").arg(entryIds.size()));
    if (!removed || removedClips == 0)
        return removed;
    const QString resetFailure = resetAfterClipRemoval();
    if (!resetFailure.isEmpty()) {
        setStatus(QStringLiteral("素材とclipは削除しましたが、") + resetFailure);
        return true;
    }
    setStatus(QStringLiteral("%1 件の素材と %2 個のclipを削除しました")
                  .arg(entryIds.size())
                  .arg(removedClips));
    return true;
}

bool MvmController::addMediaItemToTimeline(const QString& itemId) {
    const auto* item = project::findMediaItem(project_, itemId.toStdString());
    if (!item) {
        setStatus(QStringLiteral("素材がありません"));
        return false;
    }
    const QUrl url = QUrl::fromLocalFile(QString::fromStdWString(item->mediaPath.wstring()));
    switch (item->kind) {
    case project::MediaKind::Video:
        return addVideoClip(url);
    case project::MediaKind::Audio:
        return addAudioClip(url);
    case project::MediaKind::Image:
        return addImageClip(url);
    }
    setStatus(QStringLiteral("素材の種別が不正です"));
    return false;
}

bool MvmController::placeMediaAtDropPoint(const std::vector<DropMedia>& media,
                                          const QString& trackKind, int trackIndex,
                                          qint64 frame) {
    if (busy_)
        return false;
    if (media.empty()) {
        setStatus(QStringLiteral("タイムラインへ置く素材がありません"));
        return false;
    }
    project::TrackRef target;
    if (trackKind == QStringLiteral("video"))
        target.kind = project::TrackKind::Video;
    else if (trackKind == QStringLiteral("audio"))
        target.kind = project::TrackKind::Audio;
    else {
        setStatus(QStringLiteral("ドロップ先の track 種別が不正です"));
        return false;
    }
    target.index = trackIndex;
    if (!pauseTimeline())
        return false;

    project::Project candidate = project_;
    std::int64_t start = std::max<qint64>(frame, 0);
    int firstIndex = -1;
    for (const auto& entry : media) {
        const QString name = QString::fromStdWString(entry.path.filename().wstring());
        QString registerError;
        const auto* item =
            entry.itemId.empty()
                ? registerMediaItem(candidate, entry.path, registerError, entry.probed)
                : project::findMediaItem(candidate, entry.itemId);
        if (!item) {
            setStatus(entry.itemId.empty() ? registerError : QStringLiteral("素材がありません"));
            return false;
        }
        auto clips = prepareMediaClips(candidate, std::string(item->id), entry.kind, entry.probed);
        if (!clips.success) {
            setStatus(name + QStringLiteral(": ") + clips.error);
            return false;
        }
        const auto placed = project::placeMediaAtDrop(candidate, std::move(clips.primary),
                                                      std::move(clips.linkedAudio), target, start);
        if (!placed.success) {
            setStatus(name + QStringLiteral(": ") + QString::fromStdString(placed.error));
            return false;
        }
        // track を足して置いた場合も、その track は同じ index にあるので続きも同じ行へ並ぶ。
        const auto duration = project::timelineClipDuration(
            candidate, candidate.timelineClips[static_cast<std::size_t>(placed.selectedIndex)]);
        if (!duration.success) {
            setStatus(QString::fromStdString(duration.error));
            return false;
        }
        if (firstIndex < 0)
            firstIndex = placed.selectedIndex;
        start += duration.frame;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    Q_EMIT stateChanged();
    return selectClip(firstIndex);
}

bool MvmController::addMediaItemsToTimelineAt(const QStringList& itemIds,
                                              const QString& trackKind, int trackIndex,
                                              qint64 frame) {
    std::vector<DropMedia> media;
    for (const auto& id : itemIds) {
        // フォルダは中身を展開せずに飛ばす (どの順で並べるかを決められないため)。
        const auto* item = project::findMediaItem(project_, id.toStdString());
        if (item)
            media.push_back({item->mediaPath, item->kind, nullptr, item->id});
    }
    return placeMediaAtDropPoint(media, trackKind, trackIndex, frame);
}

bool MvmController::addMediaFilesToTimelineAt(const QList<QUrl>& fileUrls,
                                              const QString& trackKind, int trackIndex,
                                              qint64 frame) {
    if (busy_)
        return false;
    // probed は media から指されるので、途中で再配置されないよう先に確保する。
    std::vector<MediaImportResult> probed;
    probed.reserve(static_cast<std::size_t>(fileUrls.size()));
    std::vector<DropMedia> media;
    for (const auto& url : fileUrls) {
        const std::filesystem::path mediaPath =
            localMediaFile(url, QStringLiteral("存在するファイルを選択してください"));
        if (mediaPath.empty())
            return false;
        probed.push_back(probeMediaFile(mediaPath));
        if (!probed.back().success) {
            setStatus(QFileInfo(url.toLocalFile()).fileName() + QStringLiteral(": ") +
                      QString::fromStdString(probed.back().error));
            return false;
        }
        media.push_back({mediaPath, probed.back().item.kind, &probed.back(), {}});
    }
    return placeMediaAtDropPoint(media, trackKind, trackIndex, frame);
}

std::filesystem::path MvmController::localMediaFile(const QUrl& fileUrl,
                                                    const QString& missingText) {
    if (!fileUrl.isLocalFile()) {
        setStatus(QStringLiteral("ローカルファイルを選択してください"));
        return {};
    }
    const QFileInfo info(fileUrl.toLocalFile());
    if (!info.exists() || !info.isFile()) {
        setStatus(missingText);
        return {};
    }
    return std::filesystem::path(info.absoluteFilePath().toStdWString());
}

bool MvmController::addImageClip(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在する画像ファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    const auto probed = probeMediaFile(mediaPath);
    if (!probed.success) {
        setStatus(QString::fromStdString(probed.error));
        return false;
    }
    if (probed.item.kind != project::MediaKind::Image) {
        setStatus(QStringLiteral("画像ではありません"));
        return false;
    }
    return placeImageClip(mediaPath, QFileInfo(fileUrl.toLocalFile()).fileName(), probed);
}

bool MvmController::placeImageClip(const std::filesystem::path& mediaPath, const QString& fileName,
                                   const MediaImportResult& probed) {
    project::Project candidate = project_;
    QString registerError;
    const auto* item = registerMediaItem(candidate, mediaPath, registerError, &probed);
    if (!item) {
        setStatus(registerError);
        return false;
    }
    auto clips = prepareMediaClips(candidate, std::string(item->id), project::MediaKind::Image, &probed);
    if (!clips.success) {
        setStatus(clips.error);
        return false;
    }
    clips.primary.name = fileName.toStdString();
    const auto placed =
        project::placeStillClipAt(candidate, std::move(clips.primary), playheadFrame_);
    if (!placed.success) {
        setStatus(QString::fromStdString(placed.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("画像 clip を追加できません: ")))
        return false;
    Q_EMIT stateChanged();
    return selectClip(placed.selectedIndex);
}

bool MvmController::addMediaFileToTimeline(const QUrl& fileUrl) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    const std::filesystem::path mediaPath =
        localMediaFile(fileUrl, QStringLiteral("存在するファイルを選択してください"));
    if (mediaPath.empty())
        return false;
    const auto probed = probeMediaFile(mediaPath);
    if (!probed.success) {
        setStatus(QString::fromStdString(probed.error));
        return false;
    }
    switch (probed.item.kind) {
    case project::MediaKind::Video:
        return addVideoClip(fileUrl);
    case project::MediaKind::Audio:
        return addAudioClip(fileUrl);
    case project::MediaKind::Image:
        return placeImageClip(mediaPath, QFileInfo(fileUrl.toLocalFile()).fileName(), probed);
    }
    setStatus(QStringLiteral("素材の種別が不正です"));
    return false;
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
        if (active[index] && !project::isStillClipKind(active[index]->kind) &&
            !project_.videoTracks[index].muted)
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
    const auto mapped = mapTimelinePreviewFrame(project_, playheadFrame_);
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
    if (clip.track.kind != project::TrackKind::Video ||
        project_.videoTracks[static_cast<std::size_t>(clip.track.index)].muted)
        return false;
    const auto duration = project::timelineClipDuration(project_, clip);
    return duration.success && playheadFrame_ >= clip.timelineStartFrame &&
           playheadFrame_ < clip.timelineStartFrame + duration.frame;
}

project::ClipVisualGeometry MvmController::visualGeometryOf(int clipIndex) const {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(project_.timelineClips.size()))
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    if (clip.kind != project::TimelineClipKind::Video &&
        clip.kind != project::TimelineClipKind::Image)
        return {};
    // clip は必ずプロジェクトパネルの素材を指す (validateMediaReferences)。
    const auto* item = project::findMediaItem(project_, clip.mediaItemId);
    if (!item)
        return {};
    return project::clipVisualGeometry(effectsForPreview(clipIndex), item->width, item->height,
                                       project_.outputWidth, project_.outputHeight);
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
        if (!found.isEmpty() || (kind != project::TimelineClipKind::Video &&
                                 kind != project::TimelineClipKind::Image))
            return {};
        found = QString::fromStdString(id);
    }
    return found;
}

QVariantMap MvmController::clipVisualGeometry(const QString& clipId) const {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
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
            const bool hit =
                clip.kind == project::TimelineClipKind::Text
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
        if (clip.kind == project::TimelineClipKind::Text) {
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
    const auto* item = project::findMediaItem(
        project_, project_.timelineClips[static_cast<std::size_t>(index)].mediaItemId);
    auto effects = effectsForPreview(index);
    if (!item || !project::effectsForVisualRect(effects, item->width, item->height,
                                                project_.outputWidth, project_.outputHeight, x, y,
                                                width, height))
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
            if (textRasterBounds(index).contains(x, y))
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
    const qint64 clamped =
        std::clamp<qint64>(frame, 0, std::max<qint64>(0, navigationTimelineFrames() - 1));
    playheadFrame_ = clamped;
    if (project_.timelineClips.empty()) {
        currentClipIndex_ = -1;
        currentClipName_.clear();
        currentClipPath_.clear();
        Q_EMIT stateChanged();
        return true;
    }
    if (scrubPending_ && !scrubbing_)
        scrubTargetFrame_ = clamped;
    int index = -1;
    // 選択clipがactiveならaudio/videoを問わず維持する。videoだけを検索すると、
    // A1の見た目の選択を残したままcurrentClipIndex_だけV1へ変わってしまう。
    if (currentClipIndex_ >= 0 &&
        currentClipIndex_ < static_cast<int>(project_.timelineClips.size())) {
        const auto& current = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
        if (project::timelineClipIndexAt(project_, current.track, clamped) == currentClipIndex_)
            index = currentClipIndex_;
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
    // drag 中に再生を始めたら scrub の断片と通常再生が二重に鳴らないようにする。
    stopScrubAudio();
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
    shuttleRate_ = 0;
    pendingPlaybackStart_ = false;
    playbackClipIndex_ = -1;
    pendingPlaybackClipIndex_ = -1;
    setStatus(std::move(error));
}

bool MvmController::handOffPlaybackSources(std::int64_t frame, QString& reason) {
    const auto mappedFrame = mapTimelinePreviewFrame(project_, frame);
    if (!mappedFrame.success) {
        reason = QString::fromStdString(mappedFrame.error);
        return false;
    }
    if (mappedFrame.layers.size() != trackSources_.size()) {
        reason = QStringLiteral("表示するvideo trackの数が変わります");
        return false;
    }
    bool videoChanged = false;
    for (const auto& layer : mappedFrame.layers) {
        const auto installed = trackSources_.find(layer.videoTrackIndex);
        if (installed == trackSources_.end()) {
            reason = QStringLiteral("表示するvideo trackが変わります");
            return false;
        }
        if (installed->second.clipId == layer.clipId)
            continue;
        const auto& clip = project_.timelineClips[static_cast<std::size_t>(layer.clipIndex)];
        if (!previewVideoMappingCovers(project_, installed->second.mapping, clip)) {
            reason = QString::fromStdString(clip.name) +
                     QStringLiteral(" は直前のclipと素材の位置が連続していません");
            return false;
        }
        videoChanged = true;
    }
    const auto audioMapping = mapTimelinePreviewAudio(project_, frame);
    if (!audioMapping.success) {
        reason = QString::fromStdString(audioMapping.error);
        return false;
    }
    if (audioMapping.layers.size() != audioSources_.size()) {
        reason = QStringLiteral("鳴らすaudio clipの数が変わります");
        return false;
    }
    bool audioChanged = false;
    for (std::size_t index = 0; index < audioSources_.size(); ++index)
        audioChanged =
            audioChanged || audioMapping.layers[index].clipId != audioSources_[index].clipId;
    if (audioChanged) {
        std::vector<AudioSourceIdentity> desired;
        if (!audioIdentitiesFor(audioMapping, desired, reason))
            return false;
        for (std::size_t index = 0; index < desired.size(); ++index) {
            if (!(desired[index] == audioSources_[index].identity)) {
                reason = QStringLiteral("audio clipの素材の位置が直前のclipと連続していません");
                return false;
            }
        }
    }
    // 文字 clip は再生中にも出入りする。video / audio が変わらなくても composition は
    // 毎回組み、前回と違うときだけ出し直す (同じ文字は同じ画像 instance なので安い)。
    // 引き継いだ clip の effect もここで反映する。
    auto sources = trackSources_;
    for (const auto& layer : mappedFrame.layers) {
        auto& slot = sources[layer.videoTrackIndex];
        slot.clipId = layer.clipId;
        slot.clipIndex = layer.clipIndex;
    }
    preview::PreviewFrameRequest unusedRequest;
    const auto composition = previewCompositionFor(mappedFrame, sources, unusedRequest, reason);
    if (!composition)
        return false;
    if (!videoChanged && !audioChanged && submittedComposition_ &&
        *submittedComposition_ == *composition)
        return true;
    if (!submittedComposition_ || !(*submittedComposition_ == *composition)) {
        const auto submitted = previewEngine_->submitComposition(composition);
        if (!submitted) {
            reason = previewErrorText(submitted.error());
            return false;
        }
        submittedComposition_ = composition;
    }
    trackSources_ = std::move(sources);
    for (std::size_t index = 0; index < audioSources_.size(); ++index) {
        audioSources_[index].clipId = audioMapping.layers[index].clipId;
        audioSources_[index].clipIndex = audioMapping.layers[index].clipIndex;
    }
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
        statusText_ = QStringLiteral("timeline終端まで再生しました");
        Q_EMIT stateChanged();
        return;
    }
    // 次の clip を今の source のまま表示できる (分割直後の連続した clip など) なら、
    // 止めずに source を引き継ぐ。できなければ一時停止して source を組み直す。
    QString handOffFailure;
    if (handOffPlaybackSources(frame, handOffFailure)) {
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
    const int nextClip = selected ? static_cast<int>(selected - project_.timelineClips.data()) : -1;
    if (!queuePreparedPlayback(nextClip, frame)) {
        stopPlaybackWithError(QStringLiteral("次のclipへ切り替えられません: ") + statusText_);
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

bool MvmController::shuttleLeft() {
    return changeShuttleRate(-1);
}

bool MvmController::shuttleRight() {
    return changeShuttleRate(1);
}

bool MvmController::changeShuttleRate(int direction) {
    if (busy_ || project_.timelineClips.empty() || totalTimelineFrames_ <= 0 || direction == 0)
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
    if (busy_ || project_.timelineClips.empty() || totalTimelineFrames_ <= 0 || delta == 0)
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

QVariantList MvmController::timelineMarkers() const {
    QVariantList frames;
    for (const auto frame : project_.timelineMarkers)
        frames.push_back(QVariant::fromValue<qlonglong>(frame));
    return frames;
}

std::vector<project::TimelineClip>
MvmController::selectedTimelineClipsInOrder(const std::string& anchorId) const {
    const bool anchorSelected =
        anchorId.empty() || std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                      anchorId) != selectedClipIds_.end();
    std::vector<project::TimelineClip> clips;
    for (const auto& clip : project_.timelineClips) {
        if (clip.id == anchorId ||
            (anchorSelected && std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                         clip.id) != selectedClipIds_.end()))
            clips.push_back(clip);
    }
    return clips;
}

void MvmController::storeClipboard(std::vector<project::TimelineClip> clips) {
    // 別 Project へ貼ったときに Project パネルへ同じ素材を登録できるよう、bin の項目も持つ。
    clipboardMediaItems_.clear();
    for (const auto& clip : clips) {
        if (!project::clipUsesMediaItem(clip.kind))
            continue;
        const auto* item = project::findMediaItem(project_, clip.mediaItemId);
        if (item && !std::any_of(clipboardMediaItems_.begin(), clipboardMediaItems_.end(),
                                 [&](const project::MediaItem& stored) {
                                     return stored.mediaPath == item->mediaPath;
                                 }))
            clipboardMediaItems_.push_back(*item);
    }
    clipboardClips_ = std::move(clips);
    clipboardFpsNum_ = project_.timelineFpsNum;
    clipboardFpsDen_ = project_.timelineFpsDen;
}

bool MvmController::copySelectedClips() {
    if (busy_ || selectedClipIds_.empty()) {
        setStatus(QStringLiteral("コピーするclipがありません"));
        return false;
    }
    auto copied = selectedTimelineClipsInOrder({});
    if (copied.size() != selectedClipIds_.size()) {
        setStatus(QStringLiteral("選択clipがProjectにありません"));
        return false;
    }
    storeClipboard(std::move(copied));
    setStatus(QString::number(clipboardClips_.size()) + QStringLiteral("個のclipをコピーしました"));
    return true;
}

bool MvmController::cutSelectedClips() {
    if (busy_ || !pauseTimeline())
        return false;
    if (selectedClipIds_.empty()) {
        setStatus(QStringLiteral("カットするclipがありません"));
        return false;
    }
    auto copied = selectedTimelineClipsInOrder({});
    project::Project candidate = project_;
    if (copied.size() != selectedClipIds_.size()) {
        setStatus(QStringLiteral("カットするclipがProjectにありません"));
        return false;
    }
    candidate.timelineClips.erase(
        std::remove_if(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                       [&](const project::TimelineClip& clip) {
                           return std::find(selectedClipIds_.begin(), selectedClipIds_.end(),
                                            clip.id) != selectedClipIds_.end();
                       }),
        candidate.timelineClips.end());
    for (auto& clip : candidate.timelineClips) {
        if (clip.linkGroupId.empty())
            continue;
        const bool partnerRemains =
            std::any_of(candidate.timelineClips.begin(), candidate.timelineClips.end(),
                        [&](const project::TimelineClip& other) {
                            return other.id != clip.id && other.linkGroupId == clip.linkGroupId;
                        });
        if (!partnerRemains)
            clip.linkGroupId.clear();
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("カットできません: ")))
        return false;
    // cut は bin を変えないので、commit 後の Project から素材を控えてよい。
    storeClipboard(std::move(copied));
    setTimelineSelection({});
    if (project_.timelineClips.empty()) {
        const bool reset = resetPreviewEngine();
        currentSource_.reset();
        currentClipIndex_ = -1;
        currentClipName_.clear();
        currentClipPath_.clear();
        Q_EMIT stateChanged();
        if (!reset) {
            setStatus(QStringLiteral("clipはカットしましたが、Previewを初期化できません"));
            return true;
        }
        setStatus(QStringLiteral("clipをカットしました"));
        return true;
    }
    return refreshPreviewAfterSavedEdit({}, QStringLiteral("clipをカットしました"));
}

bool MvmController::placeCopiedClips(const std::vector<project::TimelineClip>& clips,
                                     const std::vector<project::MediaItem>& mediaItems,
                                     std::int64_t sourceFpsNum, std::int64_t sourceFpsDen,
                                     std::int64_t destinationFrame, int videoTrackDelta,
                                     int audioTrackDelta, CopyPlacement placement) {
    if (busy_ || !pauseTimeline())
        return false;
    if (clips.empty()) {
        setStatus(QStringLiteral("配置するclipがありません"));
        return false;
    }
    if (destinationFrame < 0) {
        setStatus(QStringLiteral("配置先のframeが不正です"));
        return false;
    }
    project::Project candidate = project_;
    const auto first =
        std::min_element(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            return a.timelineStartFrame < b.timelineStartFrame;
        });
    std::map<std::pair<project::TrackKind, int>, std::vector<project::TimelineClip>> lanes;
    std::map<std::string, int> linkCounts;
    for (const auto& clip : clips) {
        if (clip.kind != project::TimelineClipKind::Text &&
            !std::filesystem::exists(clip.mediaPath)) {
            setStatus(QStringLiteral("コピー元の素材が見つかりません: ") +
                      fromPath(clip.mediaPath));
            return false;
        }
        const auto offset = project::sourceBoundaryToTimelineBoundary(
            clip.timelineStartFrame - first->timelineStartFrame, sourceFpsNum, sourceFpsDen,
            candidate.timelineFpsNum, candidate.timelineFpsDen);
        if (!offset.success ||
            destinationFrame > std::numeric_limits<std::int64_t>::max() - offset.frame) {
            setStatus(QStringLiteral("貼り付け位置を変換できません"));
            return false;
        }
        auto placed = clip;
        placed.timelineStartFrame = destinationFrame + offset.frame;
        const int delta =
            clip.track.kind == project::TrackKind::Video ? videoTrackDelta : audioTrackDelta;
        // 個別に 0 へ丸めると clip 同士の上下関係が崩れる。範囲はドラッグ側で揃えておく。
        if (clip.track.index + delta < 0) {
            setStatus(QStringLiteral("配置先のtrackが範囲外です"));
            return false;
        }
        placed.track.index = clip.track.index + delta;
        if (sourceFpsNum != candidate.timelineFpsNum || sourceFpsDen != candidate.timelineFpsDen) {
            // key は clip 先頭からの timeline frame なので、fps が違えば同じ秒位置へ移す。
            const auto duration = project::timelineClipDuration(candidate, placed);
            if (!duration.success ||
                !project::retimeClipKeys(placed.effects.opacityKeys, sourceFpsNum, sourceFpsDen,
                                         candidate.timelineFpsNum, candidate.timelineFpsDen,
                                         duration.frame) ||
                !project::retimeClipKeys(placed.effects.volumeKeys, sourceFpsNum, sourceFpsDen,
                                         candidate.timelineFpsNum, candidate.timelineFpsDen,
                                         duration.frame)) {
                setStatus(QStringLiteral("キーフレームを貼り付け先のfpsへ変換できません"));
                return false;
            }
        }
        lanes[{clip.track.kind, clip.track.index}].push_back(std::move(placed));
        if (!clip.linkGroupId.empty())
            ++linkCounts[clip.linkGroupId];
    }
    // 素材の追加と同じく、timeline へ置いた素材は同じ編集で Project パネルにも載せる。
    // clip を置く前に登録する (登録の検証が、置いた clip の素材参照も見るため)。
    // コピー元の bin 項目があればそれを使い (folder は移し先に無いので root)、無ければ調べ直す。
    for (const auto& clip : clips) {
        if (!project::clipUsesMediaItem(clip.kind) ||
            project::findMediaItemByPath(candidate, clip.mediaPath))
            continue;
        const auto copied = std::find_if(
            mediaItems.begin(), mediaItems.end(),
            [&](const project::MediaItem& item) { return item.mediaPath == clip.mediaPath; });
        if (copied == mediaItems.end()) {
            QString registerError;
            if (!registerMediaItem(candidate, clip.mediaPath, registerError)) {
                setStatus(registerError);
                return false;
            }
            continue;
        }
        auto item = *copied;
        item.id = newClipId();
        item.folderId.clear();
        const auto added = project::addMediaItem(candidate, std::move(item));
        if (!added.success) {
            setStatus(QStringLiteral("素材をプロジェクトへ登録できません: ") +
                      QString::fromStdString(added.error));
            return false;
        }
    }
    std::map<std::string, std::string> newLinkIds;
    std::set<std::pair<project::TrackKind, int>> reservedTracks;
    std::vector<std::string> newIds;
    for (auto& [sourceLane, laneClips] : lanes) {
        const auto kind = sourceLane.first;
        auto& tracks = project::tracksOfKind(candidate, kind);
        int chosen = -1;
        std::vector<int> trackOrder;
        if (placement == CopyPlacement::ExactTrack) {
            // ドラッグで見せた track にそのまま置く。重なりは validateTimeline が拒否する。
            if (laneClips.front().track.index >= static_cast<int>(tracks.size())) {
                setStatus(QStringLiteral("配置先のtrackが範囲外です"));
                return false;
            }
        } else if (laneClips.front().track.index <= static_cast<int>(tracks.size()))
            trackOrder.push_back(laneClips.front().track.index);
        for (int index = 0; placement == CopyPlacement::FindFreeTrack &&
                            index <= static_cast<int>(tracks.size());
             ++index) {
            if (std::find(trackOrder.begin(), trackOrder.end(), index) == trackOrder.end())
                trackOrder.push_back(index);
        }
        if (placement == CopyPlacement::ExactTrack)
            chosen = laneClips.front().track.index;
        for (const int index : trackOrder) {
            if (index < static_cast<int>(tracks.size()) &&
                tracks[static_cast<std::size_t>(index)].muted)
                continue;
            if (reservedTracks.contains({kind, index}))
                continue;
            bool free = true;
            for (const auto& copy : laneClips) {
                const auto copyDuration = project::timelineClipDuration(candidate, copy);
                if (!copyDuration.success ||
                    copy.timelineStartFrame >
                        std::numeric_limits<std::int64_t>::max() - copyDuration.frame) {
                    setStatus(QStringLiteral("コピーしたclipの尺が不正です"));
                    return false;
                }
                const auto copyEnd = copy.timelineStartFrame + copyDuration.frame;
                for (const auto& existing : candidate.timelineClips) {
                    if (existing.track.kind != kind || existing.track.index != index)
                        continue;
                    const auto existingDuration =
                        project::timelineClipDuration(candidate, existing);
                    if (!existingDuration.success ||
                        (copy.timelineStartFrame <
                             existing.timelineStartFrame + existingDuration.frame &&
                         existing.timelineStartFrame < copyEnd)) {
                        free = false;
                        break;
                    }
                }
                if (!free)
                    break;
            }
            if (free) {
                chosen = index;
                break;
            }
        }
        if (chosen < 0) {
            setStatus(QStringLiteral("空きtrackを確保できません"));
            return false;
        }
        if (chosen == static_cast<int>(tracks.size()))
            tracks.push_back(project::Track{project::defaultTrackName(kind, chosen), false});
        reservedTracks.insert({kind, chosen});
        for (auto& copy : laneClips) {
            copy.track = {kind, chosen};
            copy.id = newClipId();
            if (!copy.linkGroupId.empty()) {
                if (linkCounts[copy.linkGroupId] == 2) {
                    auto& newId = newLinkIds[copy.linkGroupId];
                    if (newId.empty())
                        newId = newClipId();
                    copy.linkGroupId = newId;
                } else {
                    copy.linkGroupId.clear();
                }
            }
            newIds.push_back(copy.id);
            candidate.timelineClips.push_back(std::move(copy));
        }
    }
    // 置いた clip の素材 id を貼り付け先の素材へ付け替える。別 Project からの貼り付けでは
    // コピー元の id は貼り付け先に無い (同じ Project なら元の素材のまま)。
    for (auto& clip : candidate.timelineClips) {
        if (!project::clipUsesMediaItem(clip.kind) ||
            std::find(newIds.begin(), newIds.end(), clip.id) == newIds.end())
            continue;
        const auto* item = project::findMediaItem(candidate, clip.mediaItemId);
        if (!item || item->mediaPath != clip.mediaPath)
            item = project::findMediaItemByPath(candidate, clip.mediaPath);
        if (!item) {
            setStatus(QStringLiteral("貼り付けた素材がプロジェクトパネルにありません: ") +
                      fromPath(clip.mediaPath));
            return false;
        }
        clip.mediaItemId = item->id;
        clip.mediaPath = item->mediaPath;
    }
    const auto valid = project::validateTimeline(candidate);
    if (!valid.success) {
        setStatus(QStringLiteral("clipを配置できません: ") + QString::fromStdString(valid.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("clipを配置できません: ")))
        return false;
    setTimelineSelection(newIds, false);
    return refreshPreviewAfterSavedEdit(
        newIds.front(), QString::number(newIds.size()) + QStringLiteral("個のclipを配置しました"));
}

bool MvmController::pasteClips() {
    return placeCopiedClips(clipboardClips_, clipboardMediaItems_, clipboardFpsNum_,
                            clipboardFpsDen_, playheadFrame_, 0, 0, CopyPlacement::FindFreeTrack);
}

bool MvmController::duplicateSelectedClips() {
    return placeCopiedClips(selectedTimelineClipsInOrder({}), project_.mediaItems,
                            project_.timelineFpsNum, project_.timelineFpsDen, playheadFrame_, 0, 0,
                            CopyPlacement::FindFreeTrack);
}

QVariantMap MvmController::timelineDragBounds(const QString& clipId) const {
    // ドラッグで一緒に動く clip 群 (anchor が選択中なら選択全体) の端。QML はこれで
    // ドラッグ量を先に丸め、表示した位置のまま移動・複製を確定させる。
    const auto clips = selectedTimelineClipsInOrder(clipId.toStdString());
    QVariantMap bounds;
    if (clips.empty())
        return bounds;
    qint64 minStart = std::numeric_limits<qint64>::max();
    std::map<project::TrackKind, std::pair<int, int>> trackRange;
    for (const auto& clip : clips) {
        minStart = std::min<qint64>(minStart, clip.timelineStartFrame);
        const auto [range, inserted] =
            trackRange.try_emplace(clip.track.kind, clip.track.index, clip.track.index);
        range->second.first = std::min(range->second.first, clip.track.index);
        range->second.second = std::max(range->second.second, clip.track.index);
    }
    bounds.insert(QStringLiteral("minStartFrame"), minStart);
    for (const auto& [kind, range] : trackRange) {
        const QString prefix =
            kind == project::TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio");
        bounds.insert(prefix + QStringLiteral("MinTrack"), range.first);
        bounds.insert(prefix + QStringLiteral("MaxTrack"), range.second);
    }
    return bounds;
}

bool MvmController::duplicateTimelineClipsAt(const QString& clipId, const QString& trackKind,
                                             int trackIndex, qint64 timelineStartFrame) {
    project::TrackRef destination;
    if (!resolveTrackRef(trackKind, trackIndex, destination))
        return false;
    const auto anchorId = clipId.toStdString();
    const int anchorIndex = indexOfClipId(project_.timelineClips, anchorId);
    if (anchorIndex < 0)
        return false;
    const auto& anchor = project_.timelineClips[static_cast<std::size_t>(anchorIndex)];
    if (destination.kind != anchor.track.kind) {
        setStatus(QStringLiteral("異なる種別のtrackへ複製できません"));
        return false;
    }
    const auto clips = selectedTimelineClipsInOrder(anchorId);
    const auto first =
        std::min_element(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            return a.timelineStartFrame < b.timelineStartFrame;
        });
    // 位置や track を後から寄せると、ドラッグ中に見せた位置と確定位置がずれる。QML が
    // timelineDragBounds で丸めた値を渡すので、範囲外や既存 clip との重なりは拒否する。
    const qint64 destinationFirst =
        timelineStartFrame - (anchor.timelineStartFrame - first->timelineStartFrame);
    return placeCopiedClips(
        clips, project_.mediaItems, project_.timelineFpsNum, project_.timelineFpsDen,
        destinationFirst,
        destination.kind == project::TrackKind::Video ? destination.index - anchor.track.index : 0,
        destination.kind == project::TrackKind::Audio ? destination.index - anchor.track.index : 0,
        CopyPlacement::ExactTrack);
}

bool MvmController::addTimelineMarker() {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto at = std::lower_bound(candidate.timelineMarkers.begin(),
                                     candidate.timelineMarkers.end(), playheadFrame_);
    if (at != candidate.timelineMarkers.end() && *at == playheadFrame_)
        return true;
    candidate.timelineMarkers.insert(at, playheadFrame_);
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("マーカーを追加できません: ")))
        return false;
    setStatus(QStringLiteral("マーカーを追加しました"));
    return true;
}

bool MvmController::deleteTimelineMarker(qint64 frame) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto at = std::lower_bound(candidate.timelineMarkers.begin(),
                                     candidate.timelineMarkers.end(), frame);
    if (at == candidate.timelineMarkers.end() || *at != frame)
        return false;
    candidate.timelineMarkers.erase(at);
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("マーカーを削除できません: ")))
        return false;
    setStatus(QStringLiteral("マーカーを削除しました"));
    return true;
}

bool MvmController::jumpToMarker(int direction) {
    if (busy_ || direction == 0 || project_.timelineMarkers.empty())
        return false;
    const auto& markers = project_.timelineMarkers;
    if (direction > 0) {
        const auto next = std::upper_bound(markers.begin(), markers.end(), playheadFrame_);
        return next != markers.end() && seekTimelineFrame(*next);
    }
    const auto previous = std::lower_bound(markers.begin(), markers.end(), playheadFrame_);
    return previous != markers.begin() && seekTimelineFrame(*std::prev(previous));
}

bool MvmController::markIn() {
    if (busy_ || !pauseTimeline())
        return false;
    if (project_.outFrame && playheadFrame_ >= *project_.outFrame) {
        setStatus(QStringLiteral("インはアウトより前に設定してください"));
        return false;
    }
    if (project_.inFrame == playheadFrame_)
        return true;
    project::Project candidate = project_;
    candidate.inFrame = playheadFrame_;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("インを設定できません: ")))
        return false;
    setStatus(QStringLiteral("インを設定しました"));
    return true;
}

bool MvmController::markOut() {
    if (busy_ || !pauseTimeline())
        return false;
    if (project_.inFrame && playheadFrame_ <= *project_.inFrame) {
        setStatus(QStringLiteral("アウトはインより後に設定してください"));
        return false;
    }
    if (project_.outFrame == playheadFrame_)
        return true;
    project::Project candidate = project_;
    candidate.outFrame = playheadFrame_;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("アウトを設定できません: ")))
        return false;
    setStatus(QStringLiteral("アウトを設定しました"));
    return true;
}

bool MvmController::jumpToIn() {
    return project_.inFrame && seekTimelineFrame(*project_.inFrame);
}

bool MvmController::jumpToOut() {
    return project_.outFrame && seekTimelineFrame(*project_.outFrame);
}

bool MvmController::clearInOut() {
    if (busy_ || !pauseTimeline())
        return false;
    if (!project_.inFrame && !project_.outFrame)
        return true;
    project::Project candidate = project_;
    candidate.inFrame.reset();
    candidate.outFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("イン・アウトを消去できません: ")))
        return false;
    setStatus(QStringLiteral("イン・アウトを消去しました"));
    return true;
}

bool MvmController::clearIn() {
    if (busy_ || !pauseTimeline() || !project_.inFrame)
        return false;
    project::Project candidate = project_;
    candidate.inFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("インを消去できません: ")))
        return false;
    setStatus(QStringLiteral("インを消去しました"));
    return true;
}

bool MvmController::clearOut() {
    if (busy_ || !pauseTimeline() || !project_.outFrame)
        return false;
    project::Project candidate = project_;
    candidate.outFrame.reset();
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("アウトを消去できません: ")))
        return false;
    setStatus(QStringLiteral("アウトを消去しました"));
    return true;
}

bool MvmController::moveTimelineClip(const QString& clipId, const QString& trackKind,
                                     int trackIndex, qint64 timelineStartFrame, bool) {
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
    const std::string anchorId = clipId.toStdString();
    std::vector<std::string> movedIds = selectedClipIds_;
    if (std::find(movedIds.begin(), movedIds.end(), anchorId) == movedIds.end()) {
        movedIds = {anchorId};
        setTimelineSelection(movedIds);
    }
    const auto moved =
        project::moveClips(candidate, movedIds, anchorId, destination,
                           std::max<qint64>(0, timelineStartFrame), project::LinkMode::Single);
    if (!moved.success) {
        setStatus(QString::fromStdString(moved.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    const QString status = movedIds.size() > 1 ? QString::number(movedIds.size()) +
                                                     QStringLiteral("個のclipを移動しました")
                                               : QStringLiteral("clipを移動しました");
    return refreshPreviewAfterSavedEdit(anchorId, status);
}

bool MvmController::resolveTrimEdge(const QString& edge, project::TrimEdge& trimEdge) {
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else {
        setStatus(QStringLiteral("trim edgeが不正です"));
        return false;
    }
    return true;
}

bool MvmController::applyTimelineEdit(
    const std::function<project::TimelineEditResult(project::Project&)>& edit,
    const std::string& selectedClipId, const QString& successStatus) {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto edited = edit(candidate);
    if (!edited.success) {
        setStatus(QString::fromStdString(edited.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    return refreshPreviewAfterSavedEdit(selectedClipId, successStatus);
}

bool MvmController::trimClip(const QString& clipId, const QString& edge, qint64 projectFrameDelta,
                             bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::trimTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                             linkModeFor(linked));
        },
        id, QStringLiteral("clipをtrimしました"));
}

bool MvmController::rateStretchClip(const QString& clipId, const QString& edge,
                                    qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rateStretchTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                                    linkModeFor(linked));
        },
        id, QStringLiteral("clipの速度を変えました"));
}

QVariantMap MvmController::clipSpeedDurationState(const QString& clipId) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const int index = indexOfClipId(project_.timelineClips, id);
    if (index < 0)
        return {};
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(index)];
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success)
        return {};
    return {{QStringLiteral("clipId"), QString::fromStdString(id)},
            {QStringLiteral("speedPercent"), 100.0 * static_cast<double>(clip.speedNum) /
                                                    static_cast<double>(clip.speedDen)},
            {QStringLiteral("durationText"), QString::fromStdString(core::formatTimecode(
                 duration.frame, project_.timelineFpsNum, project_.timelineFpsDen))},
            {QStringLiteral("preservePitch"), clip.preservePitch},
            {QStringLiteral("still"), project::hasSyntheticSourceDomain(clip)}};
}

QVariantMap MvmController::previewClipSpeedDuration(const QString& clipId,
                                                    const QString& input,
                                                    double speedPercent,
                                                    const QString& durationText,
                                                    bool preservePitch, bool ripple) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const auto edit = speedDurationEdit(input, speedPercent, durationText, preservePitch, ripple,
                                        project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit)
        return {{QStringLiteral("error"), QStringLiteral("速度または尺が不正です")}};
    auto previewEdit = *edit;
    previewEdit.ripple = true;
    const auto preview = project::previewClipSpeedDuration(project_, id, previewEdit,
                                                            project::LinkMode::Linked);
    if (!preview.success)
        return {{QStringLiteral("error"), QString::fromStdString(preview.error)}};
    return {{QStringLiteral("durationText"), QString::fromStdString(core::formatTimecode(
                 preview.durationFrames, project_.timelineFpsNum, project_.timelineFpsDen))},
            {QStringLiteral("speedPercent"),
             100.0 * static_cast<double>(preview.speedNum) /
                 static_cast<double>(preview.speedDen)}};
}

bool MvmController::clipSpeedDurationNeedsOverwrite(const QString& clipId, const QString& input,
                                                     double speedPercent,
                                                     const QString& durationText) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    const auto edit = speedDurationEdit(input, speedPercent, durationText, false, false,
                                        project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit)
        return false;
    return project::previewClipSpeedDuration(project_, id, *edit, project::LinkMode::Linked)
        .overlapsFollowing;
}

bool MvmController::applyClipSpeedDuration(const QString& clipId, const QString& input,
                                            double speedPercent, const QString& durationText,
                                            bool preservePitch, bool ripple, bool overwrite) {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    auto edit = speedDurationEdit(input, speedPercent, durationText, preservePitch, ripple,
                                  project_.timelineFpsNum, project_.timelineFpsDen);
    if (!edit) {
        setStatus(QStringLiteral("速度または尺が不正です"));
        return false;
    }
    edit->overwrite = overwrite;
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::setClipSpeedDuration(candidate, id, *edit, project::LinkMode::Linked);
        }, id, QStringLiteral("clip の速度と尺を変更しました"));
}

bool MvmController::canInsertFrameHold(const QString& clipId) const {
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    if (busy_ || indexOfClipId(project_.timelineClips, id) < 0)
        return false;
    // 判定を別に書かず、複製した Project で実際の挿入を試す。「押せるのに実行すると失敗する」
    // メニューにしない (保持できない frame 等も同じ規則で弾く)。ID も本番と同じ生成器にする。
    // 固定の ID だと、同じ ID の clip を持つ Project でだけ試行が重複で失敗していた。
    project::Project candidate = project_;
    return project::insertFrameHold(
               candidate, id, playheadFrame_,
               project::defaultFrameHoldFrames(candidate.timelineFpsNum, candidate.timelineFpsDen),
               newClipId)
        .success;
}

bool MvmController::insertFrameHoldAtPlayhead(const QString& clipId) {
    if (!canInsertFrameHold(clipId))
        return false;
    const std::string id = clipId.isEmpty() ? currentClipId() : clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::insertFrameHold(
                candidate, id, playheadFrame_,
                project::defaultFrameHoldFrames(candidate.timelineFpsNum, candidate.timelineFpsDen),
                newClipId);
        }, id, QStringLiteral("フレーム保持を挿入しました"));
}

QVariantMap MvmController::previewRateStretch(const QString& clipId, const QString& edge,
                                              qint64 projectFrameDelta, bool linked) const {
    QVariantMap result{{QStringLiteral("delta"), qint64{0}},
                       {QStringLiteral("clips"), QVariantMap{}}};
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else
        return result;
    const auto preview = project::previewRateStretch(project_, clipId.toStdString(), trimEdge,
                                                     projectFrameDelta, linkModeFor(linked));
    if (!preview.success)
        return result;
    QVariantMap clips;
    for (const auto& shown : preview.clips) {
        clips.insert(
            QString::fromStdString(shown.clipId),
            QVariantMap{{QStringLiteral("startDelta"), qint64{shown.startDelta}},
                        {QStringLiteral("endDelta"), qint64{shown.endDelta}},
                        {QStringLiteral("speed"), static_cast<double>(shown.speedNum) /
                                                      static_cast<double>(shown.speedDen)}});
    }
    result.insert(QStringLiteral("delta"), qint64{preview.appliedDelta});
    result.insert(QStringLiteral("clips"), clips);
    return result;
}

qint64 MvmController::clampEdgeDrag(const QString& clipId, const QString& edge, const QString& tool,
                                    qint64 projectFrameDelta, bool linked) const {
    project::TrimEdge trimEdge;
    if (edge == QStringLiteral("left"))
        trimEdge = project::TrimEdge::Left;
    else if (edge == QStringLiteral("right"))
        trimEdge = project::TrimEdge::Right;
    else
        return 0;
    const project::EdgeEditKind kind =
        tool == QStringLiteral("ripple")    ? project::EdgeEditKind::Ripple
        : tool == QStringLiteral("rolling") ? project::EdgeEditKind::Roll
                                            : project::EdgeEditKind::Trim;
    const auto clamped = project::clampEdgeEdit(project_, clipId.toStdString(), trimEdge, kind,
                                                projectFrameDelta, linkModeFor(linked));
    return clamped.success ? clamped.frame : 0;
}

bool MvmController::rippleTrimClip(const QString& clipId, const QString& edge,
                                   qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rippleTrimTimelineClip(candidate, id, trimEdge, projectFrameDelta,
                                                   linkModeFor(linked));
        },
        id, QStringLiteral("clipをリップルトリムしました"));
}

bool MvmController::rollClipEdge(const QString& clipId, const QString& edge,
                                 qint64 projectFrameDelta, bool linked) {
    project::TrimEdge trimEdge;
    if (!resolveTrimEdge(edge, trimEdge))
        return false;
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::rollTimelineEdit(candidate, id, trimEdge, projectFrameDelta,
                                             linkModeFor(linked));
        },
        id, QStringLiteral("編集点をローリングしました"));
}

bool MvmController::slipClip(const QString& clipId, qint64 projectFrameDelta, bool linked) {
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::slipTimelineClip(candidate, id, projectFrameDelta, linkModeFor(linked));
        },
        id, QStringLiteral("clipをスリップしました"));
}

bool MvmController::beginSlipPreview(const QString& clipId, bool linked) {
    if (busy_ || !pauseTimeline())
        return false;
    if (indexOfClipId(project_.timelineClips, clipId.toStdString()) < 0) {
        setStatus(QStringLiteral("スリップするclipがありません"));
        return false;
    }
    endSlipPreview();
    slipPreview_.emplace();
    slipPreview_->clipId = clipId.toStdString();
    slipPreview_->linkMode = linkModeFor(linked);
    return true;
}

qint64 MvmController::previewSlip(qint64 projectFrameDelta) {
    if (!slipPreview_)
        return 0;
    const int index = indexOfClipId(project_.timelineClips, slipPreview_->clipId);
    if (index < 0)
        return 0;
    const auto& original = project_.timelineClips[static_cast<std::size_t>(index)];
    // 素材の端での止め方は確定時と同じ slipTimelineClip に任せる。1 frame も
    // ずらせない (素材の端) 場合は失敗するので、元の in のまま表示する。
    project::Project candidate = project_;
    const bool slipped = project::slipTimelineClip(candidate, slipPreview_->clipId,
                                                   projectFrameDelta, slipPreview_->linkMode)
                             .success;
    const auto& clip =
        slipped ? candidate.timelineClips[static_cast<std::size_t>(index)] : original;
    const std::int64_t sourceDelta = clip.sourceInFrame - original.sourceInFrame;

    // audio clip は timeline の波形だけで示す。preview は video の frame を出す。
    if (clip.kind == project::TimelineClipKind::Audio)
        return sourceDelta;
    if (!project::sourceRateMatchesTimelineRate(project_, clip)) {
        setStatus(timelineFpsText() +
                  QStringLiteral(" ではない素材のclipはスリップ中のPreviewに未対応です"));
        return sourceDelta;
    }
    if (slipPreview_->sourceFrame != clip.sourceInFrame || !slipPreview_->source) {
        slipPreview_->mediaPath = clip.mediaPath;
        slipPreview_->sourceFpsNum = clip.sourceFpsNum;
        slipPreview_->sourceFpsDen = clip.sourceFpsDen;
        slipPreview_->sourceFrameCount = clip.sourceFrameCount;
        slipPreview_->sourceFrame = clip.sourceInFrame;
        slipPreview_->pending = true;
        if (!slipPreviewTimer_.isActive()) {
            applySlipPreview();
            if (slipPreview_ && slipPreview_->pending)
                slipPreviewTimer_.start();
        }
    }
    return sourceDelta;
}

void MvmController::applySlipPreview() {
    if (!slipPreview_ || !slipPreview_->pending) {
        slipPreviewTimer_.stop();
        return;
    }
    const auto state = previewEngine_->status().state;
    if (state == preview::PreviewEngineState::Error) {
        slipPreview_->pending = false;
        slipPreviewTimer_.stop();
        return;
    }
    // addSource / seek は ReadyPaused でしか受理されない。Seeking 中は次の tick へ回す。
    if (state != preview::PreviewEngineState::ReadyPaused)
        return;
    if (!slipPreview_->source) {
        // timeline frame N = 素材 frame N と写す source を 1 つだけ作る。drag 中は
        // seek だけで新しい in の frame を出せ、位置が変わるたびに decoder を開き直さない。
        preview::PreviewSourceDescriptor descriptor;
        descriptor.mediaPath = slipPreview_->mediaPath;
        descriptor.videoEnabled = true;
        descriptor.videoTimelineMappingEnabled = true;
        descriptor.videoSourceInFrame = 0;
        descriptor.videoSourceFrameCount = slipPreview_->sourceFrameCount;
        descriptor.videoTimelineStartFrame = 0;
        descriptor.expectedVideoSourceFrameRate = {
            static_cast<std::uint32_t>(slipPreview_->sourceFpsNum),
            static_cast<std::uint32_t>(slipPreview_->sourceFpsDen)};
        const auto added = previewEngine_->addSource(descriptor);
        if (!added) {
            setStatus(QStringLiteral("スリップのPreviewを準備できません: ") +
                      previewErrorText(added.error()));
            slipPreview_->pending = false;
            slipPreviewTimer_.stop();
            return;
        }
        slipPreview_->source = added.value();
    }
    // スリップ中は clip の素材だけを全面に出す (Premiere のスリップ中のモニタと同じ)。
    auto composition = std::make_shared<preview::CompositionSnapshot>();
    preview::PreviewCompositionLayer layer;
    layer.source = *slipPreview_->source;
    composition->layers.push_back(layer);
    const auto submitted = previewEngine_->submitComposition(composition);
    if (!submitted)
        return;
    submittedComposition_ = composition;
    preview::PreviewFrameRequest request;
    request.outputFrameNumber = slipPreview_->sourceFrame;
    request.sources.push_back({*slipPreview_->source, slipPreview_->sourceFrame});
    if (!previewEngine_->seekFrameRequest(request))
        return;
    slipPreview_->pending = false;
    statusText_ = QStringLiteral("スリップ: 新しいイン点は素材の ") +
                  QString::number(slipPreview_->sourceFrame) + QStringLiteral(" frame目です");
    Q_EMIT stateChanged();
}

void MvmController::endSlipPreview() {
    if (!slipPreview_)
        return;
    slipPreviewTimer_.stop();
    const std::optional<preview::PreviewSourceId> source = slipPreview_->source;
    slipPreview_.reset();
    if (!source)
        return;
    // 先に通常の composition へ戻し、slip 用 source を参照から外してから削除する。
    // まだ参照中で拒否されたら retirement queue に回す。
    seekTimelineFrame(playheadFrame_);
    if (!previewEngine_->removeSource(*source))
        retiredSources_.push_back(*source);
}

qint64 MvmController::clampSlideDrag(const QString& clipId, qint64 projectFrameDelta,
                                     bool linked) const {
    const auto clamped = project::clampSlideEdit(project_, clipId.toStdString(), projectFrameDelta,
                                                 linkModeFor(linked));
    return clamped.success ? clamped.frame : 0;
}

bool MvmController::slideClip(const QString& clipId, qint64 projectFrameDelta, bool linked) {
    const std::string id = clipId.toStdString();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::slideTimelineClip(candidate, id, projectFrameDelta,
                                              linkModeFor(linked));
        },
        id, QStringLiteral("clipをスライドしました"));
}

bool MvmController::splitClipAt(const QString& clipId, qint64 frame, bool allTracks, bool linked) {
    const std::string id = clipId.toStdString();
    const std::vector<std::string> clipIds =
        allTracks ? project::clipIdsSpanningFrame(project_, frame) : std::vector<std::string>{id};
    if (clipIds.empty()) {
        setStatus(QStringLiteral("分割位置にclipがありません"));
        return false;
    }
    const QString status =
        allTracks ? QString::number(clipIds.size()) + QStringLiteral("個のclipを分割しました")
                  : QStringLiteral("clipを分割しました");
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::splitTimelineClips(candidate, clipIds, frame, newClipId,
                                               linkModeFor(linked));
        },
        allTracks ? currentClipId() : id, status);
}

bool MvmController::splitSelectionAtPlayhead() {
    const qint64 frame = playheadFrame_;
    std::vector<std::string> clipIds =
        project::clipIdsSpanningFrame(project_, frame, selectedClipIds_);
    if (clipIds.empty()) {
        const std::string current = currentClipId();
        if (!current.empty())
            clipIds = project::clipIdsSpanningFrame(project_, frame, {current});
    }
    if (clipIds.empty()) {
        setStatus(QStringLiteral("再生ヘッド位置に分割できる選択clipがありません"));
        return false;
    }
    const std::string selectedId = clipIds.front();
    return applyTimelineEdit(
        [&](project::Project& candidate) {
            return project::splitTimelineClips(candidate, clipIds, frame, newClipId,
                                               project::LinkMode::Linked);
        },
        selectedId, QStringLiteral("再生ヘッド位置でclipを分割しました"));
}

bool MvmController::selectClipsFromFrame(qint64 frame, const QString& direction,
                                         const QString& trackKind, int trackIndex) {
    project::SelectDirection selectDirection;
    if (direction == QStringLiteral("forward"))
        selectDirection = project::SelectDirection::Forward;
    else if (direction == QStringLiteral("backward"))
        selectDirection = project::SelectDirection::Backward;
    else {
        setStatus(QStringLiteral("選択方向が不正です"));
        return false;
    }
    // trackKind が空なら全 track を対象にする。
    std::optional<project::TrackRef> track;
    if (!trackKind.isEmpty()) {
        project::TrackRef resolved;
        if (!resolveTrackRef(trackKind, trackIndex, resolved)) {
            setStatus(QStringLiteral("trackが不正です"));
            return false;
        }
        track = resolved;
    }
    const auto ids =
        project::clipIdsFromFrame(project_, std::max<qint64>(0, frame), selectDirection, track);
    setTimelineSelection(ids);
    if (ids.empty()) {
        setCurrentClipSelection(-1);
        setStatus(QStringLiteral("選択できるclipがありません"));
        return true;
    }
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, ids.front()));
    setStatus(QString::number(selectedClipIds_.size()) + QStringLiteral("個のclipを選択しました"));
    return true;
}

bool MvmController::deleteCurrentClip() {
    if (busy_)
        return false;
    if (!pauseTimeline())
        return false;

    const std::string currentId = currentClipId();
    if (currentId.empty()) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }
    std::vector<std::string> deletedIds = selectedClipIds_;
    if (std::find(deletedIds.begin(), deletedIds.end(), currentId) == deletedIds.end())
        deletedIds = {currentId};

    project::Project candidate = project_;
    const int firstDeletedIndex = currentClipIndex_;
    int deletedCount = 0;
    for (const auto& id : deletedIds) {
        const int index = indexOfClipId(candidate.timelineClips, id);
        if (index < 0)
            continue; // link相手の削除で同時に消えたclip。
        const std::size_t before = candidate.timelineClips.size();
        const project::TimelineEditResult deleted = project::deleteTimelineClip(candidate, index);
        if (!deleted.success) {
            setStatus(QString::fromStdString(deleted.error));
            return false;
        }
        deletedCount += static_cast<int>(before - candidate.timelineClips.size());
    }
    if (deletedCount == 0) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }

    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;

    const QString resetFailure = resetAfterClipRemoval();
    if (!resetFailure.isEmpty()) {
        setStatus(QStringLiteral("clipは削除しましたが、") + resetFailure);
        return true;
    }

    if (!project_.timelineClips.empty()) {
        const int nextIndex =
            std::min(firstDeletedIndex, static_cast<int>(project_.timelineClips.size()) - 1);
        if (!selectClip(nextIndex)) {
            const QString previewFailure = statusText_;
            setStatus(QString::number(deletedCount) +
                      QStringLiteral("個のclipは削除しましたが、次のclipをPreviewできません: ") +
                      previewFailure);
        } else {
            setStatus(QString::number(deletedCount) + QStringLiteral("個のclipを削除しました"));
        }
        return true;
    }

    setStatus(QString::number(deletedCount) +
              QStringLiteral("個のclipを削除し、timelineが空になりました"));
    return true;
}

QString MvmController::resetAfterClipRemoval() {
    pendingVideoPath_.reset();
    pendingClipName_.clear();
    pendingClipIndex_ = -1;
    pendingSourceFrame_ = 0;
    const bool previewReset = resetPreviewEngine();
    const QString resetFailure = previewReset ? QString() : statusText_;
    currentSource_.reset();
    currentClipName_.clear();
    currentClipPath_.clear();
    currentClipIndex_ = -1;
    std::erase_if(selectedClipIds_, [&](const std::string& id) {
        return indexOfClipId(project_.timelineClips, id) < 0;
    });
    Q_EMIT stateChanged();
    return resetFailure;
}

bool MvmController::deleteTimelineClip(const QString& clipId) {
    const int index = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (index < 0) {
        setStatus(QStringLiteral("削除するclipがありません"));
        return false;
    }
    setCurrentClipSelection(index);
    return deleteCurrentClip();
}

bool MvmController::unlinkTimelineClip(const QString& clipId) {
    if (busy_ || !pauseTimeline())
        return false;
    project::Project candidate = project_;
    const auto unlinked = project::unlinkTimelineClip(candidate, clipId.toStdString());
    if (!unlinked.success) {
        setStatus(QString::fromStdString(unlinked.error));
        return false;
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    setTimelineSelection({clipId.toStdString()});
    return refreshPreviewAfterSavedEdit(clipId.toStdString(),
                                        QStringLiteral("clipのリンクを解除しました"));
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
    if (!pauseTimeline())
        return false;
    if (from.empty()) {
        setStatus(redo ? QStringLiteral("やり直せる編集がありません")
                       : QStringLiteral("元に戻せる編集がありません"));
        return false;
    }

    const UndoEntry& entry = from.back();
    const auto serialized = project::serializeProjectJson(entry.project, projectPath_);
    if (!serialized.success) {
        setStatus(failurePrefix + QString::fromStdString(serialized.error));
        return false;
    }

    // 戻した先から逆向きに辿れるよう、いまの状態を反対側の履歴へ積む。
    UndoEntry current{project_, selectedClipIds_, currentClipId(), playheadFrame_,
                      currentRevision_};
    const std::vector<std::string> previousSelection = entry.selectedClipIds;
    const std::string previousCurrentClipId = entry.currentClipId;
    project_ = entry.project;
    playheadFrame_ = entry.playheadFrame;
    currentRevision_ = entry.revision;
    from.pop_back();
    to.push_back(std::move(current));

    selectedClipIds_.clear();
    for (const auto& id : previousSelection) {
        if (indexOfClipId(project_.timelineClips, id) >= 0)
            selectedClipIds_.push_back(id);
    }
    refreshTimelineModel();
    scheduleRecoveryAutosave();
    setCurrentClipSelection(indexOfClipId(project_.timelineClips, previousCurrentClipId));

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
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
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
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
        return false;
    // index の対応が変わったので cache を捨ててから現在位置で組み直す。
    trackSources_.clear();
    audioSources_.clear();
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
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
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

bool MvmController::hasClipAt(const QString& trackKind, int trackIndex, qint64 frame) const {
    project::TrackRef track;
    if (!resolveTrackRef(trackKind, trackIndex, track))
        return false;
    return project::timelineClipIndexAt(project_, track, frame) >= 0;
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
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("Projectを更新できません: ")))
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
    if (!pauseTimeline())
        return false;
    if (project_.outputWidth == width && project_.outputHeight == height &&
        project_.timelineFpsNum == fpsNum && project_.timelineFpsDen == fpsDen) {
        setStatus(QStringLiteral("Project設定は変更されていません"));
        return true;
    }

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

QVariantMap MvmController::exportSettingsSummary() const {
    struct MediaSpec {
        int width = 0;
        int height = 0;
        std::int64_t fpsNum = 0;
        std::int64_t fpsDen = 1;
        int sarNum = 1;
        int sarDen = 1;
    };

    const auto fpsText = [](std::int64_t numerator, std::int64_t denominator) {
        const double fps = static_cast<double>(numerator) / static_cast<double>(denominator);
        return QString::number(fps, 'f', denominator == 1 ? 0 : 2) + QStringLiteral(" fps");
    };
    const auto specText = [&](const MediaSpec& spec) {
        QString text = QString::number(spec.width) + QStringLiteral("×") +
                       QString::number(spec.height) + QStringLiteral(" / ") +
                       fpsText(spec.fpsNum, spec.fpsDen);
        if (spec.sarNum != 1 || spec.sarDen != 1)
            text += QStringLiteral(" / SAR ") + QString::number(spec.sarNum) + QStringLiteral(":") +
                    QString::number(spec.sarDen);
        return text;
    };

    std::set<std::filesystem::path> probedPaths;
    std::set<std::tuple<int, int, std::int64_t, std::int64_t, int, int>> distinctSpecs;
    std::vector<MediaSpec> specs;
    int failedProbeCount = 0;
    int videoClipCount = 0;
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind == project::TimelineClipKind::Audio)
            continue;
        ++videoClipCount;
        if (!probedPaths.insert(clip.mediaPath).second)
            continue;
        const ProbedMedia media = probeMedia(clip.mediaPath);
        if (!media.success) {
            ++failedProbeCount;
            continue;
        }
        const auto key = std::make_tuple(media.width, media.height, media.fpsNum, media.fpsDen,
                                         media.sarNum, media.sarDen);
        if (distinctSpecs.insert(key).second) {
            specs.push_back({media.width, media.height, media.fpsNum, media.fpsDen, media.sarNum,
                             media.sarDen});
        }
    }

    QVariantMap summary;
    summary.insert(QStringLiteral("outputText"),
                   QStringLiteral("出力: ") + QString::number(project_.outputWidth) +
                       QStringLiteral("×") + QString::number(project_.outputHeight) +
                       QStringLiteral(" / ") +
                       fpsText(project_.timelineFpsNum, project_.timelineFpsDen));

    if (videoClipCount == 0) {
        summary.insert(QStringLiteral("inputText"), QStringLiteral("入力映像: なし"));
    } else if (specs.empty()) {
        summary.insert(QStringLiteral("inputText"),
                       QStringLiteral("入力映像: 仕様を取得できません"));
    } else if (specs.size() == 1) {
        summary.insert(QStringLiteral("inputText"),
                       QStringLiteral("入力映像: ") + specText(specs.front()));
    } else {
        QStringList descriptions;
        constexpr std::size_t maxShownSpecs = 3;
        for (std::size_t index = 0; index < std::min(specs.size(), maxShownSpecs); ++index)
            descriptions.push_back(specText(specs[index]));
        if (specs.size() > maxShownSpecs)
            descriptions.push_back(QStringLiteral("ほか%1仕様").arg(specs.size() - maxShownSpecs));
        summary.insert(QStringLiteral("inputText"), QStringLiteral("入力映像（複数仕様）: ") +
                                                        descriptions.join(QStringLiteral("、")));
    }

    bool increasesRaster = false;
    bool increasesFrameRate = false;
    for (const auto& spec : specs) {
        increasesRaster = increasesRaster || spec.width < project_.outputWidth ||
                          spec.height < project_.outputHeight;
        increasesFrameRate = increasesFrameRate || spec.fpsNum * project_.timelineFpsDen <
                                                       project_.timelineFpsNum * spec.fpsDen;
    }

    QStringList warnings;
    if (specs.size() > 1)
        warnings.push_back(QStringLiteral("入力映像の仕様が混在しています。"));
    if (increasesRaster)
        warnings.push_back(QStringLiteral("一部の入力映像を拡大して書き出します。"));
    if (increasesFrameRate)
        warnings.push_back(
            QStringLiteral("一部の入力映像より高いfpsで書き出しますが、動きの情報は増えません。"));
    if (increasesRaster || increasesFrameRate)
        warnings.push_back(QStringLiteral("出力の画素数やフレーム数が増えるため、元ファイルより容量"
                                          "が大きくなる場合があります。"));
    if (failedProbeCount > 0)
        warnings.push_back(QStringLiteral("%1件の入力ファイルは仕様を確認できませんでした。")
                               .arg(failedProbeCount));
    if (videoClipCount == 0)
        warnings.push_back(QStringLiteral("映像クリップがないため、映像は黒になります。"));
    summary.insert(QStringLiteral("warningText"), warnings.join(QStringLiteral("\n")));
    return summary;
}

bool MvmController::exportTimeline(const QUrl& outputUrl) {
    return startTimelineExport(outputUrl, 23);
}

bool MvmController::exportTimelineWithQuality(const QUrl& outputUrl, const QString& quality) {
    int videoCrf = 0;
    if (quality == QStringLiteral("high"))
        videoCrf = 18;
    else if (quality == QStringLiteral("standard"))
        videoCrf = 23;
    else if (quality == QStringLiteral("compact"))
        videoCrf = 28;
    else {
        reportExportFailure(QStringLiteral("未知の書き出し品質です: ") + quality);
        return false;
    }
    return startTimelineExport(outputUrl, videoCrf);
}

bool MvmController::startTimelineExport(const QUrl& outputUrl, int videoCrf) {
    if (busy_) {
        reportExportFailure(QStringLiteral("別の処理中のため書き出しを開始できません"));
        return false;
    }
    if (!pauseTimeline()) {
        reportExportFailure(statusText_);
        return false;
    }
    if (project_.timelineClips.empty()) {
        reportExportFailure(QStringLiteral("書き出すclipがありません"));
        return false;
    }
    if (!outputUrl.isLocalFile()) {
        reportExportFailure(QStringLiteral("ローカルの書き出し先を指定してください"));
        return false;
    }

    TimelineExportRequest request;
    request.outputPath = std::filesystem::path(outputUrl.toLocalFile().toStdWString());
    request.width = project_.outputWidth;
    request.height = project_.outputHeight;
    request.fpsNum = static_cast<int>(project_.timelineFpsNum);
    request.fpsDen = static_cast<int>(project_.timelineFpsDen);
    request.videoCrf = videoCrf;
    request.renderThreads = 4;
    request.encoderThreads = 0;

    if (exportThread_.joinable())
        exportThread_.join();
    exportCancelRequested_.store(false, std::memory_order_release);
    exporting_ = true;
    exportCancelling_ = false;
    exportProgress_ = 0.0;
    exportProgressText_ = QStringLiteral("準備しています…");
    exportBaselineFrame_ = -1;
    busy_ = true;
    statusText_ = QStringLiteral("書き出しています…");
    Q_EMIT stateChanged();

    auto lastReportedFrame = std::make_shared<std::atomic<long long>>(-1);
    request.progress = [this, lastReportedFrame](long long completed, long long total) {
        const bool cancelled = exportCancelRequested_.load(std::memory_order_acquire);
        const long long previous =
            lastReportedFrame->exchange(completed, std::memory_order_relaxed);
        if (completed != previous) {
            // 観測時刻はworker側で取る。queued eventの配送遅延をETAへ混ぜない。
            const auto observedAt = std::chrono::steady_clock::now();
            QMetaObject::invokeMethod(
                this,
                [this, completed, total, observedAt] {
                    if (!exporting_ || exportCancelling_ ||
                        exportCancelRequested_.load(std::memory_order_acquire) || total <= 0)
                        return;
                    if (exportBaselineFrame_ < 0) {
                        exportBaselineFrame_ = completed;
                        exportBaselineTime_ = observedAt;
                    }
                    exportProgress_ = std::clamp(
                        static_cast<double>(completed) / static_cast<double>(total), 0.0, 1.0);
                    const auto remaining = core::estimateExportRemainingSeconds(
                        exportBaselineFrame_, completed, total,
                        std::chrono::duration<double>(observedAt - exportBaselineTime_).count());
                    exportProgressText_ =
                        QString::number(completed) + QStringLiteral(" / ") +
                        QString::number(total) + QStringLiteral(" frame ・ ") +
                        (remaining ? QString::fromStdString(core::formatExportRemaining(*remaining))
                                   : QStringLiteral("残り時間を計算中…"));
                    Q_EMIT stateChanged();
                },
                Qt::QueuedConnection);
        }
        return cancelled;
    };

    const project::Project exportProject = project_;
    try {
        exportThread_ =
            exportThreadFactory_([this, exportProject, request = std::move(request)]() mutable {
                TimelineExportResult exported = exportRunner_(exportProject, request);
                QMetaObject::invokeMethod(
                    this,
                    [this, exported = std::move(exported)]() mutable {
                        if (shutdownStarted_)
                            return;
                        finishTimelineExport(std::move(exported));
                    },
                    Qt::QueuedConnection);
            });
    } catch (const std::system_error& error) {
        exporting_ = false;
        exportCancelling_ = false;
        busy_ = false;
        reportExportFailure(QStringLiteral("書き出しworkerを開始できません: ") +
                            QString::fromLocal8Bit(error.what()));
        return false;
    }
    return true;
}

void MvmController::cancelTimelineExport() {
    if (!exporting_ || exportCancelling_)
        return;
    exportCancelRequested_.store(true, std::memory_order_release);
    exportCancelling_ = true;
    exportProgressText_ = QStringLiteral("キャンセルしています…");
    setStatus(exportProgressText_);
}

void MvmController::finishTimelineExport(TimelineExportResult exported) {
    if (exportThread_.joinable())
        exportThread_.join();
    exporting_ = false;
    exportCancelling_ = false;
    busy_ = false;
    if (!exported.success) {
        if (exported.cancelled)
            setStatus(QStringLiteral("書き出しをキャンセルしました"));
        else
            reportExportFailure(QStringLiteral("書き出しに失敗しました: ") +
                                QString::fromStdString(exported.error));
        return;
    }
    exportProgress_ = 1.0;
    exportProgressText_ = QStringLiteral("完了");
    QString status = QStringLiteral("書き出しました: ") + fromPath(exported.outputPath) +
                     QStringLiteral(" (") + QString::number(exported.frameCount) +
                     QStringLiteral(" frame / ") + QString::number(exported.durationSec, 'f', 2) +
                     QStringLiteral(" 秒)");
    // Explorer表示の失敗は書き出しの失敗ではない。成功は保ったまま理由を併記する。
    QString revealError;
    if (!fileRevealer_(exported.outputPath, revealError))
        status += QStringLiteral(" / Explorerで表示できません: ") + revealError;
    setStatus(status);
}

QVariantMap MvmController::previewClipKey(const QString& clipId, qint64 originalFrame,
                                          qint64 requestedFrame, double valuePercent) const {
    const auto id = clipId.toStdString();
    const auto found = std::find_if(project_.timelineClips.begin(), project_.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == project_.timelineClips.end())
        return {{QStringLiteral("success"), false}};
    const bool audio = found->kind == project::TimelineClipKind::Audio;
    const auto preview = project::previewClipKeyEdit(
        project_, id, audio ? project::ClipKeyKind::Volume : project::ClipKeyKind::Opacity,
        originalFrame, requestedFrame, valuePercent);
    QVariantList keys;
    if (preview.success) {
        const auto& source = audio ? preview.effects.volumeKeys : preview.effects.opacityKeys;
        for (const auto& key : source)
            keys.append(QVariantMap{{QStringLiteral("frame"), key.frame},
                                    {QStringLiteral("value"), key.valuePercent}});
    }
    return {{QStringLiteral("success"), preview.success},
            {QStringLiteral("frame"), preview.frame},
            {QStringLiteral("keys"), keys},
            {QStringLiteral("error"), QString::fromStdString(preview.error)}};
}

bool MvmController::commitClipKey(const QString& clipId, qint64 originalFrame,
                                  qint64 requestedFrame, double valuePercent) {
    if (busy_ || !pauseTimeline())
        return false;
    const auto id = clipId.toStdString();
    const auto found = std::find_if(project_.timelineClips.begin(), project_.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == project_.timelineClips.end()) {
        setStatus(QStringLiteral("キーフレームを編集するclipがありません"));
        return false;
    }
    project::Project candidate = project_;
    const auto edited = project::editClipKey(candidate, id,
                                             found->kind == project::TimelineClipKind::Audio
                                                 ? project::ClipKeyKind::Volume
                                                 : project::ClipKeyKind::Opacity,
                                             originalFrame, requestedFrame, valuePercent);
    if (!edited.success) {
        setStatus(QString::fromStdString(edited.error));
        return false;
    }
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::deleteClipKey(const QString& clipId, qint64 frame) {
    if (busy_ || !pauseTimeline())
        return false;
    const auto id = clipId.toStdString();
    const auto found = std::find_if(project_.timelineClips.begin(), project_.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == project_.timelineClips.end()) {
        setStatus(QStringLiteral("キーフレームを削除するclipがありません"));
        return false;
    }
    project::Project candidate = project_;
    const auto deleted = project::deleteClipKey(candidate, id,
                                                found->kind == project::TimelineClipKind::Audio
                                                    ? project::ClipKeyKind::Volume
                                                    : project::ClipKeyKind::Opacity,
                                                frame);
    if (!deleted.success) {
        setStatus(QString::fromStdString(deleted.error));
        return false;
    }
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::commitClipKeyCandidate(project::Project candidate) {
    if (candidate == project_)
        return true;
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("キーフレームを保存できません: ")))
        return false;
    QString previewError;
    if (!refreshPreviewAtPlayhead(previewError))
        setStatus(QStringLiteral("キーフレームのPreview更新に失敗しました: ") + previewError);
    return true;
}

bool MvmController::setEffectValue(const QString& key, double value, bool commit) {
    return setEffectValues({{key, value}}, commit);
}

bool MvmController::setEffectValues(const QVariantMap& values, bool commit) {
    return setClipEffectValues(QString::fromStdString(currentClipId()), values, commit);
}

bool MvmController::setClipEffectValues(const QString& clipId, const QVariantMap& values,
                                        bool commit) {
    const int clipIndex = indexOfClipId(project_.timelineClips, clipId.toStdString());
    if (busy_ || clipIndex < 0) {
        setStatus(QStringLiteral("effectを適用するclipがありません"));
        return false;
    }
    if (!pauseTimeline())
        return false;

    if (values.isEmpty()) {
        setStatus(QStringLiteral("変更するeffect項目がありません"));
        return false;
    }
    project::ClipEffects candidateEffects = effectsForPreview(clipIndex);
    // 複数の項目 (位置と拡大率など) を 1 つの変更として検証し、1 つの undo にする。
    for (auto entry = values.cbegin(); entry != values.cend(); ++entry) {
        bool numeric = false;
        const double value = entry.value().toDouble(&numeric);
        if (!numeric || !applyEffectKey(candidateEffects, entry.key(), value)) {
            setStatus(QStringLiteral("未知または数値でないeffect項目です: ") + entry.key());
            return false;
        }
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
        if (!refreshPreviewAtPlayhead(previewError))
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
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("effectを更新できません: ")))
        return false;
    previewEffectsOverride_.reset();
    previewEffectsClipIndex_ = -1;
    Q_EMIT stateChanged();

    QString previewError;
    if (!refreshPreviewAtPlayhead(previewError)) {
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
    if (!refreshPreviewAtPlayhead(previewError)) {
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
    if (projectLockHeld_ && dirty())
        writeRecoveryAutosave();
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
