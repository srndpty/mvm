#ifndef MVM_APP_EQUATION_SEQUENCE_EXPORT_H
#define MVM_APP_EQUATION_SEQUENCE_EXPORT_H

#include "app/equation_sequence_preview.h"

#include <functional>

namespace mvm::app {

enum class EquationExportFailure {
    None,
    InvalidProject,
    CompileFailed,
    MissingPart,
    InvalidBinding,
    UnsupportedTexBoundary,
    BackendUnavailable,
    ArtifactPending,
    ArtifactFailed,
    ArtifactCorrupt,
    StaticArtifactMissing,
    CorruptStatic,
    ProvenanceMissing,
    ProvenanceMismatch,
    FrameMissing,
    CorruptTransition,
    CorruptActionBase,
    CorruptActionAccent,
    StaleSequenceKey,
    UnsupportedTimelineTransition,
    InvalidSourceRange,
    Cancelled
};

struct EquationExportReadiness {
    EquationExportFailure failure = EquationExportFailure::None;
    EquationCompileFailure compileFailure = EquationCompileFailure::None;
    std::string detail;

    bool ready() const { return failure == EquationExportFailure::None; }
};

// 開始時点の正準入力と provenance。callback は GUI/cache の可変 record を参照しない。
struct EquationExportSnapshot {
    std::string sequenceKey;
    math::EquationSequenceRenderSpec renderSpec;
    math::MathToolchainFingerprint toolchain;
    std::string sequenceTemplate;
    EquationPreviewInputs presentation;
    EquationExportReadiness readiness;
    // disk の全検査は export worker で行う。GUI の可変 object を捕捉しない。
    std::function<EquationExportSnapshot(const EquationPreviewInputs&,
                                         const std::function<bool()>&)>
        prepare;
    std::function<EquationExportReadiness()> validate;
    std::function<EquationExportReadiness(const EquationPreviewLayerId&,
                                          std::vector<std::uint8_t>&)>
        loadLayer;
};

struct EquationSequenceExportPlan {
    EquationExportReadiness readiness;
    EquationExportSnapshot snapshot;
    std::vector<EquationPreviewTime> frames;
};

EquationSequenceExportPlan planEquationSequenceExport(const project::TimelineClip& clip,
                                                      std::int64_t fpsNum, std::int64_t fpsDen,
                                                      int width, int height,
                                                      const EquationExportSnapshot& snapshot,
                                                      const std::function<bool()>& cancelled = {});

// 出力全体の RGBA8。内部の合成だけを行い、外側 ClipEffects は通常 exporter が一度掛ける。
EquationExportReadiness composeEquationExportFrame(const EquationExportSnapshot& snapshot,
                                                   std::int64_t outputFrame,
                                                   std::vector<std::uint8_t>& rgba);

} // namespace mvm::app
#endif
