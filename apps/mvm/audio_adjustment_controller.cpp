#include "mvm_controller.h"
#include "shuttle_audio_playback.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace mvm::app {
namespace {
project::AudioAdjustmentSettings targetSettings(const std::string& text) {
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    project::AudioAdjustmentSettings settings;
    settings.bgmTrack = object.value("bgmTrack").toInt(-1);
    for (const auto& value : object.value("voiceTracks").toArray())
        settings.voiceTracks.push_back(value.toInt(-1));
    return settings;
}

// 解析入力の射影が依存するのは対象 track の集合だけ。BGM・声の役割や目標値が違う設定も、
// 対象 track の和集合が同じなら同じ key になる。
std::string targetScopeKey(const project::AudioAdjustmentSettings& settings) {
    auto tracks = settings.voiceTracks;
    tracks.push_back(settings.bgmTrack);
    std::sort(tracks.begin(), tracks.end());
    tracks.erase(std::unique(tracks.begin(), tracks.end()), tracks.end());
    std::string key;
    for (int track : tracks)
        key += std::to_string(track) + ",";
    return key;
}

std::filesystem::path pathFromIdentityKey(const std::string& key) {
    return std::filesystem::path(QString::fromUtf8(key).toStdWString());
}
} // namespace

bool MvmController::audioAdjustmentNeedsRegeneration() const {
    return audioAdjustmentNeedsRegeneration_;
}

void MvmController::connectAudioFileWatch() {
    connect(&audioFileWatcher_, &QFileSystemWatcher::fileChanged, this,
            [this](const QString& path) {
                if (shutdownStarted_)
                    return;
                invalidateAudioFile(path.toUtf8().toStdString());
                updateAudioAdjustmentRegeneration(true);
                emit audioAdjustmentChanged();
                if (!audioFileWatcher_.files().contains(path))
                    audioFileWatcher_.addPath(path);
                ensureAudioAdjustmentTimer();
            });
}

void MvmController::invalidateAudioFile(const std::string& key) {
    // 内容が変わったかもしれない素材。size と更新時刻が同じでも保存済み SHA-256 を信用しない。
    audioFileCache_[key].hashed = false;
    audioContentDirty_ = true;
    if (audioAdjustmentResult_ &&
        std::any_of(audioAdjustmentResult_->files.begin(), audioAdjustmentResult_->files.end(),
                    [&](const auto& file) { return file.key == key; }))
        audioContentConfirmed_ = false;
}

void MvmController::refreshAudioInputAuthority(bool notify) {
    // ファイル内容はここでは読まない。音声入力の射影が同じ編集でも、保存した fingerprint は
    // 変わるので再生成フラグだけ更新する。
    // 射影は全 clip を走査するので、対象 track の集合ごとに 1 回だけ計算する。
    std::map<std::string, std::string> byScope;
    const auto projection = [&](const project::AudioAdjustmentSettings& settings) {
        auto key = targetScopeKey(settings);
        auto found = byScope.find(key);
        if (found == byScope.end())
            found = byScope.emplace(std::move(key), audioProjectionHash(project_, settings)).first;
        return found->second;
    };
    audioProjectionHash_ = projection(audioAuthoritySettings_);
    audioSavedProjectionHashes_.clear();
    for (const auto& clip : project_.timelineClips)
        if (!clip.effects.audioAdjustmentFingerprint.empty() &&
            !audioSavedProjectionHashes_.contains(clip.effects.audioAdjustmentSettings))
            audioSavedProjectionHashes_[clip.effects.audioAdjustmentSettings] =
                projection(targetSettings(clip.effects.audioAdjustmentSettings));
    syncAudioFileWatch();
    for (const auto& path : audioWatchPaths())
        if (const auto cached = audioFileCache_.find(path.toUtf8().toStdString());
            cached == audioFileCache_.end() || !cached->second.hashed)
            audioContentDirty_ = true;
    updateAudioAdjustmentRegeneration(notify);
    // 待機中の変更検知は watcher が担う。取りこぼし用の確認だけを低頻度で回す。
    if (shutdownStarted_) {
        audioWatchFallbackTimer_.stop();
    } else if (projectHasAudioAdjustmentFingerprint()) {
        if (!audioWatchFallbackTimer_.isActive())
            audioWatchFallbackTimer_.start(5000);
    } else {
        audioWatchFallbackTimer_.stop();
    }
    if (audioContentDirty_ || audioAdjustmentResult_ || audioAdjustmentJob_)
        ensureAudioAdjustmentTimer();
}

