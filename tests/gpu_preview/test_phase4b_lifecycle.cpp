// Phase 4 / B.1: docs/phase4-plan.md §7 の freeze 済み shutdown 順を固定する。
//
// **期待値は実装の step 配列を参照せず literal で書く。**
// 実装と同じ配列を共有すると、順序を入れ替えた bug を test が追認する。
#include "media/gpu_preview/shutdown_sequence.h"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mvm::gpu;

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// 実 GPU / WASAPI を使わずに順序だけを観測する。各 stop は同期 join なので、
// stub も「呼ばれた時点で join 済みになる」形にする。
struct Recorder {
    std::vector<std::string> events;
    bool audioSinkJoined = false;
    bool audioWorkerJoined = false;
    bool videoAJoined = false;
    bool videoBJoined = false;
    // teardown 要求の瞬間に観測した join 状態。後から書き換えられない。
    bool joinedAtTeardown = false;
    bool teardownCalled = false;

    ShutdownActions actions() {
        ShutdownActions actions;
        actions.disableSchedulers = [this] { events.push_back("disable"); };
        actions.stopAudioSink = [this] {
            events.push_back("audio-sink-stop");
            audioSinkJoined = true;
        };
        actions.stopAudioDecodeWorker = [this] {
            events.push_back("audio-worker-stop");
            audioWorkerJoined = true;
        };
        actions.stopVideoWorkerA = [this] {
            events.push_back("video-a-stop");
            videoAJoined = true;
        };
        actions.stopVideoWorkerB = [this] {
            events.push_back("video-b-stop");
            videoBJoined = true;
        };
        actions.detachSharedWorkerRefs = [this] { events.push_back("detach"); };
        actions.allWorkersJoined = [this] {
            events.push_back("join-check");
            return audioSinkJoined && audioWorkerJoined && videoAJoined && videoBJoined;
        };
        actions.requestRenderTeardown = [this] {
            events.push_back("teardown");
            teardownCalled = true;
            joinedAtTeardown = audioSinkJoined && audioWorkerJoined && videoAJoined && videoBJoined;
        };
        return actions;
    }
};

size_t indexOf(const std::vector<std::string>& events, const std::string& name) {
    for (size_t i = 0; i < events.size(); ++i)
        if (events[i] == name)
            return i;
    throw std::runtime_error("shutdown event が実行されていません: " + name);
}

Recorder runAll() {
    Recorder recorder;
    const auto result = runFrozenShutdownSequence(recorder.actions());
    require(result.joinVerified, "join を確認できません");
    require(result.renderTeardownRequested, "render teardown を要求していません");
    require(result.orderViolationCount == 0, "順序違反が計上されました");
    return recorder;
}

void schedulerDisabledBeforeAudioStop() {
    const auto recorder = runAll();
    require(indexOf(recorder.events, "disable") < indexOf(recorder.events, "audio-sink-stop"),
            "scheduler disable より前に audio sink を停止しました");
    require(indexOf(recorder.events, "disable") < indexOf(recorder.events, "audio-worker-stop"),
            "scheduler disable より前に audio decode worker を停止しました");
}

void audioSinkStoppedBeforeAudioWorker() {
    const auto recorder = runAll();
    require(indexOf(recorder.events, "audio-sink-stop") <
                indexOf(recorder.events, "audio-worker-stop"),
            "audio decode worker を audio sink より先に停止しました");
}

void audioWorkerStoppedBeforeVideoWorkers() {
    const auto recorder = runAll();
    const size_t audio = indexOf(recorder.events, "audio-worker-stop");
    require(audio < indexOf(recorder.events, "video-a-stop") &&
                audio < indexOf(recorder.events, "video-b-stop"),
            "video worker を audio decode worker より先に停止しました");
}

void workersJoinedBeforeRenderTeardown() {
    const auto recorder = runAll();
    const size_t teardown = indexOf(recorder.events, "teardown");
    require(indexOf(recorder.events, "audio-sink-stop") < teardown &&
                indexOf(recorder.events, "audio-worker-stop") < teardown &&
                indexOf(recorder.events, "video-a-stop") < teardown &&
                indexOf(recorder.events, "video-b-stop") < teardown,
            "worker stop+join より前に render teardown を要求しました");
    require(indexOf(recorder.events, "join-check") < teardown,
            "join 確認より前に render teardown を要求しました");
    require(recorder.joinedAtTeardown, "teardown 要求時点で join が完了していません");
}

