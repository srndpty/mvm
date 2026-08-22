#include "app/preview/native_present_hook.h"

#include <cstdio>

namespace {

mvm::gpu::CompositionLayerFrame layer(std::uint64_t sourceId, std::uint64_t generation,
                                      std::uint64_t resourceEpoch, long long frameNumber) {
    mvm::gpu::CompositionLayerFrame result;
    result.frame.sourceId = {sourceId};
    result.frame.sourceGeneration = {generation};
    result.frame.resourceEpoch = {resourceEpoch};
    result.frame.frameNumber = frameNumber;
    return result;
}

bool check(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}

} // namespace

int main() {
    mvm::gpu::ComposedFrame frame;
    frame.outputFrameNumber = 42;
    frame.compositionEpoch = {7};
    frame.compositionState = {9};
    frame.layers = {layer(2, 12, 22, 42), layer(1, 11, 21, 42)};
    MvmNativePresentCompositionToken token;
    if (!check(mvm::app::makeNativePresentCompositionToken(frame, 5, token),
               "正常なcomposition tokenを構築できません") ||
        !check(token.tokenSerial == 5 && token.outputFrameNumber == 42 &&
                   token.compositionEpoch == 7 && token.compositionState == 9,
               "composition token headerが一致しません") ||
        !check(token.sourceCount == 2 && token.sources[0].sourceId == 1 &&
                   token.sources[1].sourceId == 2 && token.sources[0].sourceGeneration == 11 &&
                   token.sources[1].resourceEpoch == 22,
               "source identityがsource id順に固定されません"))
        return 1;

    MvmNativePresentCompositionToken rejected;
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 0, rejected),
               "token serial 0を拒否しません"))
        return 1;
    frame.layers.clear();
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 6, rejected),
               "source 0件を拒否しません"))
        return 1;
    frame.layers = {layer(1, 1, 1, 42), layer(2, 1, 1, 42), layer(3, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 7, rejected),
               "固定ABI上限を超えるsourceを拒否しません"))
        return 1;
    frame.layers = {layer(1, 0, 1, 42), layer(2, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 8, rejected),
               "generation 0を拒否しません"))
        return 1;
    return 0;
}
