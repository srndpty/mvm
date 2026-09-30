// トランジションを実際の preview engine で表示・再生できることを検査する。
//
// 同じ素材を 2 つに分けた clip の cut に前後 10 frame のトランジションを置く (分割してから
// ディゾルブする普通の使い方)。区間の中では同じ素材の decode source が 2 つ同時に要る。
// 実際の preview surface (D3D11) を付けた controller で
//   - 区間の中へ seek すると 2 layer を合成し、incoming の不透明度が進み具合になること
//   - 区間を通して再生が止まらずに進むこと (区間の出入りで source を組み直す)
// を見る。音声も同じ WAV を 2 つに分けてクロスフェードさせる。期待する値は直書きする。
#include "app/preview/preview_engine_rhi_item.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>

namespace {

int failures = 0;

bool crossesBoundaryWithoutPairingMiss(const std::vector<std::int64_t>& presented,
                                       std::size_t presentedFrom,
                                       const std::vector<std::int64_t>& unpaired,
                                       std::size_t unpairedFrom, std::int64_t boundary) {
    const auto first = presented.begin() + static_cast<std::ptrdiff_t>(presentedFrom);
    const bool before =
        std::any_of(first, presented.end(), [&](std::int64_t frame) { return frame < boundary; });
    const bool after =
        std::any_of(first, presented.end(), [&](std::int64_t frame) { return frame >= boundary; });
    const bool missed = std::any_of(
        unpaired.begin() + static_cast<std::ptrdiff_t>(unpairedFrom), unpaired.end(),
        [&](std::int64_t frame) { return frame >= boundary - 1 && frame <= boundary + 1; });
    if (!before || !after || missed)
        std::fprintf(
            stderr,
            "境界 %lld の前後提示またはpairingに失敗しました (前=%d 後=%d pairing欠落=%d)\n",
            static_cast<long long>(boundary), before, after, missed);
    if (missed)
        for (auto it = unpaired.begin() + static_cast<std::ptrdiff_t>(unpairedFrom);
             it != unpaired.end(); ++it)
            if (*it >= boundary - 1 && *it <= boundary + 1)
                std::fprintf(stderr, "  pairing欠落 frame=%lld\n", static_cast<long long>(*it));
    return before && after && !missed;
}

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents();
    return predicate();
}

// 副作用のある操作 (seek・再生) を受理されるまで繰り返す。pumpUntil は最後にもう一度 predicate
// を呼ぶので、受理された操作を重ねて呼ばないよう結果を覚えておく。
bool retryUntilAccepted(const std::function<bool()>& operation, int timeoutMs) {
    bool accepted = false;
    pumpUntil([&] { return accepted || (accepted = operation()); }, timeoutMs);
    return accepted;
}

std::filesystem::path fromUtf8(const char* text) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(text));
}

mvm::project::TimelineClip half(const std::filesystem::path& media, const char* id,
                                mvm::project::TrackKind kind, std::int64_t start,
                                std::int64_t sourceIn) {
    mvm::project::TimelineClip clip;
    clip.kind = kind == mvm::project::TrackKind::Video ? mvm::project::TimelineClipKind::Video
                                                       : mvm::project::TimelineClipKind::Audio;
    clip.id = id;
    clip.name = id;
    clip.mediaPath = media;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 300;
    clip.sourceInFrame = sourceIn;
    clip.sourceOutFrame = sourceIn + 120;
    clip.timelineStartFrame = start;
    clip.track = {kind, 0};
    return clip;
}