void MvmController::checkAudioWatchFallback() {
    if (shutdownStarted_)
        return;
    // QFileSystemWatcher は削除・rename で path を外し、作り直された素材を通知しない。
    // 外れていた path は syncAudioFileWatch が再登録時に内容照合へ回す。
    syncAudioFileWatch();
    refreshAudioFileCacheCheap();
    if (audioContentDirty_)
        ensureAudioAdjustmentTimer();
}

void MvmController::updateAudioAdjustmentRegeneration(bool notify) {
    const bool need = computeAudioAdjustmentNeedsRegeneration();
    if (need == audioAdjustmentNeedsRegeneration_)
        return;
    audioAdjustmentNeedsRegeneration_ = need;
    if (notify)
        emit stateChanged();
}

bool MvmController::projectHasAudioAdjustmentFingerprint() const {
    for (const auto& clip : project_.timelineClips)
        if (!clip.effects.audioAdjustmentFingerprint.empty())
            return true;
    return false;
}

bool MvmController::computeAudioAdjustmentNeedsRegeneration() const {
    for (const auto& clip : project_.timelineClips) {
        if (clip.effects.audioAdjustmentFingerprint.empty())
            continue;
        std::string projection;
        std::vector<AudioFileIdentity> files;
        const auto savedProjection =
            audioSavedProjectionHashes_.find(clip.effects.audioAdjustmentSettings);
        if (!parseAudioInputFingerprint(clip.effects.audioAdjustmentFingerprint, projection,
                                        files) ||
            savedProjection == audioSavedProjectionHashes_.end() ||
            projection != savedProjection->second)
            return true;
        for (const auto& file : files) {
            const auto cached = audioFileCache_.find(file.key);
            if (cached == audioFileCache_.end() || !cached->second.hashed)
                return true;
            if (!cached->second.exists || cached->second.size != file.size ||
                cached->second.mtime100ns != file.mtime100ns ||
                cached->second.contentSha256 != file.contentSha256)
                return true;
        }
    }
    return false;
}

bool MvmController::audioCheapIdentityMatches(const std::vector<AudioFileIdentity>& files) const {
    if (files.empty())
        return false;
    for (const auto& file : files) {
        const auto cached = audioFileCache_.find(file.key);
        if (cached == audioFileCache_.end() || !cached->second.exists ||
            cached->second.size != file.size || cached->second.mtime100ns != file.mtime100ns)
            return false;
    }
    return true;
}

QStringList MvmController::audioWatchPaths() const {
    QStringList paths;
    const auto add = [&](const std::string& key) {
        const auto path = QString::fromUtf8(key);
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    };
    if (audioAdjustmentResult_)
        for (const auto& file : audioAdjustmentResult_->files)
            add(file.key);
    for (const auto& clip : project_.timelineClips) {
        std::string projection;
        std::vector<AudioFileIdentity> files;
        if (parseAudioInputFingerprint(clip.effects.audioAdjustmentFingerprint, projection, files))
            for (const auto& file : files)
                add(file.key);
    }
    return paths;
}

void MvmController::syncAudioFileWatch() {
    if (shutdownStarted_)
        return;
    const auto wanted = audioWatchPaths();
    const QSet<QString> wantedSet(wanted.begin(), wanted.end());
    const auto list = audioFileWatcher_.files();
    const QSet<QString> current(list.begin(), list.end());
    for (const auto& path : current)
        if (!wantedSet.contains(path)) {
            audioFileWatcher_.removePath(path);
            audioWatchedPaths_.remove(path);
        }
    for (auto watched = audioWatchedPaths_.begin(); watched != audioWatchedPaths_.end();)
        watched = wantedSet.contains(*watched) ? std::next(watched)
                                               : audioWatchedPaths_.erase(watched);
    bool invalidated = false;
    for (const auto& path : wanted) {
        if (current.contains(path) || !audioFileWatcher_.addPath(path))
            continue;
        // 監視が外れていた間の変更は通知されない。置き換えた素材が同じ size・更新時刻でも
        // 古い SHA-256 を使わないよう、再登録した素材は内容を照合し直す。
        if (audioWatchedPaths_.contains(path)) {
            invalidateAudioFile(path.toUtf8().toStdString());
            invalidated = true;
        }
        audioWatchedPaths_.insert(path);
    }
    if (invalidated) {
        updateAudioAdjustmentRegeneration(true);
        ensureAudioAdjustmentTimer();
    }
}

