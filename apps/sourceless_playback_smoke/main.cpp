// decode source を持たない composition (画像だけ・音声だけの区間) を実際に再生する検証アプリ。
//
//   1. 静止画 2 枚だけの composition を音声なしで再生し、2 layer の frame を提示する
//      (時計は wall clock)。1 枚は effect (回転) 付き
//   2. 音声 source を登録し、空の composition で再生する (音声だけの区間)。
//      endpoint へ PCM が送られていること (meter) と、frame が進むことを見る (audio master)
//   3. 再生中に seek する。seek 後に音声 transport が再開しなければ audio master の時計が
//      止まり、frame は進まない。seek 後に frame が進み続けることを見る
//
// 期待する枚数は直書きする。
#include "app/preview/preview_engine_rhi_item.h"
#include "app/preview/test_window_mode.h"
#include "media/audio_preview/audio_types.h"
#include "preview_engine/preview_engine.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

namespace {

// 各段階でこの枚数を提示できたら次へ進む。60fps で約 0.5 秒。
constexpr int kRequiredFrames = 30;
// 音声だけの区間の seek 先。再生位置から離れた位置へ飛ぶ。
constexpr std::int64_t kSeekTarget = 120;

class QtDispatcher final : public mvm::preview::PreviewEventDispatcher {
public:
    explicit QtDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> task) override {
        return QMetaObject::invokeMethod(context_, std::move(task), Qt::QueuedConnection);
    }

private:
    QObject* context_;
};

class RecordingSink final : public mvm::preview::PreviewEventSink {
public:
    void stateChanged(mvm::preview::PreviewEngineState state) override { states.push_back(state); }
    void positionChanged(mvm::preview::PreviewPosition) override {}
    void framePresented(mvm::preview::PresentedFrameInfo frame) override {
        frames.push_back(frame);
    }
    void errorOccurred(mvm::preview::PreviewError error) override {
        errors.push_back(std::move(error));
    }
    void deviceChanged(mvm::preview::PreviewDeviceInfo) override {}

    std::vector<mvm::preview::PresentedFrameInfo> frames;
    std::vector<mvm::preview::PreviewError> errors;
    std::vector<mvm::preview::PreviewEngineState> states;
};

enum class Stage { WaitDevice, StillFrames, WaitPaused, AudioFrames, SeekResume, WaitShutdown };

std::shared_ptr<mvm::preview::PreviewStillImage> makeStill(unsigned char red, unsigned char blue) {
    auto still = std::make_shared<mvm::preview::PreviewStillImage>();
    still->width = 4;
    still->height = 2;
    for (int index = 0; index < 8; ++index)
        still->rgba.insert(still->rgba.end(), {red, 0, blue, 255});
    return still;
}

