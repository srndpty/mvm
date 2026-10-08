#ifndef MVM_MEDIA_GRAPH_RENDER_H
#define MVM_MEDIA_GRAPH_RENDER_H

#include "media/graph/graph_numeric.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <variant>

namespace mvm::graph {
inline constexpr char layoutVersion[] = "graph-layout/1";
inline constexpr char rgbaVersion[] = "rgba8-straight-source-over/1";
inline constexpr char staticVersion[] = "mvm-graph-static/1";
inline constexpr char artifactDrawVersion[] = "mvm-graph-draw/1";

struct Limits {
    static constexpr int dimension = 8192;
    static constexpr std::size_t pixels = 16777216, points = 196608;
    static constexpr std::size_t jsonBytes = 32 * 1024 * 1024;
    static constexpr std::size_t artifactBytes = 1024 * 1024 * 1024;
    static constexpr std::size_t outputBytes = 1024 * 1024;
    static constexpr std::int64_t frames = 10000;
};
enum class Failure {
    InvalidGraph,
    InvalidViewport,
    InvalidExpression,
    UnsupportedExpression,
    EvaluationFailure,
    NoFiniteSamples,
    ResourceLimit,
    BackendUnavailable,
    RendererFailure,
    LabelFailure,
    ArtifactCorrupt,
    Cancelled,
    Superseded,
    PublicationFailure
};

struct Error {
    Failure failure;
    std::size_t function = 0;
    std::string message;
};

struct Rectangle {
    double left = 0, top = 0, width = 0, height = 0;
    bool operator==(const Rectangle&) const = default;
};

struct Path {
    Segment points;
    std::uint32_t argb = 0;
    double widthPixels = 0;
    std::int64_t function = -1, segment = -1;
    bool operator==(const Path&) const = default;
};

struct Curve {
    std::string ast;
    Geometry geometry; // 所有 ID は診断用。正準 identity には含めない。
    std::uint32_t argb = 0;
    double referenceWidth = 0, widthPixels = 0;
};

struct Label {
    std::string text;
    Rectangle band;
    bool rotated = false;
    Rectangle content{};
};

Rectangle labelContentRectangle(const Rectangle&);

struct GraphRenderSpec {
    int width = 0, height = 0;
    double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    Rectangle plot;
    std::vector<Path> background;
    std::vector<Curve> curves;
    std::vector<Label> labels;
    std::int64_t drawFrames = 0;
};

using SpecResult = std::variant<GraphRenderSpec, Error>;
double strokeScale(int width, int height);
Point mapPoint(const GraphRenderSpec&, Point);
std::vector<Path> framePaths(const GraphRenderSpec&, std::int64_t frame);
std::variant<std::monostate, Error> validateSpec(const GraphRenderSpec&);
// 全 field を長さ前置で直列化する。Draw の枚数は静止 identity に入れない。
std::string canonicalSpec(const GraphRenderSpec&);
std::string digest(std::string_view);
std::string staticKey(const GraphRenderSpec&, const std::string& toolchain);
std::string drawKey(const std::string& staticIdentity, std::int64_t frames);
std::string frameProtocol(const GraphRenderSpec&, std::int64_t frame);
std::string pathGeometryDigest(const GraphRenderSpec&, const Path&);
std::string requestJson(const GraphRenderSpec&, const std::string& toolchain);
void sourceOver(const std::uint8_t* under, std::uint8_t coverage, std::uint32_t argb,
                std::uint8_t* out);

struct Raster {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

using RasterResult = std::variant<Raster, Error>;
RasterResult readRgba(const std::filesystem::path&, int width, int height);
bool writeRgba(const std::filesystem::path&, const Raster&);

struct RenderRequest {
    GraphRenderSpec spec;
    std::string toolchain;
    std::filesystem::path job;
    std::chrono::milliseconds timeout{300000};
};

using RenderResult = std::variant<std::monostate, Error>;
using GraphRenderer = std::function<RenderResult(const RenderRequest&, const std::atomic<bool>*)>;

struct Artifact {
    std::filesystem::path directory;
    std::string staticIdentity, drawIdentity;
    std::vector<std::string> pixelHashes;
};

using ArtifactResult = std::variant<Artifact, Error>;
// Manim を起動せず、期待する正準 spec と全 encoded/decoded hash を照合する。
ArtifactResult validateArtifact(const std::filesystem::path&, const GraphRenderSpec&,
                                const std::string& toolchain, const std::atomic<bool>* = nullptr);

// 同じ authority の世代変更と公開を同一 mutex で直列化する。
class PublicationAuthority {
public:
    std::uint64_t supersede();
    void shutdown();
    ArtifactResult generate(const RenderRequest&, const std::filesystem::path& cache,
                            std::uint64_t generation, const GraphRenderer&,
                            const std::atomic<bool>* = nullptr);

private:
    std::mutex mutex_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
    std::shared_ptr<std::atomic<bool>> generationCancel_ =
        std::make_shared<std::atomic<bool>>(false);
};
} // namespace mvm::graph
#endif
