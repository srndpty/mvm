#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/composition_schedule.h"
#include "media/gpu_preview/compositor_coordinator.h"

#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mvm::gpu;

namespace {

constexpr CompositionStateId kS0{1};
constexpr CompositionStateId kS1{2};
constexpr CompositionStateId kS2{3};
constexpr CompositionStateId kS3{4};
constexpr SourceId kSourceA{1};
constexpr SourceId kSourceB{2};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<LayerLayout> fixedLayout() {
    return {{kSourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
            {kSourceB, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}};
}

void configure(CompositorCoordinator& coordinator) {
    require(coordinator.configure(fixedLayout(), {{kSourceA, {7}}, {kSourceB, {11}}}) ==
                ConfigureResult::Configured,
            "coordinatorを初期化できません");
}

DecodedGpuFrame sourceFrame(SourceId source, long long frameNumber, unsigned long long generation,
                            unsigned long long resourceEpoch) {
    DecodedGpuFrame frame;
    frame.sourceId = source;
    frame.frameNumber = frameNumber;
    frame.sourceGeneration = {generation};
    frame.resourceEpoch = {resourceEpoch};
    return frame;
}

CompositionSchedule formalSchedule() {
    auto schedule = CompositionSchedule::create(
        {{0, kS0}, {600, kS1}, {1200, kS2}, {1800, kS3}, {2400, kS0}, {3000, kS1}});
    require(schedule.has_value(), "formal scheduleを構築できません");
    return *schedule;
}

CompositionSchedule smokeSchedule() {
    auto schedule = CompositionSchedule::create({{0, kS0}, {200, kS1}, {400, kS2}});
    require(schedule.has_value(), "smoke scheduleを構築できません");
    return *schedule;
}

void requireResolution(const CompositionSchedule& schedule,
                       const std::vector<std::pair<long long, CompositionStateId>>& expected) {
    for (const auto& [frame, state] : expected) {
        const auto resolved = schedule.resolve(frame);
        require(resolved.has_value(), "有効なframeをresolveできません");
        require(*resolved == state, "scheduleのresolve結果が違います");
    }
}

void goodFormalScheduleResolution() {
    requireResolution(formalSchedule(), {{0, kS0},
                                         {599, kS0},
                                         {600, kS1},
                                         {1199, kS1},
                                         {1200, kS2},
                                         {1799, kS2},
                                         {1800, kS3},
                                         {2399, kS3},
                                         {2400, kS0},
                                         {2999, kS0},
                                         {3000, kS1},
                                         {3599, kS1}});
}

void goodSmokeScheduleResolution() {
    requireResolution(smokeSchedule(),
                      {{0, kS0}, {199, kS0}, {200, kS1}, {399, kS1}, {400, kS2}, {599, kS2}});
}

void rejectEmptySchedule() {
    require(!CompositionSchedule::create({}), "空scheduleを受理しました");
}

void rejectFirstBoundaryNonzero() {
    require(!CompositionSchedule::create({{1, kS0}}),
            "先頭boundaryが0でないscheduleを受理しました");
}

void rejectNegativeBoundary() {
    require(!CompositionSchedule::create({{-1, kS0}, {0, kS1}}), "負のboundaryを受理しました");
}

void rejectDuplicateBoundary() {
    require(!CompositionSchedule::create({{0, kS0}, {0, kS1}}), "重複boundaryを受理しました");
}

void rejectDescendingBoundary() {
    require(!CompositionSchedule::create({{0, kS0}, {20, kS1}, {10, kS2}}),
            "降順boundaryを受理しました");
}

void rejectInvalidState() {
    require(!CompositionSchedule::create({{0, {}}}), "invalid stateを受理しました");
}

void rejectUnresolvableFrame() {
    require(!formalSchedule().resolve(-1), "負のoutput frameをresolveしました");
}

void initialStateSetup() {
    CompositorCoordinator coordinator;
    configure(coordinator);
    const auto before = coordinator.compositionEpoch();
    require(!coordinator.compositionState().valid(), "legacy初期stateがunspecifiedではありません");
    require(coordinator.adoptCompositionState(kS0) == CompositionStateAdoptionResult::Adopted,
            "初期S0を採用できません");
    require(coordinator.compositionState() == kS0, "初期S0がactiveではありません");
    require(coordinator.compositionEpoch().value == before.value + 1,
            "初期state採用でepochがexactly 1進みません");
}

void adoptDifferentStateIncrementsEpochOnce() {
    CompositorCoordinator coordinator;
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS0) == CompositionStateAdoptionResult::Adopted,
            "S0を採用できません");
    const auto before = coordinator.compositionEpoch();
    require(coordinator.adoptCompositionState(kS1) == CompositionStateAdoptionResult::Adopted,
            "異なるstateを採用できません");
    require(coordinator.compositionEpoch().value == before.value + 1,
            "異なるstateの採用でepochがexactly 1進みません");
}

