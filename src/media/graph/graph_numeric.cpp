#include "media/graph/graph_numeric.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <utility>

namespace mvm::graph {
namespace {
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
} // namespace

class Parser {
public:
    explicit Parser(std::string_view s, CompileBudget b)
        : source_(s), nodeBudget_(std::min(b.nodes, std::size_t{4096})) {}

    Compilation run() {
        if (source_.size() > 4096) {
            fail(CompileLimit::SourceBytes);
        } else {
            const auto root = sum(0);
            skip();
            if (position_ != source_.size())
                fail();
            if (result_.status == CompileStatus::Success)
                result_.expression.root_ = root;
        }
        result_.offset = position_;
        if (result_.status != CompileStatus::Success)
            result_.expression = {};
        return std::move(result_);
    }

private:
    std::string_view source_;
    std::size_t position_ = 0;
    std::size_t nodeBudget_;
    Compilation result_{CompileStatus::Success, CompileLimit::None, 0, {}};

    void fail(CompileLimit limit = CompileLimit::None) {
        if (result_.status == CompileStatus::Success) {
            result_.status = CompileStatus::InvalidExpression;
            result_.limit = limit;
        }
    }

    void skip() {
        while (position_ < source_.size() && space(source_[position_]))
            ++position_;
    }

    bool take(char c) {
        skip();
        if (position_ < source_.size() && source_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    // 文法関数間の固定した呼び出しではなく、入力で増える再帰だけを数える。
    bool descend(std::size_t depth) {
        if (depth >= 64) {
            fail(CompileLimit::Recursion);
            return false;
        }
        return result_.status == CompileStatus::Success;
    }

    std::size_t node(Operation op, std::size_t a = absent, std::size_t b = absent, double v = 0) {
        if (result_.status != CompileStatus::Success)
            return absent;
        auto& nodes = result_.expression.nodes_;
        const auto depth =
            1 + std::max(a == absent ? 0 : nodes[a].depth, b == absent ? 0 : nodes[b].depth);
        if (depth > 64) {
            fail(CompileLimit::Depth);
            return absent;
        }
        if (nodes.size() >= nodeBudget_) {
            fail(CompileLimit::Nodes);
            return absent;
        }
        nodes.push_back({op, v, a, b, depth});
        return nodes.size() - 1;
    }

    std::size_t sum(std::size_t d) {
        auto a = product(d);
        while (result_.status == CompileStatus::Success) {
            if (take('+'))
                a = node(Operation::Add, a, product(d));
            else if (take('-'))
                a = node(Operation::Subtract, a, product(d));
            else
                break;
        }
        return a;
    }

    std::size_t product(std::size_t d) {
        auto a = unary(d);
        while (result_.status == CompileStatus::Success) {
            if (take('*'))
                a = node(Operation::Multiply, a, unary(d));
            else if (take('/'))
                a = node(Operation::Divide, a, unary(d));
            else
                break;
        }
        return a;
    }

    std::size_t unary(std::size_t d) {
        if (take('+')) {
            if (!descend(d))
                return absent;
            return node(Operation::Positive, unary(d + 1));
        }
        if (take('-')) {
            if (!descend(d))
                return absent;
            return node(Operation::Negative, unary(d + 1));
        }
        auto a = atom(d);
        if (result_.status == CompileStatus::Success && take('^')) {
            if (!descend(d))
                return absent;
            a = node(Operation::Power, a, unary(d + 1));
        }
        return a;
    }

    std::size_t atom(std::size_t d) {
        skip();
        if (take('(')) {
            if (!descend(d))
                return absent;
            auto a = sum(d + 1);
            if (!take(')'))
                fail();
            return a;
        }
        if (position_ == source_.size()) {
            fail();
            return absent;
        }
        const auto start = position_;
        if (digit(source_[position_]) || source_[position_] == '.') {
            bool digits = false;
            while (position_ < source_.size() && digit(source_[position_])) {
                ++position_;
                digits = true;
            }
            if (position_ < source_.size() && source_[position_] == '.') {
                ++position_;
                while (position_ < source_.size() && digit(source_[position_])) {
                    ++position_;
                    digits = true;
                }
            }
            if (!digits) {
                fail();
                return absent;
            }
            if (position_ < source_.size() &&
                (source_[position_] == 'e' || source_[position_] == 'E')) {
                ++position_;
                if (position_ < source_.size() &&
                    (source_[position_] == '+' || source_[position_] == '-'))
                    ++position_;
                const auto exponent = position_;
                while (position_ < source_.size() && digit(source_[position_]))
                    ++position_;
                if (position_ == exponent) {
                    fail();
                    return absent;
                }
            }
            double v = 0;
            const auto parsed =
                std::from_chars(source_.data() + start, source_.data() + position_, v);
            if (parsed.ec != std::errc{} || parsed.ptr != source_.data() + position_ ||
                !std::isfinite(v)) {
                fail();
                return absent;
            }
            return node(Operation::Literal, absent, absent, v);
        }
        if (!letter(source_[position_])) {
            fail();
            return absent;
        }
        while (position_ < source_.size() &&
               (letter(source_[position_]) || digit(source_[position_])))
            ++position_;
        const auto id = source_.substr(start, position_ - start);
        if (id == "x")
            return node(Operation::X);
        if (id == "pi")
            return node(Operation::Pi);
        if (id == "e")
            return node(Operation::E);
        Operation op;
        if (id == "sin")
            op = Operation::Sin;
        else if (id == "cos")
            op = Operation::Cos;
        else if (id == "tan")
            op = Operation::Tan;
        else if (id == "exp")
            op = Operation::Exp;
        else if (id == "log")
            op = Operation::Log;
        else if (id == "sqrt")
            op = Operation::Sqrt;
        else if (id == "abs")
            op = Operation::Abs;
        else {
            result_.status = CompileStatus::UnsupportedExpression;
            return absent;
        }
        if (!take('(')) {
            fail();
            return absent;
        }
        if (!descend(d))
            return absent;
        auto a = sum(d + 1);
        if (!take(')'))
            fail();
        return node(op, a);
    }
};

Compilation compile(std::string_view source, CompileBudget budget) {
    return Parser(source, budget).run();
}

Evaluation Expression::evaluate(double x) const {
    using S = EvaluationStatus;
    if (!std::isfinite(x) || nodes_.empty())
        return {S::UndefinedDomain, 0};
    std::vector<Evaluation> values;
    values.reserve(nodes_.size());
    for (const auto& n : nodes_) {
        auto a = n.left == absent ? Evaluation{S::Finite, 0} : values[n.left];
        auto b = n.right == absent ? Evaluation{S::Finite, 0} : values[n.right];
        if (a.status != S::Finite || b.status != S::Finite) {
            values.push_back(a.status != S::Finite ? a : b);
            continue;
        }
        double v = 0;
        S status = S::Finite;
        switch (n.operation) {
        case Operation::Literal:
            v = n.literal;
            break;
        case Operation::X:
            v = x;
            break;
        case Operation::Pi:
            v = std::numbers::pi;
            break;
        case Operation::E:
            v = std::numbers::e;
            break;
        case Operation::Positive:
            v = a.value;
            break;
        case Operation::Negative:
            v = -a.value;
            break;
        case Operation::Add:
            v = a.value + b.value;
            break;
        case Operation::Subtract:
            v = a.value - b.value;
            break;
        case Operation::Multiply:
            v = a.value * b.value;
            break;
        case Operation::Divide:
            if (b.value == 0)
                status = S::UndefinedDivision;
            else
                v = a.value / b.value;
            break;
        case Operation::Power:
            v = std::pow(a.value, b.value);
            if (std::isnan(v))
                status = S::UndefinedDomain;
            break;
        case Operation::Sin:
            v = std::sin(a.value);
            break;
        case Operation::Cos:
            v = std::cos(a.value);
            break;
        case Operation::Tan:
            if (std::abs(std::cos(a.value)) < 1e-12)
                status = S::UndefinedDomain;
            else
                v = std::tan(a.value);
            break;
        case Operation::Exp:
            v = std::exp(a.value);
            break;
        case Operation::Log:
            if (a.value <= 0)
                status = S::UndefinedDomain;
            else
                v = std::log(a.value);
            break;
        case Operation::Sqrt:
            if (a.value < 0)
                status = S::UndefinedDomain;
            else
                v = std::sqrt(a.value);
            break;
        case Operation::Abs:
            v = std::abs(a.value);
            break;
        }
        if (status == S::Finite && !std::isfinite(v))
            status = S::UndefinedOverflow;
        values.push_back({status, status == S::Finite ? v : 0});
    }
    return values[root_];
}

std::string Expression::canonical() const {
    if (nodes_.empty())
        return {};
    std::string result = "graph-expression/1:";
    constexpr char hex[] = "0123456789abcdef";
    auto append = [&](std::uint64_t value, int digits) {
        for (int i = digits - 1; i >= 0; --i)
            result += hex[(value >> (i * 4)) & 15];
    };
    // prefix traversal は所有 ID や構築時の node index を含まない。
    std::vector<std::size_t> pending{root_};
    while (!pending.empty()) {
        const auto n = nodes_[pending.back()];
        pending.pop_back();
        append(static_cast<std::uint8_t>(n.operation), 2);
        if (n.operation == Operation::Literal)
            append(std::bit_cast<std::uint64_t>(n.literal), 16);
        if (n.right != absent)
            pending.push_back(n.right);
        if (n.left != absent)
            pending.push_back(n.left);
    }
    return result;
}

namespace {
// Liang–Barsky。境界への一点接触は drawable な線ではない。
bool clipEdge(Point& a, Point& b, const SamplingRequest& r) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return false;
    double lo = 0, hi = 1;
    auto cut = [&](double p, double q) {
        if (p == 0)
            return q >= 0;
        const double t = q / p;
        if (p < 0)
            lo = std::max(lo, t);
        else
            hi = std::min(hi, t);
        return lo < hi;
    };
    if (!cut(-dx, a.x - r.xMin) || !cut(dx, r.xMax - a.x) || !cut(-dy, a.y - r.yMin) ||
        !cut(dy, r.yMax - a.y))
        return false;
    const Point begin{std::clamp(std::lerp(a.x, b.x, lo), r.xMin, r.xMax),
                      std::clamp(std::lerp(a.y, b.y, lo), r.yMin, r.yMax)};
    const Point end{std::clamp(std::lerp(a.x, b.x, hi), r.xMin, r.xMax),
                    std::clamp(std::lerp(a.y, b.y, hi), r.yMin, r.yMax)};
    a = begin;
    b = end;
    return a != b && std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) &&
           std::isfinite(b.y);
}
} // namespace

SamplingResult sample(const Expression& expression, const SamplingRequest& r) {
    SamplingResult out;
    const double span = r.yMax - r.yMin;
    for (double value : {r.domainMin, r.domainMax, r.xMin, r.xMax, r.yMin, r.yMax,
                         r.plotWidthPixels, r.plotHeightPixels})
        if (!std::isfinite(value))
            return out;
    if (!(r.xMin < r.xMax && r.yMin < r.yMax && r.domainMin < r.domainMax) ||
        !std::isfinite(r.xMax - r.xMin) || !std::isfinite(span) || r.plotWidthPixels <= 0 ||
        r.plotHeightPixels <= 0)
        return out;
    const double lo = std::max(r.domainMin, r.xMin), hi = std::min(r.domainMax, r.xMax);
    if (!(lo < hi))
        return out;
    const double density = std::ceil(4 * r.plotWidthPixels);
    if (!std::isfinite(density) || density > 32768) {
        out.status = SamplingStatus::SamplingBudgetExceeded;
        return out;
    }
    if (expression.nodeCount() == 0) {
        out.status = SamplingStatus::EvaluationFailure;
        return out;
    }
    const auto m = static_cast<std::size_t>(density);
    out.geometry = {r.functionId, lo, hi, {}};
    auto evaluate = [&](double x, bool endpoint) {
        auto v = expression.evaluate(x);
        ++out.diagnostics.evaluations;
        if (v.status == EvaluationStatus::Finite) {
            ++out.diagnostics.finiteEvaluations;
            if (endpoint)
                ++out.diagnostics.finiteSamples;
        } else if (v.status == EvaluationStatus::UndefinedDivision)
            ++out.diagnostics.undefinedDivision;
        else if (v.status == EvaluationStatus::UndefinedDomain)
            ++out.diagnostics.undefinedDomain;
        else
            ++out.diagnostics.undefinedOverflow;
        return v;
    };
    auto previous = evaluate(lo, true);
    double previousX = lo;
    bool drawable = false, connected = false;
    for (std::size_t i = 1; i <= m; ++i) {
        const double x = std::lerp(lo, hi, static_cast<double>(i) / static_cast<double>(m));
        const auto value = evaluate(x, true);
        bool edge =
            previous.status == EvaluationStatus::Finite && value.status == EvaluationStatus::Finite;
        if (edge) {
            const auto mid = evaluate(std::midpoint(previousX, x), false);
            if (mid.status != EvaluationStatus::Finite) {
                ++out.diagnostics.midpointFailures;
                edge = false;
            } else {
                // 幾何の差と平均は拡張精度で計算し、有限な binary64 の極値でも overflow させない。
                const long double y0 = previous.value, y1 = value.value, ym = mid.value;
                const long double jump = std::abs((y1 - y0) / span);
                const long double deviation = std::abs((ym - std::midpoint(y0, y1)) / span);
                if (!std::isfinite(jump) || jump > 0.125) {
                    ++out.diagnostics.jumpDiscardedEdges;
                    edge = false;
                } else if (!std::isfinite(deviation) || deviation > 0.5L / r.plotHeightPixels) {
                    ++out.diagnostics.deviationDiscardedEdges;
                    edge = false;
                }
            }
        }
        Point a{previousX, previous.value}, b{x, value.value};
        if (!edge || a == b) {
            ++out.diagnostics.discardedEdges;
            connected = false;
        } else {
            drawable = true;
            if (!clipEdge(a, b, r))
                connected = false;
            else {
                if (connected && out.geometry.segments.back().back() == a)
                    out.geometry.segments.back().push_back(b);
                else
                    out.geometry.segments.push_back({a, b});
                connected = true;
            }
        }
        previous = value;
        previousX = x;
    }
    out.status = !drawable                       ? SamplingStatus::NoFiniteSamples
                 : out.geometry.segments.empty() ? SamplingStatus::ValidEmptyCurve
                                                 : SamplingStatus::Success;
    return out;
}

Geometry reveal(const Geometry& input, std::int64_t frame, std::int64_t frames) {
    if (frames == 0 || frame >= frames)
        return input;
    Geometry out{input.functionId, input.domainMin, input.domainMax, {}};
    if (frame <= 0 || frames < 0)
        return out;
    const double boundary = std::lerp(input.domainMin, input.domainMax,
                                      static_cast<double>(frame) / static_cast<double>(frames));
    for (const auto& segment : input.segments) {
        Segment visible;
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const auto p = segment[i];
            if (p.x <= boundary)
                visible.push_back(p);
            else {
                if (i > 0 && segment[i - 1].x < boundary) {
                    const auto a = segment[i - 1];
                    visible.push_back(
                        {boundary, std::lerp(a.y, p.y, (boundary - a.x) / (p.x - a.x))});
                }
                break;
            }
        }
        if (visible.size() > 1)
            out.segments.push_back(std::move(visible));
    }
    return out;
}
} // namespace mvm::graph
