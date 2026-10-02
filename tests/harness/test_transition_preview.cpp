// トランジションを実際の preview engine で表示・再生できることを検査する。
//
// 同じ素材を 2 つに分けた clip の cut に前後 10 frame のトランジションを置く (分割してから
// ディゾルブする普通の使い方)。区間の中では同じ素材の decode source が 2 つ同時に要る。
// 実際の preview surface (D3D11) を付けた controller で
//   - 区間の中へ seek すると 2 layer を合成し、incoming の不透明度が進み具合になること
//   - 区間を通して再生が止まらずに進むこと (区間の出入りで source を組み直す)
// を見る。音声も同じ WAV を 2 つに分けてクロスフェードさせる。期待する値は直書きする。
#include "app/preview/preview_engine_rhi_item.h"
#include "media/mlt/mvm_mlt_probe.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "preview_engine/preview_engine_internal.h"
#include "test_media_fixture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <process.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTimer>

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

// cut の前後で続けて提示した frame の差の上限。先読みした音声の時計へ早く切り替えると、提示は
// 境界まで (先読み幅 2 秒 = 120 frame 以内の) 数十 frame 飛ぶ。scheduler の数 frame の drop
// と分ける。
constexpr std::int64_t kMaxPresentedStep = 10;
// 1 回の source 準備にかかってよい時間。controller は境界の 2 秒前から準備するので、その半分に
// 収まれば境界までに準備が終わる。先読み幅の根拠を文書の転記ではなくこの検査で持つ。
constexpr double kMaxPreparationMs = 1000.0;

struct PresentedSteps {
    // 続けて提示した 2 つの output frame の差の最大 (先へ進んだ量)。
    std::int64_t maxForward = 0;
    // 前に提示した frame より前の frame を提示した (時計の切り替えで戻った)。
    bool backward = false;
};

