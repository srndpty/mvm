#include "media/graph/graph_numeric.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
using namespace mvm::graph;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message);
    }
}

void value(const char* source, double x, double expected) {
    const auto compiled = compile(source);
    const auto v = compiled.expression.evaluate(x);
    check(compiled.status == CompileStatus::Success && v.status == EvaluationStatus::Finite &&
              std::abs(v.value - expected) < 1e-12,
          source);
}

void undefined(const char* source, double x, EvaluationStatus expected) {
    auto compiled = compile(source);
    check(compiled.status == CompileStatus::Success &&
              compiled.expression.evaluate(x).status == expected,
          source);
}
} // namespace

int main() {
    for (char separator : {' ', '\t', '\r', '\n'}) {
        const std::string ws(1, separator);
        const auto result =
            compile(ws + "sin" + ws + "(" + ws + "x" + ws + ")" + ws + "+" + ws + "1" + ws);
        check(result.status == CompileStatus::Success && result.expression.evaluate(0).value == 1,
              "ASCII space/tab/CR/LF は式の先頭・末尾・token 間で受容");
    }
    value(" \t\r\nsin(\t0\r)\n+ 1\r\n", 0, 1);
    for (char separator : {'\f', '\v'}) {
        const std::string ws(1, separator);
        for (const auto& source :
             {ws + "x", "x" + ws, "x" + ws + "+1", "sin" + ws + "(x)", "sin(" + ws + "x)", ws})
            check(compile(source).status == CompileStatus::InvalidExpression,
                  "form feed/vertical tab は先頭・末尾・token 間でも拒否");
    }
    value("-x^2", 3, -9);
    value("(-x)^2", 3, 9);
    value("2^3^2", 0, 512);
    value("2^-2", 0, .25);
    value("1+2*3", 0, 7);
    value("2*x+1", 3, 7);
    value("sin(pi/2)+cos(0)", 0, 2);
    value("exp(log(e))", 0, std::exp(1.0));
    value("sqrt(abs(-4))", 0, 2);
    value("tan(0)", 0, 0);
    value("0^0", 0, 1);
    value(".5+1.+1e-2", 0, 1.51);
    for (auto s : {"", "2x+1", "sin(x,x)", "sin()", "x.real", "x[0]", "1,2", ".", "1e", "(x", "x)"})
        check(compile(s).status == CompileStatus::InvalidExpression, "構文不正");
    for (auto s : {"foo(x)", "__import__('os')", "open(x)", "lambda x:x"})
        check(compile(s).status == CompileStatus::UnsupportedExpression, "未知識別子");
    check(compile("日本語").status == CompileStatus::InvalidExpression, "非 ASCII token");
    check(compile("[x for x in x]").status != CompileStatus::Success, "内包表記の拒否");
    undefined("1/x", 0, EvaluationStatus::UndefinedDivision);
    undefined("1/(-0)", 0, EvaluationStatus::UndefinedDivision);
    undefined("sqrt(x)", -1, EvaluationStatus::UndefinedDomain);
    undefined("log(x)", 0, EvaluationStatus::UndefinedDomain);
    undefined("exp(1000)", 0, EvaluationStatus::UndefinedOverflow);
    undefined("(-1)^0.5", 0, EvaluationStatus::UndefinedDomain);
    undefined("tan(pi/2)", 0, EvaluationStatus::UndefinedDomain);
    undefined("exp(1000)*0", 0, EvaluationStatus::UndefinedOverflow);
    undefined("x", std::numeric_limits<double>::infinity(), EvaluationStatus::UndefinedDomain);
    for (int depth : {63, 64, 65}) {
        auto n = static_cast<std::size_t>(depth);
        auto p = compile(std::string(n, '(') + "x" + std::string(n, ')'));
        check((p.status == CompileStatus::Success) == (depth <= 64), "括弧再帰境界");
        auto u = compile(std::string(n, '-') + "x");
        check((u.status == CompileStatus::Success) == (depth < 64), "単項 AST 深さ境界");
        std::string f = "x", e = "x";
        for (int i = 0; i < depth; ++i) {
            f = "sin(" + f + ")";
            e = "x^" + e;
        }
        check((compile(f).status == CompileStatus::Success) == (depth < 64), "関数 AST 深さ境界");
        check((compile(e).status == CompileStatus::Success) == (depth < 64), "累乗 AST 深さ境界");
        if (depth == 65) {
            check(p.limit == CompileLimit::Recursion, "括弧は降下前に拒否");
            check(compile(f).limit == CompileLimit::Recursion, "関数は降下前に拒否");
        }
    }
    check(compile(std::string(4095, ' ') + "x").status == CompileStatus::Success, "4096 byte");
    check(compile(std::string(4096, ' ') + "x").limit == CompileLimit::SourceBytes, "4097 byte");
    check(compile(std::string(4096, '9')).status == CompileStatus::InvalidExpression,
          "長い literal");
    check(compile("x+x", {3}).status == CompileStatus::Success &&
              compile("x+x", {2}).limit == CompileLimit::Nodes,
          "独立 node budget 境界");
    std::function<std::string(int)> tree = [&](int nodes) -> std::string {
        if (nodes == 1)
            return "x";
        if (nodes % 2 == 0)
            return "+" + tree(nodes - 1);
        return "(" + tree(nodes / 2) + "+" + tree(nodes / 2) + ")";
    };
    for (int nodes : {4096, 4097}) {
        const auto source = tree(nodes);
        check(source.size() > 4096 && compile(source).limit == CompileLimit::SourceBytes,
              "4096/4097 node は source 上限が先に効く");
    }
    for (int i = 0; i < 1000; ++i)
        check(compile("sin((((-").status != CompileStatus::Success, "反復不正入力");
    check(compile("1").expression.canonical() == "graph-expression/1:003ff0000000000000",
          "literal vector");
    check(compile("x+1").expression.canonical() == "graph-expression/1:0601003ff0000000000000",
          "演算 vector");
    check(compile(" (x) + 1.0 ").expression.canonical() == compile("x+1").expression.canonical(),
          "正準構文");
    check(compile("(x+1)-1").expression.canonical() != compile("x").expression.canonical(),
          "代数簡約なし");
    SamplingRequest request{-5, 5, -5, 5, -5, 5, 640, 360, "f"};
    for (auto s : {"x^2", "sin(x)", "1/x", "sqrt(x)", "log(x)", "tan(x)", "exp(-x^2)"}) {
        auto c = compile(s);
        const auto a = sample(c.expression, request), b = sample(c.expression, request);
        check(a.status == SamplingStatus::Success && !a.geometry.segments.empty(), "代表曲線");
        check(a.geometry == b.geometry && a.diagnostics == b.diagnostics, "再評価決定性");
        for (const auto& segment : a.geometry.segments) {
            check(segment.size() > 1, "孤立点なし");
            for (const auto p : segment)
                check(std::isfinite(p.x) && std::isfinite(p.y) && p.x >= -5 && p.x <= 5 &&
                          p.y >= -5 && p.y <= 5,
                      "viewport clip");
        }
    }
    auto reciprocal = sample(compile("1/x").expression, request);
    check(reciprocal.geometry.segments.size() == 2 &&
              reciprocal.diagnostics.undefinedDivision > 0 &&
              reciprocal.diagnostics.jumpDiscardedEdges > 0,
          "極を接続しない");
    auto midpoint = request;
    midpoint.plotWidthPixels = .25;
    midpoint.domainMin = -1;
    midpoint.domainMax = 1;
    auto singular = sample(compile("1/x").expression, midpoint);
    check(singular.status == SamplingStatus::NoFiniteSamples &&
              singular.diagnostics.midpointFailures == 1,
          "中央の極を接続しない");
    check(sample(compile("100").expression, request).status == SamplingStatus::ValidEmptyCurve,
          "正常な空曲線");
    check(sample(compile("sqrt(-1)").expression, request).status == SamplingStatus::NoFiniteSamples,
          "全未定義");
    const auto line = sample(compile("x").expression, request).geometry;
    check(reveal(line, 0, 1).segments.empty(), "N1 frame0");
    check(reveal(line, 1, 1) == line && reveal(line, 3, 3) == line, "静止端点同一");
    for (auto frame : {2, 0, 1, 3}) {
        auto visible = reveal(line, frame, 3);
        if (frame == 0)
            check(visible.segments.empty(), "N3 frame0");
        else
            check(!visible.segments.empty() &&
                      visible.segments.back().back().x <= -5 + 10 * frame / 3.0 + 1e-12,
                  "任意順 x reveal");
    }
    request.plotWidthPixels = 8192;
    auto maximum = sample(compile("x").expression, request);
    check(maximum.status == SamplingStatus::Success && maximum.diagnostics.evaluations == 65537,
          "評価 budget 境界");
    request.plotWidthPixels = 8192.01;
    check(sample(compile("x").expression, request).status == SamplingStatus::SamplingBudgetExceeded,
          "budget 超過");
    request.plotWidthPixels = 0;
    check(sample(compile("x").expression, request).status == SamplingStatus::InvalidRequest,
          "寸法不正");
    SamplingRequest threshold{0, 1, 0, 1, 0, 8, .25, 16, "threshold"};
    check(sample(compile("x").expression, threshold).status == SamplingStatus::Success,
          "jump 閾値の等号は接続");
    const auto jumpRejected = sample(compile("1.001*x").expression, threshold);
    check(jumpRejected.diagnostics.jumpDiscardedEdges == 1 &&
              jumpRejected.geometry.segments.empty() &&
              jumpRejected.status == SamplingStatus::NoFiniteSamples,
          "jump 閾値超過で辺を接続しない");
    check(sample(compile("x^2").expression, threshold).status == SamplingStatus::Success,
          "中央偏差の等号は接続");
    threshold.plotHeightPixels = 16.001;
    check(sample(compile("x^2").expression, threshold).diagnostics.deviationDiscardedEdges == 1,
          "中央偏差閾値超過");
    SamplingRequest narrow{1e15, 1e15 + 1, 1e15, 1e15 + 1, 0, 2, 1, 360, "narrow"};
    check(sample(compile("x-1e15").expression, narrow).status == SamplingStatus::Success,
          "大きい x の狭い domain");
    narrow.domainMin = narrow.domainMax;
    check(sample(compile("x").expression, narrow).status == SamplingStatus::InvalidRequest,
          "domain 逆転/空");
    std::printf("検査 %d 件、失敗 %d 件\n", checks, failures);
    return failures ? 1 : 0;
}
