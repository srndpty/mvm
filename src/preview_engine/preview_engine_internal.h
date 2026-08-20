#ifndef MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H
#define MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H

#include "preview_engine/preview_engine.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mvm::preview::internal {

struct StateChangedEvent {
    PreviewEngineState state;
};

struct PositionChangedEvent {
    PreviewPosition position;
};

struct FramePresentedEvent {
    PresentedFrameInfo frame;
};

struct ErrorOccurredEvent {
    PreviewError error;
};

struct DeviceChangedEvent {
    PreviewDeviceInfo device;
};

using PreviewEvent = std::variant<StateChangedEvent, PositionChangedEvent, FramePresentedEvent,
                                  ErrorOccurredEvent, DeviceChangedEvent>;

class EventMailbox {
public:
    explicit EventMailbox(std::size_t capacity);

    Result<void> push(PreviewEvent event);
    std::optional<PreviewEvent> pop();
    // 挿入順のまま複製する。test seam専用。
    std::vector<PreviewEvent> snapshot() const;
    std::size_t size() const;
    std::size_t capacity() const;
    bool empty() const;

private:
    bool isTerminal(const PreviewEvent& event) const;
    bool tryCoalesce(const PreviewEvent& event);

    std::size_t capacity_;
    std::deque<PreviewEvent> events_;
};

class PreviewStateMachine {
public:
    PreviewEngineState state() const;
    std::optional<PreviewError> lastError() const;
    bool destructionSafe() const;

    Result<void> initialize();
    Result<void> attachRenderDevice();
    Result<void> play();
    Result<void> pause();
    Result<void> seek();
    Result<void> completeSeek();
    Result<void> requestShutdown();
    Result<void> recordFatal(PreviewError error);
    Result<void> completeTeardown();

private:
    PreviewEngineState state_ = PreviewEngineState::Uninitialized;
    std::optional<PreviewError> lastError_;
    bool fatalPending_ = false;
    PreviewEngineState stateBeforeSeek_ = PreviewEngineState::ReadyPaused;
};

struct EligibleSource {
    bool videoEnabled = false;
    bool audioEnabled = false;
};

struct RenderFrameResult {
    bool presented = false;
    PresentedFrameInfo frame;
    std::int64_t sourceFrame = -1;
};

// preview-engine-contract.md §12 が固定するshutdown orderingを、最終状態ではなく
// 順序そのものとして検査するためのstep識別子。
enum class ShutdownStep {
    DisableSchedulers,
    StopAudioSink,
    StopAudioDecodeWorker,
    StopVideoWorkers,
    DetachRenderVisibleWorkerRefs,
    VerifyJoins,
    RequestRenderTeardown,
    FiniteGpuRetirementDrain,
    ReleaseRenderTargetDevice,
    PublishShutdownComplete,
};

