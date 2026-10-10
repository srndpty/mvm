#include "mvm_controller.h"
#include "mvm_controller_detail.h"

#include "app/math_clip_render.h"
#include "core/export_eta.h"
#include "media/graph_manim/graph_manim_backend.h"

#include <algorithm>
#include <set>
#include <system_error>
#include <tuple>
#include <QMetaObject>

namespace mvm::app {
using detail::fromPath;
using detail::probeMedia;
using detail::ProbedMedia;

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
    // 受理できない要求 (clip が無い・ローカルでない書き出し先) では再生を止めない。
    if (totalTimelineFrames_ == 0) {
        reportExportFailure(QStringLiteral("書き出すclipがありません"));
        return false;
    }
    if (!outputUrl.isLocalFile()) {
        reportExportFailure(QStringLiteral("ローカルの書き出し先を指定してください"));
        return false;
    }
    if (!pauseTimeline()) {
        reportExportFailure(statusText_);
        return false;
    }

    TimelineExportRequest request;
    request.outputPath = std::filesystem::path(outputUrl.toLocalFile().toStdWString());
    request.width = project_.outputWidth;
    request.height = project_.outputHeight;
    request.fpsNum = static_cast<int>(project_.timelineFpsNum);
    request.fpsDen = static_cast<int>(project_.timelineFpsDen);
    request.videoCrf = videoCrf;
    request.burnSubtitles = burnSubtitles_;
    request.renderThreads = 4;
    request.encoderThreads = 0;
    request.graphEnvironment.cache = mathCacheDirectory().parent_path().parent_path() / L"graph" /
                                     projectPath_.filename();
    request.graphEnvironment.toolchain = graphRasters_ ? graphRasters_->exportToolchain() : "";
    request.graphEnvironment.cancel = &exportCancelRequested_;
    if (projectLockHeld_)
        request.graphEnvironment.preflight =
            [python = manimExecutablePath_.parent_path() / L"python.exe"](
                const std::filesystem::path& work, const std::atomic<bool>* cancel) {
                return graph_manim::preflightGraph(python, MVM_GRAPH_BACKEND_SCRIPT, work, cancel);
            };
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind != project::TimelineClipKind::EquationSequence || !clip.enabled ||
            !project::isTrackOutputEnabled(project_, clip.track))
            continue;
        EquationExportSnapshot snapshot;
        if (mathRasters_)
            snapshot = mathRasters_->equationSequenceExportSnapshot(clip);
        else
            snapshot.readiness = {EquationExportFailure::BackendUnavailable,
                                  EquationCompileFailure::None, "数式の描画環境を利用できません"};
        request.equationSequences.emplace(clip.id, std::move(snapshot));
    }
    // 出力する数式 clip は、現在の式の描画が済んでいなければ書き出さない。描き直し中に
    // 見せている古い描画 (last-good) では書き出さない (fail-closed)。
    for (const auto& clip : project_.timelineClips) {
        if (clip.kind != project::TimelineClipKind::Math || !clip.enabled ||
            !project::isTrackOutputEnabled(project_, clip.track))
            continue;
        const auto artifact =
            mathRasters_ ? mathRasters_->readyArtifact(mathRenderSpecFor(clip.math)) : std::nullopt;
        if (!artifact) {
            const auto status = mathClipData(QString::fromStdString(clip.id));
            QString reason = status.value(QStringLiteral("message")).toString();
            if (reason.isEmpty())
                reason = QStringLiteral("描画中です。終わってから書き出してください");
            reportExportFailure(QStringLiteral("数式 clip '") + QString::fromStdString(clip.name) +
                                QStringLiteral("' の描画が完了していません: ") + reason);
            return false;
        }
        request.mathArtifacts.emplace(clip.id, *artifact);
        // Write の連番も現在の式・尺のものが描けていなければ書き出さない (静止で代用しない)。
        if (const auto write = mathSequenceSpecFor(clip)) {
            const auto [writeState, writeMessage] = mathWriteState(clip);
            const auto sequence = writeState == QStringLiteral("ready") && mathRasters_
                                      ? mathRasters_->readySequence(*write)
                                      : std::nullopt;
            if (!sequence) {
                const QString reason =
                    writeMessage.isEmpty()
                        ? QStringLiteral("描画中です。終わってから書き出してください")
                        : writeMessage;
                reportExportFailure(
                    QStringLiteral("数式 clip '") + QString::fromStdString(clip.name) +
                    QStringLiteral("' の Write の描画が完了していません: ") + reason);
                return false;
            }
            request.mathWriteFrames.emplace(clip.id, sequence->frames);
        }
    }

    for (const auto& transition : project_.timelineTransitions) {
        if (!mathTransformIsRendered(project_, transition))
            continue;
        const project::ClipIdIndex clipIndex(project_);
        const auto& source =
            project_
                .timelineClips[static_cast<std::size_t>(clipIndex.find(transition.outgoingClipId))];
        const auto& target =
            project_
                .timelineClips[static_cast<std::size_t>(clipIndex.find(transition.incomingClipId))];
        const auto spec = mathTransformSpecFor(transition, source, target);
        const auto artifact =
            spec && mathRasters_ ? mathRasters_->readyTransformForExport(*spec) : std::nullopt;
        if (!artifact) {
            reportExportFailure(QStringLiteral("数式の変形の現在の disk 成果物を検証できません"));
            return false;
        }
        TimelineMathTransformArtifact input;
        input.spec = *spec;
        input.width = artifact->width;
        input.height = artifact->height;
        input.sourceX = artifact->sourceX;
        input.sourceY = artifact->sourceY;
        input.targetX = artifact->targetX;
        input.targetY = artifact->targetY;
        input.frames = static_cast<std::int64_t>(artifact->frames.size());
        input.loadFrame = [artifact = *artifact](std::size_t index,
                                                 std::vector<std::uint8_t>& bytes,
                                                 std::string& error) {
            return loadMathTransformFrame(artifact, index, bytes, error);
        };
        if (mathTransformExportFrameLoaderForTest_)
            input.loadFrame = mathTransformExportFrameLoaderForTest_;
        request.mathTransforms.emplace(transition.id, std::move(input));
    }

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

void MvmController::reportExportFailure(QString message) {
    setStatus(message);
    Q_EMIT exportFailed(message);
}

} // namespace mvm::app