void adoptSameStateIsNoop() {
    CompositorCoordinator coordinator;
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS1) == CompositionStateAdoptionResult::Adopted,
            "S1を採用できません");
    const auto before = coordinator.compositionEpoch();
    require(coordinator.adoptCompositionState(kS1) == CompositionStateAdoptionResult::NoOp,
            "同じstateがNoOpになりません");
    require(coordinator.compositionEpoch() == before, "NoOpでepochが変化しました");
}

void rejectedAdoptionDoesNotMutate() {
    CompositorCoordinator unconfigured;
    require(unconfigured.adoptCompositionState(kS0) == CompositionStateAdoptionResult::Rejected,
            "未初期化coordinatorがstateを受理しました");
    require(unconfigured.compositionEpoch() == CompositionEpoch{} &&
                !unconfigured.compositionState().valid(),
            "reject時に未初期化stateが変化しました");

    CompositorCoordinator coordinator;
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS1) == CompositionStateAdoptionResult::Adopted,
            "S1を採用できません");
    const auto epoch = coordinator.compositionEpoch();
    const auto state = coordinator.compositionState();
    const auto generationA = coordinator.sourceGeneration(kSourceA);
    const auto generationB = coordinator.sourceGeneration(kSourceB);
    require(coordinator.adoptCompositionState({}) == CompositionStateAdoptionResult::Rejected,
            "invalid stateを受理しました");
    require(coordinator.compositionEpoch() == epoch && coordinator.compositionState() == state,
            "reject時にcomposition identityが変化しました");
    require(coordinator.sourceGeneration(kSourceA) == generationA &&
                coordinator.sourceGeneration(kSourceB) == generationB,
            "reject時にSourceGenerationが変化しました");
}

void repeatedNoopDoesNotIncrementEpoch() {
    CompositorCoordinator coordinator;
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS2) == CompositionStateAdoptionResult::Adopted,
            "S2を採用できません");
    const auto baseline = coordinator.compositionEpoch();
    for (int i = 0; i < 100; ++i)
        require(coordinator.adoptCompositionState(kS2) == CompositionStateAdoptionResult::NoOp,
                "反復した同一stateがNoOpになりません");
    require(coordinator.compositionEpoch() == baseline, "反復NoOpでepochが進みました");
}

void transitionSequenceIncrementsFiveTimes() {
    CompositorCoordinator coordinator;
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS0) == CompositionStateAdoptionResult::Adopted,
            "baseline S0を採用できません");
    const auto baseline = coordinator.compositionEpoch();
    for (const auto state : {kS1, kS2, kS3, kS0, kS1})
        require(coordinator.adoptCompositionState(state) == CompositionStateAdoptionResult::Adopted,
                "sequence stateを採用できません");
    require(coordinator.compositionEpoch().value == baseline.value + 5,
            "transition sequenceのepoch増分が5ではありません");
    require(coordinator.sourceGeneration(kSourceA) == SourceGeneration{7} &&
                coordinator.sourceGeneration(kSourceB) == SourceGeneration{11},
            "transition sequenceがSourceGenerationを変更しました");
}

ComposedFrame composeAtS1(CompositorCoordinator& coordinator, long long frameNumber) {
    configure(coordinator);
    require(coordinator.adoptCompositionState(kS1) == CompositionStateAdoptionResult::Adopted,
            "S1を採用できません");
    ComposedFrame composed;
    require(coordinator.compose(frameNumber,
                                {sourceFrame(kSourceA, frameNumber, 7, 101),
                                 sourceFrame(kSourceB, frameNumber, 11, 202)},
                                composed) == CompositionResult::Accepted,
            "S1 frameをcomposeできません");
    return composed;
}

