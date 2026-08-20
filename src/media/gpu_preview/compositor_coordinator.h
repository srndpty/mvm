#ifndef MVM_GPU_PREVIEW_COMPOSITOR_COORDINATOR_H
#define MVM_GPU_PREVIEW_COMPOSITOR_COORDINATOR_H

#include "media/gpu_preview/composed_frame.h"

#include <map>
#include <mutex>
#include <vector>

namespace mvm::gpu {

enum class CompositionResult {
    Accepted = 0,
    MissingSource,
    MixedFrame,
    StaleGeneration,
    FutureGeneration,
    StaleEpoch,
    UnknownSource,
};

enum class ConfigureResult {
    Configured = 0,
    RejectedInvalid,
    RejectedAlreadyConfigured,
};

enum class LayoutUpdateResult {
    Updated = 0,
    NoOp,
    Rejected,
};

enum class CompositionStateAdoptionResult {
    Adopted = 0,
    NoOp,
    Rejected,
};

struct LayerLayout {
    SourceId sourceId{};
    RectF destination{};
    RectF sourceUv{};
    float opacity = 1.0f;
    int zOrder = 0;
};

struct CompositorCoordinatorTestAccess;

class CompositorCoordinator {
public:
    ConfigureResult configure(std::vector<LayerLayout> layout,
                              const std::map<SourceId, SourceGeneration>& generations);
    LayoutUpdateResult updateLayout(std::vector<LayerLayout> layout);
    CompositionStateAdoptionResult adoptCompositionState(CompositionStateId requested);
    CompositionStateAdoptionResult
    adoptCompositionSnapshot(CompositionStateId requestedState,
                             std::vector<LayerLayout> requestedLayout);
    // source集合ごと入れ替わるcomposition transitionを、同一instanceのまま
    // atomicに採用する。`configure()`はlayoutとgenerationsを1:1で要求し
    // 一度きりなので、これが無いと参照source集合が変わるたびにinstanceを
    // 作り直すことになり、CompositionEpochのlineageが切れる。
    //
    //  - `CompositionEpoch`はresolved composition stateが実際に変わったときだけ
    //    ちょうど1進む。generationだけが動いた場合は進めない
    //  - **現在追跡中のsource**に対するgenerationのregressionをfail-closedで拒否する。
    //    layoutから外れて追跡対象でなくなったsourceのgenerationは保持しないので、
    //    一度外れてから戻ってきたsourceに対する歴史的なfloorは持たない。
    //    `SourceGeneration`のownerはあくまでsource側であり、coordinatorへ
    //    寄せないための意図的な線引きである
    //  - 同一`CompositionStateId`が別layoutを指す要求は拒否する
    //  - まだconfigureされていないinstanceに対しても最初の採用として成立する
    CompositionStateAdoptionResult
    adoptCompositionRuntimeSnapshot(CompositionStateId requestedState,
                                    std::vector<LayerLayout> requestedLayout,
                                    std::map<SourceId, SourceGeneration> requestedGenerations);
    bool setSourceGeneration(SourceId source, SourceGeneration generation);
    // test専用。state / layout / generationを一切変えず`CompositionEpoch`だけを
    // 1進める。composition transitionを起こさずにsupersedeだけを再現する。
    // overflow時はfalseを返し、epochを進めない。
    bool advanceCompositionEpochForTest();
    SourceGeneration sourceGeneration(SourceId source) const;
    CompositionEpoch compositionEpoch() const;
    CompositionStateId compositionState() const;
    CompositionResult compose(long long outputFrameNumber,
                              const std::vector<DecodedGpuFrame>& frames, ComposedFrame& out);
    CompositionResult validateForDisplay(const ComposedFrame& frame) const;

    long long mixedSourceFrameCount() const;
    long long mixedGenerationCount() const;
    long long staleCompositionEpochCount() const;
    long long missingSourceFrameCount() const;

private:
    friend struct CompositorCoordinatorTestAccess;
    CompositionResult validateLocked(long long outputFrameNumber,
                                     const std::vector<DecodedGpuFrame>& frames) const;
    mutable std::mutex mutex_;
    std::vector<LayerLayout> layout_;
    std::map<SourceId, SourceGeneration> generations_;
    CompositionEpoch epoch_{};
    CompositionStateId state_{};
    bool configured_ = false;
    long long mixedFrame_ = 0;
    long long mixedGeneration_ = 0;
    mutable long long staleEpoch_ = 0;
    long long missing_ = 0;
};

} // namespace mvm::gpu
#endif
