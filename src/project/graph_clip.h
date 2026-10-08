#ifndef MVM_PROJECT_GRAPH_CLIP_H
#define MVM_PROJECT_GRAPH_CLIP_H
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mvm::project {
struct GraphFunctionId {
    std::string value;
    bool operator==(const GraphFunctionId&) const = default;
};

struct GraphViewport {
    double xMin = -5, xMax = 5, yMin = -5, yMax = 5;
    bool operator==(const GraphViewport&) const = default;
};

struct GraphAxes {
    bool showAxes = true, showGrid = true;
    std::string xLabel = "x", yLabel = "y";
    bool operator==(const GraphAxes&) const = default;
};

struct GraphFunction {
    GraphFunctionId id;
    std::string expression = "x^2";
    std::optional<double> domainMin, domainMax;
    std::string label;
    std::string color = "#FFFF6655";
    double strokeWidth = 3;
    bool operator==(const GraphFunction&) const = default;
};
enum class GraphIntroKind { None, Draw };

struct GraphIntro {
    GraphIntroKind kind = GraphIntroKind::None;
    std::int64_t frames = 0;
    bool operator==(const GraphIntro&) const = default;
};

struct GraphClipData {
    GraphViewport viewport;
    GraphAxes axes;
    std::vector<GraphFunction> functions;
    GraphIntro intro;
    bool operator==(const GraphClipData&) const = default;
};
enum class GraphValidationStatus { Valid, InvalidGraph, InvalidViewport };
bool validGraphFunctionId(const std::string&);
GraphValidationStatus validateGraph(const GraphClipData&, std::int64_t sourceFrames);
GraphClipData defaultGraph(GraphFunctionId);
bool remapGraphIds(GraphClipData&, const std::function<std::string()>&, std::string& error);
bool addGraphFunction(GraphClipData&, GraphFunction, std::int64_t sourceFrames);
bool deleteGraphFunction(GraphClipData&, GraphFunctionId, std::int64_t sourceFrames);
bool moveGraphFunction(GraphClipData&, GraphFunctionId, std::size_t index,
                       std::int64_t sourceFrames);
bool updateGraphFunction(GraphClipData&, const GraphFunction&, std::int64_t sourceFrames);
} // namespace mvm::project
#endif
