#include "mvm_controller.h"
#include "shuttle_audio_playback.h"

#include <cmath>
#include <set>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace mvm::app {
QString MvmController::audioAdjustmentFingerprint(const project::Project& source) const {
    auto clean = source;
    for (auto& track : clean.audioTracks) {
        track.muted = false;
        track.solo = false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::set<std::filesystem::path> paths;
    for (auto& clip : clean.timelineClips) {
        clip.effects.normalizationGainDb = 0;
        clip.effects.duckingDb = 0;
        clip.effects.duckingKeys.clear();
        clip.effects.audioAdjustmentSettings.clear();
        clip.effects.audioAdjustmentFingerprint.clear();
        if (clip.enabled && clip.kind == project::TimelineClipKind::Audio)
            paths.insert(clip.mediaPath);
    }
    const auto json = project::serializeProjectJson(clean, projectPath_);
    if (!json.success)
        return {};
    hash.addData(QByteArray::fromStdString(json.json));
    for (const auto& path : paths) {
        const QFileInfo info(QString::fromStdWString(path.wstring()));
        if (!info.isFile())
            return {};
        hash.addData(info.absoluteFilePath().toUtf8());
        hash.addData(QByteArray::number(info.size()));
        hash.addData(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    }
    return QString::fromLatin1(hash.result().toHex());
}

int MvmController::audioAdjustmentProgress() const {
    return audioAdjustmentJob_ ? audioAdjustmentJob_->progress()
                               : (audioAdjustmentResult_ ? 100 : 0);
}

bool MvmController::canApplyAudioAdjustment() const {
    return !audioAdjustmentJob_ && audioAdjustmentResult_ && audioAdjustmentResult_->success &&
           audioAdjustmentSource_ && *audioAdjustmentSource_ == project_ && !busy_ && !playing() &&
           !audioAdjustmentInputFingerprint_.isEmpty() &&
           audioAdjustmentFingerprint(project_) == audioAdjustmentInputFingerprint_;
}

QVariantList MvmController::audioAdjustmentResults() const {
    QVariantList rows;
    if (!audioAdjustmentResult_)
        return rows;
    for (const auto& item : audioAdjustmentResult_->clips) {
        QVariantList keys;
        for (const auto& clip : audioAdjustmentResult_->candidate.timelineClips)
            if (clip.id == item.clipId)
                for (const auto& key : clip.effects.duckingKeys)
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
    for (const auto& clip : project_.timelineClips)
        if (!clip.effects.audioAdjustmentSettings.empty())
            return QJsonDocument::fromJson(
                       QByteArray::fromStdString(clip.effects.audioAdjustmentSettings))
                .object()
                .toVariantMap();
    return {};
}

bool MvmController::audioAdjustmentNeedsRegeneration() const {
    QString current;
    for (const auto& clip : project_.timelineClips)
        if (!clip.effects.audioAdjustmentFingerprint.empty()) {
            if (current.isEmpty())
                current = audioAdjustmentFingerprint(project_);
            if (current.isEmpty() ||
                current != QString::fromStdString(clip.effects.audioAdjustmentFingerprint))
                return true;
        }
    return false;
}

bool MvmController::startAudioAdjustment(const QVariantMap& options) {
    if (busy_ || audioAdjustmentJob_)
        return false;
    if (!pauseTimeline()) {
        audioAdjustmentError_ = QStringLiteral("解析の前にタイムラインの再生を停止できません");
        emit audioAdjustmentChanged();
        return false;
    }
    stopAudioAdjustmentAudition();
    audioAdjustmentResult_.reset();
    emit audioAdjustmentResultsChanged();
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
    audioAdjustmentInputFingerprint_ = audioAdjustmentFingerprint(project_);
    if (audioAdjustmentInputFingerprint_.isEmpty()) {
        audioAdjustmentError_ = QStringLiteral("解析する素材の状態を確認できません");
        emit audioAdjustmentChanged();
        return false;
    }
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
    audioAdjustmentSource_ = project_;
    audioAdjustmentJob_ = std::make_unique<AudioAdjustmentJob>(project_, settings);
    audioAdjustmentTimer_.disconnect(this);
    connect(&audioAdjustmentTimer_, &QTimer::timeout, this, &MvmController::pollAudioAdjustment);
    audioAdjustmentTimer_.start(100);
    emit audioAdjustmentChanged();
    return true;
}

void MvmController::pollAudioAdjustment() {
    bool resultsChanged = false;
    if (audioAdjustmentJob_ && audioAdjustmentJob_->ready()) {
        auto result = audioAdjustmentJob_->take();
        audioAdjustmentJob_.reset();
        if (result.cancelled)
            audioAdjustmentError_ = QStringLiteral("解析を中止しました");
        else if (!result.success)
            audioAdjustmentError_ = QString::fromStdString(result.error);
        else if (!audioAdjustmentSource_ || *audioAdjustmentSource_ != project_ ||
                 audioAdjustmentFingerprint(project_) != audioAdjustmentInputFingerprint_)
            audioAdjustmentError_ =
                QStringLiteral("解析中に編集または素材の変更がありました。再解析してください");
        else {
            audioAdjustmentResult_ = std::move(result);
            resultsChanged = true;
        }
    }
    if (audioAdjustmentResult_ &&
        (!audioAdjustmentSource_ || *audioAdjustmentSource_ != project_ ||
         audioAdjustmentFingerprint(project_) != audioAdjustmentInputFingerprint_)) {
        stopAudioAdjustmentAudition();
        audioAdjustmentResult_.reset();
        resultsChanged = true;
        audioAdjustmentError_ =
            QStringLiteral("解析後に編集または素材の変更がありました。再解析してください");
    }
    if (audioAdjustmentAudition_) {
        const auto error = audioAdjustmentAudition_->error();
        const auto valid = project::validateTimeline(audioAdjustmentResult_->candidate);
        const double endSeconds = static_cast<double>(valid.totalFrames - playheadFrame_) *
                                  static_cast<double>(project_.timelineFpsDen) /
                                  static_cast<double>(project_.timelineFpsNum);
        if (!error.empty() ||
            static_cast<double>(audioAdjustmentAudition_->elapsedSamples()) >= endSeconds * 48000) {
            if (!error.empty())
                audioAdjustmentError_ = QString::fromStdString(error);
            stopAudioAdjustmentAudition();
        }
    }
    if (!audioAdjustmentJob_ && !audioAdjustmentResult_)
        audioAdjustmentTimer_.stop();
    if (resultsChanged)
        emit audioAdjustmentResultsChanged();
    emit audioAdjustmentChanged();
}

void MvmController::stopAudioAdjustmentAudition() {
    if (audioAdjustmentAudition_) {
        audioAdjustmentAudition_->stop();
        audioAdjustmentAudition_.reset();
    }
    emit audioAdjustmentChanged();
}

void MvmController::cancelAudioAdjustment() {
    stopAudioAdjustmentAudition();
    if (audioAdjustmentJob_)
        audioAdjustmentJob_->cancel();
    audioAdjustmentJob_.reset();
    audioAdjustmentResult_.reset();
    audioAdjustmentSource_.reset();
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
    emit audioAdjustmentChanged();
    return true;
}

bool MvmController::applyAudioAdjustment() {
    stopAudioAdjustmentAudition();
    if (!canApplyAudioAdjustment()) {
        audioAdjustmentError_ =
            QStringLiteral("適用できる解析結果がありません。再解析してください");
        emit audioAdjustmentChanged();
        return false;
    }
    auto candidate = audioAdjustmentResult_->candidate;
    const auto serialized = QJsonDocument(QJsonObject::fromVariantMap(audioAdjustmentOptions_))
                                .toJson(QJsonDocument::Compact)
                                .toStdString();
    for (auto& clip : candidate.timelineClips)
        for (const auto& measured : audioAdjustmentResult_->clips)
            if (clip.id == measured.clipId) {
                clip.effects.audioAdjustmentSettings = serialized;
                clip.effects.audioAdjustmentFingerprint =
                    audioAdjustmentInputFingerprint_.toStdString();
            }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("自動音量調整を適用できません: ")))
        return false;
    cancelAudioAdjustment();
    setStatus(QStringLiteral(
        "自動音量調整を適用しました。ダッキングはエフェクトコントロールで編集できます"));
    return true;
}
} // namespace mvm::app