struct P5CRuntimeDiagnostics {
    bool nativeDeviceAttached = false;
    bool d3d11vaActive = false;
    bool decodeRenderSameDevice = false;
    bool workerJoined = true;
    bool renderTeardownComplete = false;
    bool deviceReleased = true;
    bool unsafeGpuResourcesRetained = false;
    std::uint64_t registeredVideoSourceCount = 0;
    std::uint64_t distinctPresentedSourceFrameCount = 0;
    std::uint64_t staleSubstitutionCount = 0;
    // P5-E1: supersedeされたcomposition epochのframeを提示しなかった回数。
    // `CompositorCoordinator::validateForDisplay()`が権威である。
    std::uint64_t staleCompositionEpochRejectCount = 0;
    // 直近でstale composition epochとして拒否したoutput frame番号。未発生なら-1。
    // 「rejectのbranchを踏んだ」ことだけでなく「そのframeを提示しなかった」ことを
    // 観測できるようにするためのidentityである。counterだけでは、rejectを数えた
    // 直後にそのまま描画してしまうbugを検出できない。
    std::int64_t lastStaleCompositionRejectedFrame = -1;
    std::uint64_t untrackedSubmissionCount = 0;
    std::uint64_t earlyPayloadReleaseCount = 0;
    std::uint64_t retirementTimeoutCount = 0;
    std::uint64_t deviceLostCount = 0;
    std::uint64_t lifecycleViolationCount = 0;
    std::uint64_t fullCpuReadbackCount = 0;
    std::uint64_t fullFrameGpuCopyCount = 0;
    std::uint64_t softwareFallbackCount = 0;
    std::uint64_t gpuCompositionPassCount = 0;
    // P5-D2: audio-master transportの成立を、暗黙fallbackなしで確認するための診断。
    bool audioMasterActive = false;
    bool audioSinkJoined = true;
    bool audioWorkerJoined = true;
    std::uint64_t registeredAudioSourceCount = 0;
    // QPC masterへ退避した回数ではない。audio master projectionが成立せず
    // fail-closedにした回数である。退避そのものは別counterで数える。
    std::uint64_t audioMasterProjectionFailureCount = 0;
    // audio source登録中にQPC/wall-clock masterが選ばれた回数
    // (phase5-plan.md §9.3「QPC fallback count 0」)。
    // 製品経路では常に0であり、1以上になった時点で`AudioFailure`としてfail-closedにする。
    // audio sourceが無いvideo-only経路 (P5-C) のwall-clockはqualified masterであり、
    // 退避ではないのでここには数えない。
    std::uint64_t videoMasterQpcFallbackCount = 0;
    std::uint64_t audioUnderflowCount = 0;
    std::uint64_t audioGenerationMismatchCount = 0;
    // failure authorityを混ぜない。
    //  - Sink: `WasapiAudioSink`自身が数えたdevice failure (authorityはsink snapshot)
    //  - Transport: engine側のtransport操作失敗 (open/preroll/play/pause)
    //  - DomainReject: PCM domain不一致。device failureではない
    std::uint64_t audioSinkDeviceFailureCount = 0;
    std::uint64_t audioTransportFailureCount = 0;
    std::uint64_t audioDomainRejectCount = 0;
    // endpointへ実際に適用されたsession volume。要求しただけで適用されて
    // いない状態をPASSにしないために報告する。
    float audioSessionVolume = 1.0F;
    // 実際に実行されたshutdown stepを、実行順にそのまま積んだもの。
    // 重複を畳まないので、誤った再実行や並べ替えはexact比較でそのまま失敗する。
    std::vector<ShutdownStep> shutdownSequence;
    // renderから見えるworker参照を切ったか (contract §12 DetachRenderVisibleWorkerRefs)。
    bool renderVisibleWorkersDetached = false;

    // P5-D3: exact seek。request acceptanceとcompletionを別々に数える。
    // completionはactual requested frameを提示した回数だけ増える。
    std::uint64_t seekRequestCount = 0;
    std::uint64_t seekDecodeReadyCount = 0;
    std::uint64_t seekCompletedCount = 0;
    // decode readyだけでcompleteにしていないことの証拠。decode ready後、
    // exact frameを提示できずに待った render callback の回数。
    std::uint64_t seekAwaitingPresentationCount = 0;
    std::uint64_t seekStaleGenerationRejectCount = 0;
    // shutdown が先に commit されたため seek resume を中止した回数。
    // seek failure ではないので Error にはしない。
    std::uint64_t seekCancelledByShutdownCount = 0;
    std::int64_t lastSeekTargetFrame = -1;
    std::int64_t lastSeekPresentedFrame = -1;
};

class CompositionAcceptanceState {
public:
    Result<AcceptedComposition>
    submit(const std::shared_ptr<const CompositionSnapshot>& snapshot,
           const std::unordered_map<std::uint64_t, EligibleSource>& sources,
           const PreviewCapabilities& capabilities);

