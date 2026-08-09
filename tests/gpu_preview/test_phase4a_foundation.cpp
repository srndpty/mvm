#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/composition_schedule.h"
#include "media/gpu_preview/compositor_coordinator.h"
#include "media/gpu_preview/phase4_composition_catalog.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mvm::gpu;

namespace mvm::gpu {

// overflow pathを実回数だけ進めず検査するための限定したwhite-box access。
struct CompositorCoordinatorTestAccess {
    static void setCompositionEpoch(CompositorCoordinator& coordinator, CompositionEpoch epoch) {
        std::lock_guard<std::mutex> lock(coordinator.mutex_);
        coordinator.epoch_ = epoch;
    }
};

} // namespace mvm::gpu

namespace {

// runtime と test で mapping を二重定義しない。canonical 値は catalog にだけある。
// 期待値 (下の canonicalLayoutSx) は catalog を呼ばずに literal で書く。
constexpr CompositionStateId kS0 = kPhase4S0;
constexpr CompositionStateId kS1 = kPhase4S1;
constexpr CompositionStateId kS2 = kPhase4S2;
constexpr CompositionStateId kS3 = kPhase4S3;
constexpr SourceId kSourceA = kPhase4SourceA;
constexpr SourceId kSourceB = kPhase4SourceB;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<LayerLayout> layoutFor(CompositionStateId state) {
    auto layout = phase4CanonicalLayout(state);
    require(!layout.empty(), "canonical layout が catalog にありません");
    return layout;
}

std::vector<LayerLayout> fixedLayout() {
    return layoutFor(kS0);
}

bool sameRectForTest(const RectF& a, const RectF& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

void requireCanonicalLayout(const std::vector<LayerLayout>& actual,
                            const std::vector<LayerLayout>& expected) {
    require(actual.size() == expected.size(), "canonical layoutのlayer数が違います");
    for (size_t i = 0; i < expected.size(); ++i) {
        require(actual[i].sourceId == expected[i].sourceId, "canonical layoutのSourceIdが違います");
        require(sameRectForTest(actual[i].destination, expected[i].destination),
                "canonical layoutのdestinationが違います");
        require(sameRectForTest(actual[i].sourceUv, expected[i].sourceUv),
                "canonical layoutのsourceUvが違います");
        require(actual[i].opacity == expected[i].opacity, "canonical layoutのopacityが違います");
        require(actual[i].zOrder == expected[i].zOrder, "canonical layoutのzOrderが違います");
    }
}

void canonicalLayoutS0() {
    requireCanonicalLayout(layoutFor(kS0),
                           {{kSourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
                            {kSourceB, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}});
}

void canonicalLayoutS1() {
    requireCanonicalLayout(layoutFor(kS1),
                           {{kSourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
                            {kSourceB, {0, 0, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.75f, 1}});
}

void canonicalLayoutS2() {
    requireCanonicalLayout(layoutFor(kS2), {{kSourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
                                            {kSourceB, {0, 0, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.5f, 1}});
}

void canonicalLayoutS3() {
    requireCanonicalLayout(layoutFor(kS3),
                           {{kSourceA, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0f, 0},
                            {kSourceB, {0.5f, 0.5f, 0.5f, 0.5f}, {0, 0, 1, 1}, 0.5f, 1}});
}

DecodedGpuFrame sourceFrame(SourceId source, long long frameNumber, unsigned long long generation,
                            unsigned long long resourceEpoch);

void requireComposedSnapshot(CompositorCoordinator& coordinator, CompositionStateId state,
                             CompositionEpoch epoch, const std::vector<LayerLayout>& layout,
                             long long frameNumber) {
    ComposedFrame composed;
    require(coordinator.compose(frameNumber,
                                {sourceFrame(kSourceA, frameNumber, 7, 101),
                                 sourceFrame(kSourceB, frameNumber, 11, 202)},
                                composed) == CompositionResult::Accepted,
            "snapshotをcomposeできません");
    require(composed.compositionState == state && composed.compositionEpoch == epoch,
            "compose identityがactive snapshotと一致しません");
    require(composed.layers.size() == layout.size(), "compose layer数がlayoutと一致しません");
    for (const auto& expected : layout) {
        const auto found = std::find_if(composed.layers.begin(), composed.layers.end(),
                                        [&](const CompositionLayerFrame& layer) {
                                            return layer.frame.sourceId == expected.sourceId;
                                        });
        require(found != composed.layers.end(), "layoutのsourceがcompose結果にありません");
        require(sameRectForTest(found->destination, expected.destination) &&
                    sameRectForTest(found->sourceUv, expected.sourceUv) &&
                    found->opacity == expected.opacity && found->zOrder == expected.zOrder,
                "compose結果のlayout fieldがactive snapshotと一致しません");
        require(found->frame.sourceGeneration == coordinator.sourceGeneration(expected.sourceId) &&
                    found->frame.resourceEpoch ==
                        (expected.sourceId == kSourceA ? ResourceEpoch{101} : ResourceEpoch{202}),
                "composeでsource/resource identityが変化しました");
    }
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
    auto schedule = phase4Schedule(Phase4ScheduleKind::Formal);
    require(schedule.has_value(), "formal scheduleを構築できません");
    return *schedule;
}

CompositionSchedule smokeSchedule() {
    auto schedule = phase4Schedule(Phase4ScheduleKind::Smoke);
    require(schedule.has_value(), "smoke scheduleを構築できません");
    return *schedule;
}

// canonical serialization は docs/phase4-plan.md §3.2 / §3.8 の freeze 値と
// 完全一致でなければならない。catalog を呼んで期待値を作らない。
void canonicalScheduleStrings() {
    require(phase4CanonicalScheduleString(Phase4ScheduleKind::Smoke) == "0:S0;200:S1;400:S2",
            "smoke canonical schedule 文字列が freeze 値と違います");
    require(phase4CanonicalScheduleString(Phase4ScheduleKind::Formal) ==
                "0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1",
            "formal canonical schedule 文字列が freeze 値と違います");
}

void canonicalStateNames() {
    const std::vector<std::pair<CompositionStateId, std::string>> expected{
        {kS0, "S0"}, {kS1, "S1"}, {kS2, "S2"}, {kS3, "S3"}};
    for (const auto& [state, name] : expected) {
        require(phase4StateName(state) == name, "state id -> name mapping が違います");
        require(phase4StateFromName(name) == state, "name -> state id mapping が違います");
    }
    require(phase4StateName(CompositionStateId{99}) == nullptr,
            "未知 state を既定 name へ縮退させました");
    require(!phase4StateFromName("S4").valid(), "未知 name を既定 state へ縮退させました");
    require(phase4CanonicalLayout(CompositionStateId{99}).empty(),
            "未知 state へ layout を返しました");
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

CompositionEpoch initializeAtomicSnapshot(CompositorCoordinator& coordinator) {
    configure(coordinator);
    require(coordinator.adoptCompositionSnapshot(kS0, layoutFor(kS0)) ==
                CompositionStateAdoptionResult::Adopted,
            "初期S0/layout0 snapshotを採用できません");
    return coordinator.compositionEpoch();
}

void atomicAdoptChangesStateLayoutEpochOnce() {
    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    require(coordinator.adoptCompositionSnapshot(kS1, layoutFor(kS1)) ==
                CompositionStateAdoptionResult::Adopted,
            "S1/layout1 snapshotを採用できません");
    const auto adoptedEpoch = coordinator.compositionEpoch();
    require(adoptedEpoch.value == baseline.value + 1,
            "atomic adoptionでepochがexactly 1進みません");
    require(coordinator.sourceGeneration(kSourceA) == SourceGeneration{7} &&
                coordinator.sourceGeneration(kSourceB) == SourceGeneration{11},
            "atomic adoptionがSourceGenerationを変更しました");
    requireComposedSnapshot(coordinator, kS1, adoptedEpoch, layoutFor(kS1), 80);
}

void atomicAdoptSameSnapshotIsNoOp() {
    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    require(coordinator.adoptCompositionSnapshot(kS0, layoutFor(kS0)) ==
                CompositionStateAdoptionResult::NoOp,
            "同一snapshotがNoOpになりません");
    require(coordinator.compositionEpoch() == baseline, "同一snapshotでepochが進みました");
    requireComposedSnapshot(coordinator, kS0, baseline, layoutFor(kS0), 81);
}

void atomicRejectInvalidStateDoesNotMutate() {
    CompositorCoordinator unconfigured;
    require(unconfigured.adoptCompositionSnapshot(kS1, layoutFor(kS1)) ==
                CompositionStateAdoptionResult::Rejected,
            "未初期化coordinatorがsnapshotを受理しました");
    require(unconfigured.compositionEpoch() == CompositionEpoch{} &&
                !unconfigured.compositionState().valid(),
            "未初期化rejectでidentityが変化しました");

    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    require(coordinator.adoptCompositionSnapshot({}, layoutFor(kS1)) ==
                CompositionStateAdoptionResult::Rejected,
            "invalid stateを含むsnapshotを受理しました");
    require(coordinator.compositionState() == kS0 && coordinator.compositionEpoch() == baseline,
            "invalid state rejectでidentityが変化しました");
    requireComposedSnapshot(coordinator, kS0, baseline, layoutFor(kS0), 82);
}

void atomicRejectInvalidLayoutDoesNotMutate() {
    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    auto invalid = layoutFor(kS1);
    invalid[1].sourceId = kSourceA;
    require(coordinator.adoptCompositionSnapshot(kS1, std::move(invalid)) ==
                CompositionStateAdoptionResult::Rejected,
            "duplicate sourceを含むlayoutを受理しました");
    require(coordinator.compositionState() == kS0 && coordinator.compositionEpoch() == baseline,
            "invalid layout rejectでidentityが変化しました");
    requireComposedSnapshot(coordinator, kS0, baseline, layoutFor(kS0), 83);
}

void atomicRejectSameStateDifferentLayout() {
    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    require(coordinator.adoptCompositionSnapshot(kS0, layoutFor(kS1)) ==
                CompositionStateAdoptionResult::Rejected,
            "同じstateへ異なるlayoutを割り当てました");
    require(coordinator.compositionState() == kS0 && coordinator.compositionEpoch() == baseline,
            "same-state/different-layout rejectでidentityが変化しました");
    requireComposedSnapshot(coordinator, kS0, baseline, layoutFor(kS0), 84);
}

void atomicEpochOverflowDoesNotMutate() {
    CompositorCoordinator coordinator;
    initializeAtomicSnapshot(coordinator);
    const CompositionEpoch maximum{std::numeric_limits<unsigned long long>::max()};
    CompositorCoordinatorTestAccess::setCompositionEpoch(coordinator, maximum);
    require(coordinator.adoptCompositionSnapshot(kS1, layoutFor(kS1)) ==
                CompositionStateAdoptionResult::Rejected,
            "epoch overflowとなるsnapshotを受理しました");
    require(coordinator.compositionState() == kS0 && coordinator.compositionEpoch() == maximum,
            "epoch overflow rejectでidentityが変化しました");
    requireComposedSnapshot(coordinator, kS0, maximum, layoutFor(kS0), 85);
}

void atomicSnapshotComposeMatchesStateAndLayout() {
    CompositorCoordinator coordinator;
    initializeAtomicSnapshot(coordinator);
    require(coordinator.adoptCompositionSnapshot(kS3, layoutFor(kS3)) ==
                CompositionStateAdoptionResult::Adopted,
            "S3/layout3 snapshotを採用できません");
    requireComposedSnapshot(coordinator, kS3, coordinator.compositionEpoch(), layoutFor(kS3), 86);
}

void atomicTransitionSequenceIncrementsFiveTimes() {
    CompositorCoordinator coordinator;
    const auto baseline = initializeAtomicSnapshot(coordinator);
    const std::vector<CompositionStateId> states{kS1, kS2, kS3, kS0, kS1};
    for (size_t i = 0; i < states.size(); ++i) {
        const auto state = states[i];
        require(coordinator.adoptCompositionSnapshot(state, layoutFor(state)) ==
                    CompositionStateAdoptionResult::Adopted,
                "atomic transition sequenceを採用できません");
        const CompositionEpoch expected{baseline.value + i + 1};
        require(coordinator.compositionEpoch() == expected,
                "atomic transitionのepochがexactly 1進みません");
        require(coordinator.sourceGeneration(kSourceA) == SourceGeneration{7} &&
                    coordinator.sourceGeneration(kSourceB) == SourceGeneration{11},
                "atomic transitionがSourceGenerationを変更しました");
        requireComposedSnapshot(coordinator, state, expected, layoutFor(state),
                                90 + static_cast<long long>(i));
    }
    require(coordinator.compositionEpoch().value == baseline.value + 5,
            "atomic transition sequenceのepoch増分が5ではありません");
}

using Test = std::pair<const char*, std::function<void()>>;

const std::vector<Test> kTests = {
    {"CanonicalLayoutS0", canonicalLayoutS0},
    {"CanonicalLayoutS1", canonicalLayoutS1},
    {"CanonicalLayoutS2", canonicalLayoutS2},
    {"CanonicalLayoutS3", canonicalLayoutS3},
    {"CanonicalScheduleStrings", canonicalScheduleStrings},
    {"CanonicalStateNames", canonicalStateNames},
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
    {"AtomicAdoptChangesStateLayoutEpochOnce", atomicAdoptChangesStateLayoutEpochOnce},
    {"AtomicAdoptSameSnapshotIsNoOp", atomicAdoptSameSnapshotIsNoOp},
    {"AtomicRejectInvalidStateDoesNotMutate", atomicRejectInvalidStateDoesNotMutate},
    {"AtomicRejectInvalidLayoutDoesNotMutate", atomicRejectInvalidLayoutDoesNotMutate},
    {"AtomicRejectSameStateDifferentLayout", atomicRejectSameStateDifferentLayout},
    {"AtomicEpochOverflowDoesNotMutate", atomicEpochOverflowDoesNotMutate},
    {"AtomicSnapshotComposeMatchesStateAndLayout", atomicSnapshotComposeMatchesStateAndLayout},
    {"AtomicTransitionSequenceIncrementsFiveTimes", atomicTransitionSequenceIncrementsFiveTimes},
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
