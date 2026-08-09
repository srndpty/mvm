#ifndef MVM_APP_PREVIEW_DISPLAY_TARGET_CONTRACT_H
#define MVM_APP_PREVIEW_DISPLAY_TARGET_CONTRACT_H

#include <string>

namespace mvm::app {

struct DisplayEnvironmentSnapshot {
    std::string screenName;
    std::string screenOrientation;
    int screenGeometryWidth = 0;
    int screenGeometryHeight = 0;
    int availableGeometryWidth = 0;
    int availableGeometryHeight = 0;
    double devicePixelRatio = 0.0;
    int windowLogicalWidth = 0;
    int windowLogicalHeight = 0;
    int compositorSurfaceLogicalWidth = 0;
    int compositorSurfaceLogicalHeight = 0;
    int rhiTargetPixelWidth = 0;
    int rhiTargetPixelHeight = 0;
    int nativeWindowOuterWidth = 0;
    int nativeWindowOuterHeight = 0;
    int nativeWindowClientWidth = 0;
    int nativeWindowClientHeight = 0;
};

enum class DisplayTargetPreflightState { Waiting, Passed, Failed };

struct DisplayTargetPreflightResult {
    DisplayTargetPreflightState state = DisplayTargetPreflightState::Waiting;
    std::string error;

    bool workloadMayStart() const { return state == DisplayTargetPreflightState::Passed; }
};

DisplayTargetPreflightResult evaluateP3C2DisplayTarget(const DisplayEnvironmentSnapshot& snapshot);
bool sameDisplayEnvironment(const DisplayEnvironmentSnapshot& a,
                            const DisplayEnvironmentSnapshot& b);

} // namespace mvm::app

#endif
