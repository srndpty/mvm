#include "app/preview/display_target_contract.h"

#include <iostream>

namespace {
mvm::app::DisplayEnvironmentSnapshot goodSnapshot() {
    return {.screenName = "test-screen",
            .screenOrientation = "landscape",
            .screenGeometryWidth = 1920,
            .screenGeometryHeight = 1200,
            .availableGeometryWidth = 1920,
            .availableGeometryHeight = 1152,
            .devicePixelRatio = 1.0,
            .windowLogicalWidth = 1920,
            .windowLogicalHeight = 1080,
            .compositorSurfaceLogicalWidth = 1920,
            .compositorSurfaceLogicalHeight = 1080,
            .rhiTargetPixelWidth = 1920,
            .rhiTargetPixelHeight = 1080};
}

bool require(bool condition, const char* message) {
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}
} // namespace

int main() {
    const auto good = goodSnapshot();
    const auto goodResult = mvm::app::evaluateP3C2DisplayTarget(good);
    if (!require(goodResult.workloadMayStart(), "1920x1080 / DPR 1.0 のpreflightがPASSしません"))
        return 1;

    auto portraitClamped = good;
    portraitClamped.rhiTargetPixelWidth = 1204;
    const auto badResult = mvm::app::evaluateP3C2DisplayTarget(portraitClamped);
    if (!require(badResult.state == mvm::app::DisplayTargetPreflightState::Failed,
                 "1204x1080 RHI targetをFAILにできません"))
        return 1;
    if (!require(!badResult.workloadMayStart(),
                 "preflight FAIL後にformal workload開始が許可されています"))
        return 1;

    auto waiting = good;
    waiting.rhiTargetPixelWidth = 0;
    const auto waitingResult = mvm::app::evaluateP3C2DisplayTarget(waiting);
    if (!require(waitingResult.state == mvm::app::DisplayTargetPreflightState::Waiting,
                 "RHI未初期化状態をFAIL/PASSと誤判定しています"))
        return 1;

    auto changed = good;
    changed.screenOrientation = "portrait";
    if (!require(!mvm::app::sameDisplayEnvironment(good, changed),
                 "orientation変更をprovenance差として検出できません"))
        return 1;
    return 0;
}