    // 提示したtokenと、そのtokenが指すsnapshotを対で記録する。
    // snapshotを取り違えるとremoval guardが「参照が外れた」と誤判定するため、
    // 呼び出し側が両方を明示的に渡す。
    void markPresented(AcceptedComposition composition,
                       std::shared_ptr<const CompositionSnapshot> snapshot);
    std::optional<AcceptedComposition> latestAcceptedToken() const;
    std::optional<AcceptedComposition> lastPresentedToken() const;
    const std::shared_ptr<const CompositionSnapshot>& latestAcceptedSnapshot() const;
    const std::shared_ptr<const CompositionSnapshot>& lastPresentedSnapshot() const;
    // active (last presented) または pending (accepted済みで未提示) のcompositionが
    // このsourceを参照しているか。参照が外れていないsourceのremovalを拒否するために使う
    // (preview-engine-contract.md §6)。
    bool referencesSource(PreviewSourceId source) const;

private:
    std::shared_ptr<const CompositionSnapshot> latestAcceptedSnapshot_;
    std::shared_ptr<const CompositionSnapshot> lastPresentedSnapshot_;
    std::optional<AcceptedComposition> latestAcceptedToken_;
    std::optional<AcceptedComposition> lastPresentedToken_;
    std::uint64_t nextId_ = 1;
    std::uint64_t nextRevision_ = 1;
};

class DistinctFrameCounter {
public:
    void note(std::int64_t frame);
    std::uint64_t count() const;

private:
    std::optional<std::int64_t> lastFrame_;
    std::uint64_t count_ = 0;
};

Result<void> validateSourceFrameRate(long long sourceNumerator, long long sourceDenominator,
                                     PreviewFrameRate outputFrameRate);
// 製品がqualifiedとして検証済みのinternal PCM domainは48000 Hz / stereo / float32だけである。
// これ以外を暗黙のresample/channel変換で成功へ変えない (preview-engine-contract.md §5)。
Result<void> validateQualifiedAudioDomain(int sampleRate, int channels,
                                          const std::string& sampleFormat);
std::uint64_t skippedSchedulerFrameCount(std::int64_t previousTarget, std::int64_t currentTarget);

class PreviewRenderPort {
public:
    enum class FatalDiagnostic { None, DeviceLost };

