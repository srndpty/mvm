// preview の product capability を上限ちょうどで実際に使う検証アプリ。
//
// capability (映像 8 本・合成 16 枚・音声 16 本) は性能を qualify した値ではないが、
// 「受理して描画できる上限」として公開している。この app はその契約を実体で固定する。
// fps や VRAM の余裕は見ない (docs/premiere-like-editing.md §17 で未検証として扱う)。
//
//   - 映像 source 8 本 + 静止画 8 枚 = 16 layer の composition を受理し、再生して
//     16 layer の frame が実際に提示されること
//   - 音声 source 16 本を登録して一緒に再生し、17 本目の登録は拒否すること
//   - 映像 9 本 (source 上限超過)、8 + 静止画 9 = 17 layer (layer 上限超過) の
//     composition は拒否すること
//
// 期待値の数値は capability の定数を参照せずに直書きする (実装の値をテストが追認しない)。
#include "app/preview/preview_engine_rhi_item.h"
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

constexpr int kMaxVideoSources = 8;
constexpr int kMaxCompositionLayers = 16;
constexpr int kMaxAudioSources = 16;
// 16 layer の frame をこの枚数提示できたら合格とする。60fps で約 0.5 秒。
constexpr int kRequiredFullFrames = 30;

class QtDispatcher final : public mvm::preview::PreviewEventDispatcher {
public:
    explicit QtDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> task) override {
        return QMetaObject::invokeMethod(context_, std::move(task), Qt::QueuedConnection);
    }

private:
    QObject* context_;
};

class CapacitySink final : public mvm::preview::PreviewEventSink {
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

enum class Stage { WaitDevice, WaitFrames, WaitShutdown };

// 4 x 4 の格子の index 番目の区画。映像と静止画が重ならないよう並べる。
mvm::preview::PreviewNormalizedRect gridCell(int index) {
    return {static_cast<float>(index % 4) * 0.25F, static_cast<float>(index / 4) * 0.25F, 0.25F,
            0.25F};
}

std::shared_ptr<mvm::preview::PreviewStillImage> makeStill(unsigned char shade) {
    auto still = std::make_shared<mvm::preview::PreviewStillImage>();
    still->width = 2;
    still->height = 2;
    still->rgba.assign(16, shade);
    return still;
}

bool rejectedAsUnsupported(const mvm::preview::Result<mvm::preview::AcceptedComposition>& result) {
    return !result &&
           result.error().category == mvm::preview::PreviewErrorCategory::UnsupportedCapability;
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 3) {
        std::fprintf(stderr,
                     "使い方: mvm_p5e_capacity_smoke <映像+音声の fixture> <映像の fixture>\n");
        return 2;
    }

    auto engine = std::make_shared<mvm::preview::PreviewEngine>();
    auto dispatcher = std::make_shared<QtDispatcher>(&app);
    auto sink = std::make_shared<CapacitySink>();
    if (!engine->initialize({{{60, 1}}}, dispatcher) || !engine->attachEventSink(sink))
        return 3;
    // 検証用の音量 (audio_types.h)。音声 16 本を同時に鳴らすので特に下げておく。
    // Windows の session volume なので PCM は変わらない。
    if (!engine->setMasterVolume(mvm::audio::kVerificationSessionVolume))
        return 3;
    const auto capabilities = engine->capabilities();
    if (capabilities.configuredMaxActiveVideoSources != kMaxVideoSources ||
        capabilities.configuredMaxCompositionLayers != kMaxCompositionLayers ||
        capabilities.configuredMaxActiveAudioSources != kMaxAudioSources) {
        std::fprintf(stderr, "公開 capability が 8 / 16 / 16 ではありません\n");
        return 3;
    }

    QQuickWindow window;
    window.setWidth(1280);
    window.setHeight(720);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(1280);
    surface->setHeight(720);
    surface->setEngine(engine);
    window.show();

    Stage stage = Stage::WaitDevice;
    mvm::preview::AcceptedComposition accepted;
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
            // 映像 source 9 本。9 本目は source 上限超過の composition を作るためだけに使う。
            std::vector<mvm::preview::PreviewSourceId> video;
            for (int index = 0; index < kMaxVideoSources + 1; ++index) {
                mvm::preview::PreviewSourceDescriptor descriptor;
                descriptor.mediaPath = arguments[1 + index % 2].toStdWString();
                descriptor.videoEnabled = true;
                const auto source = engine->addSource(descriptor);
                if (!source) {
                    std::fprintf(stderr, "映像 source %d を登録できません: %s\n", index + 1,
                                 source.error().detail.c_str());
                    return failWith(20, "映像 source の登録に失敗しました");
                }
                video.push_back(source.value());
            }

