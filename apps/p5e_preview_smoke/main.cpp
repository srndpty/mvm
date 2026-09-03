// P5-E product composition の検証アプリ。
// P5-E3では二source / 二layer、per-source seek generation、capability境界を
// product公開経路から固定する。
//
// `--remove-*` は P5-E2 の source removal guard を製品経路で固定する。
// `--remove-released-source`はAからBへ実際に提示を切り替えた後でAを解放し、
// active/pending参照guardが単なる登録有無判定ではないことを固定する。
//
// 特に `--fault-stale-composition-epoch` は、compose 成立後・提示前に
// `CompositionEpoch` だけを進め、`validateForDisplay()` が製品経路で実際に
// 効いていることを固定する。固定するのは reject branch を踏んだことだけではなく、
// **その output frame を実際に提示しなかったこと**である。counter だけでは
// 「reject を数えたうえでそのまま描画する」bug を検出できない。
#include "app/preview/preview_engine_rhi_item.h"
#include "preview_engine/preview_engine.h"
#include "preview_engine/preview_engine_internal.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

namespace {

class QtDispatcher final : public mvm::preview::PreviewEventDispatcher {
public:
    explicit QtDispatcher(QObject* context) : context_(context) {}

    bool post(std::function<void()> task) override {
        return QMetaObject::invokeMethod(context_, std::move(task), Qt::QueuedConnection);
    }

private:
    QObject* context_;
};

class SmokeSink final : public mvm::preview::PreviewEventSink {
public:
    void stateChanged(mvm::preview::PreviewEngineState state) override { states.push_back(state); }

    void positionChanged(mvm::preview::PreviewPosition) override {}

    void framePresented(mvm::preview::PresentedFrameInfo frame) override {
        if (!frames.empty() && frame.presentationSequence <= frames.back().presentationSequence)
            sequenceViolation = true;
        frames.push_back(frame);
    }

    void errorOccurred(mvm::preview::PreviewError error) override {
        errors.push_back(std::move(error));
    }

    void deviceChanged(mvm::preview::PreviewDeviceInfo) override {}