    static Result<void> attachLogicalDevice(PreviewEngine& engine);
    static Result<void> bindRenderThread(PreviewEngine& engine);
    static Result<void> attachNativeD3D11Device(PreviewEngine& engine, void* device, void* context);
    static Result<void> acquireNativeD3D11Device(PreviewEngine& engine, void* device,
                                                 void* context);
    static Result<void> validateNativeD3D11Device(PreviewEngine& engine, void* device,
                                                  void* context);
    static Result<RenderFrameResult> renderFrame(PreviewEngine& engine, void* renderTargetView,
                                                 int width, int height);
    static Result<bool> completeRuntimeTeardown(PreviewEngine& engine);
    static Result<void> completeRendererDetach(PreviewEngine& engine);
    static Result<void> completeTeardown(PreviewEngine& engine);
    static Result<void> injectFatal(PreviewEngine& engine, PreviewError error,
                                    FatalDiagnostic diagnostic = FatalDiagnostic::None);
    static Result<void> reportRenderTargetFailure(PreviewEngine& engine, long hresult);
    static Result<void> reportDeviceLost(PreviewEngine& engine, long hresult);
    static Result<void> reportUnsupportedRenderBackend(PreviewEngine& engine);
    static Result<void> reportMissingNativeD3D11Handles(PreviewEngine& engine);
    static Result<void> reportEngineReplacement(PreviewEngine& engine);
    static bool nativeRuntimeAttached(const PreviewEngine& engine);
    static Result<void> injectGpuDrainFailureForTest(PreviewEngine& engine);
    static Result<void> injectDecoderFatalForTest(PreviewEngine& engine, std::string detail);
    static Result<void> injectDecoderEofForTest(PreviewEngine& engine);
    // audio clockがmasterとして成立しない状況を、QPCへ退避せずfail-closedにできるか
    // 検査するためのinternal seam。
    // 検証アプリが endpoint session volume を下げるためのseam。public APIには
    // 出さない。addSource()でWASAPI endpointをopenする前に設定する必要がある。
    static Result<void> setVerificationAudioVolume(PreviewEngine& engine, float volume);
    // decode readyだけでseek completeにしていないことを検査するseam。
    // decodeは完了させたまま、exact frameの提示だけを成立させなくする。
    // 提示直前のstale composition epoch拒否を製品経路で検査するseam。
    // 完成したerrorをengineへ注入するのではなく、compose成立後・validate前に
    // `CompositionEpoch`だけを1つ進め、通常のvalidate経路を通す。
    // composition state / layout / generationは触らないので、engineから見た
    // compositionは何も変わらない。supersedeだけを再現する。
    static Result<void> injectCompositionEpochAdvanceForTest(PreviewEngine& engine);
    // `removeSource()`のaudio停止フェーズ (engine lockを持たない窓) を決定論的に
    // 止めるbarrier。この窓でfatalが入った場合にremovalをcommitしないことを
    // 再現可能に固定する。
    static Result<void> armSourceRemovalBarrierForTest(PreviewEngine& engine);
    static bool waitSourceRemovalBarrierEnteredForTest(PreviewEngine& engine, int timeoutMs);
    static void releaseSourceRemovalBarrierForTest(PreviewEngine& engine);
    // fatalをcommitしてunlockした直後、dispatchをflushする前で止めるbarrier。
    // この窓でteardownが先にterminalへ進んでもevent順序が逆転しないことを
    // 決定論的に検査する。state commitとmailbox insertionのlinearizabilityの
    // authorityであり、これが無いとorderingの回帰を捕まえられない。
    static Result<void> armFatalPublishBarrierForTest(PreviewEngine& engine);
    static bool waitFatalPublishBarrierEnteredForTest(PreviewEngine& engine, int timeoutMs);
    static void releaseFatalPublishBarrierForTest(PreviewEngine& engine);
    static Result<void> injectSeekPresentationStallForTest(PreviewEngine& engine);
    // seek completionで得たaudio generationをengineが実際にenforceしているか
    // 検査するseam。要求generationが決して揃わない状況を作る。
    static Result<void> injectSeekAudioGenerationMismatchForTest(PreviewEngine& engine);
    // seek後のtransport復帰でWASAPI playを失敗させるseam。
    static Result<void> injectAudioSinkPlayFaultForTest(PreviewEngine& engine);
    // seek resume の play() を決定論的に止める barrier。resume 中の state と
    // shutdown との interleaving を再現可能に固定する。
    static Result<void> armAudioPlayBarrierForTest(PreviewEngine& engine);
    static bool waitAudioPlayBarrierEnteredForTest(PreviewEngine& engine, int timeoutMs);
    static void releaseAudioPlayBarrierForTest(PreviewEngine& engine);
    static Result<void> injectAudioClockStallForTest(PreviewEngine& engine);
    // audio master成立中に誤ってQPC masterが選ばれる回帰を再現するseam。
    // master選択だけを誤らせる。この検査が無ければwall-clockで再生が続いてPASSしてしまう。
    static Result<void> injectVideoMasterQpcFallbackForTest(PreviewEngine& engine);
    // sink自身にdevice failureを起こさせ、product側のpolling/昇格経路を検査する。
    // 完成したerrorをengineへ直接注入しないこと。
    static Result<void> injectAudioSinkRenderFaultForTest(PreviewEngine& engine);
    static Result<void> injectAudioSinkPauseFaultForTest(PreviewEngine& engine);
    static P5CRuntimeDiagnostics runtimeDiagnostics(const PreviewEngine& engine);

    // bounded mailboxのfailure semanticsをbackend接続前に検査するinternal test seam。
    static void enqueueEventForTest(PreviewEngine& engine, PreviewEvent event);
    static std::size_t mailboxSizeForTest(const PreviewEngine& engine);
    // mailboxへ挿入済みのeventを挿入順で返すtest seam。
    // 「state commitとmailbox insertionがlinearizeされているか」は、
    // sinkへのdeliveryでは観測できない。terminal到達後のpending eventは
    // contractどおり破棄されるため、正しい実装でも配信されないからである。
    // したがってinsertion順そのものを観測する。
    static std::vector<PreviewEvent> mailboxEventsForTest(const PreviewEngine& engine);
};

} // namespace mvm::preview::internal

#endif // MVM_PREVIEW_ENGINE_PREVIEW_ENGINE_INTERNAL_H