// from 以上で最初に提示した frame から、to を最初に越えた frame までの続けて提示した組を調べる。
// to を越えた frame も組に入れる。範囲内の frame だけを見ると、範囲の外への飛び (100 -> 170) を
// 見逃す。時計が先へ飛ぶと maxForward が大きくなり、戻ると backward になる。
PresentedSteps presentedSteps(const std::vector<std::int64_t>& presented, std::size_t presentedFrom,
                              std::int64_t from, std::int64_t to) {
    PresentedSteps steps;
    std::optional<std::int64_t> previous;
    for (auto it = presented.begin() + static_cast<std::ptrdiff_t>(presentedFrom);
         it != presented.end(); ++it) {
        if (!previous) {
            if (*it >= from)
                previous = *it;
            continue;
        }
        if (*it < *previous) {
            steps.backward = true;
            std::fprintf(stderr, "  提示が戻りました: %lld -> %lld\n",
                         static_cast<long long>(*previous), static_cast<long long>(*it));
        } else {
            steps.maxForward = std::max(steps.maxForward, *it - *previous);
        }
        previous = *it;
        if (*it > to)
            break;
    }
    return steps;
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

// 再生中に提示した区間の frame が、2 layer で、incoming の不透明度を進み具合
// p = (f - 110 + 0.5) / 20 で上げていること。controller は tick の時点の frame で不透明度を
// 求めるので、提示との差を 3 frame (0.15) まで許す。区間に入った composition のまま
// 不透明度が上がらないと、outgoing が区間の終わりまで残って突然 incoming へ切り替わる。
bool dissolvesWhilePlaying(const std::vector<std::pair<std::int64_t, float>>& presented) {
    int inside = 0;
    float maxError = 0.0F;
    bool ok = true;
    for (const auto& [frame, opacity] : presented) {
        if (frame < 110 || frame >= 130)
            continue;
        ++inside;
        const float expected = (static_cast<float>(frame - 110) + 0.5F) / 20.0F;
        maxError = std::max(maxError, std::abs(opacity - expected));
        if (opacity < 0.0F || std::abs(opacity - expected) > 0.15F) {
            std::fprintf(stderr, "  frame %lld: incoming の不透明度 %.3f (期待 %.3f)\n",
                         static_cast<long long>(frame), static_cast<double>(opacity),
                         static_cast<double>(expected));
            ok = false;
        }
    }
    std::printf("再生中のディゾルブ: 区間の提示 %d frame、不透明度の最大誤差 %.3f\n", inside,
                static_cast<double>(maxError));
    // 60fps の再生で区間の 20 frame のうち半分以上は提示しているはず。
    if (inside < 10) {
        std::fprintf(stderr, "  区間の中で提示した frame が %d しかありません\n", inside);
        ok = false;
    }
    return ok;
}

// frame へ送り (フレーム送りと同じ一時停止中の seek)、提示した composition の layer 数と最背面の
// 素材 frame が、その frame の mapping (mapTimelinePreviewFrame) と一致するかを見る。
// 区間の出入りで違う clip の frame を 1 frame だけ出す不具合を検出する。
std::pair<mvm::app::MvmController::PresentedFrameForTest, bool>
stepAndCompare(mvm::app::MvmController& controller, const mvm::project::Project& project,
               qint64 frame) {
    const auto countBefore = controller.previewTelemetry().presentedFrameCount;
    if (!retryUntilAccepted([&] { return controller.seekTimelineFrame(frame); }, 10000)) {
        std::fprintf(stderr, "  frame %lld へ送れません: %s\n", static_cast<long long>(frame),
                     controller.statusText().toUtf8().constData());
        return {{}, false};
    }
    const bool presented = pumpUntil(
        [&] {
            return controller.previewPresentedLatest() &&
                   controller.lastPresentedFrameForTest().outputFrame == frame;
        },
        10000);
    // 送りの間に提示したもの全部 (目的の frame の前に別の frame を挟んでいないか)。
    const auto history = controller.presentedFramesForTest();
    const auto added = static_cast<std::size_t>(std::min<std::uint64_t>(
        controller.previewTelemetry().presentedFrameCount - countBefore, history.size()));
    bool onlyTarget = true;
    for (std::size_t index = history.size() - added; index < history.size(); ++index) {
        const auto& entry = history[index];
        if (entry.outputFrame != frame) {
            onlyTarget = false;
            std::printf("  途中で提示: frame %lld、layer %u、最背面の素材 frame %lld\n",
                        static_cast<long long>(entry.outputFrame), entry.layerCount,
                        static_cast<long long>(entry.baseSourceFrame));
        }
    }
    const auto shown = controller.lastPresentedFrameForTest();
    const auto mapped = mvm::app::mapTimelinePreviewFrame(project, frame);
    const std::int64_t expected =
        mapped.success && !mapped.layers.empty() ? mapped.layers.front().sourceFrameNumber : -1;
    const std::size_t expectedLayers = mapped.layers.size() + mapped.stillLayers.size();
    std::printf("フレーム送り %lld: 提示 %lld、layer %u (期待 %zu)、最背面の素材 frame %lld "
                "(期待 %lld)\n",
                static_cast<long long>(frame), static_cast<long long>(shown.outputFrame),
                shown.layerCount, expectedLayers, static_cast<long long>(shown.baseSourceFrame),
                static_cast<long long>(expected));
    return {shown, presented && onlyTarget && mapped.success &&
                       shown.layerCount == expectedLayers && shown.baseSourceFrame == expected};
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr,
                     "使い方: mvm_test_transition_preview <video 5s 60fps> <wav 5s> <ffmpeg>\n");
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
                             // incoming は素材の別の位置 (170) から使う。区間の前後で最背面に
                             // 出ている clip を素材 frame で見分けるため (A: f、B: f + 50)。
                             half(video, "v-in", TrackKind::Video, 120, 170),
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

            // フレーム送り: 区間の中から 1 frame ずつ送り、提示した frame の最背面の素材 frame が
            // outgoing (f) か incoming (f + 50) かを見る。区間 [110, 130) の最背面は outgoing、
            // 130 以降は incoming。区間の直後に outgoing を 1 frame 出していた不具合を検出する。
            if (!retryUntilAccepted([&] { return controller.seekTimelineFrame(126); }, 10000) ||
                !pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000)) {
                std::fprintf(stderr, "FAIL: フレーム送りの起点へ seek できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 1;
            }
            for (qint64 frame = 127; frame <= 134; ++frame) {
                const auto [shown, ok] = stepAndCompare(controller, project, frame);
                (void)shown;
                check(ok, "フレーム送りで mapping と違う frame を提示しました");
            }

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
            // CTest が渡す倍率 (tests/CMakeLists.txt) で、既定の音量より小さく鳴らしている。
            {
                const float scale =
                    qEnvironmentVariable("MVM_TEST_AUDIO_VOLUME_SCALE", QStringLiteral("1"))
                        .toFloat();
                const float endpoint = controller.audioEndpointVolumeForTest();
                const float expected = static_cast<float>(controller.masterVolume()) * scale;
                std::printf("音量: master %.3f x 倍率 %.3f = endpoint %.4f\n",
                            controller.masterVolume(), static_cast<double>(scale),
                            static_cast<double>(endpoint));
                check(std::abs(endpoint - expected) < 1e-4F &&
                          static_cast<double>(endpoint) <= controller.masterVolume() * 0.5,
                      "試験の音量を既定の半分以下へ下げていません");
            }
            const auto after = controller.previewTelemetry();
            check(controller.playbackRebuildCount() == 0,
                  "トランジションの境界でPreviewを組み直しました");
            check(after.presentedFrameCount > before.presentedFrameCount,
                  "トランジションを通してframeを提示しませんでした");
            const auto history = controller.presentedFrameHistoryForTest();
            const auto unpaired = controller.unpairedFrameHistoryForTest();
            check(dissolvesWhilePlaying(controller.presentedOverlayOpacityHistoryForTest()),
                  "再生中のトランジションで incoming の不透明度が進み具合で上がりません");
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
        // secondAudioIn は後ろの音声 clip の素材 in。0
        // は後ろに置いた素材を先頭から使う普通の置き方で、 先読みした時点の素材位置が負になる
        // (開始前に時計を切り替えると映像が境界まで飛ぶ)。
        const auto runCut = [&](const char* label, bool firstAudio, bool secondAudio,
                                std::int64_t secondAudioIn, bool forceCapacity) {
            auto cutProject = mvm::project::createDefaultProject();
            cutProject.timelineClips = {half(video, "cut-v-out", TrackKind::Video, 0, 0),
                                        half(copiedVideo, "cut-v-in", TrackKind::Video, 120, 120)};
            if (firstAudio)
                cutProject.timelineClips.push_back(half(wav, "cut-a-out", TrackKind::Audio, 0, 0));
            if (secondAudio)
                cutProject.timelineClips.push_back(
                    half(copiedWav, "cut-a-in", TrackKind::Audio, 120, secondAudioIn));
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
                    check(controller.playbackCapacityResetCount() > 0,
                          "登録上限を登録枠の不足として扱わず、engine を作り直しませんでした");
                } else {
                    check(controller.playbackRebuildCount() == 0,
                          "cut の境界でPreviewを組み直しました");
                }
                check(after.presentedFrameCount > before.presentedFrameCount,
                      "cut の前後でframeを提示しませんでした");
                // 組み直しの負例は seek を挟むので、提示の連続性は見ない。
                PresentedSteps steps;
                if (!forceCapacity) {
                    steps = presentedSteps(controller.presentedFrameHistoryForTest(), beforeEvents,
                                           90, 150);
                    check(crossesBoundaryWithoutPairingMiss(
                              controller.presentedFrameHistoryForTest(), beforeEvents,
                              controller.unpairedFrameHistoryForTest(), beforeUnpaired, 120),
                          "cut の境界で提示またはpairingが途切れました");
                    // 先読みした音声の時計へ境界の前に切り替えると、提示が境界の先まで飛ぶ。
                    // 時計の切り替えで戻るのも検出する。
                    check(steps.maxForward <= kMaxPresentedStep,
                          "cut の前後で提示した frame が飛びました");
                    check(!steps.backward, "cut の前後で提示した frame が戻りました");
                    check(controller.playbackMaxPreparationMs() < kMaxPreparationMs,
                          "source の準備が先読み幅の半分を超えました");
                }
                // 映像だけの区間から audio の区間へ入ると、engine は最初の audio を公開するときに
                // WASAPI endpoint を control thread で open する (準備用の thread へ移せない)。
                // その時間を記録する (閾値は置かない)。
                const auto endpoint = mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(
                    *controller.previewEngineForTest());
                if (std::string(label) == "cut-silent-to-audio")
                    check(endpoint.playingAudioEndpointOpenCount >= 1 &&
                              endpoint.playingAudioEndpointOpenAttemptCount >=
                                  endpoint.playingAudioEndpointOpenCount &&
                              endpoint.maxPlayingAudioEndpointOpenAttemptMs >=
                                  endpoint.maxPlayingAudioEndpointOpenMs,
                          "前提: 再生中に最初の audio の endpoint を open していません");
                std::printf("%s: 提示 %llu、drop %llu、最大の提示間隔 %lld frame、組み直し %llu、"
                            "準備最大 %.1fms、再生中の endpoint open %llu 回 (最大 %.1fms)、"
                            "理由: %s、status: %s\n",
                            label,
                            static_cast<unsigned long long>(after.presentedFrameCount -
                                                            before.presentedFrameCount),
                            static_cast<unsigned long long>(after.droppedFrameCount -
                                                            before.droppedFrameCount),
                            static_cast<long long>(steps.maxForward),
                            static_cast<unsigned long long>(controller.playbackRebuildCount()),
                            controller.playbackMaxPreparationMs(),
                            static_cast<unsigned long long>(endpoint.playingAudioEndpointOpenCount),
                            endpoint.maxPlayingAudioEndpointOpenMs,
                            controller.lastPlaybackRebuildReason().toUtf8().constData(),
                            controller.statusText().toUtf8().constData());
                controller.pauseTimeline();
                if (secondAudio) {
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
                          "音声の区間に入った後に一時停止から再生できません");
                    if (controller.playing())
                        controller.pauseTimeline();
                }
            }
            controller.shutdown();
        };
        runCut("cut-av", true, true, 120, false);
        runCut("cut-silent-to-audio", false, true, 120, false);
        runCut("cut-silent-to-head-audio", false, true, 0, false);
        runCut("cut-audio-to-silent", true, false, 120, false);
        runCut("cut-capacity-fallback", false, false, 120, true);

        // 登録枠の不足ではない UnsupportedCapability (audio を扱えない構成) は、作り直しても
        // 直らない。clip 境界で audio source を準備できなくても、登録枠の不足として engine を
        // 作り直さない。上の cut-capacity-fallback が、登録枠の不足なら作り直す対照になる。
        {
            auto audioProject = mvm::project::createDefaultProject();
            audioProject.timelineClips = {
                half(video, "noaudio-v-out", TrackKind::Video, 0, 0),
                half(copiedVideo, "noaudio-v-in", TrackKind::Video, 120, 120),
                half(copiedWav, "noaudio-a-in", TrackKind::Audio, 120, 120)};
            check(mvm::project::validateTimeline(audioProject).success,
                  "audio 無効化試験の timeline が不正です");
            const auto audioPath = std::filesystem::path(
                directory.filePath(QStringLiteral("cut-audio-unsupported.mvm")).toStdWString());
            mvm::app::MvmController controller(audioPath, {}, audioProject);
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
            const bool disabled = sought && controller.disablePreviewAudioSourcesForTest();
            const bool started =
                disabled && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            check(started, "audio を無効にした engine で cut の前から再生を開始できません");
            if (started) {
                pumpUntil(
                    [&] {
                        return controller.playbackRebuildCount() > 0 ||
                               controller.playheadFrame() >= 150 || !controller.playing();
                    },
                    15000);
                // 境界を越えた後の組み直しの結果まで待つ。
                pumpUntil([&] { return !controller.playing(); }, 5000);
                const QString reason =
                    controller.lastPlaybackRebuildReason() + controller.statusText();
                check(reason.contains(QStringLiteral("扱えません")),
                      "audio を扱えない構成の失敗が境界で観測されませんでした");
                check(controller.playbackCapacityResetCount() == 0,
                      "audio を扱えない構成を登録枠の不足として engine を作り直しました");
                std::printf(
                    "cut-audio-unsupported: 組み直し %llu、作り直し %llu、理由: %s\n",
                    static_cast<unsigned long long>(controller.playbackRebuildCount()),
                    static_cast<unsigned long long>(controller.playbackCapacityResetCount()),
                    reason.toUtf8().constData());
                if (controller.playing())
                    controller.pauseTimeline();
            }
            controller.shutdown();
        }

        // 先読みの準備 (open / seek) は engine の準備用の thread で進み、完了は後から公開される。
        // 準備している間に transport や Project が変わったら、完了しても公開・使用しない。
        // 準備用の thread を open の前で止めておき、その間に変化を起こしてから再開する。
        enum class StaleCause {
            EnginePause,
            EngineSeek,
            ControllerPause,
            TrackEdit,
            TrackEditHeldToBoundary,
            EngineReset
        };
        const auto runStalePreparation = [&](const char* label, StaleCause cause) {
            auto staleProject = mvm::project::createDefaultProject();
            staleProject.timelineClips = {
                half(video, "stale-v-out", TrackKind::Video, 0, 0),
                half(copiedVideo, "stale-v-in", TrackKind::Video, 120, 120),
                half(copiedWav, "stale-a-in", TrackKind::Audio, 120, 120)};
            // 境界まで止める場合は映像だけにする。audio sink がまだ無いとき、始まった後に完了した
            // 主入力の audio は公開できない (sample の連続を要求する。§21.4) ので、境界で待つと
            // 準備し直しの成否に関係なく組み直しになる。
            const bool withAudio = cause != StaleCause::TrackEditHeldToBoundary;
            if (!withAudio)
                staleProject.timelineClips.pop_back();
            const std::size_t boundarySources = withAudio ? 2 : 1;
            // 編集の commit (保存の検証) は clip が素材を指すことを要求する。
            mvm::test::attachFixtureMedia(staleProject);
            check(mvm::project::validateTimeline(staleProject).success,
                  "先読みの負例の timeline が不正です");
            const auto stalePath = std::filesystem::path(
                directory.filePath(QString::fromUtf8(label) + QStringLiteral(".mvm"))
                    .toStdWString());
            mvm::app::MvmController controller(stalePath, {}, staleProject);
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
            controller.holdSourcePreparationsForTest(true);
            const bool started =
                sought && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            // 境界 (120) の video と audio の準備を要求し、準備用の thread が止まっている。
            const bool requested =
                started &&
                pumpUntil(
                    [&] { return controller.pendingSourcePreparationCount() == boundarySources; },
                    5000);
            check(requested,
                  (std::string(label) + ": 前提: 境界の source の準備を要求しません").c_str());
            if (!requested) {
                controller.holdSourcePreparationsForTest(false);
                controller.shutdown();
                return;
            }
            const auto engine = controller.previewEngineForTest();
            const auto diagnostics = [&] {
                return mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            };
            const auto publishedBefore = diagnostics().publishedSourceCount;
            const auto engineStaleBefore = diagnostics().staleSourcePreparationRejectCount;
            const auto controllerStaleBefore = controller.playbackStalePreparationCount();
            switch (cause) {
            case StaleCause::EnginePause:
                // controller を通さない (controller は準備を取り消さない)。engine
                // だけが古さを判定する。
                check(static_cast<bool>(engine->pause()), "前提: engine を pause できません");
                break;
            case StaleCause::EngineSeek:
                check(static_cast<bool>(engine->seek({100})), "前提: engine を seek できません");
                break;
            case StaleCause::ControllerPause:
                check(controller.pauseTimeline(), "前提: 再生を止められません");
                break;
            case StaleCause::TrackEdit:
            case StaleCause::TrackEditHeldToBoundary:
                // 再生を止めない編集 (空の V2 を隠す)。準備は変わる前の Project で決めた。
                check(controller.setTracksMuted(QStringLiteral("video"), {1}, true) &&
                          controller.playing(),
                      "前提: 再生中に track を隠せません");
                break;
            case StaleCause::EngineReset:
                // 準備用の thread が止まったままでも、shutdown が取り消して join する
                // (待ち続けない)。
                check(controller.resetPreviewEngineForTest(), "準備中の engine を作り直せません");
                check(controller.pendingSourcePreparationCount() == 0,
                      "作り直した後も古い engine の準備を持っています");
                break;
            }
            if (cause == StaleCause::TrackEditHeldToBoundary) {
                // 準備用の thread を止めたまま境界を越える (境界で完了を待たれた準備だけが進む)。
                // 変わる前の Project の準備が境界まで残っても、それを「準備中」と数えて新しい
                // 要求を出さないままにしない。変わった後の Project で要求し直し、境界で待って
                // 引き継ぐ (組み直さない)。
                const bool rerequested = pumpUntil(
                    [&] {
                        return controller.pendingSourcePreparationCount() == boundarySources &&
                               controller.playbackStalePreparationCount() >=
                                   controllerStaleBefore + boundarySources;
                    },
                    5000);
                check(rerequested, "Project が変わった後に境界の source を準備し直しません");
                pumpUntil(
                    [&] { return !controller.playing() || controller.playheadFrame() >= 150; },
                    15000);
                check(controller.playbackPreparationWaitCount() >= 1,
                      "前提: 境界で準備の完了を待っていません (止めた準備が境界まで残りません)");
            }
            controller.holdSourcePreparationsForTest(false);
            if (cause == StaleCause::EngineReset) {
                controller.shutdown();
                return;
            }
            if (cause == StaleCause::TrackEdit || cause == StaleCause::TrackEditHeldToBoundary) {
                // 古い完了は外し、変わった後の Project で準備し直して境界を越える。
                const bool crossed = pumpUntil(
                    [&] { return !controller.playing() || controller.playheadFrame() >= 150; },
                    15000);
                check(controller.playbackStalePreparationCount() >=
                          controllerStaleBefore + boundarySources,
                      "Project が変わる前に要求した準備の完了を使いました");
                check(crossed && controller.playing() && controller.playbackRebuildCount() == 0,
                      "Project が変わった後に準備し直して境界を越えられません");
                std::printf(
                    "%s: controller が捨てた準備 %llu、組み直し %llu、境界で待った準備 %llu、"
                    "理由: %s\n",
                    label,
                    static_cast<unsigned long long>(controller.playbackStalePreparationCount() -
                                                    controllerStaleBefore),
                    static_cast<unsigned long long>(controller.playbackRebuildCount()),
                    static_cast<unsigned long long>(controller.playbackPreparationWaitCount()),
                    controller.lastPlaybackRebuildReason().toUtf8().constData());
                controller.pauseTimeline();
                controller.shutdown();
                return;
            }
            // 準備の完了は controller の poll が受け取る。engine が古いと判定して捨て、公開しない。
            const bool drained = pumpUntil(
                [&] {
                    return diagnostics().pendingSourcePreparationCount == 0 &&
                           controller.pendingSourcePreparationCount() == 0;
                },
                10000);
            check(drained, (std::string(label) + ": 準備の完了を受け取りません").c_str());
            check(diagnostics().staleSourcePreparationRejectCount >=
                      engineStaleBefore + boundarySources,
                  (std::string(label) + ": 古くなった準備を engine が捨てません").c_str());
            // engine だけを止めた・seek した準備は engine が捨て、controller まで届かない。
            // controller の pause は自分で取り消して数える。
            if (cause == StaleCause::ControllerPause)
                check(controller.playbackStalePreparationCount() >=
                          controllerStaleBefore + boundarySources,
                      (std::string(label) + ": 取り消した準備を controller が外しません").c_str());
            else
                check(controller.playbackStalePreparationCount() == controllerStaleBefore,
                      (std::string(label) + ": 古くなった準備の完了が controller へ届きました")
                          .c_str());
            // pause した engine は準備し直しも受理しないので、公開数は増えない (seek は再生を
            // 続け、新しい世代で準備し直すので見ない)。
            if (cause != StaleCause::EngineSeek)
                check(diagnostics().publishedSourceCount <= publishedBefore,
                      (std::string(label) + ": 古くなった準備の source を公開しました").c_str());
            std::printf("%s: engine が捨てた準備 %llu、公開 %llu -> %llu\n", label,
                        static_cast<unsigned long long>(
                            diagnostics().staleSourcePreparationRejectCount - engineStaleBefore),
                        static_cast<unsigned long long>(publishedBefore),
                        static_cast<unsigned long long>(diagnostics().publishedSourceCount));
            controller.shutdown();
        };
        runStalePreparation("stale-engine-pause", StaleCause::EnginePause);
        runStalePreparation("stale-engine-seek", StaleCause::EngineSeek);
        runStalePreparation("stale-controller-pause", StaleCause::ControllerPause);
        runStalePreparation("stale-track-edit", StaleCause::TrackEdit);
        runStalePreparation("stale-track-edit-held", StaleCause::TrackEditHeldToBoundary);
        runStalePreparation("stale-engine-reset", StaleCause::EngineReset);

        const auto playFrom =
            [&](const char* label, const mvm::project::Project& playProject, qint64 from,
                const std::function<void()>& beforePlay,
                const std::function<void(mvm::app::MvmController&)>& whilePlaying) {
                const auto playPath = std::filesystem::path(
                    directory.filePath(QString::fromUtf8(label) + QStringLiteral(".mvm"))
                        .toStdWString());
                mvm::app::MvmController controller(playPath, {}, playProject);
                QQuickWindow window;
                window.setWidth(640);
                window.setHeight(360);
                auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
                surface->setWidth(640);
                surface->setHeight(360);
                window.show();
                controller.attachPreview(surface);
                // 準備ができた直後に controller が初期 frame の seek を始めるので、ready はすぐ
                // false に戻りうる。ready で打ち切らず、seek を受理されるまで繰り返す。
                const bool ready = pumpUntil([&] { return controller.previewReady(); }, 30000);
                const bool seekAccepted =
                    retryUntilAccepted([&] { return controller.seekTimelineFrame(from); }, 30000);
                const bool sought =
                    seekAccepted &&
                    pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
                if (sought)
                    beforePlay();
                const bool started =
                    sought && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
                if (!started) {
                    const auto telemetry = controller.previewTelemetry();
                    std::fprintf(stderr,
                                 "FAIL: %s: 再生を開始できません (準備=%d seek受理=%d 提示=%d "
                                 "engine state=%d 提示 frame=%lld decode失敗=%llu error=%s): %s\n",
                                 label, ready, seekAccepted, sought,
                                 static_cast<int>(telemetry.status.state),
                                 static_cast<long long>(telemetry.status.position.outputFrame),
                                 static_cast<unsigned long long>(telemetry.decodeFailureCount),
                                 telemetry.status.lastError
                                     ? telemetry.status.lastError->detail.c_str()
                                     : "-",
                                 controller.statusText().toUtf8().constData());
                }
                check(started, "再生を開始できません");
                if (started)
                    whilePlaying(controller);
                if (controller.playing())
                    controller.pauseTimeline();
                controller.shutdown();
            };

        // 再生中の track の表示・M/S の切り替えは再生を止めない。変わる前の Project で準備し終えた
        // source (まだ引き継いでいない) は、使わなくなっても登録枠を持ち続けてはいけない。
        {
            auto hideProject = mvm::project::createDefaultProject();
            auto a2 = half(copiedVideo, "hide-a2", TrackKind::Video, 0, 0);
            auto b2 = half(video, "hide-b2", TrackKind::Video, 120, 170);
            a2.track.index = 1;
            b2.track.index = 1;
            hideProject.timelineClips = {half(video, "hide-a1", TrackKind::Video, 0, 0),
                                         half(copiedVideo, "hide-b1", TrackKind::Video, 120, 170),
                                         a2, b2};
            mvm::test::attachFixtureMedia(hideProject);
            check(mvm::project::validateTimeline(hideProject).success,
                  "準備済みの source の試験の timeline が不正です");
            playFrom(
                "prepared-track-hide", hideProject, 90, [] {},
                [&](mvm::app::MvmController& controller) {
                    const bool prepared = pumpUntil(
                        [&] {
                            return controller.preparedPlaybackSourceCountForTest() == 2 &&
                                   controller.pendingSourcePreparationCount() == 0;
                        },
                        5000);
                    check(prepared && controller.playheadFrame() < 120,
                          "前提: 境界の前に 2 track の source を準備し終えません");
                    check(controller.setTracksMuted(QStringLiteral("video"), {1}, true) &&
                              controller.playing(),
                          "前提: 再生中に V2 を隠せません");
                    const bool crossed = pumpUntil(
                        [&] { return !controller.playing() || controller.playheadFrame() >= 150; },
                        15000);
                    check(crossed && controller.playing() &&
                              controller.playbackRebuildCount() == 0 &&
                              controller.playbackCapacityResetCount() == 0,
                          "準備済みの track を隠した後に組み直しなしで境界を越えられません");
                    // 越えた後は次の境界が無いので、登録しているのは再生中の source だけになる。
                    // 終端 (240) で止まると準備済みの source も外れるので、再生している間に見る。
                    const auto releasedNow = [&] {
                        return controller.preparedPlaybackSourceCountForTest() == 0 &&
                               controller.publishedPreviewSourceCountForTest() ==
                                   controller.activePlaybackSourceCountForTest();
                    };
                    pumpUntil(
                        [&] {
                            return releasedNow() || !controller.playing() ||
                                   controller.playheadFrame() >= 200;
                        },
                        3000);
                    const bool released =
                        controller.playing() && controller.playheadFrame() < 240 && releasedNow();
                    check(released,
                          "Project が変わる前に準備した source が登録枠を持ち続けています");
                    std::printf("prepared-track-hide: 準備済み %zu、公開 %llu、再生中 %zu、"
                                "組み直し %llu\n",
                                controller.preparedPlaybackSourceCountForTest(),
                                static_cast<unsigned long long>(
                                    controller.publishedPreviewSourceCountForTest()),
                                controller.activePlaybackSourceCountForTest(),
                                static_cast<unsigned long long>(controller.playbackRebuildCount()));
                });
        }

        // 変わる前の Project で準備に失敗した境界 (V1 の素材が無い) は、track を隠して原因が
        // 無くなったら、同じ frame の境界でも変わった後の Project で準備し直す。
        {
            auto failedProject = mvm::project::createDefaultProject();
            auto missing = half(video, "fail-missing", TrackKind::Video, 120, 170);
            missing.mediaPath = std::filesystem::path(
                directory.filePath(QStringLiteral("missing.mp4")).toStdWString());
            auto other = half(copiedVideo, "fail-other", TrackKind::Video, 120, 170);
            other.track.index = 1;
            failedProject.timelineClips = {half(video, "fail-a1", TrackKind::Video, 0, 0), missing,
                                           other};
            mvm::test::attachFixtureMedia(failedProject);
            check(mvm::project::validateTimeline(failedProject).success,
                  "準備に失敗する境界の試験の timeline が不正です");
            playFrom(
                "failed-boundary-track-hide", failedProject, 90, [] {},
                [&](mvm::app::MvmController& controller) {
                    const bool failed = pumpUntil(
                        [&] { return controller.playbackPreparationFailureCount() >= 1; }, 5000);
                    check(failed && controller.playheadFrame() < 120 &&
                              controller.preparedPlaybackSourceCountForTest() == 0 &&
                              controller.pendingSourcePreparationCount() == 0,
                          "前提: 境界の前に準備に失敗し、他の source を準備していません");
                    check(controller.setTracksMuted(QStringLiteral("video"), {0}, true) &&
                              controller.playing(),
                          "前提: 再生中に V1 を隠せません");
                    const bool crossed = pumpUntil(
                        [&] { return !controller.playing() || controller.playheadFrame() >= 150; },
                        15000);
                    check(crossed && controller.playing() && controller.playbackRebuildCount() == 0,
                          "失敗の原因の track を隠した後も、同じ frame の境界を準備し直しません");
                    std::printf("failed-boundary-track-hide: 失敗 %llu、組み直し %llu、理由: %s\n",
                                static_cast<unsigned long long>(
                                    controller.playbackPreparationFailureCount()),
                                static_cast<unsigned long long>(controller.playbackRebuildCount()),
                                controller.lastPlaybackRebuildReason().toUtf8().constData());
                });
        }

        // 取り消した準備は、engine が受け取るまで登録枠を使う。decoder の seek
        // の途中のように取り消しの 効かない段で止まっていても、control thread
        // でその完了を待たない。
        // - 境界の前に枠が返る (800ms): 枠が返った後の tick で要求し直し、組み直しなしで越える
        // - 境界を過ぎても枠が返らない (3000ms): engine を作り直さず (作り直しは止まった準備の
        // thread
        //   を join する)、境界で止めたまま枠が返るのを待ち、同じ engine で組み直して再生を続ける
        // どちらも UI の timer (10ms) が止まらないことを見る。
        const auto runBlockedStaleCapacity = [&](const char* label, int blockedMs) {
            const bool crossesWhileBlocked = blockedMs > 2000;
            auto blockedProject = mvm::project::createDefaultProject();
            blockedProject.timelineClips = {
                half(video, "blocked-a", TrackKind::Video, 0, 0),
                half(copiedVideo, "blocked-b", TrackKind::Video, 120, 170)};
            mvm::test::attachFixtureMedia(blockedProject);
            check(mvm::project::validateTimeline(blockedProject).success,
                  "取り消しの効かない準備の試験の timeline が不正です");
            const auto blockedPath = std::filesystem::path(
                directory.filePath(QString::fromUtf8(label) + QStringLiteral(".mvm"))
                    .toStdWString());
            mvm::app::MvmController controller(blockedPath, {}, blockedProject);
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
                retryUntilAccepted([&] { return controller.seekTimelineFrame(0); }, 30000) &&
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000);
            // 再生中の A と、境界 (120 = 2 秒先) の B の準備だけが登録枠に入る。
            const bool limited =
                sought &&
                retryUntilAccepted([&] { return controller.setPreviewRegistrationLimitForTest(2); },
                                   10000);
            controller.blockNextSourcePreparationForTest(blockedMs);
            const bool started =
                limited && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            const bool requested =
                started &&
                pumpUntil([&] { return controller.pendingSourcePreparationCount() == 1; }, 5000);
            check(requested && controller.playheadFrame() < 60,
                  (std::string(label) + ": 前提: 境界の source の準備を要求しません").c_str());
            if (requested) {
                const double before = controller.playbackMaxPreparationMs();
                // UI の heartbeat。control thread が止まると、間隔が開く。
                QTimer heartbeat;
                heartbeat.setInterval(10);
                auto lastBeat = std::chrono::steady_clock::now();
                double maxBeatGapMs = 0.0;
                QObject::connect(&heartbeat, &QTimer::timeout, [&] {
                    const auto now = std::chrono::steady_clock::now();
                    maxBeatGapMs =
                        std::max(maxBeatGapMs,
                                 std::chrono::duration<double, std::milli>(now - lastBeat).count());
                    lastBeat = now;
                });
                heartbeat.start();
                // 再生を止めない編集 (空の V2 を隠す)。B の準備は取り消され、止まったまま枠を
                // 持つ。次の tick の B の要求は登録上限に当たる。
                const auto editBegan = std::chrono::steady_clock::now();
                check(controller.setTracksMuted(QStringLiteral("video"), {1}, true) &&
                          controller.playing(),
                      "前提: 再生中に track を隠せません");
                const double editMs = std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - editBegan)
                                          .count();
                const bool crossed = pumpUntil(
                    [&] {
                        return (!crossesWhileBlocked && !controller.playing()) ||
                               (controller.playing() && controller.playheadFrame() >= 150);
                    },
                    15000);
                heartbeat.stop();
                check(editMs < 200 && controller.playbackMaxPreparationMs() < 200 &&
                          maxBeatGapMs < 400,
                      (std::string(label) + ": 取り消した準備の完了を control thread で待ちました")
                          .c_str());
                check(
                    crossed && controller.playing() && controller.playbackCapacityResetCount() == 0,
                    (std::string(label) +
                     ": engine を作り直さずに、取り消した準備の枠が返った後で境界を越えられません")
                        .c_str());
                if (crossesWhileBlocked)
                    check(controller.playbackSlotWaitCount() >= 1,
                          "前提: 境界で登録枠が空くのを待っていません");
                else
                    check(controller.playbackRebuildCount() == 0,
                          "境界の前に枠が返ったのに組み直しました");
                std::printf(
                    "%s: 編集 %.1fms、準備の最大 %.1fms -> %.1fms、UI の最大間隔 %.1fms、"
                    "外した準備 %llu、枠待ち %llu、組み直し %llu、作り直し %llu、理由: %s\n",
                    label, editMs, before, controller.playbackMaxPreparationMs(), maxBeatGapMs,
                    static_cast<unsigned long long>(controller.playbackStalePreparationCount()),
                    static_cast<unsigned long long>(controller.playbackSlotWaitCount()),
                    static_cast<unsigned long long>(controller.playbackRebuildCount()),
                    static_cast<unsigned long long>(controller.playbackCapacityResetCount()),
                    controller.lastPlaybackRebuildReason().toUtf8().constData());
                controller.pauseTimeline();
            }
            controller.shutdown();
        };
        runBlockedStaleCapacity("blocked-stale-capacity", 800);
        runBlockedStaleCapacity("blocked-stale-past-boundary", 3000);

        // 準備の thread を作れない (OS の thread の上限・メモリ不足) ときは、例外を再生の tick へ
        // 通さず失敗として返し、予約した video source を登録枠へ返す。登録上限を 2 (再生中の A と
        // 境界の B) にしておくので、返さなければ境界で登録上限に当たる。
        {
            auto threadProject = mvm::project::createDefaultProject();
            threadProject.timelineClips = {
                half(video, "thread-a", TrackKind::Video, 0, 0),
                half(copiedVideo, "thread-b", TrackKind::Video, 120, 170)};
            mvm::test::attachFixtureMedia(threadProject);
            const auto threadPath = std::filesystem::path(
                directory.filePath(QStringLiteral("preparation-thread-failure.mvm"))
                    .toStdWString());
            mvm::app::MvmController controller(threadPath, {}, threadProject);
            QQuickWindow window;
            window.setWidth(640);
            window.setHeight(360);
            auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
            surface->setWidth(640);
            surface->setHeight(360);
            window.show();
            controller.attachPreview(surface);
            const bool ready = pumpUntil([&] { return controller.previewReady(); }, 30000);
            const bool limited =
                ready &&
                retryUntilAccepted([&] { return controller.seekTimelineFrame(0); }, 30000) &&
                pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000) &&
                retryUntilAccepted([&] { return controller.setPreviewRegistrationLimitForTest(2); },
                                   10000);
            const auto engine = controller.previewEngineForTest();
            using mvm::preview::internal::PreviewRenderPort;
            if (engine)
                PreviewRenderPort::failNextSourcePreparationThreadForTest(*engine);
            const bool started =
                limited && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            const bool failed =
                started &&
                pumpUntil([&] { return controller.playbackPreparationFailureCount() >= 1; }, 5000);
            const auto afterFailure = PreviewRenderPort::runtimeDiagnostics(*engine);
            check(failed && controller.playing() && controller.playheadFrame() < 120 &&
                      afterFailure.registeredVideoSourceCount == 1,
                  "準備の thread を作れないときに、予約した source を返して失敗として閉じません");
            const bool crossed =
                failed &&
                pumpUntil(
                    [&] { return !controller.playing() || controller.playheadFrame() >= 150; },
                    15000);
            check(crossed && controller.playing() && controller.playbackCapacityResetCount() == 0 &&
                      controller.previewEngineForTest() == engine,
                  "準備の thread を作れなかった後に、engine を作り直さずに境界を越えられません");
            std::printf(
                "preparation-thread-failure: 準備の失敗 %llu、登録済みの video %llu、"
                "組み直し %llu、作り直し %llu、理由: %s\n",
                static_cast<unsigned long long>(controller.playbackPreparationFailureCount()),
                static_cast<unsigned long long>(afterFailure.registeredVideoSourceCount),
                static_cast<unsigned long long>(controller.playbackRebuildCount()),
                static_cast<unsigned long long>(controller.playbackCapacityResetCount()),
                controller.lastPlaybackRebuildReason().toUtf8().constData());
            if (controller.playing())
                controller.pauseTimeline();
            controller.shutdown();
        }

        // 受理されない編集・変更の無い編集では再生を止めない。Project・Undo 履歴も変えない。
        // 編集の入口が増えても同じ表で確かめる。
        {
            auto noopProject = mvm::project::createDefaultProject();
            // 0-60 は空白 (選択中の clip が無い)。clip は 60 から。
            noopProject.timelineClips = {half(video, "noop-v", TrackKind::Video, 60, 0)};
            mvm::test::attachFixtureMedia(noopProject);
            const auto noopPath = std::filesystem::path(
                directory.filePath(QStringLiteral("noop-edits-while-playing.mvm")).toStdWString());
            mvm::app::MvmController controller(noopPath, {}, noopProject);
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
                ready && retryUntilAccepted([&] { return controller.seekTimelineFrame(0); }, 30000);
            const bool started =
                sought && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            check(started && controller.currentClipIndex() < 0,
                  "前提: 選択中の clip が無い位置から再生を始められません");

            const struct {
                const char* label;
                std::function<void()> call;
            } noops[] = {
                {"元に戻す編集が無い Undo", [&] { controller.undoLastEdit(); }},
                {"やり直す編集が無い Redo", [&] { controller.redoLastEdit(); }},
                {"同じ値の Project 設定",
                 [&] {
                     controller.setProjectVideoSettings(
                         controller.outputWidth(), controller.outputHeight(),
                         controller.timelineFpsNum(), controller.timelineFpsDen());
                 }},
                {"不正な値の Project 設定",
                 [&] {
                     controller.setProjectVideoSettings(0, 0, controller.timelineFpsNum(),
                                                        controller.timelineFpsDen());
                 }},
                {"削除する clip が無い削除", [&] { controller.deleteCurrentClip(); }},
                {"空白ではない位置のリップル削除",
                 [&] { controller.rippleDeleteGap(QStringLiteral("video"), 0, 90); }},
                {"存在しない track の削除",
                 [&] { controller.removeTrack(QStringLiteral("video"), 99); }},
                {"存在しない clip の移動",
                 [&] {
                     controller.moveTimelineClip(QStringLiteral("missing"), QStringLiteral("video"),
                                                 0, 10, false);
                 }},
            };

            for (const auto& noop : noops) {
                if (!started)
                    break;
                // 前の行で止まっていたら、行ごとに判定できるよう選択中の clip が無い位置から
                // 再生し直す (止めない実装では再生し直さない)。
                if (!controller.playing() &&
                    !(retryUntilAccepted([&] { return controller.seekTimelineFrame(0); }, 30000) &&
                      retryUntilAccepted([&] { return controller.playTimeline(); }, 30000)))
                    break;
                const auto revision = controller.currentRevisionForTest();
                const auto undoDepth = controller.undoDepthForTest();
                noop.call();
                check(
                    controller.playing() && controller.currentRevisionForTest() == revision &&
                        controller.undoDepthForTest() == undoDepth,
                    (std::string(noop.label) + " で再生を止めたか、Project を変えました").c_str());
            }
            if (controller.playing())
                controller.pauseTimeline();
            controller.shutdown();
        }

        // 再生中の endpoint の open が失敗しても、control thread を止めた時間を記録する。open の
        // 失敗と、open した後の再生開始の失敗は分けて数える。
        const auto runEndpointFailure = [&](const char* label, bool failStart) {
            auto failProject = mvm::project::createDefaultProject();
            failProject.timelineClips = {
                half(video, "endpoint-v-out", TrackKind::Video, 0, 0),
                half(copiedVideo, "endpoint-v-in", TrackKind::Video, 120, 120),
                half(copiedWav, "endpoint-a-in", TrackKind::Audio, 120, 120)};
            mvm::test::attachFixtureMedia(failProject);
            constexpr int kFailDelayMs = 150;
            const auto failPath = std::filesystem::path(
                directory.filePath(QString::fromUtf8(label) + QStringLiteral(".mvm"))
                    .toStdWString());
            mvm::app::MvmController controller(failPath, {}, failProject);
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
            const auto engine = controller.previewEngineForTest();
            using mvm::preview::internal::PreviewRenderPort;
            if (failStart)
                PreviewRenderPort::failNextPlayingAudioTransportStartForTest(*engine);
            else
                PreviewRenderPort::failNextPlayingAudioEndpointOpenForTest(*engine, kFailDelayMs);
            const bool started =
                sought && retryUntilAccepted([&] { return controller.playTimeline(); }, 30000);
            const bool attempted =
                started && pumpUntil(
                               [&] {
                                   const auto now = PreviewRenderPort::runtimeDiagnostics(*engine);
                                   return now.playingAudioEndpointOpenFailureCount +
                                                  now.playingAudioTransportStartFailureCount >=
                                              1 ||
                                          controller.previewEngineForTest() != engine;
                               },
                               10000);
            const auto failed = PreviewRenderPort::runtimeDiagnostics(*engine);
            if (failStart)
                check(attempted && failed.playingAudioEndpointOpenAttemptCount >= 1 &&
                          failed.playingAudioEndpointOpenFailureCount == 0 &&
                          failed.playingAudioTransportStartFailureCount >= 1,
                      "open した後の再生開始の失敗を open の失敗と分けて数えません");
            else
                check(attempted && failed.playingAudioEndpointOpenAttemptCount >= 1 &&
                          failed.playingAudioEndpointOpenFailureCount >= 1 &&
                          failed.playingAudioTransportStartFailureCount == 0 &&
                          failed.maxPlayingAudioEndpointOpenAttemptMs >= kFailDelayMs,
                      "失敗した再生中の endpoint の open の時間を記録しません");
            std::printf(
                "%s: 試み %llu、open の失敗 %llu、再生開始の失敗 %llu、試みの最大 %.1fms、"
                "成功の最大 %.1fms\n",
                label, static_cast<unsigned long long>(failed.playingAudioEndpointOpenAttemptCount),
                static_cast<unsigned long long>(failed.playingAudioEndpointOpenFailureCount),
                static_cast<unsigned long long>(failed.playingAudioTransportStartFailureCount),
                failed.maxPlayingAudioEndpointOpenAttemptMs, failed.maxPlayingAudioEndpointOpenMs);
            if (controller.playing())
                controller.pauseTimeline();
            controller.shutdown();
        };
        runEndpointFailure("endpoint-open-failure", false);
        runEndpointFailure("endpoint-start-failure", true);

        // 別の clip を先読み幅 (2 秒) より密に並べる (100ms の clip を 24 個)。先読み幅の中の境界を
        // 全部準備すると、使う前の source を engine の既定の登録上限 (2 × 8 + 1) まで積み、同期の
        // 準備を毎 tick 重ねる。準備するのは次の境界の source だけで、組み直しも起こさない。
        {
            constexpr int kClips = 24;
            constexpr std::int64_t kClipFrames = 6;
            auto montage = mvm::project::createDefaultProject();
            for (int index = 0; index < kClips; ++index) {
                const std::string id = "montage-" + std::to_string(index);
                // 隣と別のファイル・離れた素材位置にして、前の clip の source
                // で覆えないようにする。
                auto clip = half(index % 2 == 0 ? video : copiedVideo, id.c_str(), TrackKind::Video,
                                 index * kClipFrames, (index * 37) % 200);
                clip.sourceOutFrame = clip.sourceInFrame + kClipFrames;
                montage.timelineClips.push_back(clip);
            }
            check(mvm::project::validateTimeline(montage).success,
                  "短い clip の timeline が不正です");
            const std::int64_t last = (kClips - 1) * kClipFrames;
            playFrom(
                "montage", montage, 0, [] {},
                [&](mvm::app::MvmController& controller) {
                    const bool passed = pumpUntil(
                        [&] { return !controller.playing() || controller.playheadFrame() >= last; },
                        15000);
                    check(passed && controller.playing(),
                          "短い clip の連続を通して再生が続きません");
                    check(controller.playbackRebuildCount() == 0,
                          "短い clip の連続でPreviewを組み直しました");
                    // 各境界で変わる source は video 1 つ。次の境界の分だけを先に準備している。
                    check(controller.playbackMaxPreparedSourceCount() == 1,
                          "短い clip の連続で次の境界より先の source まで準備しました");
                    check(controller.playbackMaxPreparationMs() < kMaxPreparationMs,
                          "短い clip の source の準備が先読み幅の半分を超えました");
                    std::printf(
                        "短い clip %d 個: playhead %lld、組み直し %llu、先に準備した source "
                        "最大 %zu、準備最大 %.1fms、理由: %s\n",
                        kClips, static_cast<long long>(controller.playheadFrame()),
                        static_cast<unsigned long long>(controller.playbackRebuildCount()),
                        controller.playbackMaxPreparedSourceCount(),
                        controller.playbackMaxPreparationMs(),
                        controller.lastPlaybackRebuildReason().toUtf8().constData());
                });
        }

        // 8 video track が同時に cut する世代を 3 つ並べる (A -> B -> C)。A -> B
        // を引き継いだ直後は、 A の 8 本が B の提示まで登録枠を使い続け、C の準備は既定の登録上限
        // (2 × 8 + 1) の途中で 断られる。この一時的な不足を失敗の境界として覚えると、A
        // を削除した後も C を準備し直さず、 B -> C の境界で Preview を組み直してしまう。
        {
            constexpr int kTracks = 8;
            constexpr int kGenerations = 3;
            constexpr std::int64_t kGenerationFrames = 60;
            auto layered = mvm::project::createDefaultProject();
            while (layered.videoTracks.size() < static_cast<std::size_t>(kTracks))
                layered.videoTracks.push_back(
                    {"V" + std::to_string(layered.videoTracks.size() + 1), false});
            for (int generation = 0; generation < kGenerations; ++generation) {
                for (int track = 0; track < kTracks; ++track) {
                    const std::string id =
                        "layer-" + std::to_string(generation) + "-" + std::to_string(track);
                    const int serial = generation * kTracks + track;
                    auto clip =
                        half(track % 2 == 0 ? video : copiedVideo, id.c_str(), TrackKind::Video,
                             generation * kGenerationFrames, (serial * 7) % 200);
                    clip.sourceOutFrame = clip.sourceInFrame + kGenerationFrames;
                    clip.track = {TrackKind::Video, track};
                    layered.timelineClips.push_back(clip);
                }
            }
            const auto layeredValid = mvm::project::validateTimeline(layered);
            check(layeredValid.success, "8 track の timeline が不正です");
            if (!layeredValid.success)
                std::fprintf(stderr, "  %s\n", layeredValid.error.c_str());
            const std::int64_t pastLastCut = (kGenerations - 1) * kGenerationFrames + 20;
            playFrom(
                "layered-cut", layered, 30, [] {},
                [&](mvm::app::MvmController& controller) {
                    const bool passed = pumpUntil(
                        [&] {
                            return !controller.playing() ||
                                   controller.playheadFrame() >= pastLastCut;
                        },
                        20000);
                    check(passed && controller.playing(),
                          "8 track の cut を通して再生が続きません");
                    check(controller.playbackRebuildCount() == 0,
                          "8 track の cut でPreviewを組み直しました");
                    check(controller.playbackPreparationFailureCount() == 0,
                          "8 track の cut で旧 source の削除待ちを準備の失敗にしました");
                    std::printf("8 track x %d 世代: playhead %lld、組み直し %llu、準備失敗 %llu、"
                                "先に準備した source 最大 %zu、準備最大 %.1fms、理由: %s\n",
                                kGenerations, static_cast<long long>(controller.playheadFrame()),
                                static_cast<unsigned long long>(controller.playbackRebuildCount()),
                                static_cast<unsigned long long>(
                                    controller.playbackPreparationFailureCount()),
                                controller.playbackMaxPreparedSourceCount(),
                                controller.playbackMaxPreparationMs(),
                                controller.lastPlaybackRebuildReason().toUtf8().constData());
                });
        }

        // 準備に失敗した境界は、越えるまで準備し直さない。境界の素材を再生の直前に消しておく。
        // 毎 tick 準備し直すと、境界までの約 30 frame (tick 16ms) の間に失敗が積み上がる。
        {
            const auto doomed = std::filesystem::path(
                directory.filePath(QStringLiteral("doomed.mp4")).toStdWString());
            std::filesystem::copy_file(copiedVideo, doomed,
                                       std::filesystem::copy_options::overwrite_existing);
            auto doomedProject = mvm::project::createDefaultProject();
            doomedProject.timelineClips = {half(video, "doomed-out", TrackKind::Video, 0, 0),
                                           half(doomed, "doomed-in", TrackKind::Video, 120, 120)};
            check(mvm::project::validateTimeline(doomedProject).success,
                  "準備失敗の試験の timeline が不正です");
            playFrom(
                "prepare-failure", doomedProject, 90,
                [&] {
                    std::error_code removeError;
                    check(std::filesystem::remove(doomed, removeError), "境界の素材を消せません");
                },
                [&](mvm::app::MvmController& controller) {
                    pumpUntil(
                        [&] { return !controller.playing() || controller.playheadFrame() >= 115; },
                        15000);
                    check(controller.playheadFrame() >= 115 && controller.playing(),
                          "準備に失敗した境界の手前まで再生が続きません");
                    check(controller.playbackPreparationFailureCount() == 1,
                          "準備に失敗した境界を境界の前に準備し直しました");
                    std::printf("準備失敗: playhead %lld、失敗 %llu 回\n",
                                static_cast<long long>(controller.playheadFrame()),
                                static_cast<unsigned long long>(
                                    controller.playbackPreparationFailureCount()));
                });
        }
        if (failures != 0)
            exitCode = 1;
    }
    // 23.976fps の素材を 60fps の timeline に置き、同じ素材の離れた 2 か所を Shift+D と同じ
    // 既定のトランジション (60 frame) でつなぐ。素材 1 frame が timeline 2〜3 frame に当たるので、
    // 区間の端の timeline frame が素材 frame の途中になる。区間の前後を 1 frame ずつ送る。
    {
        // outgoing は横長、incoming は縦長 (shorts のような) の別ファイル。
        const auto ffmpeg = fromUtf8(argv[3]);
        const auto makeFixture = [&](const QString& name, const wchar_t* size,
                                     MvmMltProbeResult& probed) {
            const auto path = std::filesystem::path(directory.filePath(name).toStdWString());
            const std::wstring videoSource =
                std::wstring(L"testsrc2=s=") + size + L":r=24000/1001:d=12";
            const bool generated =
                _wspawnl(_P_WAIT, ffmpeg.c_str(), ffmpeg.c_str(), L"-y", L"-loglevel", L"error",
                         L"-f", L"lavfi", L"-i", videoSource.c_str(), L"-f", L"lavfi", L"-i",
                         L"sine=frequency=440:sample_rate=48000:d=12", L"-c:v", L"libx264",
                         L"-preset", L"ultrafast", L"-pix_fmt", L"yuv420p", L"-c:a", L"aac",
                         L"-shortest", path.c_str(), static_cast<wchar_t*>(nullptr)) == 0;
            const auto utf8 = path.u8string();
            const std::string text(utf8.begin(), utf8.end());
            const bool ok = generated && mvm_mlt_probe_file(text.c_str(), &probed) == 0 &&
                            probed.frame_count > 0;
            check(ok, "23.976fps の fixture を生成できません");
            return ok ? path : std::filesystem::path();
        };
        MvmMltProbeResult landscapeProbe{};
        MvmMltProbeResult portraitProbe{};
        const auto landscape =
            makeFixture(QStringLiteral("ntsc-landscape.mp4"), L"640x360", landscapeProbe);
        const auto portrait =
            makeFixture(QStringLiteral("ntsc-portrait.mp4"), L"360x640", portraitProbe);
        if (!landscape.empty() && !portrait.empty()) {
            auto ntscProject = mvm::project::createDefaultProject();
            auto a = half(landscape, "ntsc-out", TrackKind::Video, 0, 0);
            a.sourceFpsNum = 24000;
            a.sourceFpsDen = 1001;
            a.sourceFrameCount = landscapeProbe.frame_count;
            auto b = a;
            b.id = b.name = "ntsc-in";
            b.mediaPath = portrait;
            b.sourceFrameCount = portraitProbe.frame_count;
            b.sourceInFrame = 150;
            b.sourceOutFrame = 250;
            b.timelineStartFrame = mvm::project::timelineClipDuration(ntscProject, a).frame;
            // 映像と音声をリンクし、Shift+D と同じく両方にトランジションを置く。
            a.linkGroupId = "ntsc-link-a";
            b.linkGroupId = "ntsc-link-b";
            // 音声 clip は取り込みと同じく timeline fps の単位で素材範囲を持つ。
            const auto audioOf = [&](const mvm::project::TimelineClip& source, const char* id) {
                auto audio = source;
                audio.id = audio.name = id;
                audio.kind = mvm::project::TimelineClipKind::Audio;
                audio.track = {TrackKind::Audio, 0};
                audio.sourceFpsNum = ntscProject.timelineFpsNum;
                audio.sourceFpsDen = ntscProject.timelineFpsDen;
                audio.sourceFrameCount = 12 * 60;
                audio.sourceInFrame = mvm::project::sourceBoundaryToTimelineBoundary(
                                          source.sourceInFrame, 24000, 1001,
                                          ntscProject.timelineFpsNum, ntscProject.timelineFpsDen)
                                          .frame;
                audio.sourceOutFrame =
                    audio.sourceInFrame +
                    mvm::project::timelineClipDuration(ntscProject, source).frame;
                return audio;
            };
            const auto aAudio = audioOf(a, "ntsc-out-audio");
            const auto bAudio = audioOf(b, "ntsc-in-audio");
            ntscProject.timelineClips = {a, b, aAudio, bAudio};
            int nextId = 0;
            const auto placed = mvm::project::applyDefaultEditTransition(
                ntscProject, a.id, b.id, 60, mvm::project::LinkMode::Linked,
                [&] { return "ntsc-t" + std::to_string(nextId++); });
            check(placed.success && placed.transitionCount == 2,
                  "23.976fps の編集点へ映像と音声のトランジションを置けません");
            if (!placed.success)
                std::fprintf(stderr, "  %s\n", placed.error.c_str());
            if (placed.success) {
                const auto& transition = ntscProject.timelineTransitions.front();
                const qint64 cut = b.timelineStartFrame;
                const qint64 regionStart = cut - transition.framesBeforeCut;
                const qint64 regionEnd = cut + transition.framesAfterCut;
                std::printf("23.976fps: cut %lld、区間 [%lld, %lld)\n", static_cast<long long>(cut),
                            static_cast<long long>(regionStart), static_cast<long long>(regionEnd));
                mvm::app::MvmController controller(
                    std::filesystem::path(
                        directory.filePath(QStringLiteral("ntsc.mvm")).toStdWString()),
                    {}, ntscProject);
                QQuickWindow window;
                window.setWidth(640);
                window.setHeight(360);
                auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
                surface->setWidth(640);
                surface->setHeight(360);
                window.show();
                controller.attachPreview(surface);
                // 起動直後の frame 0 の提示を送りの計測に混ぜない。
                check(pumpUntil([&] { return controller.previewReady(); }, 30000) &&
                          pumpUntil([&] { return controller.previewPresentedLatest(); }, 10000),
                      "23.976fps の preview が準備できません");
                for (qint64 frame = regionStart - 3; frame <= regionStart + 3; ++frame)
                    check(stepAndCompare(controller, ntscProject, frame).second,
                          "23.976fps: 区間の始まりのフレーム送りで mapping と違う frame "
                          "を提示しました");
                // 画面に実際に出た画素も見る。左端は outgoing (横長) なら絵柄、incoming (縦長) なら
                // 余白の黒。区間の後に outgoing が出ると左端が明るくなる。
                for (qint64 frame = regionEnd - 4; frame <= regionEnd + 4; ++frame) {
                    check(stepAndCompare(controller, ntscProject, frame).second,
                          "23.976fps: 区間の終わりのフレーム送りで mapping と違う frame "
                          "を提示しました");
                    pumpUntil([] { return false; }, 100);
                    const QImage shot = window.grabWindow();
                    const QColor left = shot.isNull() ? QColor() : shot.pixelColor(8, 180);
                    const int level = std::max({left.red(), left.green(), left.blue()});
                    std::printf("  画面の左端 frame %lld: %d,%d,%d\n",
                                static_cast<long long>(frame), left.red(), left.green(),
                                left.blue());
                    if (frame >= regionEnd)
                        check(!shot.isNull() && level < 24,
                              "23.976fps: 区間の後の画面に outgoing が見えています");
                    else {
                        // 余白の所の outgoing は 1 - p 以下 (p は区間の進み具合)。
                        const double p = (static_cast<double>(frame - regionStart) + 0.5) /
                                         static_cast<double>(regionEnd - regionStart);
                        check(!shot.isNull() && level <= 255.0 * (1.0 - p) + 16.0,
                              "23.976fps: 区間の中で余白の outgoing が 1 - p に減っていません");
                    }
                }
                controller.shutdown();
            }
        }
        if (failures != 0)
            exitCode = 1;
    }
    mvm_mlt_runtime_shutdown();
    if (exitCode == 0)
        std::puts("トランジションを preview で表示・再生できることを確認しました");
    return exitCode;
}
