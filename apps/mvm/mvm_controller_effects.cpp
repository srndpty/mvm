#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "clip_keyframe_values.h"
#include "project/clip_effects.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cmath>

namespace mvm::app {
using detail::indexOfClipId;

project::ClipEffects MvmController::currentEffects() const {
    static const project::ClipEffects defaults;
    if (currentClipIndex_ < 0 ||
        currentClipIndex_ >= static_cast<int>(project_.timelineClips.size()))
        return defaults;
    if (previewEffectsOverride_ && previewEffectsClipIndex_ == currentClipIndex_)
        return project::evaluateClipEffects(
            *previewEffectsOverride_,
            playheadFrame_ - project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)]
                                 .timelineStartFrame);
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    return project::evaluateClipEffects(clip.effects, playheadFrame_ - clip.timelineStartFrame);
}

project::ClipEffects MvmController::effectsForPreview(int clipIndex) const {
    if (previewEffectsOverride_ && previewEffectsClipIndex_ == clipIndex)
        return *previewEffectsOverride_;
    return project_.timelineClips[static_cast<std::size_t>(clipIndex)].effects;
}

bool MvmController::applyEffectKey(project::ClipEffects& effects, const QString& key,
                                   double value) {
    if (const auto* channel = project::effectChannel(key.toStdString()))
        effects.*channel->base = value;
    else if (key == QStringLiteral("fadeIn"))
        effects.fadeInFrames = static_cast<std::int64_t>(std::llround(value));
    else if (key == QStringLiteral("fadeOut"))
        effects.fadeOutFrames = static_cast<std::int64_t>(std::llround(value));
    else
        return false;
    return true;
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

QVariantMap MvmController::previewClipKey(const QString& clipId, qint64 originalFrame,
                                          qint64 requestedFrame, double value) const {
    const auto id = clipId.toStdString();
    const auto found = std::find_if(project_.timelineClips.begin(), project_.timelineClips.end(),
                                    [&](const auto& clip) { return clip.id == id; });
    if (found == project_.timelineClips.end())
        return {{QStringLiteral("success"), false}};
    const bool audio = found->kind == project::TimelineClipKind::Audio;
    const auto preview = project::previewClipKeyEdit(
        project_, id, audio ? project::ClipKeyKind::Volume : project::ClipKeyKind::Opacity,
        originalFrame, requestedFrame, value);
    QVariantList keys;
    if (preview.success) {
        const auto& source = audio ? preview.effects.volumeKeys : preview.effects.opacityKeys;
        keys = clipKeyframeValues(source);
    }
    return {{QStringLiteral("success"), preview.success},
            {QStringLiteral("frame"), preview.frame},
            {QStringLiteral("keys"), keys},
            {QStringLiteral("error"), QString::fromStdString(preview.error)}};
}

bool MvmController::commitClipKey(const QString& clipId, qint64 originalFrame,
                                  qint64 requestedFrame, double value) {
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
                                             originalFrame, requestedFrame, value);
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
    Q_EMIT stateChanged();
    QString previewError;
    if (!refreshPreviewAtPlayhead(previewError))
        setStatus(QStringLiteral("キーフレームのPreview更新に失敗しました: ") + previewError);
    return true;
}

qint64 MvmController::effectEditFrame(const project::TimelineClip& clip) const {
    const auto duration = project::timelineClipDuration(project_, clip);
    const auto local = playheadFrame_ - clip.timelineStartFrame;
    // 区間外でも選択クリップを編集できる。追加・貼り付けは先頭を基準にする。
    return duration.success && local >= 0 && local < duration.frame ? local : 0;
}

