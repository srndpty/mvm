#include "app/equation_sequence_compile.h"

#include "core/text_offsets.h"

#include <algorithm>
#include <limits>
#include <string_view>

namespace mvm::app {
namespace {
bool letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 支持範囲を明示する字句検査。TeX の展開・任意 macro の引数仕様は推測しない。
bool safeRange(const std::string& s, std::size_t begin, std::size_t end) {
    int depth = 0, beginDepth = -1, endDepth = -1;
    auto split = [&](std::size_t a, std::size_t b) {
        return (a < begin && begin < b) || (a < end && end < b);
    };
    for (std::size_t i = 0; i <= s.size();) {
        if (i == begin)
            beginDepth = depth;
        if (i == end)
            endDepth = depth;
        if (i == s.size())
            break;
        auto next = i + 1;
        if (s[i] == '%') {
            next = s.find('\n', i);
            next = next == std::string::npos ? s.size() : next + 1;
            // comment 自体を semantic target に含めない。
            if (begin < next && i < end)
                return false;
        } else if (s[i] == '\\') {
            if (next == s.size())
                return false;
            if (letter(s[next])) {
                while (next < s.size() && letter(s[next]))
                    ++next;
                const auto token = std::string_view(s).substr(i, next - i);
                if (token == "\\begin" || token == "\\end" || token == "\\left" ||
                    token == "\\right")
                    return false;
                constexpr std::string_view known[] = {
                    "\\frac",  "\\sqrt",   "\\alpha", "\\beta", "\\gamma", "\\delta",
                    "\\Delta", "\\pi",     "\\theta", "\\pm",   "\\mp",    "\\cdot",
                    "\\times", "\\le",     "\\ge",    "\\leq",  "\\geq",   "\\ne",
                    "\\neq",   "\\approx", "\\equiv", "\\infty"};
                if (std::find(std::begin(known), std::end(known), token) == std::end(known))
                    return false;
                if (token == "\\frac" || token == "\\sqrt") {
                    auto arg = next;
                    const int count = token == "\\frac" ? 2 : 1;
                    for (int n = 0; n < count; ++n) {
                        while (arg < s.size() && space(s[arg]))
                            ++arg;
                        if (arg == s.size() || s[arg] != '{')
                            return false;
                        // 命令だけの target を作らない。引数内部の範囲は許可する。
                        if (begin <= i && end > i && end <= arg)
                            return false;
                        int nesting = 1;
                        for (++arg; arg < s.size() && nesting; ++arg) {
                            if (s[arg] == '\\') {
                                ++arg;
                                continue;
                            }
                            if (s[arg] == '{')
                                ++nesting;
                            if (s[arg] == '}')
                                --nesting;
                        }
                        if (nesting)
                            return false;
                    }
                }
            } else {
                // 数学 mode を切り替える escape などは初期契約に含めない。
                constexpr std::string_view supportedEscapes = "{}%_#$& ,;:!\\";
                if (supportedEscapes.find(s[next]) == std::string_view::npos)
                    return false;
                ++next;
            }
        } else if (s[i] == '{')
            ++depth;
        else if (s[i] == '}') {
            if (--depth < 0)
                return false;
        } else if (s[i] == '^' || s[i] == '_') {
            while (next < s.size() && space(s[next]))
                ++next;
            if (next == s.size())
                return false;
            if (s[next] != '{') {
                if (s[next] == '\\' || static_cast<unsigned char>(s[next]) >= 0x80)
                    return false;
                ++next;
            } else {
                if (begin <= i && end > i && end <= next)
                    return false;
                next = i + 1;
            }
        }
        if (split(i, next))
            return false;
        i = next;
    }
    if (depth != 0 || beginDepth < 0 || beginDepth != endDepth)
        return false;
    // 同じ深さだけでは }{ をまたぐ範囲を許してしまうため、内部の最低深さも検査する。
    int local = 0;
    for (std::size_t i = begin; i < end; ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '{')
            ++local;
        if (s[i] == '}' && --local < 0)
            return false;
    }
    return local == 0;
}

std::optional<std::size_t> handle(const EquationPartition& p, const project::PartId& id) {
    for (std::size_t i = 0; i < p.owners.size(); ++i)
        if (p.owners[i] && *p.owners[i] == id)
            return i;
    return std::nullopt;
}

bool emptyTarget(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return space(c) || c == '{' || c == '}'; });
}
} // namespace