void MvmController::dropAudioFileWatchForTest() {
    const auto paths = audioFileWatcher_.files();
    if (!paths.isEmpty())
        audioFileWatcher_.removePaths(paths);
}

void MvmController::ensureAudioAdjustmentTimer() {
    if (shutdownStarted_)
        return;
    if (!audioAdjustmentTimer_.isActive())
        audioAdjustmentTimer_.start(100);
}

void MvmController::reapAudioAdjustmentJobs() {
    std::erase_if(audioAdjustmentRetired_,
                  [](const std::unique_ptr<AudioAdjustmentJob>& job) { return job->ready(); });
    std::erase_if(audioContentRetired_,
                  [](const std::unique_ptr<AudioContentHashJob>& job) { return job->ready(); });
}

void MvmController::refreshAudioFileCacheCheap() {
    for (const auto& path : audioWatchPaths()) {
        const auto key = path.toUtf8().toStdString();
        AudioFileIdentity now;
        std::string error;
        auto& cached = audioFileCache_[key];
        if (!inspectAudioFile(pathFromIdentityKey(key), now, false, nullptr, error)) {
            cached.key = key;
            cached.exists = false;
            cached.hashed = false;
            continue;
        }
        const bool same = cached.exists && cached.size == now.size &&
                          cached.mtime100ns == now.mtime100ns && cached.key == now.key;
        const auto sha = cached.contentSha256;
        const bool hashed = cached.hashed;
        cached = now;
        if (same) {
            cached.contentSha256 = sha;
            cached.hashed = hashed;
        } else {
            audioContentDirty_ = true;
            audioContentConfirmed_ = false;
        }
    }
    updateAudioAdjustmentRegeneration(true);
}

void MvmController::scheduleAudioContentRecheck() {
    if (audioContentJob_ || !audioContentDirty_)
        return;
    std::vector<std::filesystem::path> paths;
    if (audioAdjustmentResult_) {
        for (const auto& file : audioAdjustmentResult_->files) {
            paths.push_back(pathFromIdentityKey(file.key));
        }
    } else if (projectHasAudioAdjustmentFingerprint()) {
        for (const auto& path : audioWatchPaths())
            paths.push_back(std::filesystem::path(path.toStdWString()));
    }
    audioContentDirty_ = false;
    if (paths.empty())
        return;
    audioContentConfirmed_ = false;
    audioContentJob_ = std::make_unique<AudioContentHashJob>(std::move(paths));
    ensureAudioAdjustmentTimer();
}

void MvmController::finishAudioContentRecheck() {
    if (!audioContentJob_ || !audioContentJob_->ready())
        return;
    const auto hashed = audioContentJob_->take();
    audioContentJob_.reset();
    if (hashed.status == AudioContentHashResult::Status::Cancelled)
        return;
    bool mismatch = hashed.status != AudioContentHashResult::Status::Complete;
    for (const auto& file : hashed.files)
        audioFileCache_[file.key] = file;
    if (audioAdjustmentResult_) {
        if (hashed.files.size() != audioAdjustmentResult_->files.size())
            mismatch = true;
        for (const auto& expected : audioAdjustmentResult_->files) {
            const auto found =
                std::find_if(hashed.files.begin(), hashed.files.end(),
                             [&](const auto& file) { return file.key == expected.key; });
            if (found == hashed.files.end() || found->contentSha256 != expected.contentSha256 ||
                found->size != expected.size || found->mtime100ns != expected.mtime100ns)
                mismatch = true;
        }
    }
    updateAudioAdjustmentRegeneration(true);
    if (mismatch)
        dropAudioAdjustmentResult(
            QStringLiteral("解析後に素材の内容が変わりました。再解析してください"));
    else if (audioAdjustmentResult_ && !audioContentDirty_) {
        audioContentConfirmed_ = true;
        if (audioApplyPending_) {
            audioApplyPending_ = false;
            if (!canApplyAudioAdjustment()) {
                dropAudioAdjustmentResult(
                    QStringLiteral("最終照合中に状態が変わりました。再解析してください"));
            } else if (commitAudioAdjustment())
                emit audioAdjustmentApplied();
        }
    } else if (audioApplyPending_) {
        dropAudioAdjustmentResult(
            QStringLiteral("最終照合中に素材が変更されました。再解析してください"));
    }
    emit audioAdjustmentChanged();
}