QVariantList MvmController::keyframeChannels() const {
    QVariantList result;
    if (currentClipIndex_ < 0)
        return result;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto duration = project::timelineClipDuration(project_, clip);
    const auto& effects = previewEffectsOverride_ && previewEffectsClipIndex_ == currentClipIndex_
                              ? *previewEffectsOverride_
                              : clip.effects;
    if (keyframeDisplayClip_ != currentClipIndex_ || keyframeDisplayEffects_ != effects) {
        keyframeDisplayClip_ = currentClipIndex_;
        keyframeDisplayEffects_ = effects;
        keyframeDisplayKeys_.clear();
        for (const auto& channel : project::effectChannels())
            keyframeDisplayKeys_[channel.name] = clipKeyframeValues(effects.*channel.keys);
    }
    const auto local = effectEditFrame(clip);
    const auto evaluated = project::evaluateClipEffects(effects, local);
    const QStringList labels{QStringLiteral("不透明度"),        QStringLiteral("音量"),
                             QStringLiteral("ダッキング (dB)"), QStringLiteral("位置 X"),
                             QStringLiteral("位置 Y"),          QStringLiteral("拡大率 X"),
                             QStringLiteral("拡大率 Y"),        QStringLiteral("回転"),
                             QStringLiteral("クロップ 左"),     QStringLiteral("クロップ 上"),
                             QStringLiteral("クロップ 右"),     QStringLiteral("クロップ 下")};
    std::size_t index = 0;
    for (const auto& channel : project::effectChannels()) {
        const auto label = labels[static_cast<qsizetype>(index++)];
        if (project::isAudioEffectChannel(channel.kind) !=
            (clip.kind == project::TimelineClipKind::Audio))
            continue;
        const auto& keys = keyframeDisplayKeys_.at(channel.name);
        const bool atKey =
            std::any_of((effects.*channel.keys).begin(), (effects.*channel.keys).end(),
                        [local](const auto& key) { return key.frame == local; });
        result.append(
            QVariantMap{{QStringLiteral("clipId"), QString::fromStdString(clip.id)},
                        {QStringLiteral("name"), QString::fromLatin1(channel.name)},
                        {QStringLiteral("label"), label},
                        {QStringLiteral("value"), evaluated.*channel.base},
                        {QStringLiteral("minimum"), channel.minimum},
                        {QStringLiteral("maximum"),
                         channel.kind >= project::ClipKeyKind::CropLeft ? 99.99 : channel.maximum},
                        {QStringLiteral("keys"), keys},
                        {QStringLiteral("animated"), !keys.isEmpty()},
                        {QStringLiteral("atKey"), atKey},
                        {QStringLiteral("frame"), local},
                        {QStringLiteral("duration"), duration.frame},
                        {QStringLiteral("editable"), !playing_ && !busy_ && duration.success}});
    }
    return result;
}

bool MvmController::setEffectAnimation(const QString& name, bool enabled) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || busy_ || playing_ || currentClipIndex_ < 0)
        return false;
    auto candidate = project_;
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    if (project::isAudioEffectChannel(channel->kind) !=
        (clip.kind == project::TimelineClipKind::Audio))
        return false;
    const auto duration = project::timelineClipDuration(candidate, clip);
    const auto local = effectEditFrame(clip);
    if (!duration.success)
        return false;
    auto& keys = clip.effects.*channel->keys;
    if (enabled == !keys.empty())
        return true;
    const auto value = project::evaluateClipKeys(keys, clip.effects.*channel->base, local);
    if (enabled)
        keys.push_back({local, value});
    else {
        clip.effects.*channel->base = value;
        keys.clear();
    }
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::toggleEffectKey(const QString& name) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || busy_ || playing_ || currentClipIndex_ < 0)
        return false;
    auto candidate = project_;
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    if (project::isAudioEffectChannel(channel->kind) !=
        (clip.kind == project::TimelineClipKind::Audio))
        return false;
    const auto duration = project::timelineClipDuration(candidate, clip);
    const auto local = effectEditFrame(clip);
    if (!duration.success)
        return false;
    auto& keys = clip.effects.*channel->keys;
    const auto value = project::evaluateClipKeys(keys, clip.effects.*channel->base, local);
    const auto found = std::find_if(keys.begin(), keys.end(),
                                    [local](const auto& key) { return key.frame == local; });
    if (found == keys.end())
        project::insertClipKey(keys, local, value);
    else {
        project::removeClipKeys(keys, {local});
        if (keys.empty())
            clip.effects.*channel->base = value;
    }
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::moveEffectKey(const QString& name, qint64 from, qint64 to, bool commit) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0)
        return false;
    const auto& effects =
        project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects;
    return editEffectKey(
        name, from, to,
        project::evaluateClipKeys(effects.*channel->keys, effects.*channel->base, from), commit);
}