EquationCompileResult<EquationPartition> compileEquationPartition(const project::EquationState& s) {
    using F = EquationCompileFailure;
    EquationPartition result;
    result.state = s.id;
    result.spec.equation = s.equation;
    result.spec.holdFrames = s.holdFrames;
    const auto& source = s.equation.source;
    if (!core::utf8ToUtf16Offset(source, 0))
        return {{}, F::InvalidBinding};
    std::vector<const project::SemanticPart*> parts;
    for (const auto& p : s.parts) {
        const auto& b = p.binding;
        if (!project::equationBindingMatchesSource(s, b))
            return {{}, F::InvalidBinding};
        if (p.id.value.empty() || std::any_of(parts.begin(), parts.end(),
                                              [&](const auto* old) { return old->id == p.id; }))
            return {{}, F::PartitionConflict};
        parts.push_back(&p);
    }
    std::sort(parts.begin(), parts.end(),
              [](auto* a, auto* b) { return a->binding.begin < b->binding.begin; });
    std::size_t position = 0;
    auto autoRegion = [&](std::size_t end) {
        for (const auto& segment : math::segmentMathTex(source.substr(position, end - position))) {
            const auto next = position + segment.text.size();
            result.spec.segments.push_back(
                {EquationSegmentKind::Auto, position, next, segment.text, segment.key,
                 emptyTarget(segment.text) ? EquationTargetProof::Empty
                                           : EquationTargetProof::BackendValidationRequired});
            result.owners.push_back(std::nullopt);
            position = next;
        }
    };
    for (auto* p : parts) {
        const auto begin = static_cast<std::size_t>(p->binding.begin),
                   end = static_cast<std::size_t>(p->binding.end);
        if (begin < position)
            return {{}, F::PartitionConflict};
        if (!safeRange(source, begin, end))
            return {{}, F::UnsupportedTexBoundary};
        if (emptyTarget(p->binding.expectedText))
            return {{}, F::UnsupportedEmptyTarget};
        autoRegion(begin);
        auto matchKey = p->binding.expectedText;
        matchKey = math::mathTexSegmentKey(matchKey);
        result.spec.segments.push_back({EquationSegmentKind::Semantic, begin, end,
                                        p->binding.expectedText, matchKey,
                                        EquationTargetProof::BackendValidationRequired});
        result.owners.push_back(p->id);
        position = end;
    }
    autoRegion(source.size());
    return {std::move(result), F::None};
}

EquationCompileResult<EquationTransitionSpec>
compileEquationTransition(const EquationPartition& from, const EquationPartition& to,
                          const project::EquationStepTransition& t) {
    using F = EquationCompileFailure;
    if (t.from != from.state || t.to != to.state || t.frames < 1)
        return {{}, F::InvalidCorrespondencePlan};
    EquationTransitionSpec result;
    result.frames = t.frames;
    std::vector<bool> usedFrom(from.spec.segments.size()), usedTo(to.spec.segments.size());
    for (const auto& pair : t.correspondence) {
        const auto a = handle(from, pair.from), b = handle(to, pair.to);
        if (!a || !b || usedFrom[*a] || usedTo[*b])
            return {{}, F::InvalidCorrespondencePlan};
        result.matching.pairs.push_back({*a, *b});
        usedFrom[*a] = usedTo[*b] = true;
    }
    std::vector<math::MathTexSegment> a, b;
    std::vector<std::size_t> ai, bi;
    auto remaining = [](const auto& p, const auto& used, auto& segments, auto& indices) {
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (used[i])
                continue;
            const auto& s = p.spec.segments[i];
            segments.push_back({s.text, s.key});
            indices.push_back(i);
        }
    };
    remaining(from, usedFrom, a, ai);
    remaining(to, usedTo, b, bi);
    const auto matching = math::matchMathTexSegments(a, b);
    for (const auto& pair : matching.pairs)
        result.matching.pairs.push_back({ai[pair.source], bi[pair.target]});
    for (auto i : matching.unmatchedSource)
        result.matching.unmatchedSource.push_back(ai[i]);
    for (auto i : matching.unmatchedTarget)
        result.matching.unmatchedTarget.push_back(bi[i]);
    std::sort(result.matching.pairs.begin(), result.matching.pairs.end(),
              [](const auto& x, const auto& y) { return x.source < y.source; });
    return {std::move(result), F::None};
}

