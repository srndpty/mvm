#ifndef MVM_APP_GRAPH_EXPORT_H
#define MVM_APP_GRAPH_EXPORT_H

#include "app/graph_render_compile.h"
#include "media/manim/manim_graph.h"
#include "project/graph_edit.h"

#include <map>
#include <set>

namespace mvm::app {
enum class GraphExportFailure {
    None,
    InvalidGraph,
    InvalidExpression,
    UnsupportedExpression,
    NoFiniteSamples,
    BackendUnavailable,
    ArtifactMissing,
    ArtifactCorrupt,
    ProvenanceMismatch,
    FrameMissing,
    ResourceLimit,
    UnsupportedTransition,
    RendererFailure,
    DecodeFailure,
    EncoderFailure,
    Cancelled,
    Superseded,
    PublicationFailure
};

struct GraphExportReadiness {
    GraphExportFailure failure = GraphExportFailure::None;
    std::string detail;

    bool ready() const { return failure == GraphExportFailure::None; }
};

// GUI で値を捕捉する。確認済み toolchain は provenance の入力であり常駐画素ではない。
struct GraphExportEnvironment {
    std::filesystem::path cache;
    std::string toolchain;
    std::function<std::variant<manim::GraphBackend, graph::Error>(const std::filesystem::path&,
                                                                  const std::atomic<bool>*)>
        preflight;
    const std::atomic<bool>* cancel = nullptr;
};

struct GraphExportFrame {
    std::int64_t outputFrame = 0;
    std::int64_t sourceFrame = 0;
    // -1 は静止端点。Draw の source index を clip-local index にしない。
    std::int64_t artifactFrame = -1;
};

struct GraphExportPackage {
    graph::GraphRenderSpec spec;
    std::string toolchain;
    graph::Artifact artifact;
};

struct GraphExportClip {
    std::string packageKey;
    std::vector<GraphExportFrame> frames;
};

// key の共有と clip の時間 authority を分離する。画素は ledger に常駐させない。
struct GraphExportLedger {
    GraphExportReadiness readiness;
    std::map<std::string, GraphExportClip> clips;
    std::map<std::string, std::shared_ptr<const GraphExportPackage>> packages;
};

GraphExportLedger prepareGraphExport(const project::Project&, int width, int height,
                                     std::int64_t outputBegin, std::int64_t outputEnd,
                                     const GraphExportEnvironment&,
                                     const std::function<bool()>& cancelled = {});

struct GraphExportFrameResult {
    GraphExportReadiness readiness;
    std::shared_ptr<const graph::Raster> raster;
    std::int64_t sourceFrame = 0;
    std::string packageKey, pixelHash;
};

GraphExportFrameResult loadGraphExportFrame(const GraphExportLedger&, const std::string& clipId,
                                            std::int64_t outputFrame,
                                            const std::atomic<bool>* cancel = nullptr);
GraphExportReadiness graphExportError(const graph::Error&);
} // namespace mvm::app
#endif