void composedFrameSnapshotIdentity() {
    CompositorCoordinator coordinator;
    const ComposedFrame frame = composeAtS1(coordinator, 42);
    const auto snapshotEpoch = frame.compositionEpoch;
    require(frame.compositionState == kS1, "ComposedFrameにS1が保存されません");
    require(coordinator.adoptCompositionState(kS2) == CompositionStateAdoptionResult::Adopted,
            "S2へ進めません");
    require(frame.outputFrameNumber == 42 && frame.compositionState == kS1 &&
                frame.compositionEpoch == snapshotEpoch,
            "coordinator更新後にComposedFrame snapshotが変化しました");
    require(frame.layers[0].frame.frameNumber == 42 && frame.layers[1].frame.frameNumber == 42,
            "ComposedFrameのlayer frame snapshotが違います");
}

void displayLedgerSnapshotIdentity() {
    CompositorCoordinator coordinator;
    ComposedFrame frame = composeAtS1(coordinator, 73);
    const auto snapshotEpoch = frame.compositionEpoch;
    CompositionDisplayLedger ledger(2);
    const auto baseline = ledger.baseline();
    ledger.record(frame, 1000);

    require(coordinator.adoptCompositionState(kS2) == CompositionStateAdoptionResult::Adopted,
            "ledger記録後にS2へ進めません");
    frame.outputFrameNumber = 999;
    frame.compositionState = kS3;
    frame.compositionEpoch = {999};
    frame.layers[0].frame.frameNumber = 999;
    frame.layers[0].frame.sourceGeneration = {999};
    frame.layers[0].frame.resourceEpoch = {999};

    CompositionDisplayExpectation expected;
    expected.outputFrameNumber = 73;
    expected.compositionEpoch = snapshotEpoch;
    expected.compositionState = kS1;
    expected.sources = {{kSourceA, {7}, {101}, 73}, {kSourceB, {11}, {202}, 73}};
    CompositionDisplayRecord found;
    require(ledger.findAfter(baseline, expected, found),
            "ledgerから記録時点のfull identityを取得できません");
    require(found.outputFrameNumber == 73 && found.compositionState == kS1 &&
                found.compositionEpoch == snapshotEpoch,
            "ledgerのcomposition snapshotが変化しました");
    require(found.sources == expected.sources, "ledgerのA/B layer snapshotが変化しました");
}

using Test = std::pair<const char*, std::function<void()>>;

const std::vector<Test> kTests = {
    {"GoodFormalScheduleResolution", goodFormalScheduleResolution},
    {"GoodSmokeScheduleResolution", goodSmokeScheduleResolution},
    {"RejectEmptySchedule", rejectEmptySchedule},
    {"RejectFirstBoundaryNonzero", rejectFirstBoundaryNonzero},
    {"RejectNegativeBoundary", rejectNegativeBoundary},
    {"RejectDuplicateBoundary", rejectDuplicateBoundary},
    {"RejectDescendingBoundary", rejectDescendingBoundary},
    {"RejectInvalidState", rejectInvalidState},
    {"RejectUnresolvableFrame", rejectUnresolvableFrame},
    {"InitialStateSetup", initialStateSetup},
    {"AdoptDifferentStateIncrementsEpochOnce", adoptDifferentStateIncrementsEpochOnce},
    {"AdoptSameStateIsNoop", adoptSameStateIsNoop},
    {"RejectedAdoptionDoesNotMutate", rejectedAdoptionDoesNotMutate},
    {"RepeatedNoopDoesNotIncrementEpoch", repeatedNoopDoesNotIncrementEpoch},
    {"TransitionSequenceIncrementsFiveTimes", transitionSequenceIncrementsFiveTimes},
    {"ComposedFrameSnapshotIdentity", composedFrameSnapshotIdentity},
    {"DisplayLedgerSnapshotIdentity", displayLedgerSnapshotIdentity},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_phase4a_foundation <test-name>\n");
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