// cut = 120、区間 [110, 130)。
constexpr qint64 kInsideFrame = 125; // p = (125 - 110 + 0.5) / 20 = 0.775
constexpr float kInsideOpacity = 0.775F;

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "使い方: mvm_test_transition_preview <video 5s 60fps> <wav 5s>\n");
        return 2;
    }
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "FAIL: MLT runtime を初期化できません\n");
        return 3;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::fprintf(stderr, "FAIL: 一時 directory を作れません\n");
        return 3;
    }
    const auto video = fromUtf8(argv[1]);
    const auto wav = fromUtf8(argv[2]);
    using mvm::project::TrackKind;
    auto project = mvm::project::createDefaultProject();
    project.timelineClips = {half(video, "v-out", TrackKind::Video, 0, 0),
                             half(video, "v-in", TrackKind::Video, 120, 120),
                             half(wav, "a-out", TrackKind::Audio, 0, 0),
                             half(wav, "a-in", TrackKind::Audio, 120, 120)};
    project.timelineTransitions = {{"dissolve", "v-out", "v-in", 10, 10},
                                   {"crossfade", "a-out", "a-in", 10, 10}};
    if (!mvm::project::validateTimeline(project).success) {
        std::fprintf(stderr, "FAIL: 試験の timeline が不正です\n");
        return 3;
    }
    const std::filesystem::path projectPath =
        directory.filePath(QStringLiteral("transition.mvm")).toStdWString();

    int exitCode = 0;
    {
        mvm::app::MvmController controller(projectPath, {}, project);
        QQuickWindow window;
        window.setWidth(640);
        window.setHeight(360);
        auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
        surface->setWidth(640);
        surface->setHeight(360);
        window.show();
        controller.attachPreview(surface);

        const auto run = [&]() -> int {
            if (!pumpUntil([&] { return controller.previewReady(); }, 30000)) {
                std::fprintf(stderr, "FAIL: preview が準備できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            // 起動直後の初回 seek が終わるまで受理しないことがある。
            if (!retryUntilAccepted([&] { return controller.seekTimelineFrame(kInsideFrame); },
                                    30000)) {
                std::fprintf(stderr, "FAIL: トランジションの区間へ seek できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 1;
            }
            pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
            const auto inside = controller.submittedLayerOpacities();
            check(controller.previewPresentedLatest(), "区間の中の frame を提示し終えません");
            check(inside.size() == 2 && std::abs(inside[0] - 1.0F) < 1e-6F &&
                      std::abs(inside[1] - kInsideOpacity) < 1e-6F,
                  "区間の中で outgoing の上に incoming を進み具合の不透明度で重ねません");

            if (!retryUntilAccepted([&] { return controller.seekTimelineFrame(135); }, 10000)) {
                std::fprintf(stderr, "FAIL: 区間の後へ seek できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 1;
            }
            pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
            check(controller.submittedLayerOpacities().size() == 1,
                  "区間の後で incoming だけを表示しません");

            // 区間の前から再生し、区間を通り抜けても再生が続くこと。
            if (!retryUntilAccepted([&] { return controller.seekTimelineFrame(90); }, 10000) ||
                !pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000) ||
                !retryUntilAccepted([&] { return controller.playTimeline(); }, 30000)) {
                std::fprintf(stderr, "FAIL: 区間の前から再生できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 1;
            }
            const auto before = controller.previewTelemetry();
            const auto beforeEvents = controller.presentedFrameHistoryForTest().size();
            const auto beforeUnpaired = controller.unpairedFrameHistoryForTest().size();
            const bool passed = pumpUntil(
                [&] { return !controller.playing() || controller.playheadFrame() >= 150; }, 15000);
            check(passed && controller.playing(), "トランジションの区間を通して再生が続きません");
            const auto after = controller.previewTelemetry();
            check(controller.playbackRebuildCount() == 0,
                  "トランジションの境界でPreviewを組み直しました");
            check(after.presentedFrameCount > before.presentedFrameCount,
                  "トランジションを通してframeを提示しませんでした");
            const auto history = controller.presentedFrameHistoryForTest();
            const auto unpaired = controller.unpairedFrameHistoryForTest();
            check(crossesBoundaryWithoutPairingMiss(history, beforeEvents, unpaired, beforeUnpaired,
                                                    110) &&
                      crossesBoundaryWithoutPairingMiss(history, beforeEvents, unpaired,
                                                        beforeUnpaired, 130),
                  "トランジションの境界で提示またはpairingが途切れました");
            std::printf(
                "再生: playhead %lld、提示 %llu、drop %llu、組み直し %llu、準備最大 "
                "%.1fms、status: %s\n",
                static_cast<long long>(controller.playheadFrame()),
                static_cast<unsigned long long>(after.presentedFrameCount -
                                                before.presentedFrameCount),
                static_cast<unsigned long long>(after.droppedFrameCount - before.droppedFrameCount),
                static_cast<unsigned long long>(controller.playbackRebuildCount()),
                controller.playbackMaxPreparationMs(),
                controller.statusText().toUtf8().constData());
            controller.pauseTimeline();
            return failures == 0 ? 0 : 1;
        };
        exitCode = run();
        controller.shutdown();
    }
    const auto copiedVideo = video.parent_path() / "v1080p60_hevc.mp4";
    const auto copiedWav =
        video.parent_path() /
        fromUtf8("素材/日本語 テスト/第1回　微分積分＆演習/ナレーション　音声.wav");
    if (!std::filesystem::is_regular_file(copiedVideo) ||
        !std::filesystem::is_regular_file(copiedWav)) {
        std::fprintf(stderr, "FAIL: 別ファイルの cut 試験素材がありません\n");
        exitCode = 1;
    } else {
        const auto runCut = [&](const char* label, bool firstAudio, bool secondAudio,
                                bool forceCapacity) {
            auto cutProject = mvm::project::createDefaultProject();
            cutProject.timelineClips = {half(video, "cut-v-out", TrackKind::Video, 0, 0),
                                        half(copiedVideo, "cut-v-in", TrackKind::Video, 120, 120)};
            if (firstAudio)
                cutProject.timelineClips.push_back(half(wav, "cut-a-out", TrackKind::Audio, 0, 0));
            if (secondAudio)
                cutProject.timelineClips.push_back(
                    half(copiedWav, "cut-a-in", TrackKind::Audio, 120, 120));
            check(mvm::project::validateTimeline(cutProject).success,
                  "cut 試験の timeline が不正です");
            const auto cutPath = std::filesystem::path(
                directory.filePath(QString::fromUtf8(label) + QStringLiteral(".mvm"))
                    .toStdWString());
            mvm::app::MvmController controller(cutPath, {}, cutProject);
            QQuickWindow window;
            window.setWidth(640);
            window.setHeight(360);
            auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
            surface->setWidth(640);
            surface->setHeight(360);
            window.show();
            controller.attachPreview(surface);
            const bool ready = pumpUntil([&] { return controller.previewReady(); }, 30000);
            const bool sought =
                ready &&
                retryUntilAccepted([&] { return controller.seekTimelineFrame(90); }, 30000) &&
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
            const bool limited =
                !forceCapacity || (sought && controller.setPreviewRegistrationLimitForTest(1));
            const bool started =
                sought && limited &&
                retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            check(started, "cut の前から再生を開始できません");
            if (started) {
                const auto before = controller.previewTelemetry();
                const auto beforeEvents = controller.presentedFrameHistoryForTest().size();
                const auto beforeUnpaired = controller.unpairedFrameHistoryForTest().size();
                const bool passed =
                    pumpUntil([&] { return controller.playheadFrame() >= 150; }, 15000);
                const auto after = controller.previewTelemetry();
                check(passed && controller.playing(), "cut を通して再生が続きません");
                if (forceCapacity) {
                    check(controller.playbackRebuildCount() > 0 &&
                              controller.lastPlaybackRebuildReason().contains(
                                  QStringLiteral("登録上限")),
                          "登録上限で理由付きの組み直しに戻りませんでした");
                } else {
                    check(controller.playbackRebuildCount() == 0,
                          "cut の境界でPreviewを組み直しました");
                }
                check(after.presentedFrameCount > before.presentedFrameCount,
                      "cut の前後でframeを提示しませんでした");
                if (!forceCapacity)
                    check(crossesBoundaryWithoutPairingMiss(
                              controller.presentedFrameHistoryForTest(), beforeEvents,
                              controller.unpairedFrameHistoryForTest(), beforeUnpaired, 120),
                          "cut の境界で提示またはpairingが途切れました");
                std::printf("%s: 提示 %llu、drop %llu、組み直し %llu、準備最大 %.1fms、理由: "
                            "%s、status: %s\n",
                            label,
                            static_cast<unsigned long long>(after.presentedFrameCount -
                                                            before.presentedFrameCount),
                            static_cast<unsigned long long>(after.droppedFrameCount -
                                                            before.droppedFrameCount),
                            static_cast<unsigned long long>(controller.playbackRebuildCount()),
                            controller.playbackMaxPreparationMs(),
                            controller.lastPlaybackRebuildReason().toUtf8().constData(),
                            controller.statusText().toUtf8().constData());
                controller.pauseTimeline();
                if (firstAudio && secondAudio) {
                    const bool resumed =
                        retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
                    const bool advanced =
                        resumed && pumpUntil(
                                       [&] {
                                           return !controller.playing() ||
                                                  controller.playheadFrame() >= 175;
                                       },
                                       15000);
                    check(advanced && controller.playing(),
                          "主音声の交代後に一時停止から再生できません");
                    if (controller.playing())
                        controller.pauseTimeline();
                }
            }
            controller.shutdown();
        };
        runCut("cut-av", true, true, false);
        runCut("cut-silent-to-audio", false, true, false);
        runCut("cut-audio-to-silent", true, false, false);
        runCut("cut-capacity-fallback", false, false, true);
        if (failures != 0)
            exitCode = 1;
    }
    mvm_mlt_runtime_shutdown();
    if (exitCode == 0)
        std::puts("トランジションを preview で表示・再生できることを確認しました");
    return exitCode;
}