void MvmController::dropAudioAdjustmentResult(const QString& error) {
    const bool hadResult = audioAdjustmentResult_.has_value();
    stopAudioAdjustmentAudition();
    audioAdjustmentResult_.reset();
    audioContentConfirmed_ = false;
    audioApplyPending_ = false;
    if (audioContentJob_) {
        audioContentJob_->cancel();
        audioContentRetired_.push_back(std::move(audioContentJob_));
    }
    audioAdjustmentError_ = error;
    if (hadResult)
        emit audioAdjustmentResultsChanged();
    emit audioAdjustmentChanged();
}

int MvmController::audioAdjustmentProgress() const {
    return audioAdjustmentJob_ ? audioAdjustmentJob_->progress()
                               : (audioAdjustmentResult_ ? 100 : 0);
}

bool MvmController::canApplyAudioAdjustment() const {
    return !audioAdjustmentJob_ && audioAdjustmentResult_ && audioAdjustmentResult_->success &&
           !busy_ && !playing() && !audioApplyPending_ && audioContentConfirmed_ &&
           audioAdjustmentResult_->projectionHash == audioProjectionHash_ &&
           audioCheapIdentityMatches(audioAdjustmentResult_->files);
}

QVariantList MvmController::audioAdjustmentResults() const {
    QVariantList rows;
    if (!audioAdjustmentResult_)
        return rows;
    std::unordered_map<std::string_view, const project::TimelineClip*> candidates;
    for (const auto& clip : audioAdjustmentResult_->candidate.timelineClips)
        candidates.try_emplace(clip.id, &clip);
    for (const auto& item : audioAdjustmentResult_->clips) {
        QVariantList keys;
        if (const auto found = candidates.find(item.clipId); found != candidates.end())
            for (const auto& key : found->second->effects.duckingKeys)
                keys.append(
                    QVariantMap{{"frame", QVariant::fromValue<qlonglong>(key.frame)},
                                {"seconds", static_cast<double>(key.frame) *
                                                static_cast<double>(project_.timelineFpsDen) /
                                                static_cast<double>(project_.timelineFpsNum)},
                                {"db", key.value}});
        rows.append(QVariantMap{{"clipId", QString::fromStdString(item.clipId)},
                                {"name", QString::fromStdString(item.name)},
                                {"measurable", item.measurement.measurable},
                                {"lufs", item.measurement.integratedLufs},
                                {"truePeakDb", item.measurement.truePeakDb},
                                {"correctionDb", item.correctionDb},
                                {"peakLimited", item.peakLimited},
                                {"gainLimited", item.gainLimited},
                                {"keys", keys}});
    }
    return rows;
}

QVariantList MvmController::audioAdjustmentRanges() const {
    QVariantList ranges;
    if (audioAdjustmentResult_)
        for (const auto& range : audioAdjustmentResult_->ranges)
            ranges.append(
                QVariantMap{{"startSeconds", static_cast<double>(range.startSample) / 48000},
                            {"endSeconds", static_cast<double>(range.endSample) / 48000}});
    return ranges;
}

QVariantMap MvmController::savedAudioAdjustmentSettings() const {
    const auto parse = [](const std::string& json) {
        return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().toVariantMap();
    };
    if (!project_.lastAudioAdjustmentSettings.empty())
        return parse(project_.lastAudioAdjustmentSettings);
    for (auto clip = project_.timelineClips.rbegin(); clip != project_.timelineClips.rend(); ++clip)
        if (!clip->effects.audioAdjustmentSettings.empty())
            return parse(clip->effects.audioAdjustmentSettings);
    return {};
}