int countFrames(const std::vector<mvm::preview::PresentedFrameInfo>& frames,
                const mvm::preview::AcceptedComposition& composition, std::uint32_t layers,
                std::int64_t fromOutputFrame = 0) {
    int count = 0;
    for (const auto& frame : frames) {
        if (frame.composition == composition && frame.activeLayerCount == layers &&
            frame.position.outputFrame >= fromOutputFrame)
            ++count;
    }
    return count;
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    mvm::app::prepareTestFixedWindowEnvironment();
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 2) {
        std::fprintf(stderr, "使い方: mvm_sourceless_playback_smoke <音声の fixture>\n");
        return 2;
    }

    auto engine = std::make_shared<mvm::preview::PreviewEngine>();
    auto dispatcher = std::make_shared<QtDispatcher>(&app);
    auto sink = std::make_shared<RecordingSink>();
    if (!engine->initialize({{{60, 1}}}, dispatcher) || !engine->attachEventSink(sink))
        return 3;
    // 検証用の音量 (audio_types.h)。Windows の session volume なので PCM と meter は変わらない。
    if (!engine->setMasterVolume(mvm::audio::kVerificationSessionVolume))
        return 3;

    QQuickWindow window;
    mvm::app::applyTestFixedWindow(window);
    window.setWidth(640);
    window.setHeight(360);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(640);
    surface->setHeight(360);
    surface->setEngine(engine);
    window.show();

    Stage stage = Stage::WaitDevice;
    mvm::preview::AcceptedComposition stillComposition;
    mvm::preview::AcceptedComposition emptyComposition;
    int exitCode = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto failWith = [&](int code, const char* message) {
        std::fprintf(stderr, "%s\n", message);
        exitCode = code;
        app.quit();
    };

    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(60)) {
            std::fprintf(stderr, "60 秒以内に完了しませんでした (stage=%d frames=%zu errors=%zu)\n",
                         static_cast<int>(stage), sink->frames.size(), sink->errors.size());
            exitCode = 10;
            app.quit();
            return;
        }
        if (!sink->errors.empty()) {
            std::fprintf(stderr, "engine error: %s\n", sink->errors.front().detail.c_str());
            exitCode = 11;
            app.quit();
            return;
        }
        const auto status = engine->status();

        if (stage == Stage::WaitDevice &&
            status.state == mvm::preview::PreviewEngineState::ReadyPaused) {
            auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            mvm::preview::PreviewCompositionLayer plain;
            plain.destination = {0.0F, 0.0F, 0.5F, 0.5F};
            plain.sourceRect = {0, 0, 1, 1};
            plain.stillImage = makeStill(255, 0);
            snapshot->layers.push_back(plain);
            mvm::preview::PreviewCompositionLayer rotated;
            rotated.destination = {0.25F, 0.25F, 0.5F, 0.5F};
            rotated.sourceRect = {0, 0, 1, 1};
            rotated.opacity = 0.75F;
            rotated.stillImage = makeStill(0, 255);
            rotated.effectsEnabled = true;
            rotated.rotationDegrees = 15.0F;
            rotated.sourceDurationFrames = 1;
            snapshot->layers.push_back(rotated);
            const auto accepted = engine->submitComposition(snapshot);
            if (!accepted) {
                std::fprintf(stderr, "静止画だけの composition: %s\n",
                             accepted.error().detail.c_str());
                return failWith(20, "静止画だけの composition を受理しません");
            }
            stillComposition = accepted.value();
            if (!engine->play())
                return failWith(21, "静止画だけの composition で再生を開始できません");
            stage = Stage::StillFrames;
            return;
        }

        if (stage == Stage::StillFrames) {
            if (countFrames(sink->frames, stillComposition, 2) < kRequiredFrames)
                return;
            std::printf("静止画 2 枚の frame を %d 枚提示しました\n",
                        countFrames(sink->frames, stillComposition, 2));
            if (!engine->pause())
                return failWith(22, "静止画の再生を一時停止できません");
            stage = Stage::WaitPaused;
            return;
        }

        if (stage == Stage::WaitPaused &&
            status.state == mvm::preview::PreviewEngineState::ReadyPaused) {
            mvm::preview::PreviewSourceDescriptor descriptor;
            descriptor.mediaPath = arguments[1].toStdWString();
            descriptor.audioEnabled = true;
            const auto audio = engine->addSource(descriptor);
            if (!audio) {
                std::fprintf(stderr, "音声 source: %s\n", audio.error().detail.c_str());
                return failWith(23, "音声 source を登録できません");
            }
            const auto accepted =
                engine->submitComposition(std::make_shared<mvm::preview::CompositionSnapshot>());
            if (!accepted)
                return failWith(24, "空の composition を受理しません");
            emptyComposition = accepted.value();
            if (!engine->play())
                return failWith(25, "音声だけの区間で再生を開始できません");
            stage = Stage::AudioFrames;
            return;
        }

        if (stage == Stage::AudioFrames) {
            const auto telemetry = engine->telemetry();
            if (countFrames(sink->frames, emptyComposition, 0) < kRequiredFrames ||
                !(telemetry.audioMeterPeakLeft > 0.01F || telemetry.audioMeterPeakRight > 0.01F))
                return;
            std::printf("音声だけの区間で frame を %d 枚提示しました (meter %.3f / %.3f)\n",
                        countFrames(sink->frames, emptyComposition, 0),
                        static_cast<double>(telemetry.audioMeterPeakLeft),
                        static_cast<double>(telemetry.audioMeterPeakRight));
            if (!engine->seek({kSeekTarget}))
                return failWith(26, "音声だけの区間で seek できません");
            stage = Stage::SeekResume;
            return;
        }

        if (stage == Stage::SeekResume) {
            // seek 先より後の frame が進み続けること。音声 transport が再開しなければ
            // audio master の時計が止まり、ここで 60 秒の timeout になる。
            if (status.state != mvm::preview::PreviewEngineState::Playing ||
                countFrames(sink->frames, emptyComposition, 0, kSeekTarget + 1) < kRequiredFrames)
                return;
            std::printf("seek 後も frame を %d 枚提示しました\n",
                        countFrames(sink->frames, emptyComposition, 0, kSeekTarget + 1));
            if (!engine->requestShutdown())
                return failWith(27, "shutdown を開始できません");
            stage = Stage::WaitShutdown;
            return;
        }

        if (stage == Stage::WaitShutdown &&
            status.state == mvm::preview::PreviewEngineState::Shutdown) {
            std::puts("decode source の無い composition (静止画だけ・音声だけ) を再生し、"
                      "seek 後も再生を続けました");
            app.quit();
        }
    });
    timer.start();
    app.exec();
    if (exitCode != 0)
        std::fprintf(stderr, "sourceless playback smoke 失敗: exit code %d\n", exitCode);
    return exitCode;
}
