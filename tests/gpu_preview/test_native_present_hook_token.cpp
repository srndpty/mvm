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
    if (!check(mvm::app::makeNativePresentCompositionToken(frame, 5, 0, true, token),
               "正常なcomposition tokenを構築できません") ||
        !check(token.tokenSerial == 5 && token.outputFrameNumber == 42 &&
                   token.compositionEpoch == 7 && token.compositionState == 9 &&
                   token.intentOrdinalValid == 1 && token.intentOrdinal == 0,
               "composition token headerが一致しません") ||
        !check(token.sourceCount == 2 && token.sources[0].sourceId == 1 &&
                   token.sources[1].sourceId == 2 && token.sources[0].sourceGeneration == 11 &&
                   token.sources[1].resourceEpoch == 22,
               "source identityがsource id順に固定されません"))
        return 1;

    MvmNativePresentCompositionToken rejected;
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 0, 0, false, rejected),
               "token serial 0を拒否しません"))
        return 1;
    frame.layers.clear();
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 6, 0, false, rejected),
               "source 0件を拒否しません"))
        return 1;
    frame.layers = {layer(1, 1, 1, 42), layer(2, 1, 1, 42), layer(3, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 7, 0, false, rejected),
               "固定ABI上限を超えるsourceを拒否しません"))
        return 1;
    frame.layers = {layer(1, 0, 1, 42), layer(2, 1, 1, 42)};
    if (!check(!mvm::app::makeNativePresentCompositionToken(frame, 8, 0, false, rejected),
               "generation 0を拒否しません"))
        return 1;
    if (!check(mvmNativePresentHookLayoutCompatible(4, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                                                    sizeof(MvmNativePresentCompositionToken),
                                                    sizeof(MvmNativePresentRecord)),
               "ABI v4 layoutを受理しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(3, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                                                     sizeof(MvmNativePresentCompositionToken),
                                                     sizeof(MvmNativePresentRecord)),
               "v3 app / v4 Qt相当のABI mismatchを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(4, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                                                     sizeof(MvmNativePresentCompositionToken) - 1,
                                                     sizeof(MvmNativePresentRecord)),
               "composition token layout mutationを拒否しません") ||
        !check(!mvmNativePresentHookLayoutCompatible(4, MVM_NATIVE_PRESENT_HOOK_RING_CAPACITY,
                                                     sizeof(MvmNativePresentCompositionToken),
                                                     sizeof(MvmNativePresentRecord) + 1),
               "native record layout mutationを拒否しません"))
        return 1;
    if (!check(!mvmNativePresentHookAbiVersionsCompatible(3, 4), "v3 app / v4 Qtを拒否しません") ||
        !check(!mvmNativePresentHookAbiVersionsCompatible(4, 3), "v4 app / v3 Qtを拒否しません"))
        return 1;
    return 0;
}