bool MvmController::startAudioAdjustment(const QVariantMap& options) {
    if (busy_ || audioAdjustmentJob_)
        return false;
    audioAdjustmentError_.clear();
    project::AudioAdjustmentSettings settings;
    bool valid = true;
    const auto integer = [&](const char* key, int fallback) {
        if (!options.contains(key))
            return fallback;
        bool ok = false;
        const double number = options.value(key).toDouble(&ok);
        if (!ok || !std::isfinite(number) || std::floor(number) != number || number < -1 ||
            number > 10000) {
            valid = false;
            return fallback;
        }
        return static_cast<int>(number);
    };
    const auto number = [&](const char* key, double fallback) {
        if (!options.contains(key))
            return fallback;
        bool ok = false;
        const double value = options.value(key).toDouble(&ok);
        valid = valid && ok && std::isfinite(value);
        return value;
    };
    settings.bgmTrack = integer("bgmTrack", -1);
    for (const auto& value : options.value("voiceTracks").toList()) {
        bool ok = false;
        const double index = value.toDouble(&ok);
        if (!ok || !std::isfinite(index) || std::floor(index) != index || index < 0 ||
            index >= static_cast<double>(project_.audioTracks.size()))
            valid = false;
        else
            settings.voiceTracks.push_back(static_cast<int>(index));
    }
    settings.normalize = options.value("normalize", true).toBool();
    settings.duck = options.value("duck", true).toBool();
    settings.voiceLufs = number("voiceLufs", -16);
    settings.bgmLufs = number("bgmLufs", -24);
    settings.reductionDb = number("reductionDb", 12);
    settings.thresholdDb = number("thresholdDb", -35);
    settings.attackMs = integer("attackMs", 150);
    settings.holdMs = integer("holdMs", 300);
    settings.releaseMs = integer("releaseMs", 600);
    std::string error;
    if (!valid || !project::validateAudioAdjustmentSettings(
                      settings, static_cast<int>(project_.audioTracks.size()), error)) {
        audioAdjustmentError_ = error.empty() ? QStringLiteral("自動音量調整の入力が不正です")
                                              : QString::fromStdString(error);
        emit audioAdjustmentChanged();
        return false;
    }
    bool hasAudio = false;
    for (const auto& clip : project_.timelineClips) {
        if (!clip.enabled || clip.kind != project::TimelineClipKind::Audio ||
            !project::audioAdjustmentTargetsTrack(settings, clip.track.index))
            continue;
        hasAudio = true;
        AudioFileIdentity identity;
        if (!inspectAudioFile(clip.mediaPath, identity, false, nullptr, error)) {
            audioAdjustmentError_ = QStringLiteral("解析する素材の状態を確認できません");
            emit audioAdjustmentChanged();
            return false;
        }
    }
    if (!hasAudio) {
        audioAdjustmentError_ = QStringLiteral("解析する素材の状態を確認できません");
        emit audioAdjustmentChanged();
        return false;
    }
    if (!pauseTimeline()) {
        audioAdjustmentError_ = QStringLiteral("解析の前にタイムラインの再生を停止できません");
        emit audioAdjustmentChanged();
        return false;
    }
    stopAudioAdjustmentAudition();
    if (audioContentJob_) {
        audioContentJob_->cancel();
        audioContentRetired_.push_back(std::move(audioContentJob_));
    }
    audioApplyPending_ = false;
    audioAdjustmentResult_.reset();
    audioContentConfirmed_ = false;
    emit audioAdjustmentResultsChanged();
    QVariantList voiceTracks;
    for (int track : settings.voiceTracks)
        voiceTracks.append(track);
    audioAdjustmentOptions_ = {
        {"voiceTracks", voiceTracks},          {"bgmTrack", settings.bgmTrack},
        {"normalize", settings.normalize},     {"duck", settings.duck},
        {"voiceLufs", settings.voiceLufs},     {"bgmLufs", settings.bgmLufs},
        {"reductionDb", settings.reductionDb}, {"thresholdDb", settings.thresholdDb},
        {"attackMs", settings.attackMs},       {"holdMs", settings.holdMs},
        {"releaseMs", settings.releaseMs}};
    audioAdjustmentLastProgress_ = -1;
    audioAuthoritySettings_ = settings;
    refreshAudioInputAuthority(false);
    audioAdjustmentJob_ = std::make_unique<AudioAdjustmentJob>(project_, settings);
    ensureAudioAdjustmentTimer();
    emit audioAdjustmentChanged();
    return true;
}