bool MvmController::editEffectKey(const QString& name, qint64 from, qint64 to, double value,
                                  bool commit) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || busy_ || playing_ || currentClipIndex_ < 0)
        return false;
    auto effects = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects;
    auto& keys = effects.*channel->keys;
    const auto found = std::find_if(keys.begin(), keys.end(),
                                    [from](const auto& key) { return key.frame == from; });
    const auto duration = project::timelineClipDuration(
        project_, project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)]);
    if (found == keys.end() || !duration.success || to < 0 || to >= duration.frame ||
        (from != to &&
         std::any_of(keys.begin(), keys.end(), [to](const auto& key) { return key.frame == to; })))
        return false;
    found->frame = to;
    found->value = value;
    std::sort(keys.begin(), keys.end(),
              [](const auto& a, const auto& b) { return a.frame < b.frame; });
    std::string error;
    if (!project::validateEffectKeys(effects, duration.frame,
                                     project::isAudioEffectChannel(channel->kind), error)) {
        setStatus(QString::fromStdString(error));
        return failEffectEdit(commit);
    }
    if (!commit) {
        previewEffectsOverride_ = effects;
        previewEffectsClipIndex_ = currentClipIndex_;
    } else {
        auto candidate = project_;
        candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects = effects;
        if (!commitProjectEdit(std::move(candidate), QStringLiteral("キーを移動できません: ")))
            return failEffectEdit(true);
        previewEffectsOverride_.reset();
        previewEffectsClipIndex_ = -1;
    }
    Q_EMIT stateChanged();
    QString previewError;
    refreshPreviewAtPlayhead(previewError);
    return true;
}

bool MvmController::setEffectInterpolation(const QString& name, qint64 frame, int interpolation) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || busy_ || playing_ || currentClipIndex_ < 0 || interpolation < 0 ||
        interpolation > 4)
        return false;
    const auto& current = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto duration = project::timelineClipDuration(project_, current);
    if (!duration.success)
        return false;
    auto candidate = project_;
    auto& keys =
        candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects.*channel->keys;
    const auto found = std::find_if(keys.begin(), keys.end(),
                                    [frame](const auto& key) { return key.frame == frame; });
    if (found == keys.end())
        return false;
    // 同じ補間の再選択は何もしない。trim・分割で残した部分曲線 (curveStart/End) を
    // 0..1 へ戻すと、補間を変えていないのに動きが変わる。
    if (found->interpolation == static_cast<project::KeyInterpolation>(interpolation))
        return true;
    if (interpolation == static_cast<int>(project::KeyInterpolation::Spline)) {
        const auto controls = project::clipKeySplineControls(*found);
        found->control1 = std::clamp(controls.first, 0.0, 1.0);
        found->control2 = std::clamp(controls.second, 0.0, 1.0);
    }
    found->interpolation = static_cast<project::KeyInterpolation>(interpolation);
    found->curveStart = 0;
    found->curveEnd = 1;
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::setEffectSpline(const QString& name, qint64 frame, double control1,
                                    double control2, bool commit) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0 || busy_ || playing_)
        return false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success)
        return false;
    auto effects = previewEffectsOverride_ && previewEffectsClipIndex_ == currentClipIndex_
                       ? *previewEffectsOverride_
                       : clip.effects;
    auto& keys = effects.*channel->keys;
    auto key = std::find_if(keys.begin(), keys.end(),
                            [frame](const auto& value) { return value.frame == frame; });
    if (key == keys.end())
        return false;
    key->interpolation = project::KeyInterpolation::Spline;
    key->control1 = control1;
    key->control2 = control2;
    key->curveStart = 0;
    key->curveEnd = 1;
    std::string error;
    if (!project::validateEffectKeys(effects, duration.frame,
                                     clip.kind == project::TimelineClipKind::Audio, error)) {
        setStatus(QString::fromStdString(error));
        return failEffectEdit(commit);
    }
    if (commit) {
        auto candidate = project_;
        candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)].effects = effects;
        if (!commitProjectEdit(std::move(candidate), QStringLiteral("曲線を更新できません: ")))
            return failEffectEdit(true);
        previewEffectsOverride_.reset();
        previewEffectsClipIndex_ = -1;
    } else {
        previewEffectsOverride_ = effects;
        previewEffectsClipIndex_ = currentClipIndex_;
    }
    Q_EMIT stateChanged();
    QString previewError;
    refreshPreviewAtPlayhead(previewError);
    return true;
}