EquationCompileResult<EquationActionPlan> compileEquationAction(const EquationPartition& p,
                                                                const project::EquationAction& a) {
    using F = EquationCompileFailure;
    const auto target = handle(p, a.target);
    if (a.targetStatus == project::EquationTargetStatus::Missing || a.state != p.state || !target)
        return {{}, F::MissingPart};
    if (a.start < 0 || a.duration < 1 || a.start > p.spec.holdFrames ||
        a.duration > p.spec.holdFrames - a.start ||
        (a.operation != project::EquationOperation::Outline &&
         a.operation != project::EquationOperation::Pulse))
        return {{}, F::InvalidSequence};
    const auto proof = p.spec.segments[*target].targetProof;
    if (proof == EquationTargetProof::Empty)
        return {{}, F::UnsupportedEmptyTarget};
    return {EquationActionPlan{
                a.state, a.target, {0, *target, a.start, a.duration, a.operation, proof}},
            F::None};
}

EquationCompileResult<EquationSequenceSpec>
compileEquationSequence(const project::EquationSequenceClipData& d) {
    using F = EquationCompileFailure;
    // 参照の診断を構造エラーへ潰さない。
    for (const auto& a : d.actions) {
        const auto s = std::find_if(d.states.begin(), d.states.end(),
                                    [&](const auto& state) { return state.id == a.state; });
        if (s == d.states.end() || a.targetStatus == project::EquationTargetStatus::Missing)
            return {{}, F::MissingPart};
        const auto p = std::find_if(s->parts.begin(), s->parts.end(),
                                    [&](const auto& part) { return part.id == a.target; });
        if (p == s->parts.end())
            return {{}, F::MissingPart};
        if (p->binding.status != project::BindingStatus::Bound)
            return {{}, F::InvalidBinding};
    }
    std::vector<EquationPartition> partitions;
    EquationSequenceSpec result;
    for (const auto& s : d.states) {
        auto compiled = compileEquationPartition(s);
        if (!compiled.value)
            return {{}, compiled.failure};
        result.states.push_back(compiled.value->spec);
        partitions.push_back(std::move(*compiled.value));
    }
    if (d.transitions.size() + 1 != partitions.size())
        return {{}, F::InvalidSequence};
    for (std::size_t i = 0; i < d.transitions.size(); ++i) {
        auto compiled =
            compileEquationTransition(partitions[i], partitions[i + 1], d.transitions[i]);
        if (!compiled.value)
            return {{}, compiled.failure};
        compiled.value->fromState = i;
        compiled.value->toState = i + 1;
        result.transitions.push_back(std::move(*compiled.value));
    }
    std::string error;
    if (!project::validateEquationSequence(d, std::numeric_limits<int>::max(), error))
        return {{}, F::InvalidSequence};
    for (const auto& a : d.actions) {
        const auto p = std::find_if(partitions.begin(), partitions.end(),
                                    [&](const auto& item) { return item.state == a.state; });
        auto compiled = compileEquationAction(*p, a);
        if (!compiled.value)
            return {{}, compiled.failure};
        compiled.value->spec.state = static_cast<std::size_t>(p - partitions.begin());
        result.actions.push_back(compiled.value->spec);
    }
    std::sort(result.actions.begin(), result.actions.end(), [](const auto& a, const auto& b) {
        return a.state < b.state || (a.state == b.state && a.start < b.start);
    });
    return {std::move(result), F::None};
}

EquationCompileFailure equationTargetReadiness(EquationTargetProof proof) {
    return proof == EquationTargetProof::Empty ? EquationCompileFailure::UnsupportedEmptyTarget
                                               : EquationCompileFailure::BackendValidationRequired;
}
} // namespace mvm::app