void MvmController::pollAudioAdjustment() {
    reapAudioAdjustmentJobs();
    bool changed = false;
    if (audioAdjustmentJob_ && audioAdjustmentJob_->ready()) {
        auto result = audioAdjustmentJob_->take();
        audioAdjustmentJob_.reset();
        if (result.cancelled)
            audioAdjustmentError_ = QStringLiteral("解析を中止しました");
        else if (!result.success)
            audioAdjustmentError_ = QString::fromStdString(result.error);
        else if (result.projectionHash != audioProjectionHash_ || result.fingerprintText.empty())
            audioAdjustmentError_ =
                QStringLiteral("解析中に編集または素材の変更がありました。再解析してください");
        else {
            for (const auto& file : result.files)
                audioFileCache_[file.key] = file;
            audioAdjustmentResult_ = std::move(result);
            audioContentConfirmed_ = true;
            audioContentDirty_ = false;
            emit audioAdjustmentResultsChanged();
        }
        changed = true;
    }
    refreshAudioFileCacheCheap();
    if (audioAdjustmentResult_ && (audioAdjustmentResult_->projectionHash != audioProjectionHash_ ||
                                   !audioCheapIdentityMatches(audioAdjustmentResult_->files))) {
        dropAudioAdjustmentResult(
            QStringLiteral("解析後に編集または素材の変更がありました。再解析してください"));
        changed = true;
    }
    finishAudioContentRecheck();
    scheduleAudioContentRecheck();
    syncAudioFileWatch();
    if (audioAdjustmentAudition_) {
        const auto error = audioAdjustmentAudition_->error();
        const auto valid = audioAdjustmentResult_
                               ? project::validateTimeline(audioAdjustmentResult_->candidate)
                               : project::TimelineValidationResult{};
        const double endSeconds = audioAdjustmentResult_ && valid.success
                                      ? static_cast<double>(valid.totalFrames - playheadFrame_) *
                                            static_cast<double>(project_.timelineFpsDen) /
                                            static_cast<double>(project_.timelineFpsNum)
                                      : 0;
        if (!audioAdjustmentResult_ || !error.empty() ||
            static_cast<double>(audioAdjustmentAudition_->elapsedSamples()) >= endSeconds * 48000) {
            if (!error.empty())
                audioAdjustmentError_ = QString::fromStdString(error);
            stopAudioAdjustmentAudition();
            changed = true;
        }
    }
    if (audioAdjustmentJob_) {
        const int progress = audioAdjustmentJob_->progress();
        if (progress != audioAdjustmentLastProgress_) {
            audioAdjustmentLastProgress_ = progress;
            changed = true;
        }
    }
    // 保存済み調整の待機中は止め、watcher の通知か取りこぼし確認で dirty になったら再開する。
    if (!audioAdjustmentJob_ && audioAdjustmentRetired_.empty() && !audioAdjustmentResult_ &&
        !audioAdjustmentAudition_ && !audioContentJob_ && audioContentRetired_.empty() &&
        !audioContentDirty_)
        audioAdjustmentTimer_.stop();
    if (changed)
        emit audioAdjustmentChanged();
}

void MvmController::stopAudioAdjustmentAudition() {
    if (audioAdjustmentAudition_) {
        audioAdjustmentAudition_->stop();
        audioAdjustmentAudition_.reset();
        emit audioAdjustmentChanged();
    }
}

void MvmController::cancelAudioAdjustment() {
    stopAudioAdjustmentAudition();
    if (audioAdjustmentJob_) {
        audioAdjustmentJob_->cancel();
        audioAdjustmentRetired_.push_back(std::move(audioAdjustmentJob_));
    }
    audioAdjustmentResult_.reset();
    audioContentConfirmed_ = false;
    audioApplyPending_ = false;
    if (audioContentJob_) {
        audioContentJob_->cancel();
        audioContentRetired_.push_back(std::move(audioContentJob_));
    }
    if (!audioAdjustmentRetired_.empty() || !audioContentRetired_.empty() || audioContentDirty_)
        ensureAudioAdjustmentTimer();
    else
        audioAdjustmentTimer_.stop();
    emit audioAdjustmentResultsChanged();
    emit audioAdjustmentChanged();
}

