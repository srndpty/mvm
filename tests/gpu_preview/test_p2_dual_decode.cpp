// mvm Phase 1 / P2-B - dual-source decodeとexact pairingのGPU統合検査
// GPU合成・Qt表示・full-frame readbackは行わない。

#include "media/gpu_preview/exact_frame_pairer.h"
#include "media/gpu_preview/lifecycle.h"
#include "media/gpu_preview/qpc_clock.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/source_decode_worker.h"
#include "media/gpu_preview/source_registry.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace mvm::gpu;

namespace {

constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kMismatch = 3;
constexpr int kNoDevice = 5;

int fail(const std::string& message, int code = kMismatch) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    return code;
}

class OwnedDevice {
public:
    ~OwnedDevice() {
        shared.release();
        if (context_)
            context_->Release();
        if (device_)
            device_->Release();
    }

    bool create(std::string& err) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        const HRESULT result =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device_, &selected, &context_);
        if (FAILED(result)) {
            char text[128];
            std::snprintf(text, sizeof text, "D3D11CreateDeviceに失敗しました (0x%08lX)",
                          static_cast<unsigned long>(result));
            err = text;
            return false;
        }
        return shared.adopt(device_, context_, err);
    }

    SharedD3D11Device shared;

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
};

bool actualDeviceMatches(const DecodedGpuFrame& frame, ID3D11Device* expected) {
    ID3D11Device* actual = nullptr;
    frame.texture->GetDevice(&actual);
    const bool matches = actual == expected;
    if (actual)
        actual->Release();
    return matches;
}

const DecodedGpuFrame* findSource(const ComposedFrame& pair, SourceId source) {
    const auto found =
        std::find_if(pair.layers.begin(), pair.layers.end(),
                     [source](const auto& layer) { return layer.frame.sourceId == source; });
    return found == pair.layers.end() ? nullptr : &found->frame;
}

bool waitForPair(long long requested, ExactFramePairer& pairer, SourceDecodeWorker& workerA,
                 SourceDecodeWorker& workerB, ComposedFrame& pair, std::string& err) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const PairResult result = pairer.tryPair(requested, pair);
        if (result == PairResult::Paired)
            return true;
        if (result != PairResult::WaitingForSource) {
            err = "frame " + std::to_string(requested) + " のpairingが拒否されました (結果 " +
                  std::to_string(static_cast<int>(result)) + ")";
            return false;
        }

        SourceFrameIdentity identity;
        if (!workerA.buffer().peekFrontIdentity(identity) && !workerA.buffer().waitForFrame(5000)) {
            const SourceDecoderSnapshot snapshot = workerA.snapshot();
            err = "Source A frame待機に失敗しました: " + snapshot.lastError;
            return false;
        }
        if (!workerB.buffer().peekFrontIdentity(identity) && !workerB.buffer().waitForFrame(5000)) {
            const SourceDecoderSnapshot snapshot = workerB.snapshot();
            err = "Source B frame待機に失敗しました: " + snapshot.lastError;
            return false;
        }
    }
    err = "frame " + std::to_string(requested) + " のpairが有限回の待機で成立しませんでした";
    return false;
}

bool verifyPair(const ComposedFrame& pair, long long requested, SourceId sourceA, SourceId sourceB,
                ID3D11Device* sharedDevice, std::string& err) {
    if (pair.outputFrameNumber != requested || pair.layers.size() != 2) {
        err = "ComposedFrameのoutput番号またはlayer数が違います";
        return false;
    }
    const DecodedGpuFrame* frameA = findSource(pair, sourceA);
    const DecodedGpuFrame* frameB = findSource(pair, sourceB);
    if (!frameA || !frameB || frameA->sourceId == frameB->sourceId) {
        err = "ComposedFrameのSourceId集合が違います";
        return false;
    }
    if (frameA->frameNumber != requested || frameB->frameNumber != requested) {
        err = "A/Bが要求frameへ完全一致していません";
        return false;
    }
    if (!actualDeviceMatches(*frameA, sharedDevice) ||
        !actualDeviceMatches(*frameB, sharedDevice)) {
        err = "実textureのowner deviceがshared deviceと一致しません";
        return false;
    }
    return true;
}

std::vector<long long> makeSeekTargets() {
    std::vector<long long> targets = {0,   1,   2,   59,  60,  61,   137,
                                      299, 300, 301, 599, 600, 1799, 3599};
    std::set<long long> unique(targets.begin(), targets.end());
    std::mt19937_64 random(0x503242ULL);
    std::uniform_int_distribution<long long> distribution(0, 3599);
    while (targets.size() < 64) {
        const long long candidate = distribution(random);
        if (unique.insert(candidate).second)
            targets.push_back(candidate);
    }
    return targets;
}