bool MvmController::copyEffectKeys(const QString& name, const QVariantList& frames, bool cut) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0 || busy_ || (cut && playing_))
        return false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    std::vector<project::ClipKeyframe> copied;
    for (const auto& key : clip.effects.*channel->keys)
        if (std::any_of(frames.begin(), frames.end(),
                        [&](const auto& frame) { return frame.toLongLong() == key.frame; }))
            copied.push_back(key);
    if (copied.empty())
        return false;
    if (cut && !removeEffectKeys(name, frames))
        return false;
    const auto origin = copied.front().frame;
    for (auto& key : copied)
        key.frame -= origin;
    effectKeyClipboard_ = std::move(copied);
    return true;
}

bool MvmController::pasteEffectKeys(const QString& name) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0 || busy_ || playing_ || effectKeyClipboard_.empty())
        return false;
    auto candidate = project_;
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto duration = project::timelineClipDuration(candidate, clip);
    const auto local = effectEditFrame(clip);
    if (!duration.success)
        return false;
    auto& keys = clip.effects.*channel->keys;
    for (auto key : effectKeyClipboard_) {
        if (key.frame >= duration.frame - local) {
            setStatus(QStringLiteral("貼り付けるキーがクリップ尺を超えます"));
            return false;
        }
        key.frame += local;
        auto found = std::find_if(keys.begin(), keys.end(),
                                  [&](const auto& current) { return current.frame == key.frame; });
        if (found != keys.end())
            *found = key;
        else
            keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end(),
              [](const auto& left, const auto& right) { return left.frame < right.frame; });
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::removeEffectKeys(const QString& name, const QVariantList& frames) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0 || busy_ || playing_)
        return false;
    auto candidate = project_;
    auto& clip = candidate.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    auto& keys = clip.effects.*channel->keys;
    const double value =
        project::evaluateClipKeys(keys, clip.effects.*channel->base, effectEditFrame(clip));
    std::vector<std::int64_t> removed;
    for (const auto& frame : frames)
        removed.push_back(frame.toLongLong());
    if (project::removeClipKeys(keys, removed) == 0)
        return false;
    if (keys.empty())
        clip.effects.*channel->base = value;
    return commitClipKeyCandidate(std::move(candidate));
}

bool MvmController::deleteEffectKeys(const QString& name, const QVariantList& frames) {
    return removeEffectKeys(name, frames);
}

bool MvmController::seekEffectFrame(qint64 localFrame, bool scrub) {
    if (playing_ || busy_ || currentClipIndex_ < 0)
        return false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto duration = project::timelineClipDuration(project_, clip);
    if (!duration.success || localFrame < 0 || localFrame >= duration.frame)
        return false;
    const auto frame = clip.timelineStartFrame + localFrame;
    if (scrub) {
        scrubToFrame(frame);
        return true;
    }
    return seekTimelineFrame(frame);
}

