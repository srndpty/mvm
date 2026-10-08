#ifndef MVM_MEDIA_GRAPH_NUMERIC_H
#define MVM_MEDIA_GRAPH_NUMERIC_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mvm::graph {
inline constexpr std::string_view expressionVersion = "graph-expression/1";
inline constexpr std::string_view samplingVersion = "graph-sampling/1";
inline constexpr std::string_view drawVersion = "graph-draw/1";
enum class Operation : std::uint8_t {
    Literal,
    X,
    Pi,
    E,
    Positive,
    Negative,
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Sin,
    Cos,
    Tan,
    Exp,
    Log,
    Sqrt,
    Abs
};
enum class CompileStatus { Success, InvalidExpression, UnsupportedExpression };
enum class CompileLimit { None, SourceBytes, Recursion, Depth, Nodes };
enum class EvaluationStatus { Finite, UndefinedDivision, UndefinedDomain, UndefinedOverflow };

struct Evaluation {
    EvaluationStatus status = EvaluationStatus::UndefinedDomain;
    double value = 0;
};

class Expression {
public:
    Evaluation evaluate(double x) const;
    std::string canonical() const;

    std::size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        Operation operation;
        double literal;
        std::size_t left, right, depth;
    };

    std::vector<Node> nodes_;
    std::size_t root_ = 0;
    friend class Parser;
};

struct Compilation {
    CompileStatus status = CompileStatus::InvalidExpression;
    CompileLimit limit = CompileLimit::None;
    std::size_t offset = 0;
    Expression expression;
};

// 呼び出し側は上限を小さくできる。凍結された最大値より大きくはできない。
struct CompileBudget {
    std::size_t nodes = 4096;
};

Compilation compile(std::string_view source, CompileBudget = {});

struct Point {
    double x = 0, y = 0;
    bool operator==(const Point&) const = default;
};

using Segment = std::vector<Point>;

struct SamplingRequest {
    double domainMin, domainMax;
    double xMin, xMax, yMin, yMax;
    double plotWidthPixels, plotHeightPixels;
    std::string functionId;
};
enum class SamplingStatus {
    Success,
    ValidEmptyCurve,
    NoFiniteSamples,
    InvalidRequest,
    SamplingBudgetExceeded,
    EvaluationFailure
};

struct SamplingDiagnostics {
    std::size_t evaluations = 0, finiteSamples = 0, finiteEvaluations = 0;
    std::size_t undefinedDivision = 0, undefinedDomain = 0, undefinedOverflow = 0;
    std::size_t discardedEdges = 0, midpointFailures = 0, jumpDiscardedEdges = 0;
    std::size_t deviationDiscardedEdges = 0;
    bool operator==(const SamplingDiagnostics&) const = default;
};

struct Geometry {
    std::string functionId;
    double domainMin = 0, domainMax = 0;
    std::vector<Segment> segments;
    bool operator==(const Geometry&) const = default;
};

struct SamplingResult {
    SamplingStatus status = SamplingStatus::InvalidRequest;
    SamplingDiagnostics diagnostics;
    Geometry geometry;
};

SamplingResult sample(const Expression&, const SamplingRequest&);
// source frame の進捗だけで決まる。終端では点列をそのまま返す。
Geometry reveal(const Geometry&, std::int64_t sourceFrame, std::int64_t introFrames);
} // namespace mvm::graph
#endif