    std::vector<mvm::preview::PresentedFrameInfo> frames;
    std::vector<mvm::preview::PreviewError> errors;
    std::vector<mvm::preview::PreviewEngineState> states;
    bool sequenceViolation = false;
};

enum class Stage {
    WaitDevice,
    WaitPrepareSeek,
    WaitInitial,
    WaitRecovered,
    WaitResume,
    WaitReplacement,
    WaitMissingPair,
    WaitSeek,
    WaitSeekContinued,
    WaitShutdown
};
enum class Fault {
    None,
    SingleLayer,
    ExceedSourceCount,
    ExceedLayerCount,
    DuplicateSourceLayer,
    MissingPair,
    RemoveReleasedSource,
    SeekTwoSource,
    SeekPartialGeneration,
    SeekPartialRequest,
    StaleCompositionEpoch,
    RemoveUnreferencedSource,
    RemoveReferencedSource,
    RemoveAudioSource,
    RemoveAudioStopFailure,
    RemoveShutdownRace,
    RemoveFatalEventOrder
};

bool usesTwoVideoSources(Fault fault) {
    return fault == Fault::None || fault == Fault::ExceedSourceCount ||
           fault == Fault::ExceedLayerCount || fault == Fault::DuplicateSourceLayer ||
           fault == Fault::MissingPair || fault == Fault::RemoveReleasedSource ||
           fault == Fault::SeekTwoSource || fault == Fault::SeekPartialGeneration ||
           fault == Fault::SeekPartialRequest;
}

// audio sourceを伴うremoval faultかどうか。
bool needsAudioSource(Fault fault) {
    return fault == Fault::RemoveAudioSource || fault == Fault::RemoveAudioStopFailure ||
           fault == Fault::RemoveShutdownRace || fault == Fault::RemoveFatalEventOrder;
}

// pauseしてからremoval検査を行うfaultかどうか。
bool checksRemovalAfterPause(Fault fault) {
    return fault == Fault::RemoveReferencedSource || needsAudioSource(fault);
}

} // namespace

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 3 || arguments.size() > 4) {
        std::fprintf(stderr, "使い方: mvm_p5e_preview_smoke <fixture-a-path> <fixture-b-path> "
                             "[--single-layer|--exceed-source-count|--exceed-layer-count"
                             "|--duplicate-source-layer|--fault-missing-pair"
                             "|--remove-released-source|--seek-two-source"
                             "|--fault-seek-partial-generation|--fault-seek-partial-request"
                             "|--fault-stale-composition-epoch|--remove-unreferenced-source"
                             "|--remove-referenced-source|--remove-audio-source"
                             "|--fault-remove-audio-stop|--remove-shutdown-race"
                             "|--remove-fatal-event-order]\n");
        return 2;
    }
    Fault fault = Fault::None;
    if (arguments.size() == 4) {
        if (arguments[3] == "--single-layer")
            fault = Fault::SingleLayer;
        else if (arguments[3] == "--exceed-source-count")
            fault = Fault::ExceedSourceCount;
        else if (arguments[3] == "--exceed-layer-count")
            fault = Fault::ExceedLayerCount;
        else if (arguments[3] == "--duplicate-source-layer")
            fault = Fault::DuplicateSourceLayer;
        else if (arguments[3] == "--fault-missing-pair")
            fault = Fault::MissingPair;
        else if (arguments[3] == "--remove-released-source")
            fault = Fault::RemoveReleasedSource;
        else if (arguments[3] == "--seek-two-source")
            fault = Fault::SeekTwoSource;
        else if (arguments[3] == "--fault-seek-partial-generation")
            fault = Fault::SeekPartialGeneration;
        else if (arguments[3] == "--fault-seek-partial-request")
            fault = Fault::SeekPartialRequest;
        else if (arguments[3] == "--fault-stale-composition-epoch")
            fault = Fault::StaleCompositionEpoch;
        else if (arguments[3] == "--remove-unreferenced-source")
            fault = Fault::RemoveUnreferencedSource;
        else if (arguments[3] == "--remove-referenced-source")
            fault = Fault::RemoveReferencedSource;
        else if (arguments[3] == "--remove-audio-source")
            fault = Fault::RemoveAudioSource;
        else if (arguments[3] == "--fault-remove-audio-stop")
            fault = Fault::RemoveAudioStopFailure;
        else if (arguments[3] == "--remove-shutdown-race")
            fault = Fault::RemoveShutdownRace;
        else if (arguments[3] == "--remove-fatal-event-order")
            fault = Fault::RemoveFatalEventOrder;
        else
            return 2;
    }
    QFile fixture(arguments[1]);
    QFile fixtureB(arguments[2]);
    QCryptographicHash fixtureHash(QCryptographicHash::Sha256);
    if (!fixture.open(QIODevice::ReadOnly) || !fixtureHash.addData(&fixture) ||
        fixtureHash.result().toHex() !=
            "d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308") {
        std::fprintf(stderr, "fixture AのSHA-256がauthoritative値と一致しません\n");
        return 2;
    }
    QCryptographicHash fixtureBHash(QCryptographicHash::Sha256);
    if (!fixtureB.open(QIODevice::ReadOnly) || !fixtureBHash.addData(&fixtureB) ||
        fixtureBHash.result().toHex() !=
            "fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479") {
        std::fprintf(stderr, "fixture BのSHA-256がauthoritative値と一致しません\n");
        return 2;
    }

    auto engine = std::make_shared<mvm::preview::PreviewEngine>();
    auto dispatcher = std::make_shared<QtDispatcher>(&app);
    auto sink = std::make_shared<SmokeSink>();
    if (!engine->initialize({{{60, 1}}}, dispatcher) || !engine->attachEventSink(sink))
        return 3;
    const auto capabilities = engine->capabilities();
    if (capabilities.configuredMaxActiveVideoSources != 2 ||
        capabilities.configuredMaxCompositionLayers != 2 ||
        capabilities.duplicateSourceLayersSupported) {
        std::fprintf(stderr, "P5-E3 capabilityが2/2/duplicate=falseではありません\n");
        return 3;
    }

    QQuickWindow window;
    window.setWidth(1920);
    window.setHeight(1080);
    auto* surface = new mvm::app::PreviewEngineRhiItem(window.contentItem());
    surface->setWidth(1920);
    surface->setHeight(1080);
    surface->setEngine(engine);
    window.show();

    Stage stage = Stage::WaitDevice;
    mvm::preview::AcceptedComposition accepted;
    std::uint64_t presentedAtInjection = 0;
    std::int64_t rejectedFrame = -1;
    mvm::preview::PreviewSourceId videoSource{};
    mvm::preview::PreviewSourceId videoSourceB{};
    mvm::preview::PreviewSourceId videoSourceC{};
    mvm::preview::PreviewSourceId firstSource{};
    mvm::preview::PreviewSourceId audioSource{};
    std::uint64_t presentedAtPause = 0;
    std::chrono::steady_clock::time_point modeStarted;
    bool missingPairSettled = false;
    bool removalChecked = false;
    bool partialRequestFailureValid = false;
    std::uint64_t seekRequestsBeforeTwoSource = 0;
    std::uint64_t seekCompletedBeforeTwoSource = 0;
    std::uint64_t presentedAfterSeek = 0;
    mvm::preview::internal::P5CRuntimeDiagnostics afterRemoval;
    mvm::preview::internal::P5CRuntimeDiagnostics activeDiagnostics;
    const auto started = std::chrono::steady_clock::now();
    int exitCode = 0;

    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - started > std::chrono::seconds(30)) {
            std::fprintf(stderr, "P5-E smokeが30秒以内に完了しませんでした\n");
            exitCode = 10;
            app.quit();
            return;
        }
        const auto status = engine->status();
        const auto telemetry = engine->telemetry();

        if (stage == Stage::WaitDevice &&
            status.state == mvm::preview::PreviewEngineState::ReadyPaused) {
            mvm::preview::PreviewSourceDescriptor descriptor;
            descriptor.mediaPath = arguments[1].toStdWString();
            descriptor.videoEnabled = true;

            if (fault == Fault::ExceedLayerCount &&
                !mvm::preview::internal::PreviewRenderPort::setVideoSourceLimitForTest(*engine,
                                                                                       3)) {
                exitCode = 49;
                app.quit();
                return;
            }

            if (fault == Fault::RemoveUnreferencedSource) {
                // composition から参照される前の source は解放できる。
                // 削除 -> 再登録で ID が使い回されないことも同時に固定する。
                const auto first = engine->addSource(descriptor);
                if (!first) {
                    std::fprintf(stderr, "初回source open失敗: %s\n",
                                 first.error().detail.c_str());
                    exitCode = 4;
                    app.quit();
                    return;
                }
                firstSource = first.value();
                const auto beforeRemoval =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                const auto removed = engine->removeSource(first.value());
                const auto afterFirstRemoval =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                // 二重削除は`InvalidSource`。削除済みIDを黙って成功にしない。
                const auto again = engine->removeSource(first.value());
                if (!removed || beforeRemoval.registeredVideoSourceCount != 1 ||
                    afterFirstRemoval.registeredVideoSourceCount != 0 || again ||
                    again.error().category != mvm::preview::PreviewErrorCategory::InvalidSource) {
                    std::fprintf(stderr, "未参照sourceのremovalが期待どおりではありません\n");
                    exitCode = 14;
                    app.quit();
                    return;
                }
                removalChecked = true;
            }

            auto source = engine->addSource(descriptor);
            if (!source) {
                std::fprintf(stderr, "source open失敗: %s\n", source.error().detail.c_str());
                exitCode = 4;
                app.quit();
                return;
            }
            videoSource = source.value();
            if (fault == Fault::RemoveUnreferencedSource && firstSource == videoSource) {
                // 削除したpublic IDを再登録で使い回さない。使い回すと、古い参照が
                // 別のsourceへ黙って結び付く (preview-engine-contract.md §6)。
                std::fprintf(stderr, "削除済みのPreviewSourceIdを再利用しました\n");
                exitCode = 22;
                app.quit();
                return;
            }
            if (fault == Fault::SeekTwoSource) {
                auto aOnly = std::make_shared<mvm::preview::CompositionSnapshot>();
                aOnly->layers.push_back(
                    {videoSource, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
                const auto aOnlyToken = engine->submitComposition(aOnly);
                if (!aOnlyToken || aOnlyToken.value().id.value != 1 ||
                    aOnlyToken.value().revision != 1 || !engine->seek({60})) {
                    std::fprintf(stderr, "A-only paused seekを開始できません\n");
                    exitCode = 50;
                    app.quit();
                    return;
                }
                accepted = aOnlyToken.value();
                stage = Stage::WaitPrepareSeek;
                return;
            }
            if (usesTwoVideoSources(fault)) {
                mvm::preview::PreviewSourceDescriptor descriptorB;
                descriptorB.mediaPath = arguments[2].toStdWString();
                descriptorB.videoEnabled = true;
                const auto second = engine->addSource(descriptorB);
                if (!second) {
                    std::fprintf(stderr, "二つ目のsource open失敗: %s\n",
                                 second.error().detail.c_str());
                    exitCode = 31;
                    app.quit();
                    return;
                }
                videoSourceB = second.value();
                if (fault == Fault::ExceedSourceCount) {
                    const auto third = engine->addSource(descriptor);
                    if (third || third.error().category !=
                                     mvm::preview::PreviewErrorCategory::UnsupportedCapability) {
                        std::fprintf(stderr, "capabilityを超えるvideo sourceを受理しました\n");
                        exitCode = 32;
                        app.quit();
                        return;
                    }
                }
                if (fault == Fault::ExceedLayerCount) {
                    const auto third = engine->addSource(descriptorB);
                    if (!third) {
                        std::fprintf(stderr, "三つ目のsource open失敗: %s\n",
                                     third.error().detail.c_str());
                        exitCode = 51;
                        app.quit();
                        return;
                    }
                    videoSourceC = third.value();
                }
            }
            if (needsAudioSource(fault)) {
                // audio-onlyのsourceはcompositionへ参加しないので、video再生中でも
                // 参照は外れている。authoritative audio sourceの解放経路を固定する。
                mvm::preview::PreviewSourceDescriptor audioDescriptor;
                audioDescriptor.mediaPath = arguments[1].toStdWString();
                audioDescriptor.videoEnabled = false;
                audioDescriptor.audioEnabled = true;
                const auto audio = engine->addSource(audioDescriptor);
                if (!audio) {
                    std::fprintf(stderr, "audio source open失敗: %s\n",
                                 audio.error().detail.c_str());
                    exitCode = 15;
                    app.quit();
                    return;
                }
                audioSource = audio.value();
            }
            // composition runtimeが未構成の間はepoch advance seamも成立しない。
            // seam自体がfail-closedであることをここで固定する。
            const auto tooEarly =
                mvm::preview::internal::PreviewRenderPort::injectCompositionEpochAdvanceForTest(
                    *engine);
            if (tooEarly ||
                tooEarly.error().category != mvm::preview::PreviewErrorCategory::InvalidState) {
                exitCode = 11;
                app.quit();
                return;
            }
            auto snapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            snapshot->layers.push_back(
                {source.value(), {0, 0, usesTwoVideoSources(fault) ? 0.5F : 1.0F, 1},
                 {0, 0, 1, 1}, 1.0F});
            if (usesTwoVideoSources(fault)) {
                snapshot->layers.push_back(
                    {videoSourceB, {0.5F, 0, 0.5F, 1}, {0, 0, 1, 1}, 1.0F});
            }
            if (fault == Fault::ExceedLayerCount) {
                auto excessive = std::make_shared<mvm::preview::CompositionSnapshot>(*snapshot);
                excessive->layers.push_back(
                    {videoSourceC, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
                const auto rejected = engine->submitComposition(excessive);
                if (rejected || rejected.error().category !=
                                    mvm::preview::PreviewErrorCategory::UnsupportedCapability) {
                    std::fprintf(stderr, "capabilityを超えるlayer数を受理しました\n");
                    exitCode = 33;
                    app.quit();
                    return;
                }
            }
            if (fault == Fault::DuplicateSourceLayer) {
                auto duplicate = std::make_shared<mvm::preview::CompositionSnapshot>();
                duplicate->layers.push_back(
                    {videoSource, {0, 0, 0.5F, 1}, {0, 0, 1, 1}, 1.0F});
                duplicate->layers.push_back(
                    {videoSource, {0.5F, 0, 0.5F, 1}, {0, 0, 1, 1}, 1.0F});
                const auto rejected = engine->submitComposition(duplicate);
                if (rejected || rejected.error().category !=
                                    mvm::preview::PreviewErrorCategory::UnsupportedCapability) {
                    std::fprintf(stderr, "同一sourceの重複layerを受理しました\n");
                    exitCode = 34;
                    app.quit();
                    return;
                }
            }
            auto unknown = std::make_shared<mvm::preview::CompositionSnapshot>();
            unknown->layers.push_back(
                {{videoSourceB.value + 1000}, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            const auto unknownResult = engine->submitComposition(unknown);
            if (unknownResult || unknownResult.error().category !=
                                     mvm::preview::PreviewErrorCategory::InvalidSource) {
                std::fprintf(stderr, "未登録sourceを含むcompositionを受理しました\n");
                exitCode = 35;
                app.quit();
                return;
            }
            auto composition = engine->submitComposition(snapshot);
            const auto noOp = engine->submitComposition(snapshot);
            if (!composition || !noOp || noOp.value() != composition.value() ||
                composition.value().id.value != 1 || composition.value().revision != 1 ||
                !engine->play()) {
                exitCode = 5;
                app.quit();
                return;
            }
            accepted = composition.value();
            stage = Stage::WaitInitial;
            return;
        }
        if (stage == Stage::WaitPrepareSeek) {
            const auto prepared =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (status.state != mvm::preview::PreviewEngineState::ReadyPaused ||
                prepared.seekCompletedCount != 1) {
                return;
            }
            const auto generationA = prepared.videoSourceGenerations.find(videoSource.value);
            if (generationA == prepared.videoSourceGenerations.end()) {
                exitCode = 52;
                app.quit();
                return;
            }

            mvm::preview::PreviewSourceDescriptor descriptorB;
            descriptorB.mediaPath = arguments[2].toStdWString();
            descriptorB.videoEnabled = true;
            const auto second = engine->addSource(descriptorB);
            if (!second) {
                exitCode = 53;
                app.quit();
                return;
            }
            videoSourceB = second.value();
            const auto divergent =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            const auto currentA = divergent.videoSourceGenerations.find(videoSource.value);
            const auto generationB = divergent.videoSourceGenerations.find(videoSourceB.value);
            if (currentA == divergent.videoSourceGenerations.end() ||
                generationB == divergent.videoSourceGenerations.end() ||
                currentA->second != generationA->second || currentA->second == generationB->second) {
                std::fprintf(stderr, "A/Bのseek前generationが分岐していません\n");
                exitCode = 54;
                app.quit();
                return;
            }

            auto twoSource = std::make_shared<mvm::preview::CompositionSnapshot>();
            twoSource->layers.push_back(
                {videoSource, {0, 0, 0.5F, 1}, {0, 0, 1, 1}, 1.0F});
            twoSource->layers.push_back(
                {videoSourceB, {0.5F, 0, 0.5F, 1}, {0, 0, 1, 1}, 1.0F});
            const auto composition = engine->submitComposition(twoSource);
            if (!composition || composition.value().id.value != 2 ||
                composition.value().revision != 2 || !engine->play()) {
                exitCode = 55;
                app.quit();
                return;
            }
            accepted = composition.value();
            stage = Stage::WaitInitial;
            return;
        }
        if (stage == Stage::WaitInitial && telemetry.presentedFrameCount >= 12) {
            if (fault == Fault::MissingPair) {
                if (!mvm::preview::internal::PreviewRenderPort::suspendVideoSourceForTest(
                        *engine, videoSourceB)) {
                    exitCode = 36;
                    app.quit();
                    return;
                }
                modeStarted = now;
                stage = Stage::WaitMissingPair;
                return;
            }
            if (fault == Fault::RemoveReleasedSource) {
                auto replacement = std::make_shared<mvm::preview::CompositionSnapshot>();
                replacement->layers.push_back(
                    {videoSourceB, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
                const auto replacementToken = engine->submitComposition(replacement);
                if (!replacementToken) {
                    exitCode = 37;
                    app.quit();
                    return;
                }
                accepted = replacementToken.value();
                stage = Stage::WaitReplacement;
                return;
            }
            if (fault == Fault::SeekTwoSource || fault == Fault::SeekPartialGeneration ||
                fault == Fault::SeekPartialRequest) {
                activeDiagnostics =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                seekRequestsBeforeTwoSource = activeDiagnostics.seekRequestCount;
                seekCompletedBeforeTwoSource = activeDiagnostics.seekCompletedCount;
                if (fault == Fault::SeekPartialGeneration &&
                    !mvm::preview::internal::PreviewRenderPort::
                        injectSeekVideoGenerationMismatchForTest(*engine, videoSourceB)) {
                    exitCode = 38;
                    app.quit();
                    return;
                }
                if (fault == Fault::SeekPartialRequest) {
                    if (!mvm::preview::internal::PreviewRenderPort::armVideoSeekRequestForTest(
                            *engine, videoSourceB, {240})) {
                        exitCode = 56;
                        app.quit();
                        return;
                    }
                    const auto rejected = engine->seek({120});
                    partialRequestFailureValid =
                        !rejected &&
                        rejected.error().category ==
                            mvm::preview::PreviewErrorCategory::SeekFailure &&
                        rejected.error().operation == mvm::preview::PreviewOperation::Seek &&
                        rejected.error().severity ==
                            mvm::preview::PreviewErrorSeverity::FatalToSession &&
                        rejected.error().source == videoSourceB;
                    if (!partialRequestFailureValid) {
                        std::fprintf(stderr, "一部だけ受理されたseek requestを拒否できません\n");
                        exitCode = 57;
                        app.quit();
                        return;
                    }
                    stage = Stage::WaitShutdown;
                    return;
                }
                if (!engine->seek({120})) {
                    exitCode = 39;
                    app.quit();
                    return;
                }
                modeStarted = now;
                stage = Stage::WaitSeek;
                return;
            }
            if (checksRemovalAfterPause(fault)) {
                // removalはReadyPausedでのみ受理する。Playing中の拒否も同時に固定する。
                const auto whilePlaying = engine->removeSource(videoSource);
                if (whilePlaying || whilePlaying.error().category !=
                                        mvm::preview::PreviewErrorCategory::InvalidState) {
                    std::fprintf(stderr, "Playing中のremoveSourceを受理しました\n");
                    exitCode = 16;
                    app.quit();
                    return;
                }
                if (!engine->pause()) {
                    exitCode = 17;
                    app.quit();
                    return;
                }
                presentedAtPause = engine->telemetry().presentedFrameCount;
                // fault経路はここから先でterminalへ落ちる。active runtimeの証拠は
                // 落ちる前に採る (teardown後のdiagnosticsでは device が解放済み)。
                activeDiagnostics =
                    mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);

                // 未登録IDは`InvalidSource`。stateの都合で別errorへすり替えない。
                const auto unknown = engine->removeSource({videoSource.value + 100});
                // active / pending compositionが参照しているsourceは解放できない。
                const auto referenced = engine->removeSource(videoSource);
                if (unknown ||
                    unknown.error().category !=
                        mvm::preview::PreviewErrorCategory::InvalidSource ||
                    referenced || referenced.error().category !=
                                      mvm::preview::PreviewErrorCategory::InvalidState) {
                    std::fprintf(stderr, "参照中sourceのremoval guardが効いていません\n");
                    exitCode = 18;
                    app.quit();
                    return;
                }
                if (fault == Fault::RemoveAudioSource) {
                    const auto removedAudio = engine->removeSource(audioSource);
                    afterRemoval =
                        mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                    if (!removedAudio || afterRemoval.registeredAudioSourceCount != 0 ||
                        afterRemoval.audioMasterActive) {
                        std::fprintf(stderr, "audio authorityを空へ戻せていません\n");
                        exitCode = 19;
                        app.quit();
                        return;
                    }
                }
                if (fault == Fault::RemoveAudioStopFailure) {
                    // 完成したerrorをengineへ注入するのではなく、audio transportの
                    // pause/stop sequenceのうちfallibleな`pause()`をsink自身に
                    // 失敗させ、通常のremoval経路を通す。
                    if (!mvm::preview::internal::PreviewRenderPort::
                            injectAudioSinkPauseFaultForTest(*engine)) {
                        exitCode = 23;
                        app.quit();
                        return;
                    }
                    const auto removedAudio = engine->removeSource(audioSource);
                    afterRemoval =
                        mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                    // 停止できなかったのでremovalはcommitしない。
                    if (removedAudio ||
                        removedAudio.error().category !=
                            mvm::preview::PreviewErrorCategory::AudioFailure ||
                        removedAudio.error().severity !=
                            mvm::preview::PreviewErrorSeverity::FatalToSession ||
                        removedAudio.error().operation !=
                            mvm::preview::PreviewOperation::RemoveSource ||
                        afterRemoval.registeredAudioSourceCount != 1) {
                        std::fprintf(stderr, "audio stop失敗をfail-closedにできていません\n");
                        exitCode = 24;
                        app.quit();
                        return;
                    }
                    removalChecked = true;
                    stage = Stage::WaitShutdown;
                    return;
                }
                if (fault == Fault::RemoveFatalEventOrder) {
                    // removal fatalをcommitしてunlockした直後で止め、その間に
                    // teardownを終端(Error)まで進める。state commitとmailbox
                    // insertionがlinearizeされていれば、mailbox insertion順は
                    //   removal ErrorOccurred < ShuttingDown < terminal Error
                    // になる。分離しているとShuttingDownがErrorに追い越される。
                    if (!mvm::preview::internal::PreviewRenderPort::
                            injectAudioSinkPauseFaultForTest(*engine) ||
                        !mvm::preview::internal::PreviewRenderPort::armFatalPublishBarrierForTest(
                            *engine)) {
                        exitCode = 27;
                        app.quit();
                        return;
                    }
                    bool reachedTerminal = false;
                    bool linearizedAtCommit = false;
                    bool orderedAcrossTerminal = false;
                    // mailbox snapshotから3つのeventのinsertion位置を取る。
                    // ErrorOccurredは「最初の1件」ではなくremoval由来のものだけを
                    // 識別する。別要因のerrorが先に入っていても誤判定しない。
                    const auto orderIndices = [&](const std::vector<
                                                      mvm::preview::internal::PreviewEvent>& queued,
                                                  std::size_t& removeErrorIndex,
                                                  std::size_t& shuttingDownIndex,
                                                  std::size_t& terminalErrorIndex) {
                        removeErrorIndex = queued.size();
                        shuttingDownIndex = queued.size();
                        terminalErrorIndex = queued.size();
                        for (std::size_t i = 0; i < queued.size(); ++i) {
                            const auto* occurred =
                                std::get_if<mvm::preview::internal::ErrorOccurredEvent>(&queued[i]);
                            if (occurred != nullptr && removeErrorIndex == queued.size() &&
                                occurred->error.operation ==
                                    mvm::preview::PreviewOperation::RemoveSource &&
                                occurred->error.category ==
                                    mvm::preview::PreviewErrorCategory::AudioFailure &&
                                occurred->error.source && *occurred->error.source == audioSource) {
                                removeErrorIndex = i;
                            }
                            const auto* queuedState =
                                std::get_if<mvm::preview::internal::StateChangedEvent>(&queued[i]);
                            if (queuedState == nullptr)
                                continue;
                            if (queuedState->state ==
                                    mvm::preview::PreviewEngineState::ShuttingDown &&
                                shuttingDownIndex == queued.size()) {
                                shuttingDownIndex = i;
                            }
                            if (queuedState->state == mvm::preview::PreviewEngineState::Error &&
                                terminalErrorIndex == queued.size()) {
                                terminalErrorIndex = i;
                            }
                        }
                    };
                    std::thread racer([&] {
                        if (!mvm::preview::internal::PreviewRenderPort::
                                waitFatalPublishBarrierEnteredForTest(*engine, 5000)) {
                            mvm::preview::internal::PreviewRenderPort::
                                releaseFatalPublishBarrierForTest(*engine);
                            return;
                        }
                        // (1) unlock直後・flush前の時点で、removalのeventが既にmailboxへ
                        // 入っていること。空なら state commit と mailbox insertion が
                        // 分離しており、後続の terminal event に追い越される。
                        {
                            const auto queued = mvm::preview::internal::PreviewRenderPort::
                                mailboxEventsForTest(*engine);
                            std::size_t removeErrorIndex = 0;
                            std::size_t shuttingDownIndex = 0;
                            std::size_t terminalErrorIndex = 0;
                            orderIndices(queued, removeErrorIndex, shuttingDownIndex,
                                         terminalErrorIndex);
                            linearizedAtCommit = removeErrorIndex < queued.size() &&
                                                 shuttingDownIndex < queued.size() &&
                                                 removeErrorIndex < shuttingDownIndex;
                        }
                        // render threadがteardownを終端まで進めるのを待つ。
                        // main threadはremoveSource()の中で止まっているが、
                        // event順序はmailbox insertion順で決まる。
                        const auto deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        while (std::chrono::steady_clock::now() < deadline) {
                            if (engine->status().state ==
                                mvm::preview::PreviewEngineState::Error) {
                                reachedTerminal = true;
                                break;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        }
                        // (2) terminal到達後・barrier release前にもう一度観測する。
                        // ここが本題である。terminal `Error` がmailboxへ入ってもなお
                        //   removal ErrorOccurred < ShuttingDown < terminal Error
                        // でなければ、`ShuttingDown`はstaleとして追い越されている。
                        // (1)だけでは、Errorが先頭に入ったmailboxでも通ってしまう。
                        if (reachedTerminal) {
                            const auto terminalDeadline =
                                std::chrono::steady_clock::now() + std::chrono::seconds(5);
                            while (std::chrono::steady_clock::now() < terminalDeadline) {
                                const auto queued = mvm::preview::internal::PreviewRenderPort::
                                    mailboxEventsForTest(*engine);
                                std::size_t removeErrorIndex = 0;
                                std::size_t shuttingDownIndex = 0;
                                std::size_t terminalErrorIndex = 0;
                                orderIndices(queued, removeErrorIndex, shuttingDownIndex,
                                             terminalErrorIndex);
                                if (terminalErrorIndex == queued.size()) {
                                    // state machineはErrorだがeventがまだ積まれていない。
                                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                    continue;
                                }
                                orderedAcrossTerminal = removeErrorIndex < shuttingDownIndex &&
                                                        shuttingDownIndex < terminalErrorIndex;
                                break;
                            }
                        }
                        mvm::preview::internal::PreviewRenderPort::
                            releaseFatalPublishBarrierForTest(*engine);
                    });
                    const auto removedAudio = engine->removeSource(audioSource);
                    racer.join();
                    afterRemoval =
                        mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                    if (!reachedTerminal || removedAudio ||
                        removedAudio.error().category !=
                            mvm::preview::PreviewErrorCategory::AudioFailure ||
                        afterRemoval.registeredAudioSourceCount != 1) {
                        std::fprintf(stderr, "fatal publish barrierの前提が成立しません\n");
                        exitCode = 28;
                        app.quit();
                        return;
                    }
                    if (!linearizedAtCommit) {
                        std::fprintf(stderr,
                                     "state commitとmailbox insertionが分離しています\n");
                        exitCode = 29;
                        app.quit();
                        return;
                    }
                    if (!orderedAcrossTerminal) {
                        std::fprintf(
                            stderr,
                            "ShuttingDownがterminal Errorに追い越されています\n");
                        exitCode = 30;
                        app.quit();
                        return;
                    }
                    removalChecked = true;
                    stage = Stage::WaitShutdown;
                    return;
                }
                if (fault == Fault::RemoveShutdownRace) {
                    // audio停止フェーズ (engine lockを持たない窓) で止め、
                    // その間に別threadからfatalを入れる。removalはcommitされない。
                    if (!mvm::preview::internal::PreviewRenderPort::
                            armSourceRemovalBarrierForTest(*engine)) {
                        exitCode = 25;
                        app.quit();
                        return;
                    }
                    bool raced = false;
                    std::thread racer([&] {
                        if (!mvm::preview::internal::PreviewRenderPort::
                                waitSourceRemovalBarrierEnteredForTest(*engine, 5000)) {
                            mvm::preview::internal::PreviewRenderPort::
                                releaseSourceRemovalBarrierForTest(*engine);
                            return;
                        }
                        mvm::preview::PreviewError error;
                        error.category = mvm::preview::PreviewErrorCategory::DeviceFailure;
                        error.severity = mvm::preview::PreviewErrorSeverity::FatalToSession;
                        error.operation = mvm::preview::PreviewOperation::RenderDeviceAttach;
                        error.detail = "P5-E removal race injected fatal";
                        raced = static_cast<bool>(
                            mvm::preview::internal::PreviewRenderPort::injectFatal(*engine, error));
                        mvm::preview::internal::PreviewRenderPort::
                            releaseSourceRemovalBarrierForTest(*engine);
                    });
                    const auto removedAudio = engine->removeSource(audioSource);
                    racer.join();
                    afterRemoval =
                        mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
                    if (!raced || removedAudio ||
                        removedAudio.error().category !=
                            mvm::preview::PreviewErrorCategory::InvalidState ||
                        afterRemoval.registeredAudioSourceCount != 1) {
                        std::fprintf(stderr, "removal中のstate変化でcommitを止めていません\n");
                        exitCode = 26;
                        app.quit();
                        return;
                    }
                    removalChecked = true;
                    stage = Stage::WaitShutdown;
                    return;
                }
                removalChecked = true;
                // 解放後もsessionは継続できる。audioを外した場合はwall-clock masterへ戻る。
                if (!engine->play()) {
                    exitCode = 20;
                    app.quit();
                    return;
                }
                stage = Stage::WaitResume;
                return;
            }
            if (fault == Fault::StaleCompositionEpoch) {
                presentedAtInjection = telemetry.presentedFrameCount;
                if (!mvm::preview::internal::PreviewRenderPort::
                        injectCompositionEpochAdvanceForTest(*engine)) {
                    exitCode = 12;
                    app.quit();
                    return;
                }
                stage = Stage::WaitRecovered;
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 6;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitReplacement) {
            if (!status.lastPresentedComposition || *status.lastPresentedComposition != accepted)
                return;
            if (!engine->pause()) {
                exitCode = 40;
                app.quit();
                return;
            }
            const auto removed = engine->removeSource(videoSource);
            auto removedSnapshot = std::make_shared<mvm::preview::CompositionSnapshot>();
            removedSnapshot->layers.push_back(
                {videoSource, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F});
            const auto rejected = engine->submitComposition(removedSnapshot);
            if (!removed || rejected ||
                rejected.error().category != mvm::preview::PreviewErrorCategory::InvalidSource) {
                std::fprintf(stderr, "参照解放後のsource removalが成立しません\n");
                exitCode = 41;
                app.quit();
                return;
            }
            removalChecked = true;
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 42;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitMissingPair) {
            // pause直前にdecode中だった1 frameがbufferへ到着し得る。workerが
            // pauseを観測する時間を置いて再度clearし、その後を観測窓にする。
            if (!missingPairSettled && now - modeStarted >= std::chrono::milliseconds(250)) {
                if (!mvm::preview::internal::PreviewRenderPort::suspendVideoSourceForTest(
                        *engine, videoSourceB)) {
                    exitCode = 48;
                    app.quit();
                    return;
                }
                presentedAtInjection = engine->telemetry().presentedFrameCount;
                modeStarted = now;
                missingPairSettled = true;
                return;
            }
            if (!missingPairSettled)
                return;
            if (now - modeStarted < std::chrono::milliseconds(500))
                return;
            if (telemetry.presentedFrameCount != presentedAtInjection) {
                std::fprintf(stderr, "exact pair不足時にframeを代用して提示しました\n");
                exitCode = 43;
                app.quit();
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 44;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitSeek) {
            const auto seekDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (fault == Fault::SeekPartialGeneration) {
                if (status.state != mvm::preview::PreviewEngineState::Error)
                    return;
                const bool classified = status.lastError &&
                                        status.lastError->category ==
                                            mvm::preview::PreviewErrorCategory::SeekFailure &&
                                        status.lastError->operation ==
                                            mvm::preview::PreviewOperation::Seek &&
                                        status.lastError->severity ==
                                            mvm::preview::PreviewErrorSeverity::FatalToSession;
                if (!classified || seekDiagnostics.seekRequestCount != 1 ||
                    seekDiagnostics.seekDecodeReadyCount != 1 ||
                    seekDiagnostics.seekCompletedCount != 0 ||
                    seekDiagnostics.seekStaleGenerationRejectCount == 0) {
                    std::fprintf(stderr, "片側generation不一致のseekを完了扱いしました\n");
                    exitCode = 45;
                    app.quit();
                    return;
                }
                stage = Stage::WaitShutdown;
                return;
            }
            if (seekDiagnostics.seekRequestCount != seekRequestsBeforeTwoSource + 1 ||
                seekDiagnostics.seekCompletedCount != seekCompletedBeforeTwoSource + 1 ||
                status.state != mvm::preview::PreviewEngineState::Playing ||
                seekDiagnostics.seekCancelledByShutdownCount != 0)
                return;
            if (seekDiagnostics.lastSeekPresentedFrame != 120 ||
                !status.lastPresentedComposition || *status.lastPresentedComposition != accepted) {
                std::fprintf(stderr, "二source exact seekのcompletionが不正です\n");
                exitCode = 46;
                app.quit();
                return;
            }
            presentedAfterSeek = telemetry.presentedFrameCount;
            stage = Stage::WaitSeekContinued;
            return;
        }
        if (stage == Stage::WaitSeekContinued) {
            if (status.state != mvm::preview::PreviewEngineState::Playing ||
                telemetry.presentedFrameCount < presentedAfterSeek + 8) {
                return;
            }
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            const auto generationA =
                activeDiagnostics.videoSourceGenerations.find(videoSource.value);
            const auto generationB =
                activeDiagnostics.videoSourceGenerations.find(videoSourceB.value);
            if (generationA == activeDiagnostics.videoSourceGenerations.end() ||
                generationB == activeDiagnostics.videoSourceGenerations.end() ||
                generationA->second == generationB->second) {
                std::fprintf(stderr, "二source seek完了後のA/B generationが分岐していません\n");
                exitCode = 58;
                app.quit();
                return;
            }
            if (activeDiagnostics.seekCancelledByShutdownCount != 0 ||
                !engine->requestShutdown()) {
                exitCode = 47;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitResume) {
            if (telemetry.presentedFrameCount < presentedAtPause + 8)
                return;
            activeDiagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (!engine->requestShutdown()) {
                exitCode = 21;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitRecovered) {
            const auto probe =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            if (probe.staleCompositionEpochRejectCount == 0) {
                // 進めたepochが提示前に弾かれないまま提示が進み続けたなら、
                // 製品経路にvalidateForDisplay()が効いていない。timeoutではなく
                // 即座にFAILとして落とす。
                if (telemetry.presentedFrameCount >= presentedAtInjection + 60) {
                    std::fprintf(
                        stderr, "stale composition epochが提示前に拒否されませんでした\n");
                    exitCode = 8;
                    app.quit();
                }
                return;
            }
            // rejectはskipであってfatalではない。engineは次のrender callbackで
            // 実tokenのstateへ戻り、提示を再開できなければならない。
            if (telemetry.presentedFrameCount < presentedAtInjection + 8)
                return;
            activeDiagnostics = probe;
            rejectedFrame = probe.lastStaleCompositionRejectedFrame;
            if (rejectedFrame < 0) {
                std::fprintf(stderr, "拒否したoutput frameを記録していません\n");
                exitCode = 13;
                app.quit();
                return;
            }
            if (!engine->requestShutdown()) {
                exitCode = 7;
                app.quit();
                return;
            }
            stage = Stage::WaitShutdown;
            return;
        }
        if (stage == Stage::WaitShutdown &&
            (status.state == mvm::preview::PreviewEngineState::Shutdown ||
             status.state == mvm::preview::PreviewEngineState::Error)) {
            const auto terminalTelemetry = engine->telemetry();
            const auto diagnostics =
                mvm::preview::internal::PreviewRenderPort::runtimeDiagnostics(*engine);
            // 拒否したframeが実際に提示されていないことを、frame identityで確認する。
            // rejectを数えたうえでそのまま描画するbugは、counterだけでは通ってしまう。
            bool rejectedFramePresented = false;
            bool resumedAfterRejectedFrame = false;
            for (const auto& presented : sink->frames) {
                if (presented.position.outputFrame == rejectedFrame)
                    rejectedFramePresented = true;
                if (presented.position.outputFrame > rejectedFrame)
                    resumedAfterRejectedFrame = true;
            }
            const bool staleEpochPass =
                fault == Fault::StaleCompositionEpoch
                    ? diagnostics.staleCompositionEpochRejectCount == 1 &&
                          diagnostics.lifecycleViolationCount == 1 && rejectedFrame >= 0 &&
                          diagnostics.lastStaleCompositionRejectedFrame == rejectedFrame &&
                          !rejectedFramePresented && resumedAfterRejectedFrame
                    : diagnostics.staleCompositionEpochRejectCount == 0 &&
                          diagnostics.lifecycleViolationCount == 0 &&
                          diagnostics.lastStaleCompositionRejectedFrame == -1;

            // fatalを伴うfaultはterminal Errorで終わる。
            const bool fatalFault = fault == Fault::RemoveAudioStopFailure ||
                                    fault == Fault::RemoveShutdownRace ||
                                    fault == Fault::RemoveFatalEventOrder ||
                                    fault == Fault::SeekPartialGeneration ||
                                    fault == Fault::SeekPartialRequest;
            // event streamの順序を固定する。ShuttingDownはErrorより前に出る。
            // state commitとmailbox insertionをlinearizeしていないと、先に進んだ
            // teardownのErrorがShuttingDownを追い越し得る。
            std::size_t shuttingDownIndex = sink->states.size();
            std::size_t errorIndex = sink->states.size();
            for (std::size_t i = 0; i < sink->states.size(); ++i) {
                if (sink->states[i] == mvm::preview::PreviewEngineState::ShuttingDown &&
                    shuttingDownIndex == sink->states.size()) {
                    shuttingDownIndex = i;
                }
                if (sink->states[i] == mvm::preview::PreviewEngineState::Error &&
                    errorIndex == sink->states.size()) {
                    errorIndex = i;
                }
            }
            // terminal到達後のpending eventはcontractどおり破棄されるため、
            // 「ShuttingDownがsinkへ届くこと」をfatal fault一般には要求できない。
            // 届いた場合の順序だけを要求し、linearizabilityそのものは
            // mailbox insertion順で別途固定する。
            const bool stateOrderPass =
                fatalFault ? (shuttingDownIndex == sink->states.size() ||
                              errorIndex == sink->states.size() ||
                              shuttingDownIndex < errorIndex)
                           : errorIndex == sink->states.size();
            const bool fatalPass =
                fault == Fault::RemoveFatalEventOrder
                    // barrierでmain threadを止めている間にterminalへ到達するため、
                    // pending eventはcontractどおり破棄され得る。ここで固定するのは
                    // counter authorityであり、deliveryではない。
                    ? diagnostics.audioTransportFailureCount == 1
                : fault == Fault::RemoveAudioStopFailure
                    // sink停止失敗はtransport failureとして1件だけ数える。
                    ? sink->errors.size() == 1 &&
                          sink->errors.front().category ==
                              mvm::preview::PreviewErrorCategory::AudioFailure &&
                          sink->errors.front().operation ==
                              mvm::preview::PreviewOperation::RemoveSource &&
                          sink->errors.front().severity ==
                              mvm::preview::PreviewErrorSeverity::FatalToSession &&
                          diagnostics.audioTransportFailureCount == 1
                : fault == Fault::RemoveShutdownRace
                    // raceで止めたのはremovalであり、removal自体はfailureではない。
                    ? sink->errors.size() == 1 &&
                          sink->errors.front().category ==
                              mvm::preview::PreviewErrorCategory::DeviceFailure &&
                          diagnostics.audioTransportFailureCount == 0
                : fault == Fault::SeekPartialGeneration
                    ? sink->errors.size() == 1 &&
                          sink->errors.front().category ==
                              mvm::preview::PreviewErrorCategory::SeekFailure &&
                          sink->errors.front().operation == mvm::preview::PreviewOperation::Seek &&
                          sink->errors.front().severity ==
                              mvm::preview::PreviewErrorSeverity::FatalToSession &&
                          diagnostics.seekRequestCount == 1 &&
                          diagnostics.seekDecodeReadyCount == 1 &&
                          diagnostics.seekCompletedCount == 0 &&
                          diagnostics.seekStaleGenerationRejectCount > 0
                : fault == Fault::SeekPartialRequest
                    ? partialRequestFailureValid && sink->errors.size() == 1 &&
                          sink->errors.front().category ==
                              mvm::preview::PreviewErrorCategory::SeekFailure &&
                          sink->errors.front().operation ==
                              mvm::preview::PreviewOperation::Seek &&
                          sink->errors.front().severity ==
                              mvm::preview::PreviewErrorSeverity::FatalToSession &&
                          sink->errors.front().source == videoSourceB &&
                          diagnostics.seekVideoRequestAcceptedCount == 1 &&
                          diagnostics.seekRequestCount == 0 &&
                          diagnostics.seekCompletedCount == 0
                    : sink->errors.empty();
            const bool removalFault =
                fault == Fault::RemoveUnreferencedSource ||
                fault == Fault::RemoveReferencedSource || fault == Fault::RemoveAudioSource ||
                fault == Fault::RemoveAudioStopFailure || fault == Fault::RemoveShutdownRace ||
                fault == Fault::RemoveFatalEventOrder || fault == Fault::RemoveReleasedSource;
            const bool removalPass =
                !removalFault
                    ? !removalChecked
                    : removalChecked &&
                          (fault != Fault::RemoveAudioSource ||
                           (diagnostics.registeredAudioSourceCount == 0 &&
                            !diagnostics.audioMasterActive &&
                            // audio解放はfailureではない。counterを汚さない。
                            diagnostics.audioTransportFailureCount == 0 &&
                            diagnostics.audioSinkDeviceFailureCount == 0 &&
                            diagnostics.audioDomainRejectCount == 0 &&
                            // video-only経路のwall-clockはqualified masterであり退避ではない。
                            diagnostics.videoMasterQpcFallbackCount == 0)) &&
                          // 中断されたremovalはcommitされない。
                          (!fatalFault || (afterRemoval.registeredAudioSourceCount == 1 &&
                                           diagnostics.registeredAudioSourceCount == 1));
            const std::uint32_t expectedLayerCount =
                fault == Fault::RemoveReleasedSource || !usesTwoVideoSources(fault) ? 1U : 2U;
            const std::uint64_t expectedVideoSourceCount =
                fault == Fault::RemoveReleasedSource
                    ? 1U
                    : fault == Fault::ExceedLayerCount ? 3U : usesTwoVideoSources(fault) ? 2U : 1U;
            const bool pass =
                status.state == (fatalFault ? mvm::preview::PreviewEngineState::Error
                                            : mvm::preview::PreviewEngineState::Shutdown) &&
                staleEpochPass && removalPass && fatalPass && stateOrderPass &&
                !sink->sequenceViolation && !sink->frames.empty() &&
                // PresentedFrameInfoはactual accepted tokenとactual layer数を運ぶ。
                sink->frames.back().composition == accepted &&
                sink->frames.back().activeLayerCount == expectedLayerCount &&
                diagnostics.distinctPresentedSourceFrameCount >= 12 &&
                activeDiagnostics.nativeDeviceAttached && activeDiagnostics.d3d11vaActive &&
                activeDiagnostics.decodeRenderSameDevice &&
                activeDiagnostics.registeredVideoSourceCount == expectedVideoSourceCount &&
                diagnostics.fullCpuReadbackCount == 0 && diagnostics.fullFrameGpuCopyCount == 0 &&
                diagnostics.softwareFallbackCount == 0 && diagnostics.staleSubstitutionCount == 0 &&
                diagnostics.untrackedSubmissionCount == 0 &&
                diagnostics.earlyPayloadReleaseCount == 0 &&
                diagnostics.retirementTimeoutCount == 0 && diagnostics.workerJoined &&
                diagnostics.renderTeardownComplete && diagnostics.deviceReleased &&
                !diagnostics.unsafeGpuResourcesRetained &&
                // 中断されたremovalが二重stopやordering violationを起こしていないこと。
                (fault == Fault::StaleCompositionEpoch ||
                 diagnostics.lifecycleViolationCount == 0) &&
                terminalTelemetry.eventDeliveryFailureCount == 0;
            std::printf("{\"verdict\":\"%s\",\"presented\":%llu,"
                        "\"stale_composition_epoch_rejects\":%llu,"
                        "\"rejected_frame\":%lld,\"rejected_frame_presented\":%s,"
                        "\"removal_checked\":%s,\"audio_sources\":%llu,"
                        "\"state_order_ok\":%s,"
                        "\"lifecycle_violations\":%llu,\"errors\":%zu,\"terminal\":%d}\n",
                        pass ? "PASS" : "FAIL",
                        static_cast<unsigned long long>(terminalTelemetry.presentedFrameCount),
                        static_cast<unsigned long long>(
                            diagnostics.staleCompositionEpochRejectCount),
                        static_cast<long long>(diagnostics.lastStaleCompositionRejectedFrame),
                        rejectedFramePresented ? "true" : "false",
                        removalChecked ? "true" : "false",
                        static_cast<unsigned long long>(diagnostics.registeredAudioSourceCount),
                        stateOrderPass ? "true" : "false",
                        static_cast<unsigned long long>(diagnostics.lifecycleViolationCount),
                        sink->errors.size(), static_cast<int>(status.state));
            exitCode = pass ? 0 : 9;
            app.quit();
        }
    });
    timer.start();
    app.exec();
    return exitCode;
}
