// Phase 4 / B: schedule resolve -> atomic snapshot adoption -> exact pair -> compose ->
// display ledger の順序と、ledger からの独立再計算を検査する。
//
// **期待値は catalog / driver を呼ばずに literal で書く。** 実装と同じ式を共有すると、
// 実装のバグを test が追認する (AGENTS.md)。
#include "media/gpu_preview/composition_display_ledger.h"
#include "media/gpu_preview/phase4_composition_catalog.h"
#include "media/gpu_preview/phase4_composition_driver.h"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mvm::gpu;

namespace {

constexpr SourceId kSourceA{1};
constexpr SourceId kSourceB{2};
constexpr SourceGeneration kGenerationA{7};
constexpr SourceGeneration kGenerationB{11};
constexpr ResourceEpoch kResourceA{101};
constexpr ResourceEpoch kResourceB{202};
constexpr long long kMeasurementFrames = 600;
constexpr long long kFormalMeasurementFrames = 3600;
constexpr size_t kFormalLedgerSafetyMargin = 512;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

// docs/phase4-plan.md §3.8 の smoke segment を literal で書く。
std::string expectedStateName(long long frame) {
    if (frame < 200)
        return "S0";
    if (frame < 400)
        return "S1";
    return "S2";
}

unsigned long long expectedSegmentIndex(long long frame) {
    if (frame < 200)
        return 0;
    if (frame < 400)
        return 1;
    return 2;
}

DecodedGpuFrame sourceFrame(SourceId source, long long frameNumber) {
    DecodedGpuFrame frame;
    frame.sourceId = source;
    frame.frameNumber = frameNumber;
    frame.sourceGeneration = source == kSourceA ? kGenerationA : kGenerationB;
    frame.resourceEpoch = source == kSourceA ? kResourceA : kResourceB;
    return frame;
}

Phase4CompositionDriver makeDriver() {
    auto schedule = phase4Schedule(Phase4ScheduleKind::Smoke);
    require(schedule.has_value(), "smoke schedule を構築できません");
    return Phase4CompositionDriver(*schedule, {kSourceA, kSourceB});
}

// controller と同じ startup: configure -> initial S0 adoption。この adoption は
// measurement baseline より前なので driver counter へ入れない。
CompositionEpoch initialize(CompositorCoordinator& coordinator) {
    require(coordinator.configure(phase4CanonicalLayout(kPhase4S0),
                                  {{kSourceA, kGenerationA}, {kSourceB, kGenerationB}}) ==
                ConfigureResult::Configured,
            "coordinator を初期化できません");
    require(coordinator.adoptCompositionSnapshot(kPhase4S0, phase4CanonicalLayout(kPhase4S0)) ==
                CompositionStateAdoptionResult::Adopted,
            "initial S0 snapshot を採用できません");
    return coordinator.compositionEpoch();
}

// resolve -> adopt -> pair -> compose -> record の 1 frame 分。
void driveAndDisplay(Phase4CompositionDriver& driver, CompositorCoordinator& coordinator,
                     CompositionDisplayLedger& ledger, long long frame) {
    const auto driven = driver.onTargetFrame(coordinator, frame);
    require(driven == Phase4DriveResult::Adopted || driven == Phase4DriveResult::NoOp,
            "target frame の adoption が拒否されました");
    ComposedFrame composed;
    require(coordinator.compose(frame, {sourceFrame(kSourceA, frame), sourceFrame(kSourceB, frame)},
                                composed) == CompositionResult::Accepted,
            "exact pair を compose できません");
    require(coordinator.validateForDisplay(composed) == CompositionResult::Accepted,
            "compose 結果が display validation を通りません");
    ledger.record(composed, frame);
}

struct LedgerVerdict {
    long long stateMismatch = 0;
    long long epochMismatch = 0;
    long long identityMismatch = 0;
    long long oldStateAfterBoundary = 0;
};

// producer の counter を使わず、ledger record から独立に再計算する。
LedgerVerdict verifyLedger(const std::vector<CompositionDisplayRecord>& records,
                           CompositionEpoch baselineEpoch) {
    LedgerVerdict verdict;
    for (const auto& record : records) {
        const std::string expectedName = expectedStateName(record.outputFrameNumber);
        const char* actualName = phase4StateName(record.compositionState);
        const bool stateOk = actualName && expectedName == actualName;
        if (!stateOk) {
            ++verdict.stateMismatch;
            if (record.outputFrameNumber >= 200)
                ++verdict.oldStateAfterBoundary;
        }
        if (record.compositionEpoch.value !=
            baselineEpoch.value + expectedSegmentIndex(record.outputFrameNumber))
            ++verdict.epochMismatch;
        if (record.sources.size() != 2 ||
            record.sources[0] !=
                SourceFrameIdentity{kSourceA, kGenerationA, kResourceA, record.outputFrameNumber} ||
            record.sources[1] !=
                SourceFrameIdentity{kSourceB, kGenerationB, kResourceB, record.outputFrameNumber})
            ++verdict.identityMismatch;
    }
    return verdict;
}

void smokeRunAdoptsExactlyTwice() {
    CompositorCoordinator coordinator;
    const auto baseline = initialize(coordinator);
    auto driver = makeDriver();
    CompositionDisplayLedger ledger(1024);
    for (long long frame = 0; frame < kMeasurementFrames; ++frame)
        driveAndDisplay(driver, coordinator, ledger, frame);

    const auto counters = driver.counters();
    require(counters.adoptionCount == 2, "smoke 区間の adoption が 2 ではありません");
    require(counters.epochIncrementCount == 2, "smoke 区間の epoch increment が 2 ではありません");
    require(counters.rejectCount == 0, "smoke 区間で reject が発生しました");
    require(counters.unresolvedFrameCount == 0, "smoke 区間で resolve できない frame があります");
    require(counters.sourceGenerationChangeDueToLayoutCount == 0,
            "layout transition が SourceGeneration を変更しました");
    require(counters.resolveCount ==
                counters.adoptionCount + counters.noopCount + counters.rejectCount,
            "resolve == adoption + noop + reject が成立しません");
    require(coordinator.compositionEpoch().value == baseline.value + 2,
            "smoke 区間の epoch 増分が 2 ではありません");

    const auto records = ledger.recordsAfter(0);
    require(records.size() == static_cast<size_t>(kMeasurementFrames),
            "measurement ledger の件数が 600 ではありません");
    const auto verdict = verifyLedger(records, baseline);
    require(verdict.stateMismatch == 0, "ledger の state が schedule と一致しません");
    require(verdict.epochMismatch == 0, "ledger の epoch が E0 相対期待値と一致しません");
    require(verdict.identityMismatch == 0, "ledger の A/B layer identity が一致しません");
    require(verdict.oldStateAfterBoundary == 0, "boundary 後に旧 state を表示しました");
}

void formalLedgerRetainsAll3600() {
    CompositionDisplayLedger ledger(kCompositionDisplayLedgerCapacity);
    const auto baseline = ledger.baseline();
    ComposedFrame frame;
    for (long long outputFrame = 0; outputFrame < kFormalMeasurementFrames; ++outputFrame) {
        frame.outputFrameNumber = outputFrame;
        ledger.record(frame, outputFrame);
    }

    const auto records = ledger.recordsAfter(baseline);
    require(records.size() == static_cast<size_t>(kFormalMeasurementFrames),
            "formal 3600 displayを全件保持できません");
    require(records.front().outputFrameNumber == 0, "formal ledgerの先頭からframe 0が失われました");
    require(records.back().outputFrameNumber == 3599,
            "formal ledgerの末尾がframe 3599ではありません");
}

void formalLedgerCapacityExceedsMeasurement() {
    CompositionDisplayLedger ledger(kCompositionDisplayLedgerCapacity);
    require(ledger.capacity() == kCompositionDisplayLedgerCapacity,
            "production ledger capacity constantがconstructionに反映されません");
    require(ledger.capacity() >=
                static_cast<size_t>(kFormalMeasurementFrames) + kFormalLedgerSafetyMargin,
            "formal measurementに対するledger safety marginが不足しています");
}

void boundedLedgerEvictsOldest() {
    CompositionDisplayLedger ledger(3);
    const auto baseline = ledger.baseline();
    ComposedFrame frame;
    for (long long outputFrame = 0; outputFrame < 4; ++outputFrame) {
        frame.outputFrameNumber = outputFrame;
        ledger.record(frame, outputFrame);
    }

    const auto records = ledger.recordsAfter(baseline);
    require(records.size() == 3, "small capacity ledgerが上限3件を守りません");
    require(records.front().outputFrameNumber == 1 && records.back().outputFrameNumber == 3,
            "bounded ringが最古の1件を破棄しません");
}

// verify 側が本当に効いていることを示す。これが通らなければ上の 0 件は空振りである。
void ledgerVerificationDetectsWrongState() {
    CompositorCoordinator coordinator;
    const auto baseline = initialize(coordinator);
    auto driver = makeDriver();
    CompositionDisplayLedger ledger(16);
    driveAndDisplay(driver, coordinator, ledger, 0);
    auto records = ledger.recordsAfter(0);
    require(records.size() == 1, "record を 1 件記録できません");
    records[0].outputFrameNumber = 250; // S0 の record を S1 区間の frame と主張させる
    const auto verdict = verifyLedger(records, baseline);
    require(verdict.stateMismatch == 1 && verdict.oldStateAfterBoundary == 1,
            "boundary 後の旧 state を検出できません");
    require(verdict.epochMismatch == 1, "E0 相対 epoch の不一致を検出できません");
    require(verdict.identityMismatch == 1, "layer identity の不一致を検出できません");
}

// boundary frame 自体が drop されても、次の display は新 state でなければならない。
void boundaryDropStillActivatesNewState() {
    CompositorCoordinator coordinator;
    const auto baseline = initialize(coordinator);
    auto driver = makeDriver();
    CompositionDisplayLedger ledger(16);
    driveAndDisplay(driver, coordinator, ledger, 199);
    driveAndDisplay(driver, coordinator, ledger, 201); // frame 200 は drop された
    const auto records = ledger.recordsAfter(0);
    require(records.size() == 2, "record を 2 件記録できません");
    require(records[0].compositionState == kPhase4S0 && records[0].compositionEpoch == baseline,
            "frame 199 が S0 / E0 ではありません");
    require(records[1].compositionState == kPhase4S1 &&
                records[1].compositionEpoch.value == baseline.value + 1,
            "boundary drop 後の frame 201 が S1 / E0+1 ではありません");
    require(verifyLedger(records, baseline).stateMismatch == 0, "drop 後に旧 state を表示しました");
}

void repeatedSameFrameIsNoop() {
    CompositorCoordinator coordinator;
    const auto baseline = initialize(coordinator);
    auto driver = makeDriver();
    for (int i = 0; i < 50; ++i)
        require(driver.onTargetFrame(coordinator, 42) == Phase4DriveResult::NoOp,
                "同一 state の再解決が NoOp になりません");
    const auto counters = driver.counters();
    require(counters.noopCount == 50 && counters.adoptionCount == 0 &&
                counters.epochIncrementCount == 0,
            "NoOp が epoch を進めました");
    require(coordinator.compositionEpoch() == baseline, "NoOp で epoch が変化しました");
}

void unconfiguredCoordinatorIsRejected() {
    CompositorCoordinator coordinator; // configure していない
    auto driver = makeDriver();
    require(driver.onTargetFrame(coordinator, 0) == Phase4DriveResult::Rejected,
            "未初期化 coordinator が snapshot を受理しました");
    const auto counters = driver.counters();
    require(counters.rejectCount == 1 && counters.adoptionCount == 0 && counters.noopCount == 0,
            "reject が counter へ計上されません");
    require(counters.resolveCount ==
                counters.adoptionCount + counters.noopCount + counters.rejectCount,
            "reject 時に resolve == adoption + noop + reject が崩れます");
}

void unresolvableFrameIsNotCountedAsResolve() {
    CompositorCoordinator coordinator;
    initialize(coordinator);
    auto driver = makeDriver();
    require(driver.onTargetFrame(coordinator, -1) == Phase4DriveResult::Unresolved,
            "負の output frame を resolve しました");
    const auto counters = driver.counters();
    require(counters.unresolvedFrameCount == 1 && counters.resolveCount == 0,
            "resolve 失敗を resolve として数えました");
}

// transition しても decoder / audio の generation は変わらない。
void transitionKeepsSourceGeneration() {
    CompositorCoordinator coordinator;
    initialize(coordinator);
    auto driver = makeDriver();
    for (const long long frame : {0LL, 200LL, 400LL}) {
        require(driver.onTargetFrame(coordinator, frame) != Phase4DriveResult::Rejected,
                "transition を採用できません");
        require(coordinator.sourceGeneration(kSourceA) == kGenerationA &&
                    coordinator.sourceGeneration(kSourceB) == kGenerationB,
                "transition が SourceGeneration を変更しました");
    }
    require(driver.counters().sourceGenerationChangeDueToLayoutCount == 0,
            "layout 起因の generation 変更が計上されました");
}

// adopt 済み state に対応する canonical layout が compose 結果に出る。
void composedLayoutMatchesScheduleState() {
    CompositorCoordinator coordinator;
    initialize(coordinator);
    auto driver = makeDriver();
    // S2 (frame 400 以降) は B が左上 960x540 / opacity 0.50。literal で書く。
    require(driver.onTargetFrame(coordinator, 400) == Phase4DriveResult::Adopted,
            "S2 を採用できません");
    ComposedFrame composed;
    require(coordinator.compose(400, {sourceFrame(kSourceA, 400), sourceFrame(kSourceB, 400)},
                                composed) == CompositionResult::Accepted,
            "S2 frame を compose できません");
    require(composed.compositionState == kPhase4S2, "compose 結果の state が S2 ではありません");
    require(composed.layers.size() == 2, "layer 数が 2 ではありません");
    require(composed.layers[0].frame.sourceId == kSourceA && composed.layers[0].opacity == 1.0f &&
                composed.layers[0].destination.x == 0.0f &&
                composed.layers[0].destination.width == 1.0f,
            "A layer が全面 opacity 1.0 ではありません");
    require(composed.layers[1].frame.sourceId == kSourceB && composed.layers[1].opacity == 0.5f &&
                composed.layers[1].destination.x == 0.0f &&
                composed.layers[1].destination.y == 0.0f &&
                composed.layers[1].destination.width == 0.5f &&
                composed.layers[1].destination.height == 0.5f,
            "B layer が左上 0.5 x 0.5 / opacity 0.50 ではありません");
}

using Test = std::pair<const char*, std::function<void()>>;

const std::vector<Test> kTests = {
    {"SmokeRunAdoptsExactlyTwice", smokeRunAdoptsExactlyTwice},
    {"FormalLedgerRetainsAll3600", formalLedgerRetainsAll3600},
    {"FormalLedgerCapacityExceedsMeasurement", formalLedgerCapacityExceedsMeasurement},
    {"BoundedLedgerEvictsOldest", boundedLedgerEvictsOldest},
    {"LedgerVerificationDetectsWrongState", ledgerVerificationDetectsWrongState},
    {"BoundaryDropStillActivatesNewState", boundaryDropStillActivatesNewState},
    {"RepeatedSameFrameIsNoop", repeatedSameFrameIsNoop},
    {"UnconfiguredCoordinatorIsRejected", unconfiguredCoordinatorIsRejected},
    {"UnresolvableFrameIsNotCountedAsResolve", unresolvableFrameIsNotCountedAsResolve},
    {"TransitionKeepsSourceGeneration", transitionKeepsSourceGeneration},
    {"ComposedLayoutMatchesScheduleState", composedLayoutMatchesScheduleState},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_phase4b_integration <test-name>\n");
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