void sharedRefsDetachedBeforeRenderTeardown() {
    const auto recorder = runAll();
    require(indexOf(recorder.events, "detach") < indexOf(recorder.events, "teardown"),
            "shared worker reference の detach より先に render teardown を要求しました");
    require(indexOf(recorder.events, "video-b-stop") < indexOf(recorder.events, "detach"),
            "video worker の join より先に shared reference を detach しました");
}

// 実行順そのものを literal で固定する。上の相対順だけでは
// 「step が 1 つ欠けている」形の退行を捕まえられない。
void frozenOrderMatchesContract() {
    Recorder recorder;
    runFrozenShutdownSequence(recorder.actions());
    const std::vector<std::string> expected{"disable",      "audio-sink-stop", "audio-worker-stop",
                                            "video-a-stop", "video-b-stop",    "detach",
                                            "join-check",   "teardown"};
    require(recorder.events == expected, "shutdown 実行順が freeze 済み契約と一致しません");
}

// negative: join を確認できなければ teardown を要求しない。これが通らなければ
// 上の「join 済み」検査は空振りである。
void unjoinedWorkerBlocksRenderTeardown() {
    Recorder recorder;
    auto actions = recorder.actions();
    // video B の stop が join を完了しない (thread が残っている) 状況を作る。
    actions.stopVideoWorkerB = [&recorder] { recorder.events.push_back("video-b-stop"); };
    const auto result = runFrozenShutdownSequence(actions);
    require(!result.joinVerified, "join 未完了を join 済みと判定しました");
    require(!result.renderTeardownRequested && !recorder.teardownCalled,
            "join 未完了のまま render teardown を要求しました");
    require(result.orderViolationCount == 1, "順序違反が計上されません");
}

// negative: join 確認手段が無い場合も fail-closed とする。
void missingJoinCheckBlocksRenderTeardown() {
    Recorder recorder;
    auto actions = recorder.actions();
    actions.allWorkersJoined = nullptr;
    const auto result = runFrozenShutdownSequence(actions);
    require(!result.joinVerified && !result.renderTeardownRequested && !recorder.teardownCalled,
            "join を確認できないのに render teardown を要求しました");
    require(result.orderViolationCount == 1, "順序違反が計上されません");
}

// pipeline が開く前の shutdown。対象が無い step は skip し、順序は崩さない。
void missingPipelinesStillReachesTeardown() {
    Recorder recorder;
    auto actions = recorder.actions();
    actions.stopAudioSink = nullptr;
    actions.stopAudioDecodeWorker = nullptr;
    actions.stopVideoWorkerA = nullptr;
    actions.stopVideoWorkerB = nullptr;
    actions.allWorkersJoined = [] { return true; };
    const auto result = runFrozenShutdownSequence(actions);
    require(result.renderTeardownRequested, "対象が無い場合に teardown へ到達しません");
    const std::vector<std::string> expected{"disable", "detach", "teardown"};
    require(recorder.events == expected, "skip 時の実行順が違います");
}

using Test = std::pair<const char*, std::function<void()>>;

const std::vector<Test> kTests = {
    {"SchedulerDisabledBeforeAudioStop", schedulerDisabledBeforeAudioStop},
    {"AudioSinkStoppedBeforeAudioWorker", audioSinkStoppedBeforeAudioWorker},
    {"AudioWorkerStoppedBeforeVideoWorkers", audioWorkerStoppedBeforeVideoWorkers},
    {"WorkersJoinedBeforeRenderTeardown", workersJoinedBeforeRenderTeardown},
    {"SharedRefsDetachedBeforeRenderTeardown", sharedRefsDetachedBeforeRenderTeardown},
    {"FrozenOrderMatchesContract", frozenOrderMatchesContract},
    {"UnjoinedWorkerBlocksRenderTeardown", unjoinedWorkerBlocksRenderTeardown},
    {"MissingJoinCheckBlocksRenderTeardown", missingJoinCheckBlocksRenderTeardown},
    {"MissingPipelinesStillReachesTeardown", missingPipelinesStillReachesTeardown},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_phase4b_lifecycle <test-name>\n");
        return 2;
    }
    for (const auto& [name, test] : kTests) {
        if (name != std::string(argv[1]))
            continue;
        try {
            test();
            std::fprintf(stderr, "PASS %s\n", name);
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
            return 1;
        }
    }
    std::fprintf(stderr, "未知のtest名です: %s\n", argv[1]);
    return 2;
}
