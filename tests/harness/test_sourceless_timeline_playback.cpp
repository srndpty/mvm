#include "app/preview/test_window_mode.h"
// 映像 clip の無い timeline を controller 経由で 1x 再生し、音声が鳴ることを検査する。
//
// 以前の controller は映像の無い区間で engine を再生せず時計だけを進めていた
// (clockOnlyPlayback_)。そのため音声だけの区間は 1x で無音だった。
// 実際の preview surface (D3D11) を付けた controller で A1 の音声だけを再生し、
//   - 再生が続き playhead が進むこと
//   - endpoint へ送った PCM の meter が振れること (無音なら下限のまま)
// を見る。期待する値は直書きする。
#include "app/preview/preview_engine_rhi_item.h"
#include "media/audio_preview/audio_types.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "test_event_wait.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <thread>

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

using mvm::test::pumpUntil;

// wav_48k.wav は -6dBFS 程度の正弦波。無音なら meter は下限 (-60dB 以下) に留まる。
constexpr double kAudibleDb = -30.0;
// 5 秒の素材を 60fps で 300 frame。再生開始から 1 秒以上進めば再生が続いている。
constexpr qint64 kAdvancedFrames = 60;

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication application(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_sourceless_timeline_playback <wav>\n");
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

    // A1 に音声だけを置く。V1 / V2 は空。
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip audio;
    audio.kind = mvm::project::TimelineClipKind::Audio;
    audio.id = "audio";
    audio.name = "audio";
    audio.mediaPath = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[1]));
    audio.sourceFpsNum = 60;
    audio.sourceFpsDen = 1;
    audio.sourceFrameCount = 300;
    audio.sourceOutFrame = 300;
    audio.track = {mvm::project::TrackKind::Audio, 0};
    project.timelineClips.push_back(audio);
    const std::filesystem::path projectPath =
        directory.filePath(QStringLiteral("audio-only.mvm")).toStdWString();

    int exitCode = 0;
    {
        mvm::app::MvmController controller(projectPath, {}, project);
        QQuickWindow window;
        window.setFlags(mvm::app::testBackgroundWindowFlags());
        window.setWidth(640);
        window.setHeight(360);
        auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
        surface->setWidth(640);
        surface->setHeight(360);
        controller.attachPreview(surface);
        window.show();
        // 検証用の音量 (audio_types.h)。session volume なので meter の値は変わらない。
        controller.setMasterVolume(mvm::audio::kVerificationSessionVolume);

        const auto run = [&]() -> int {
            if (!pumpUntil([&] { return controller.previewReady(); }, 30000)) {
                std::fprintf(stderr, "FAIL: preview が準備できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 3;
            }
            check(!controller.previewVideoAtPlayhead(), "前提: playhead に映像があります");
            // 起動直後の初回 seek が終わるまで再生を受理しないことがある。受理されるまで待つ。
            if (!pumpUntil([&] { return controller.playTimeline(); }, 30000)) {
                std::fprintf(stderr, "FAIL: 音声だけの timeline を再生できません: %s\n",
                             controller.statusText().toUtf8().constData());
                return 1;
            }
            const qint64 startFrame = controller.playheadFrame();
            double loudest = -1000.0;
            const bool advanced = pumpUntil(
                [&] {
                    loudest = std::max(
                        {loudest, controller.audioMeterDbLeft(), controller.audioMeterDbRight()});
                    return !controller.playing() ||
                           controller.playheadFrame() >= startFrame + kAdvancedFrames;
                },
                10000);
            check(advanced && controller.playing(),
                  "音声だけの timeline の再生が続きません (playhead が進みません)");
            check(loudest > kAudibleDb, "音声だけの区間が無音です (meter が振れません)");
            std::printf("playhead %lld -> %lld、meter の最大 %.1f dB\n",
                        static_cast<long long>(startFrame),
                        static_cast<long long>(controller.playheadFrame()), loudest);
            check(controller.pauseTimeline(), "一時停止できません");
            return failures == 0 ? 0 : 1;
        };
        exitCode = run();
        controller.shutdown();
    }
    mvm_mlt_runtime_shutdown();
    if (exitCode == 0)
        std::puts("映像の無い timeline を 1x で再生し、音声が鳴ることを確認しました");
    return exitCode;
}