bool waitForBackpressure(SourceDecodeWorker& worker, int timeoutMs) {
    const long long begin = qpcTicks();
    while (qpcMsBetween(begin, qpcTicks()) < timeoutMs) {
        const SourceDecoderSnapshot snapshot = worker.snapshot();
        if (snapshot.bufferDepth == snapshot.bufferCapacity && snapshot.backpressureWaitCount > 0)
            return true;
        std::this_thread::yield();
    }
    return false;
}

int run(const std::string& pathA, const std::string& pathB) {
    std::fprintf(stderr, "[dual_open_same_device]\n");
    OwnedDevice owned;
    std::string err;
    if (!owned.create(err))
        return fail(err, kNoDevice);

    ReadbackCounters counters;
    SourceRegistry registry;
    const SourceId sourceA = registry.registerSource();
    const SourceId sourceB = registry.registerSource();
    if (sourceA == sourceB)
        return fail("SourceRegistryが重複SourceIdを発行しました");

    SourceDecodeWorker workerA(sourceA, owned.shared, counters, 3);
    SourceDecodeWorker workerB(sourceB, owned.shared, counters, 3);
    if (!workerA.start(pathA, err))
        return fail("Source Aをopenできません: " + err);
    if (!workerB.start(pathB, err))
        return fail("Source Bをopenできません: " + err);

    const SourceDecoderSnapshot openedA = workerA.snapshot();
    const SourceDecoderSnapshot openedB = workerB.snapshot();
    if (openedA.info.codecName.find("h264") == std::string::npos)
        return fail("Source AがH.264 decoderではありません");
    if (openedB.info.codecName.find("hevc") == std::string::npos)
        return fail("Source BがHEVC decoderではありません");
    if (openedA.info.frameCount < 3600 || openedB.info.frameCount < 3600)
        return fail("64点seekに必要な3600 frameがありません");
    if (openedA.resourceEpoch.value == 0 || openedB.resourceEpoch.value == 0 ||
        openedA.resourceEpoch == openedB.resourceEpoch)
        return fail("A/Bのresource epochがsourceごとに独立していません");

    const std::vector<LayerLayout> layout = {
        {sourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
        {sourceB, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1},
    };
    CompositorCoordinator coordinator;
    if (coordinator.configure(
            layout, {{sourceA, openedA.sourceGeneration}, {sourceB, openedB.sourceGeneration}}) !=
        ConfigureResult::Configured)
        return fail("CompositorCoordinatorを構成できません");
    ExactFramePairer pairer(workerA.buffer(), workerB.buffer(), coordinator);

    std::fprintf(stderr, "[dual_sequential_600_frames]\n[exact_pair_only]\n");
    const long long sequentialBegin = qpcTicks();
    workerA.play();
    workerB.play();
    for (long long requested = 0; requested < 600; ++requested) {
        ComposedFrame pair;
        if (!waitForPair(requested, pairer, workerA, workerB, pair, err))
            return fail(err);
        if (!verifyPair(pair, requested, sourceA, sourceB, owned.shared.device(), err))
            return fail("sequential frame " + std::to_string(requested) + ": " + err);

        if (requested == 0) {
            std::fprintf(stderr, "[missing_source_negative]\n");
            CompositorCoordinator negative;
            if (negative.configure(layout, {{sourceA, openedA.sourceGeneration},
                                            {sourceB, openedB.sourceGeneration}}) !=
                ConfigureResult::Configured)
                return fail("negative coordinatorを構成できません");
            const DecodedGpuFrame* firstA = findSource(pair, sourceA);
            const DecodedGpuFrame* firstB = findSource(pair, sourceB);
            ComposedFrame rejected;
            if (!firstA || !firstB ||
                negative.compose(0, {*firstA}, rejected) != CompositionResult::MissingSource)
                return fail("missing sourceをfail-closedで拒否できません");

            std::fprintf(stderr, "[stale_generation_negative]\n");
            DecodedGpuFrame stale = *firstB;
            stale.sourceGeneration.value--;
            if (negative.compose(0, {*firstA, stale}, rejected) !=
                CompositionResult::StaleGeneration)
                return fail("stale generationを拒否できません");

            std::fprintf(stderr, "[future_generation_negative]\n");
            DecodedGpuFrame future = *firstB;
            future.sourceGeneration.value++;
            if (negative.compose(0, {*firstA, future}, rejected) !=
                CompositionResult::FutureGeneration)
                return fail("future generationを拒否できません");
        }
    }
    workerA.pause();
    workerB.pause();
    const double sequentialMs = qpcMsBetween(sequentialBegin, qpcTicks());
    if (pairer.counters().pairedCount != 600 || pairer.counters().mixedFrameRejected != 0 ||
        pairer.counters().missingACount != 0 || pairer.counters().missingBCount != 0)
        return fail("600 frameのpair counterが契約と一致しません");

    SourceDecoderSnapshot decodedA = workerA.snapshot();
    SourceDecoderSnapshot decodedB = workerB.snapshot();
    const unsigned long long sharedPointer =
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(owned.shared.device()));
    if (decodedA.decodeDevicePointer != sharedPointer ||
        decodedB.decodeDevicePointer != sharedPointer)
        return fail("decoderが記録した実texture device pointerがshared deviceと違います");
    if (!decodedA.adapter.sameAdapterAs(owned.shared.adapter()) ||
        !decodedB.adapter.sameAdapterAs(owned.shared.adapter()) ||
        !decodedA.adapter.sameAdapterAs(decodedB.adapter))
        return fail("A/Bの実decode adapter LUIDがshared adapterと違います");
    std::fprintf(stderr, "[actual_texture_device_match]\n");

    std::fprintf(stderr, "[dual_exact_seek_64]\n[dual_generation_independence]\n");
    const std::vector<long long> targets = makeSeekTargets();
    if (targets.size() != 64)
        return fail("seek対象が64点ではありません");
    double seekElapsedA = 0.0;
    double seekElapsedB = 0.0;
    for (const long long requested : targets) {
        workerA.pause();
        workerB.pause();
        const SourceGeneration beforeA = workerA.snapshot().sourceGeneration;
        const SourceGeneration beforeB = workerB.snapshot().sourceGeneration;
        double elapsedA = 0.0;
        if (!workerA.seekBlocking(requested, elapsedA, err))
            return fail("Source A seek " + std::to_string(requested) + ": " + err);
        seekElapsedA += elapsedA;
        const SourceGeneration afterA = workerA.snapshot().sourceGeneration;
        if (!(beforeA < afterA) || workerB.snapshot().sourceGeneration != beforeB)
            return fail("Source A seekがsource-local generation契約を破りました");
        if (!coordinator.setSourceGeneration(sourceA, afterA))
            return fail("coordinatorへSource A generationを設定できません");

        double elapsedB = 0.0;
        if (!workerB.seekBlocking(requested, elapsedB, err))
            return fail("Source B seek " + std::to_string(requested) + ": " + err);
        seekElapsedB += elapsedB;
        const SourceGeneration afterB = workerB.snapshot().sourceGeneration;
        if (!(beforeB < afterB) || workerA.snapshot().sourceGeneration != afterA)
            return fail("Source B seekがsource-local generation契約を破りました");
        if (!coordinator.setSourceGeneration(sourceB, afterB))
            return fail("coordinatorへSource B generationを設定できません");

        ComposedFrame pair;
        if (!waitForPair(requested, pairer, workerA, workerB, pair, err) ||
            !verifyPair(pair, requested, sourceA, sourceB, owned.shared.device(), err))
            return fail("exact seek pair " + std::to_string(requested) + ": " + err);
    }
    if (pairer.counters().pairedCount != 664 || pairer.counters().mixedFrameRejected != 0 ||
        pairer.counters().staleGenerationRejected != 0 ||
        pairer.counters().futureGenerationRejected != 0)
        return fail("64 exact seek後のpair counterが契約と一致しません");

    decodedA = workerA.snapshot();
    decodedB = workerB.snapshot();
    if (decodedA.softwareFrameRejectCount != 0 || decodedB.softwareFrameRejectCount != 0)
        return fail("software frameがdecode経路へ入りました");

    std::fprintf(stderr, "[source_a_stop_does_not_stop_b]\n");
    double resetMs = 0.0;
    if (!workerB.seekBlocking(0, resetMs, err))
        return fail("独立stop検査のSource B seekに失敗: " + err);
    DecodedGpuFrame resetFrame;
    if (!workerB.buffer().takeExact(0, resetFrame))
        return fail("独立stop検査のSource B frame 0を取得できません");
    resetFrame = {};
    workerB.play();

    std::fprintf(stderr, "[two_worker_join_barrier]\n");
    WorkerJoinBarrier joins(2);
    workerA.stop();
    if (!workerA.joined() || !joins.noteJoined(0) || joins.allJoined())
        return fail("Source Aだけのjoinでbarrierがreadyになりました");
    if (!workerB.running() || workerB.buffer().stopped() || !workerB.buffer().waitForFrame(5000))
        return fail("Source A stopがSource Bのdecodeを停止しました");
    workerA.stop(); // repeated stop
    workerB.stop();
    if (!workerB.joined() || !joins.noteJoined(1) || !joins.allJoined())
        return fail("A/B両workerのjoin後にbarrierがreadyになりません");
    workerB.stop(); // repeated stop

    std::fprintf(stderr, "[source_b_stop_does_not_stop_a]\n");
    const SourceId reverseA = registry.registerSource();
    const SourceId reverseB = registry.registerSource();
    SourceDecodeWorker workerReverseA(reverseA, owned.shared, counters, 3);
    SourceDecodeWorker workerReverseB(reverseB, owned.shared, counters, 3);
    if (!workerReverseA.start(pathA, err) || !workerReverseB.start(pathB, err))
        return fail("逆順stop検査のdual openに失敗: " + err);

    std::fprintf(stderr, "[backpressure_no_busy_loop]\n");
    workerReverseA.play();
    if (!waitForBackpressure(workerReverseA, 5000))
        return fail("source-local bufferがbounded backpressure待機へ入りませんでした");
    const SourceDecoderSnapshot pressure = workerReverseA.snapshot();
    if (pressure.bufferDepth > pressure.bufferCapacity || pressure.bufferCapacity != 3)
        return fail("source-local bufferがcapacityを超えました");

    WorkerJoinBarrier reverseJoins(2);
    workerReverseB.stop();
    if (!workerReverseB.joined() || !reverseJoins.noteJoined(1) || reverseJoins.allJoined())
        return fail("Source Bだけのjoinでbarrierがreadyになりました");
    if (!workerReverseA.running() || workerReverseA.buffer().stopped())
        return fail("Source B stopがSource Aを停止しました");
    workerReverseA.buffer().clear();
    if (!workerReverseA.buffer().waitForFrame(5000))
        return fail("Source B stop後にSource Aがdecodeを継続できません");
    workerReverseB.stop();
    workerReverseA.stop();
    if (!workerReverseA.joined() || !reverseJoins.noteJoined(0) || !reverseJoins.allJoined())
        return fail("逆順でもA/B両worker join後にbarrierがreadyになりません");
    workerReverseA.stop();

    if (!registry.unregisterSource(sourceA) || !registry.unregisterSource(sourceB) ||
        !registry.unregisterSource(reverseA) || !registry.unregisterSource(reverseB) ||
        registry.registeredSourceCount() != 0)
        return fail("worker join後にSourceRegistryを空にできません");

    if (counters.fullFrameReadbacks() != 0)
        return fail("CPU full-frame readbackが発生しました");

    std::fprintf(stdout, "shared_device=0x%llx\n", sharedPointer);
    std::fprintf(stdout, "source_a_decode_device=0x%llx\n", decodedA.decodeDevicePointer);
    std::fprintf(stdout, "source_b_decode_device=0x%llx\n", decodedB.decodeDevicePointer);
    std::fprintf(stdout, "source_a_same_device=true\nsource_b_same_device=true\n");
    std::fprintf(stdout, "source_a_adapter=%08x:%08x\n",
                 static_cast<unsigned int>(decodedA.adapter.luidHigh), decodedA.adapter.luidLow);
    std::fprintf(stdout, "source_b_adapter=%08x:%08x\n",
                 static_cast<unsigned int>(decodedB.adapter.luidHigh), decodedB.adapter.luidLow);
    std::fprintf(stdout, "buffer_capacity=3\npaired_count=%lld\n", pairer.counters().pairedCount);
    std::fprintf(stdout,
                 "missing_a_count=%lld\nmissing_b_count=%lld\nstale_a_discard_count=%lld\n"
                 "stale_b_discard_count=%lld\nmixed_frame_rejected=%lld\n"
                 "stale_generation_rejected=%lld\nfuture_generation_rejected=%lld\n",
                 pairer.counters().missingACount, pairer.counters().missingBCount,
                 pairer.counters().staleADiscardCount, pairer.counters().staleBDiscardCount,
                 pairer.counters().mixedFrameRejected, pairer.counters().staleGenerationRejected,
                 pairer.counters().futureGenerationRejected);
    std::fprintf(stdout,
                 "sequential_600_elapsed_ms=%.3f\nseek_a_total_ms=%.3f\nseek_b_total_ms=%.3f\n",
                 sequentialMs, seekElapsedA, seekElapsedB);
    std::fprintf(stdout,
                 "backpressure_wait_count=%lld\nqueue_full_count=%lld\n"
                 "buffer_overflow_implicit_drop_count=0\n",
                 pressure.backpressureWaitCount, pressure.queueFullCount);
    std::fprintf(stdout,
                 "source_a_generation=%llu\nsource_b_generation=%llu\n"
                 "source_a_seek_backoff=%lld\nsource_b_seek_backoff=%lld\n",
                 decodedA.sourceGeneration.value, decodedB.sourceGeneration.value,
                 decodedA.seekBackoffCount, decodedB.seekBackoffCount);
    std::fprintf(stdout, "cpu_full_frame_readback_count=%lld\nsoftware_fallback_count=0\n",
                 counters.fullFrameReadbacks());
    std::fprintf(stdout, "worker_join_leak_count=0\nOK P2-B dual-source decode / exact pairing\n");
    return kOk;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        return fail("使い方: mvm_test_p2_dual_decode <h264> <hevc>", kUsage);
    return run(argv[1], argv[2]);
}