bool MvmController::auditionAudioAdjustment() {
    stopAudioAdjustmentAudition();
    if (!canApplyAudioAdjustment())
        return false;
    pauseTimeline();
    auto audition = std::make_unique<ShuttleAudioPlayback>();
    std::string error;
    if (!audition->start(audioAdjustmentResult_->candidate, 1, playheadFrame_,
                         static_cast<float>(masterVolume_), error)) {
        audioAdjustmentError_ = QString::fromStdString(error);
        emit audioAdjustmentChanged();
        return false;
    }
    audioAdjustmentAudition_ = std::move(audition);
    ensureAudioAdjustmentTimer();
    emit audioAdjustmentChanged();
    return true;
}

bool MvmController::applyAudioAdjustment() {
    stopAudioAdjustmentAudition();
    // 適用要求は非同期の全内容照合へ渡し、保護ハンドルを保持した完了処理で確定する。
    refreshAudioFileCacheCheap();
    if (audioAdjustmentResult_ && (audioAdjustmentResult_->projectionHash != audioProjectionHash_ ||
                                   !audioCheapIdentityMatches(audioAdjustmentResult_->files))) {
        dropAudioAdjustmentResult(
            QStringLiteral("解析後に編集または素材の変更がありました。再解析してください"));
    }
    if (!canApplyAudioAdjustment()) {
        audioAdjustmentError_ =
            QStringLiteral("適用できる解析結果がありません。再解析してください");
        emit audioAdjustmentChanged();
        return false;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& file : audioAdjustmentResult_->files)
        paths.push_back(pathFromIdentityKey(file.key));
    if (audioContentJob_) {
        audioContentJob_->cancel();
        audioContentRetired_.push_back(std::move(audioContentJob_));
    }
    audioApplyPending_ = true;
    audioAdjustmentError_.clear();
    audioContentConfirmed_ = false;
    audioContentDirty_ = false;
    audioContentJob_ = std::make_unique<AudioContentHashJob>(std::move(paths), true);
    ensureAudioAdjustmentTimer();
    emit audioAdjustmentChanged();
    return false;
}

bool MvmController::commitAudioAdjustment() {
    // 最終照合の直後に GUI thread で走るので、clip 数の二乗にしない。
    std::unordered_map<std::string_view, const project::TimelineClip*> measuredById;
    for (const auto& measured : audioAdjustmentResult_->candidate.timelineClips)
        measuredById.try_emplace(measured.id, &measured);
    std::unordered_set<std::string_view> measuredClipIds;
    for (const auto& measured : audioAdjustmentResult_->clips)
        measuredClipIds.insert(measured.clipId);
    const auto serialized = QJsonDocument(QJsonObject::fromVariantMap(audioAdjustmentOptions_))
                                .toJson(QJsonDocument::Compact)
                                .toStdString();
    auto candidate = project_;
    candidate.lastAudioAdjustmentSettings = serialized;
    for (auto& clip : candidate.timelineClips) {
        if (project::audioAdjustmentTargetsTrack(audioAdjustmentResult_->settings,
                                                 clip.track.index))
            if (const auto found = measuredById.find(clip.id); found != measuredById.end()) {
                clip.effects.normalizationGainDb = found->second->effects.normalizationGainDb;
                clip.effects.duckingDb = found->second->effects.duckingDb;
                clip.effects.duckingKeys = found->second->effects.duckingKeys;
            }
        if (measuredClipIds.contains(clip.id)) {
            clip.effects.audioAdjustmentSettings = serialized;
            clip.effects.audioAdjustmentFingerprint = audioAdjustmentResult_->fingerprintText;
        }
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("自動音量調整を適用できません: ")))
        return false;
    cancelAudioAdjustment();
    setStatus(QStringLiteral(
        "自動音量調整を適用しました。ダッキングはエフェクトコントロールで編集できます"));
    return true;
}
} // namespace mvm::app
