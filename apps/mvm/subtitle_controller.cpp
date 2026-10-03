#include "app/text_raster.h"
#include "media_source_identity.h"
#include "mvm_controller.h"
#include "project/subtitles.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <QFile>
#include <QSaveFile>
#include <QUuid>

namespace mvm::app {
std::shared_ptr<preview::CompositionSnapshot>
MvmController::subtitleCompositionForTest(qint64 frame, QString& error) const {
    const auto mapped = mapTimelinePreviewFrame(project_, frame);
    if (!mapped.success) {
        error = QString::fromStdString(mapped.error);
        return {};
    }
    preview::PreviewFrameRequest request;
    return previewCompositionFor(mapped, {}, request, error);
}

namespace {
std::string cueId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

QVariantMap cueMap(const project::SubtitleCue& cue) {
    return {{"id", QString::fromStdString(cue.id)},
            {"content", QString::fromStdString(cue.content)},
            {"startFrame", QVariant::fromValue<qlonglong>(cue.startFrame)},
            {"endFrame", QVariant::fromValue<qlonglong>(cue.endFrame)},
            {"linked", !cue.linkClipId.empty()}};
}

// QML から来た共通書式の変更を反映する。確定 (setSubtitleStyle) と preview の両方が使う。
void applySubtitleStyleValues(project::SubtitleStyle& style, const QVariantMap& values) {
    const auto string = [&](const char* key, std::string& value) {
        if (values.contains(key))
            value = values.value(key).toString().toStdString();
    };
    string("fontFamily", style.fontFamily);
    string("color", style.color);
    string("outlineColor", style.outlineColor);
    string("backgroundColor", style.backgroundColor);
    string("alignment", style.alignment);
    if (values.contains("fontSize"))
        style.fontSize = values.value("fontSize").toInt();
    if (values.contains("outlineWidth"))
        style.outlineWidth = values.value("outlineWidth").toInt();
    if (values.contains("bold"))
        style.bold = values.value("bold").toBool();
    if (values.contains("sideMargin"))
        style.sideMargin = values.value("sideMargin").toDouble();
    if (values.contains("bottomMargin"))
        style.bottomMargin = values.value("bottomMargin").toDouble();
}
} // namespace

QVariantMap MvmController::selectedSubtitle() const {
    if (project_.subtitles)
        for (const auto& cue : project_.subtitles->cues)
            if (QString::fromStdString(cue.id) == selectedSubtitleId_)
                return cueMap(cue);
    return {};
}

QVariantMap MvmController::subtitleStyle() const {
    const auto style = project_.subtitles ? project_.subtitles->style : project::SubtitleStyle{};
    return {{"fontFamily", QString::fromStdString(style.fontFamily)},
            {"fontSize", style.fontSize},
            {"bold", style.bold},
            {"color", QString::fromStdString(style.color)},
            {"outlineColor", QString::fromStdString(style.outlineColor)},
            {"outlineWidth", style.outlineWidth},
            {"backgroundColor", QString::fromStdString(style.backgroundColor)},
            {"alignment", QString::fromStdString(style.alignment)},
            {"sideMargin", style.sideMargin},
            {"bottomMargin", style.bottomMargin}};
}

bool MvmController::commitSubtitleEdit(project::Project candidate) {
    if (busy_ || playing())
        return false;
    std::string error;
    if (!project::validateSubtitles(candidate, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    // 欠けたフォントや画面に収まらない本文を確定前に拒否する。
    // 書式・出力寸法が同じなら、本文の変わっていない字幕は検査済みなので飛ばす。
    std::unordered_map<std::string_view, std::string_view> checked;
    if (project_.subtitles && candidate.subtitles &&
        candidate.subtitles->style == project_.subtitles->style &&
        candidate.outputWidth == project_.outputWidth &&
        candidate.outputHeight == project_.outputHeight)
        for (const auto& old : project_.subtitles->cues)
            checked.emplace(old.id, old.content);
    if (candidate.subtitles)
        for (const auto& cue : candidate.subtitles->cues) {
            if (const auto found = checked.find(cue.id);
                found != checked.end() && found->second == cue.content)
                continue;
            QString rasterError;
            if (!checkSubtitleLayout(cue, candidate.subtitles->style, candidate.outputWidth,
                                     candidate.outputHeight, rasterError)) {
                setStatus(rasterError);
                return false;
            }
        }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("字幕を更新できません: ")))
        return false;
    QString previewError;
    if (!refreshPreviewAtPlayhead(previewError))
        setStatus(QStringLiteral("字幕を更新しましたが、プレビューを更新できません: ") +
                  previewError);
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::selectSubtitle(const QString& id) {
    if (!project_.subtitles)
        return false;
    for (const auto& cue : project_.subtitles->cues)
        if (QString::fromStdString(cue.id) == id) {
            setTimelineSelection({}, false);
            setCurrentClipSelection(-1);
            setSubtitleSelection({cue.id}, cue.id);
            Q_EMIT stateChanged();
            return seekTimelineFrame(cue.startFrame);
        }
    return false;
}

bool MvmController::selectTimelineSubtitle(const QString& id, bool additive) {
    const auto target = id.toStdString();
    if (!project_.subtitles ||
        std::none_of(project_.subtitles->cues.begin(), project_.subtitles->cues.end(),
                     [&](const auto& cue) { return cue.id == target; })) {
        setStatus(QStringLiteral("選択した字幕がありません"));
        return false;
    }
    auto next = additive ? selectedSubtitleIds_ : std::vector<std::string>{};
    const auto found = std::find(next.begin(), next.end(), target);
    if (additive && found != next.end())
        next.erase(found);
    else if (found == next.end())
        next.push_back(target);
    // clip と字幕を同時には選ばない。Delete やコピーの対象を 1 種類に決めるため。
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    const bool added = std::find(next.begin(), next.end(), target) != next.end();
    setSubtitleSelection(std::move(next), added ? target : std::string{});
    Q_EMIT stateChanged();
    return true;
}

void MvmController::setSubtitleSelection(std::vector<std::string> ids,
                                         const std::string& preferredPrimary) {
    // 選択の唯一の入口。空なら主選択 (パネルの編集対象) も外す。空でなければ主選択は必ず
    // 選択の中にある。timeline の選択とパネルの編集対象が別々の字幕を指さないようにする。
    selectedSubtitleIds_ = std::move(ids);
    const auto contains = [&](const std::string& id) {
        return !id.empty() && std::find(selectedSubtitleIds_.begin(), selectedSubtitleIds_.end(),
                                        id) != selectedSubtitleIds_.end();
    };
    if (selectedSubtitleIds_.empty())
        selectedSubtitleId_.clear();
    else if (contains(preferredPrimary))
        selectedSubtitleId_ = QString::fromStdString(preferredPrimary);
    else if (!contains(selectedSubtitleId_.toStdString()))
        selectedSubtitleId_ = QString::fromStdString(selectedSubtitleIds_.front());
}

QStringList MvmController::selectedSubtitleIds() const {
    QStringList ids;
    for (const auto& id : selectedSubtitleIds_)
        ids.append(QString::fromStdString(id));
    return ids;
}

bool MvmController::addSubtitle(const QString& content, qint64 start, qint64 end) {
    auto candidate = project_;
    std::string error;
    const auto id = cueId();
    if (!project::editSubtitle(candidate, {id, start, end, content.toStdString(), {}}, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    setSubtitleSelection({id}, id);
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::updateSubtitle(const QString& id, const QString& content, qint64 start,
                                   qint64 end) {
    auto candidate = project_;
    std::string error;
    const project::SubtitleCue* existing = nullptr;
    if (candidate.subtitles)
        for (const auto& cue : candidate.subtitles->cues)
            if (cue.id == id.toStdString())
                existing = &cue;
    if (!existing) {
        setStatus(QStringLiteral("更新する字幕がありません"));
        return false;
    }
    // 本文と区間だけを変える。clip とのリンクは字幕だけを動かしても保つ。
    auto cue = *existing;
    cue.content = content.toStdString();
    cue.startFrame = start;
    cue.endFrame = end;
    if (!project::editSubtitle(candidate, std::move(cue), error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::deleteSelectedSubtitle() {
    auto candidate = project_;
    std::string error;
    if (!project::deleteSubtitle(candidate, selectedSubtitleId_.toStdString(), error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    setSubtitleSelection({}, {});
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::splitSelectedSubtitle(int textCursor) {
    auto candidate = project_;
    std::string error;
    // QML のカーソル位置は UTF-16 の位置なので、本文の文字 (code point) の位置へ直す。
    std::optional<std::size_t> textBreak;
    if (textCursor > 0)
        if (const auto cue = selectedSubtitle(); !cue.isEmpty()) {
            const auto content = cue.value("content").toString();
            if (textCursor < content.size())
                textBreak = static_cast<std::size_t>(content.left(textCursor).toUcs4().size());
        }
    if (!project::splitSubtitle(candidate, selectedSubtitleId_.toStdString(), playheadFrame_,
                                cueId(), error, textBreak)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::splitSelectedSubtitlesAtPlayhead() {
    // timeline で選んだ字幕のうち、再生位置を内側に含むものを分ける (clip の Ctrl+K と同じ)。
    auto candidate = project_;
    std::string error;
    int split = 0;
    for (const auto& id : selectedSubtitleIds_) {
        const auto* cue = [&]() -> const project::SubtitleCue* {
            for (const auto& c : candidate.subtitles->cues)
                if (c.id == id)
                    return &c;
            return nullptr;
        }();
        if (!cue || !(cue->startFrame < playheadFrame_ && playheadFrame_ < cue->endFrame))
            continue;
        if (!project::splitSubtitle(candidate, id, playheadFrame_, cueId(), error)) {
            setStatus(QString::fromStdString(error));
            return false;
        }
        ++split;
    }
    if (split == 0) {
        setStatus(QStringLiteral("再生ヘッド位置に分割できる選択字幕がありません"));
        return false;
    }
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::mergeSelectedSubtitle() {
    auto candidate = project_;
    std::string error;
    if (!project::mergeSubtitle(candidate, selectedSubtitleId_.toStdString(), error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::setSubtitleStyle(const QVariantMap& values) {
    auto candidate = project_;
    if (!candidate.subtitles)
        candidate.subtitles.emplace();
    applySubtitleStyleValues(candidate.subtitles->style, values);
    // 確定したら preview は不要になる。失敗しても preview を残さず Project の値へ戻す。
    const bool hadPreview = subtitleStylePreview_.has_value();
    if (candidate == project_) {
        cancelSubtitleStylePreview();
        return true;
    }
    subtitleStylePreview_.reset();
    if (commitSubtitleEdit(std::move(candidate)))
        return true;
    if (hadPreview) {
        subtitleRaster_.reset();
        subtitleRasterId_.clear();
        QString previewError;
        refreshPreviewAtPlayhead(previewError);
    }
    return false;
}

bool MvmController::previewSubtitleStyle(const QVariantMap& values) {
    if (busy_ || playing_)
        return false;
    // 同じ編集の途中なら、前回の preview に重ねる (色をドラッグしながら幅も変える場合など)。
    auto style = subtitleStylePreview_.value_or(project_.subtitles ? project_.subtitles->style
                                                                   : project::SubtitleStyle{});
    applySubtitleStyleValues(style, values);
    auto check = project_;
    if (!check.subtitles)
        check.subtitles.emplace();
    check.subtitles->style = style;
    std::string error;
    if (!project::validateSubtitles(check, error))
        return false;
    subtitleStylePreview_ = std::move(style);
    // 字幕の画像は cue ID を key にしているので、書式だけを変えたときは明示的に捨てる。
    subtitleRaster_.reset();
    subtitleRasterId_.clear();
    QString previewError;
    if (!refreshPreviewAtPlayhead(previewError))
        setStatus(QStringLiteral("書式のプレビューを更新できません: ") + previewError);
    return true;
}

void MvmController::cancelSubtitleStylePreview() {
    if (!subtitleStylePreview_)
        return;
    subtitleStylePreview_.reset();
    subtitleRaster_.reset();
    subtitleRasterId_.clear();
    QString previewError;
    refreshPreviewAtPlayhead(previewError);
}

bool MvmController::setSubtitlesVisible(bool visible) {
    auto candidate = project_;
    if (!candidate.subtitles)
        candidate.subtitles.emplace();
    candidate.subtitles->visible = visible;
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::importSubtitles(const QUrl& url, bool replace) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("ローカルのSRTファイルを指定してください"));
        return false;
    }
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("SRTを開けません: ") + file.errorString());
        return false;
    }
    const auto bytes = file.readAll();
    std::vector<project::SubtitleCue> cues;
    std::string error;
    if (!project::parseSrt(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            project_.timelineFpsNum, project_.timelineFpsDen, cueId, cues, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    auto candidate = project_;
    if (!project::applySubtitleCues(candidate, std::move(cues), replace, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    return commitSubtitleEdit(std::move(candidate));
}

bool MvmController::exportSubtitles(const QUrl& url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("ローカルのSRT出力先を指定してください"));
        return false;
    }
    std::string text, error;
    if (!project::writeSrt(project_, text, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    if (text.empty()) {
        setStatus(QStringLiteral("書き出す字幕がありません"));
        return false;
    }
    QSaveFile file(url.toLocalFile());
    const auto size = static_cast<qint64>(text.size());
    if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), size) != size ||
        !file.commit()) {
        setStatus(QStringLiteral("SRTを書き出せません: ") + file.errorString());
        return false;
    }
    setStatus(QStringLiteral("SRTを書き出しました"));
    return true;
}

bool MvmController::rippleDeleteSelection() {
    if (busy_ || playing())
        return false;
    std::vector<std::pair<std::int64_t, std::int64_t>> spans;
    for (const auto& clip : project_.timelineClips)
        if (std::find(selectedClipIds_.begin(), selectedClipIds_.end(), clip.id) !=
                selectedClipIds_.end() ||
            clip.id == currentClipId()) {
            const auto duration = project::timelineClipDuration(project_, clip);
            if (!duration.success) {
                setStatus(QString::fromStdString(duration.error));
                return false;
            }
            spans.emplace_back(clip.timelineStartFrame, clip.timelineStartFrame + duration.frame);
        }
    if (spans.empty()) {
        setStatus(QStringLiteral("リップル削除するクリップがありません"));
        return false;
    }
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (auto span : spans) {
        if (!merged.empty() && span.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, span.second);
        else
            merged.push_back(span);
    }
    auto candidate = project_;
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        auto result = project::editTimelineTime(candidate, it->first, it->second - it->first, 0);
        if (!result.success) {
            setStatus(QString::fromStdString(result.error));
            return false;
        }
    }
    if (!commitProjectEdit(std::move(candidate), QStringLiteral("リップル削除できません: ")))
        return false;
    selectedClipIds_.clear();
    currentClipIndex_ = -1;
    QString error;
    if (!refreshPreviewAtPlayhead(error))
        setStatus(error);
    Q_EMIT stateChanged();
    return true;
}
} // namespace mvm::app

namespace mvm::app {
QVariantList MvmController::transcriptionSources() const {
    QVariantList sources;
    for (const auto& clip : project_.timelineClips)
        if (clip.kind == project::TimelineClipKind::Audio ||
            clip.kind == project::TimelineClipKind::Video ||
            clip.kind == project::TimelineClipKind::Manim) {
            sources.push_back(QVariantMap{
                {"id", QString::fromStdString(clip.id)},
                {"label", QStringLiteral("クリップ: %1（%2%3・%4f）")
                              .arg(QString::fromStdString(clip.name))
                              .arg(clip.track.kind == project::TrackKind::Audio ? "A" : "V")
                              .arg(clip.track.index + 1)
                              .arg(clip.timelineStartFrame)},
                {"timelineClip", true}});
        }
    for (const auto& item : project_.mediaItems)
        if (item.kind != project::MediaKind::Image)
            sources.push_back(
                QVariantMap{{"id", QString::fromStdString(item.id)},
                            {"label", QStringLiteral("素材: ") + QString::fromStdString(item.name)},
                            {"timelineClip", false}});
    return sources;
}

bool MvmController::startTranscription(const QString& sourceId, bool timelineClip,
                                       const QUrl& modelUrl, const QString& backend,
                                       const QString& language, qint64 insertionFrame,
                                       const QString& initialPrompt) {
    if (transcribing_ || busy_ || playing())
        return false;
    if (!modelUrl.isLocalFile() || (backend != "cpu" && backend != "vulkan") ||
        insertionFrame < 0) {
        setStatus(QStringLiteral("認識モデル・実行経路・配置時刻が不正です"));
        return false;
    }
    transcribe::Request request;
    request.modelPath = std::filesystem::path(modelUrl.toLocalFile().toStdWString());
    request.backend = backend == "vulkan" ? transcribe::Backend::Vulkan : transcribe::Backend::Cpu;
    request.language = language.toStdString();
    // 語彙ヒントは Whisper の文脈長 (448 token) の半分までしか効かない。長すぎる入力は
    // 黙って切り詰めず、短くするよう求める。
    if (initialPrompt.size() > 200) {
        setStatus(QStringLiteral("語彙ヒントは200文字以内にしてください"));
        return false;
    }
    request.initialPrompt = initialPrompt.trimmed().toStdString();
    // 精度優先でビーム探索を使うので、CPU 経路では全コアを使う。
    request.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 32U));
    std::string linkClipId;
    std::int64_t offset = insertionFrame, speedNum = 1, speedDen = 1;
    bool found = false;
    std::string error;
    if (timelineClip) {
        for (const auto& clip : project_.timelineClips)
            if (clip.id == sourceId.toStdString()) {
                if (clip.frameHold || project::isStillClipKind(clip.kind)) {
                    setStatus(QStringLiteral("このクリップは文字起こしの対象にできません"));
                    return false;
                }
                request.mediaPath = clip.mediaPath;
                linkClipId = clip.id;
                offset = clip.timelineStartFrame;
                speedNum = clip.speedNum;
                speedDen = clip.speedDen;
                if (!project::subtitleFrameToTime(clip.sourceInFrame, clip.sourceFpsNum,
                                                  clip.sourceFpsDen, request.sourceBeginMs,
                                                  error) ||
                    !project::subtitleFrameToTime(clip.sourceOutFrame, clip.sourceFpsNum,
                                                  clip.sourceFpsDen, request.sourceEndMs, error)) {
                    setStatus(QString::fromStdString(error));
                    return false;
                }
                found = true;
                break;
            }
    } else
        for (const auto& item : project_.mediaItems)
            if (item.id == sourceId.toStdString() && item.kind != project::MediaKind::Image) {
                request.mediaPath = item.mediaPath;
                found = true;
                break;
            }
    if (!found) {
        setStatus(QStringLiteral("文字起こし対象の素材がありません"));
        return false;
    }
    if (transcriptionThread_.joinable())
        transcriptionThread_.join();
    transcriptionCancel_.store(false);
    transcriptionCues_.clear();
    transcriptionModel_.setCues({});
    transcriptionError_.clear();
    transcriptionProgress_ = 0;
    transcriptionRevision_ = currentRevision_;
    transcriptionEditSerial_ = nextRevision_;
    transcriptionProjectGeneration_ = projectGeneration_;
    transcriptionProjectPath_ = projectPath_;
    // クリップから作った字幕は、そのクリップへリンクする (移動に追従する)。
    transcriptionLinkClipId_ = linkClipId;
    transcriptionSourcePath_ = QString::fromStdWString(request.mediaPath.wstring());
    transcriptionSource_ = {};
    transcribing_ = true;
    const auto fpsNum = project_.timelineFpsNum, fpsDen = project_.timelineFpsDen;
    request.progress = [this](int progress) {
        QMetaObject::invokeMethod(
            this,
            [this, progress] {
                if (transcribing_) {
                    transcriptionProgress_ = progress;
                    Q_EMIT stateChanged();
                }
            },
            Qt::QueuedConnection);
    };
    try {
        transcriptionThread_ = std::thread(
            [this, request = std::move(request), fpsNum, fpsDen, offset, speedNum, speedDen,
             runner = transcriptionRunner_, hashObserver = transcriptionHashObserver_] {
                transcribe::Result result;
                // 認識した素材の出どころ。認識の前後で、実体 (volume・file ID)・size・更新時刻と
                // 内容全体の hash が一致することを確かめる。外部で同じ path の素材を差し替えると、
                // Project の revision
                // は変わらないので、ここで見ないと古い字幕を新しい素材へ適用する。
                // 内容を全部読むので worker で行う。数十 GB の素材でもキャンセル・終了を待たせない
                // よう、1 MiB ごとにキャンセルを見る。
                const auto stopHash = [this, &hashObserver] {
                    if (hashObserver)
                        hashObserver();
                    return transcriptionCancel_.load();
                };
                using HashStatus = MediaContentHashResult::Status;
                const auto cancelled = [&result] {
                    result = {};
                    result.cancelled = true;
                    result.error = "文字起こしをキャンセルしました";
                };
                const QString mediaPath = QString::fromStdWString(request.mediaPath.wstring());
                const auto before = probeMediaSource(mediaPath);
                const auto hashBefore = mediaContentHash(mediaPath, stopHash);
                if (hashBefore.status == HashStatus::Cancelled) {
                    cancelled();
                } else if (!before.identity.exists || hashBefore.status != HashStatus::Ok) {
                    result.error = "文字起こしの素材を読めません";
                } else {
                    try {
                        result = runner(request, &transcriptionCancel_);
                    } catch (const std::exception&) {
                        result.error = "文字起こし処理に失敗しました";
                    }
                    if (result.success) {
                        const auto after = probeMediaSource(mediaPath);
                        const auto hashAfter = mediaContentHash(mediaPath, stopHash);
                        if (hashAfter.status == HashStatus::Cancelled) {
                            cancelled();
                        } else if (after.key != before.key || after.identity != before.identity ||
                                   hashAfter.status != HashStatus::Ok ||
                                   hashAfter.hash != hashBefore.hash) {
                            result.success = false;
                            result.segments.clear();
                            result.error = "認識中に素材が変更されました。再実行してください";
                        }
                    }
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, result = std::move(result), fpsNum, fpsDen, offset, speedNum, speedDen,
                     source = before] {
                        transcriptionSource_ = source;
                        if (transcriptionThread_.joinable())
                            transcriptionThread_.join();
                        transcribing_ = false;
                        if (result.success && !transcriptionCancel_.load()) {
                            std::vector<project::SubtitleTimedText> segments;
                            for (const auto& segment : result.segments)
                                segments.push_back({segment.startMs, segment.endMs, segment.text});
                            std::string conversionError;
                            if (project::subtitleCuesFromTimedText(
                                    segments, fpsNum, fpsDen, speedNum, speedDen, offset, cueId,
                                    transcriptionCues_, conversionError)) {
                                for (auto& cue : transcriptionCues_)
                                    cue.linkClipId = transcriptionLinkClipId_;
                                transcriptionModel_.setCues(transcriptionCues_);
                                transcriptionProgress_ = 100;
                                if (!canApplyTranscription())
                                    transcriptionError_ = QStringLiteral(
                                        "認識中にプロジェクトが変更されました。再実行してください");
                            } else
                                transcriptionError_ = QString::fromStdString(conversionError);
                        } else
                            transcriptionError_ =
                                transcriptionCancel_.load()
                                    ? QStringLiteral("文字起こしをキャンセルしました")
                                    : QString::fromStdString(result.error);
                        Q_EMIT stateChanged();
                    },
                    Qt::QueuedConnection);
            });
    } catch (const std::exception&) {
        transcribing_ = false;
        transcriptionError_ = QStringLiteral("認識スレッドを起動できません");
        Q_EMIT stateChanged();
        return false;
    }
    Q_EMIT stateChanged();
    return true;
}

void MvmController::cancelTranscription() {
    transcriptionCancel_.store(true);
}

bool MvmController::updateTranscriptionCue(const QString& id, const QString& content, qint64 start,
                                           qint64 end) {
    if (!canApplyTranscription()) {
        setStatus(QStringLiteral("候補は編集できません。必要なら認識を再実行してください"));
        return false;
    }
    auto temporary = project_;
    temporary.subtitles.emplace();
    temporary.subtitles->cues = transcriptionCues_;
    std::string error;
    if (!project::editSubtitle(
            temporary,
            {id.toStdString(), start, end, content.toStdString(), transcriptionLinkClipId_},
            error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    transcriptionCues_ = temporary.subtitles->cues;
    transcriptionModel_.setCues(transcriptionCues_);
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::applyTranscription(bool replace) {
    if (!canApplyTranscription()) {
        transcriptionError_ =
            QStringLiteral("認識結果が無いか、プロジェクトが変更されています。再実行してください");
        Q_EMIT stateChanged();
        return false;
    }
    // 候補を確認している間に素材が差し替えられていないか。GUI thread で数 GB を読まないよう、
    // ここでは実体・size・更新時刻だけを見る (内容の hash は認識の前後で worker が見ている)。
    const auto current = probeMediaSource(transcriptionSourcePath_);
    if (!current.identity.exists || current.key != transcriptionSource_.key ||
        current.identity != transcriptionSource_.identity) {
        transcriptionError_ =
            QStringLiteral("認識した後に素材が変更されました。再実行してください");
        Q_EMIT stateChanged();
        return false;
    }
    auto candidate = project_;
    std::string error;
    if (!project::applySubtitleCues(candidate, transcriptionCues_, replace, error)) {
        setStatus(QString::fromStdString(error));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    transcriptionCues_.clear();
    transcriptionModel_.setCues({});
    Q_EMIT stateChanged();
    return true;
}
} // namespace mvm::app

namespace mvm::app {
// timeline で選んだ字幕を clip と同じショートカットで扱う (Delete・Ctrl+C/X/V/D)。
bool MvmController::copySelectedSubtitles(bool cut) {
    if (busy_ || !project_.subtitles || selectedSubtitleIds_.empty()) {
        setStatus(QStringLiteral("コピーする字幕がありません"));
        return false;
    }
    std::vector<project::SubtitleCue> copied;
    for (const auto& cue : project_.subtitles->cues)
        if (std::find(selectedSubtitleIds_.begin(), selectedSubtitleIds_.end(), cue.id) !=
            selectedSubtitleIds_.end())
            copied.push_back(cue);
    if (copied.size() != selectedSubtitleIds_.size()) {
        setStatus(QStringLiteral("選択した字幕がProjectにありません"));
        return false;
    }
    // 先頭の字幕からの相対位置で持つ。貼り付けた字幕は元の clip へリンクしない
    // (元の clip の移動に、別の位置へ貼った字幕まで追従させない)。
    const auto origin = copied.front().startFrame;
    for (auto& cue : copied) {
        cue.startFrame -= origin;
        cue.endFrame -= origin;
        cue.linkClipId.clear();
    }
    if (cut && !deleteSelectedSubtitles())
        return false;
    subtitleClipboard_ = std::move(copied);
    clipboardHoldsSubtitles_ = true;
    setStatus(QString::number(subtitleClipboard_.size()) +
              (cut ? QStringLiteral("件の字幕をカットしました")
                   : QStringLiteral("件の字幕をコピーしました")));
    return true;
}

bool MvmController::pasteSubtitles(std::int64_t frame) {
    if (subtitleClipboard_.empty()) {
        setStatus(QStringLiteral("ペーストする字幕がありません"));
        return false;
    }
    auto cues = subtitleClipboard_;
    std::vector<std::string> ids;
    for (auto& cue : cues) {
        cue.id = cueId();
        cue.startFrame += frame;
        cue.endFrame += frame;
        ids.push_back(cue.id);
    }
    auto candidate = project_;
    std::string error;
    if (!project::applySubtitleCues(candidate, std::move(cues), false, error)) {
        // 重なりは「既存の字幕と重なる」と分かる文で返す。
        setStatus(QStringLiteral("ペースト先が既存の字幕と重なっています"));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    setSubtitleSelection(std::move(ids), {});
    setStatus(QString::number(selectedSubtitleIds_.size()) +
              QStringLiteral("件の字幕をペーストしました"));
    Q_EMIT stateChanged();
    return true;
}

bool MvmController::deleteSelectedSubtitles() {
    if (!project_.subtitles || selectedSubtitleIds_.empty()) {
        setStatus(QStringLiteral("削除する字幕がありません"));
        return false;
    }
    auto candidate = project_;
    const auto removed = std::erase_if(candidate.subtitles->cues, [&](const auto& cue) {
        return std::find(selectedSubtitleIds_.begin(), selectedSubtitleIds_.end(), cue.id) !=
               selectedSubtitleIds_.end();
    });
    if (removed != selectedSubtitleIds_.size()) {
        setStatus(QStringLiteral("選択した字幕がProjectにありません"));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    setSubtitleSelection({}, {});
    setStatus(QString::number(removed) + QStringLiteral("件の字幕を削除しました"));
    Q_EMIT stateChanged();
    return true;
}
} // namespace mvm::app

namespace mvm::app {
bool MvmController::placeTimelineSubtitles(const QString& anchorId, qint64 startFrame,
                                           bool duplicate) {
    if (busy_ || playing() || !project_.subtitles)
        return false;
    const auto anchor = anchorId.toStdString();
    // clip と同じく、選択中の字幕を掴んだら選択全体を、選択外なら掴んだ字幕だけを動かす。
    auto targets = selectedSubtitleIds_;
    if (std::find(targets.begin(), targets.end(), anchor) == targets.end())
        targets = {anchor};
    auto candidate = project_;
    auto& cues = candidate.subtitles->cues;
    const auto found =
        std::find_if(cues.begin(), cues.end(), [&](const auto& cue) { return cue.id == anchor; });
    if (found == cues.end()) {
        setStatus(QStringLiteral("動かす字幕がありません"));
        return false;
    }
    std::int64_t minimumStart = std::numeric_limits<std::int64_t>::max();
    for (const auto& cue : cues)
        if (std::find(targets.begin(), targets.end(), cue.id) != targets.end())
            minimumStart = std::min(minimumStart, cue.startFrame);
    // 最も左の字幕が 0 に接する位置で止め、全字幕へ同じ量を適用する (clip の移動と同じ)。
    const auto delta = std::max<std::int64_t>(startFrame - found->startFrame, -minimumStart);
    if (delta == 0)
        return true;
    std::vector<std::string> placedIds;
    std::vector<project::SubtitleCue> copies;
    for (auto& cue : cues) {
        if (std::find(targets.begin(), targets.end(), cue.id) == targets.end())
            continue;
        if (duplicate) {
            auto copy = cue;
            copy.id = cueId();
            copy.linkClipId.clear();
            copy.startFrame += delta;
            copy.endFrame += delta;
            placedIds.push_back(copy.id);
            copies.push_back(std::move(copy));
        } else {
            cue.startFrame += delta;
            cue.endFrame += delta;
            placedIds.push_back(cue.id);
        }
    }
    std::string error;
    if (!project::applySubtitleCues(candidate, std::move(copies), false, error)) {
        setStatus(duplicate ? QStringLiteral("複製先が既存の字幕と重なっています")
                            : QStringLiteral("移動先が他の字幕と重なっています"));
        return false;
    }
    if (!commitSubtitleEdit(std::move(candidate)))
        return false;
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    setSubtitleSelection(std::move(placedIds), duplicate ? std::string{} : anchor);
    setStatus(QString::number(selectedSubtitleIds_.size()) +
              (duplicate ? QStringLiteral("件の字幕を複製しました")
                         : QStringLiteral("件の字幕を移動しました")));
    Q_EMIT stateChanged();
    return true;
}
} // namespace mvm::app

namespace mvm::app {
bool MvmController::selectTimelineSubtitlesInRange(qint64 fromFrame, qint64 toFrame) {
    // S1 の矩形選択。区間 [fromFrame, toFrame] に掛かる字幕をすべて選ぶ。表示範囲の外の字幕も
    // 含める (delegate は表示範囲の字幕にしか無いので、Project から求める)。clip の選択は外す。
    if (toFrame < fromFrame)
        std::swap(fromFrame, toFrame);
    std::vector<std::string> ids;
    if (project_.subtitles)
        for (const auto& cue : project_.subtitles->cues)
            if (cue.startFrame <= toFrame && cue.endFrame > fromFrame)
                ids.push_back(cue.id);
    setTimelineSelection({}, false);
    setCurrentClipSelection(-1);
    setSubtitleSelection(std::move(ids), {});
    setStatus(selectedSubtitleIds_.empty() ? QStringLiteral("字幕の選択を解除しました")
                                           : QString::number(selectedSubtitleIds_.size()) +
                                                 QStringLiteral("件の字幕を選択しました"));
    Q_EMIT stateChanged();
    return true;
}
} // namespace mvm::app