            // 音声 source 16 本は受理し、17 本目は拒否する。
            for (int index = 0; index < kMaxAudioSources + 1; ++index) {
                mvm::preview::PreviewSourceDescriptor descriptor;
                descriptor.mediaPath = arguments[1].toStdWString();
                descriptor.audioEnabled = true;
                const auto source = engine->addSource(descriptor);
                if (index < kMaxAudioSources && !source) {
                    std::fprintf(stderr, "音声 source %d を登録できません: %s\n", index + 1,
                                 source.error().detail.c_str());
                    return failWith(21, "上限内の音声 source を拒否しました");
                }
                if (index == kMaxAudioSources &&
                    (source || source.error().category !=
                                   mvm::preview::PreviewErrorCategory::UnsupportedCapability))
                    return failWith(22, "17 本目の音声 source を拒否しませんでした");
            }

            std::vector<std::shared_ptr<mvm::preview::PreviewStillImage>> stills;
            for (int index = 0; index < kMaxCompositionLayers - kMaxVideoSources + 1; ++index)
                stills.push_back(makeStill(static_cast<unsigned char>(40 + index * 20)));
            const auto build = [&](int videoCount, int stillCount) {
                auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
                for (int index = 0; index < videoCount; ++index)
                    snapshot->layers.push_back(
                        {video[static_cast<std::size_t>(index)], gridCell(index), {0, 0, 1, 1}, 1.0F});
                for (int index = 0; index < stillCount; ++index) {
                    mvm::preview::PreviewCompositionLayer layer;
                    layer.destination = gridCell(videoCount + index);
                    layer.sourceRect = {0, 0, 1, 1};
                    layer.stillImage = stills[static_cast<std::size_t>(index)];
                    snapshot->layers.push_back(layer);
                }
                return snapshot;
            };

            // 上限超過の composition は拒否する (受理済み composition は変えない)。
            if (!rejectedAsUnsupported(engine->submitComposition(build(kMaxVideoSources + 1, 0))))
                return failWith(23, "映像 9 本の composition を拒否しませんでした");
            if (!rejectedAsUnsupported(engine->submitComposition(
                    build(kMaxVideoSources, kMaxCompositionLayers - kMaxVideoSources + 1))))
                return failWith(24, "17 layer の composition を拒否しませんでした");
            if (!engine->removeSource(video.back()))
                return failWith(25, "9 本目の映像 source を解放できません");
            video.pop_back();

            // 上限ちょうど: 映像 8 本 + 静止画 8 枚 = 16 layer。
            const auto full =
                engine->submitComposition(build(kMaxVideoSources, kMaxCompositionLayers - kMaxVideoSources));
            if (!full) {
                std::fprintf(stderr, "16 layer の composition: %s\n", full.error().detail.c_str());
                return failWith(26, "上限ちょうどの 16 layer composition を拒否しました");
            }
            accepted = full.value();
            if (!engine->play())
                return failWith(27, "16 layer + 音声 16 本で再生を開始できません");
            stage = Stage::WaitFrames;
            return;
        }

        if (stage == Stage::WaitFrames) {
            int fullFrames = 0;
            for (const auto& frame : sink->frames) {
                if (frame.composition == accepted &&
                    frame.activeLayerCount == static_cast<std::uint32_t>(kMaxCompositionLayers))
                    ++fullFrames;
            }
            if (fullFrames < kRequiredFullFrames)
                return;
            std::printf("16 layer の frame を %d 枚提示しました (全提示 %zu 枚)\n", fullFrames,
                        sink->frames.size());
            if (!engine->requestShutdown())
                return failWith(28, "shutdown を開始できません");
            stage = Stage::WaitShutdown;
            return;
        }

        if (stage == Stage::WaitShutdown &&
            status.state == mvm::preview::PreviewEngineState::Shutdown) {
            std::puts("preview capability 上限ちょうど (映像 8 / 合成 16 / 音声 16) を受理して"
                      "描画し、超過を拒否しました");
            app.quit();
        }
    });
    timer.start();
    app.exec();
    // 早期失敗の経路は engine を shutdown しないため、engine 破棄の std::terminate が
    // 終了コードを上書きする (p5e smoke と同じ)。原因を追えるよう、破棄より前に残す。
    if (exitCode != 0)
        std::fprintf(stderr, "P5-E capacity smoke 失敗: exit code %d\n", exitCode);
    return exitCode;
}