bool MvmController::seekEffectKey(const QString& name, int direction) {
    const auto* channel = project::effectChannel(name.toStdString());
    if (!channel || currentClipIndex_ < 0)
        return false;
    const auto& clip = project_.timelineClips[static_cast<std::size_t>(currentClipIndex_)];
    const auto local = playheadFrame_ - clip.timelineStartFrame;
    const auto& keys = clip.effects.*channel->keys;
    if (direction > 0) {
        for (const auto& key : keys)
            if (key.frame > local)
                return seekEffectFrame(key.frame);
    } else {
        for (auto key = keys.rbegin(); key != keys.rend(); ++key)
            if (key->frame < local)
                return seekEffectFrame(key->frame);
    }
    return false;
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
    if (busy_ || playing_ || clipIndex < 0) {
        setStatus(QStringLiteral("effectを適用するclipがありません"));
        return false;
    }

    if (values.isEmpty()) {
        setStatus(QStringLiteral("変更するeffect項目がありません"));
        return false;
    }
    project::ClipEffects candidateEffects =
        previewEffectsOverride_ && previewEffectsClipIndex_ == clipIndex
            ? *previewEffectsOverride_
            : project_.timelineClips[static_cast<std::size_t>(clipIndex)].effects;
    // 複数の項目 (位置と拡大率など) を 1 つの変更として検証し、1 つの undo にする。
    for (auto entry = values.cbegin(); entry != values.cend(); ++entry) {
        bool numeric = false;
        const double value = entry.value().toDouble(&numeric);
        const auto* channel = project::effectChannel(entry.key().toStdString());
        if (channel && !(candidateEffects.*channel->keys).empty()) {
            const auto& editedClip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
            const auto duration = project::timelineClipDuration(project_, editedClip);
            const auto local = effectEditFrame(editedClip);
            if (!numeric || !std::isfinite(value) || !duration.success || local < 0 ||
                local >= duration.frame) {
                setStatus(QStringLiteral("クリップ内のフレームで数値を指定してください"));
                return failEffectEdit(commit);
            }
            project::insertClipKey(candidateEffects.*channel->keys, local, value);
            continue;
        }
        if (!numeric || !applyEffectKey(candidateEffects, entry.key(), value)) {
            setStatus(QStringLiteral("未知または数値でないeffect項目です: ") + entry.key());
            return failEffectEdit(commit);
        }
    }

    const auto& clip = project_.timelineClips[static_cast<std::size_t>(clipIndex)];
    std::string effectsError;
    if (!project::validateClipEffects(candidateEffects, clip.sourceOutFrame - clip.sourceInFrame,
                                      effectsError)) {
        setStatus(QString::fromStdString(effectsError));
        return failEffectEdit(commit);
    }

    const auto keyDuration = project::timelineClipDuration(project_, clip);
    if (!keyDuration.success ||
        !project::validateEffectKeys(candidateEffects, keyDuration.frame,
                                     clip.kind == project::TimelineClipKind::Audio, effectsError)) {
        setStatus(QString::fromStdString(effectsError));
        return failEffectEdit(commit);
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
        return failEffectEdit(commit);
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("effectを更新できません: ")))
        return failEffectEdit(true);
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
    QString previewError;
    if (!discardEffectPreview(previewError)) {
        setStatus(QStringLiteral("effectのPreview更新に失敗しました: ") + previewError);
        return true;
    }
    setStatus(QStringLiteral("effectの編集を取り消しました"));
    return true;
}

bool MvmController::discardEffectPreview(QString& previewError) {
    if (!previewEffectsOverride_)
        return true;
    previewEffectsOverride_.reset();
    previewEffectsClipIndex_ = -1;
    Q_EMIT stateChanged();
    return refreshPreviewAtPlayhead(previewError);
}

bool MvmController::failEffectEdit(bool commit) {
    // 確定の失敗では、保存されていない一時表示を残さない。失敗理由を status に
    // 残すため、Preview 更新の失敗では上書きしない。
    if (commit) {
        QString ignored;
        discardEffectPreview(ignored);
    }
    return false;
}

} // namespace mvm::app
