#include "project/equation_sequence.h"

#include "project/project.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace mvm::project {
namespace {
bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool add(std::int64_t a, std::int64_t b, std::int64_t& out) {
    return a >= 0 && b >= 0 && !__builtin_add_overflow(a, b, &out);
}

const SemanticPart* part(const EquationState& state, const PartId& id) {
    for (const auto& item : state.parts)
        if (item.id == id)
            return &item;
    return nullptr;
}

bool byteBoundary(const std::string& source, std::int64_t offset) {
    return offset >= 0 && static_cast<std::uint64_t>(offset) <= source.size() &&
           (static_cast<std::uint64_t>(offset) == source.size() ||
            (static_cast<unsigned char>(source[static_cast<std::size_t>(offset)]) & 0xc0) != 0x80);
}

template<class Id>
bool unique(const Id& id, std::unordered_set<std::string>& ids) {
    return !id.value.empty() && ids.insert(id.value).second;
}

template<class Edit>
bool edit(EquationSequenceClipData& data, int height, std::string& error, Edit apply) {
    if (!validateEquationSequence(data, height, error))
        return false;
    auto candidate = data;
    if (!apply(candidate) || !validateEquationSequence(candidate, height, error))
        return false;
    data = std::move(candidate);
    return true;
}

std::optional<std::size_t> stateIndex(const EquationSequenceClipData& data, StateId id) {
    for (std::size_t i = 0; i < data.states.size(); ++i)
        if (data.states[i].id == id)
            return i;
    return std::nullopt;
}
} // namespace

bool equationIntervals(const EquationSequenceClipData& data,
                       std::vector<EquationInterval>& intervals, std::int64_t& length,
                       std::string& error) {
    intervals.clear();
    length = 0;
    if (data.states.empty() || data.transitions.size() != data.states.size() - 1)
        return fail(error, "数式 sequence の状態数と辺数が不正です");
    for (std::size_t i = 0; i < data.states.size(); ++i) {
        auto append = [&](bool transition, std::int64_t frames) {
            std::int64_t end = 0;
            if (frames < 1 || !add(length, frames, end))
                return false;
            intervals.push_back({transition, i, length, end});
            length = end;
            return true;
        };
        if (!append(false, data.states[i].holdFrames) ||
            (i < data.transitions.size() && !append(true, data.transitions[i].frames)))
            return fail(error, "数式 sequence の尺が 0 以下または overflow です");
    }
    return true;
}

bool validateEquationSequence(const EquationSequenceClipData& data, int height,
                              std::string& error) {
    error.clear();
    std::vector<EquationInterval> intervals;
    std::int64_t length = 0;
    if (!equationIntervals(data, intervals, length, error))
        return false;
    std::unordered_set<std::string> states, parts, transitions, actions;
    for (const auto& state : data.states) {
        if (!unique(state.id, states))
            return fail(error, "StateId が空または重複しています");
        if (!validateMathClipData(state.equation, height, error))
            return false;
        std::uint32_t background = 0;
        if (!parseArgbColor(state.equation.backgroundColor, background) || (background >> 24) != 0)
            return fail(error, "数式 sequence の背景は透明でなければなりません");
        if (state.revision.empty())
            return fail(error, "数式の revision が空です");
        std::vector<std::pair<std::int64_t, std::int64_t>> boundRanges;
        for (const auto& p : state.parts) {
            if (!unique(p.id, parts))
                return fail(error, "PartId が空または重複しています");
            const auto& b = p.binding;
            if (b.status != BindingStatus::Bound && b.status != BindingStatus::Invalid)
                return fail(error, "部分式の binding 状態が不正です");
            if (b.revision.empty() || b.begin < 0 || b.end <= b.begin)
                return fail(error, "部分式の範囲または revision が不正です");
            if (b.expectedText.empty() ||
                static_cast<std::uint64_t>(b.end - b.begin) != b.expectedText.size())
                return fail(error, "部分式の範囲長と expectedText が一致しません");
            // invalid は旧 revision の証人を保持する。新 source の範囲へ黙って移さない。
            if (b.status == BindingStatus::Bound) {
                if (b.revision != state.revision || !byteBoundary(state.equation.source, b.begin) ||
                    !byteBoundary(state.equation.source, b.end) ||
                    state.equation.source.substr(static_cast<std::size_t>(b.begin),
                                                 static_cast<std::size_t>(b.end - b.begin)) !=
                        b.expectedText)
                    return fail(error, "部分式の binding の証人が一致しません");
                boundRanges.emplace_back(b.begin, b.end);
            }
        }
        std::sort(boundRanges.begin(), boundRanges.end());
        for (std::size_t i = 1; i < boundRanges.size(); ++i)
            if (boundRanges[i].first < boundRanges[i - 1].second)
                return fail(error, "部分式の範囲が重複しています");
    }
    for (std::size_t i = 0; i < data.transitions.size(); ++i) {
        const auto& t = data.transitions[i];
        if (!unique(t.id, transitions))
            return fail(error, "TransitionId が空または重複しています");
        if (t.from != data.states[i].id || t.to != data.states[i + 1].id)
            return fail(error, "数式の辺が vector 順の隣接状態を結んでいません");
        std::unordered_set<std::string> from, to;
        for (const auto& pair : t.correspondence) {
            const auto* a = part(data.states[i], pair.from);
            const auto* b = part(data.states[i + 1], pair.to);
            if (!a || !b || a->binding.status != BindingStatus::Bound ||
                b->binding.status != BindingStatus::Bound || !from.insert(pair.from.value).second ||
                !to.insert(pair.to.value).second)
                return fail(error,
                            "部分式の対応は隣接状態の有効な部分式どうしの一対一に限定されます");
        }
    }
    std::unordered_map<std::string, std::vector<std::pair<std::int64_t, std::int64_t>>>
        actionRanges;
    std::unordered_map<std::string, std::string> missingOwners;
    for (const auto& a : data.actions) {
        if (!unique(a.id, actions))
            return fail(error, "ActionId が空または重複しています");
        const auto index = stateIndex(data, a.state);
        if (!index)
            return fail(error, "action の StateId が存在しません");
        std::int64_t end = 0;
        if (a.start < 0 || a.duration < 1 || !add(a.start, a.duration, end) ||
            end > data.states[*index].holdFrames)
            return fail(error, "action の区間が hold 内に収まりません");
        if (a.operation != EquationOperation::Outline && a.operation != EquationOperation::Pulse)
            return fail(error, "数式の operation は outline / pulse だけです");
        if (a.target.value.empty())
            return fail(error, "action の PartId が空です");
        const auto* target = part(data.states[*index], a.target);
        if (target ? a.targetStatus != EquationTargetStatus::Present
                   : (a.targetStatus != EquationTargetStatus::Missing ||
                      parts.contains(a.target.value)))
            return fail(error, "action の対象状態または明示的な missing が不正です");
        if (!target) {
            const auto [it, inserted] = missingOwners.emplace(a.target.value, a.state.value);
            if (!inserted && it->second != a.state.value)
                return fail(error, "欠落 PartId の所有状態が一致しません");
        }
        actionRanges[a.state.value].emplace_back(a.start, end);
    }
    for (auto& [id, ranges] : actionRanges) {
        std::sort(ranges.begin(), ranges.end());
        for (std::size_t i = 1; i < ranges.size(); ++i)
            if (ranges[i].first < ranges[i - 1].second)
                return fail(error, "初期 P3 では同時 action を指定できません");
    }
    return true;
}

std::optional<EquationEvaluation> evaluateEquationSequence(const EquationSequenceClipData& data,
                                                           std::int64_t frame, std::string& error) {
    if (!validateEquationSequence(data, std::numeric_limits<int>::max(), error))
        return std::nullopt;
    std::vector<EquationInterval> intervals;
    std::int64_t length = 0;
    if (!equationIntervals(data, intervals, length, error))
        return std::nullopt;
    if (frame < 0 || frame >= length) {
        fail(error, "数式 sequence の frame が範囲外です");
        return std::nullopt;
    }
    for (const auto& interval : intervals) {
        if (frame < interval.begin || frame >= interval.end)
            continue;
        EquationEvaluation result;
        result.transition = interval.transition;
        result.state = data.states[interval.index].id;
        result.localFrame = frame - interval.begin;
        result.frames = interval.end - interval.begin;
        if (result.transition) {
            result.toState = data.states[interval.index + 1].id;
            result.transitionId = data.transitions[interval.index].id;
            result.progressNumerator = result.localFrame;
            result.progressDenominator = result.frames;
        } else {
            for (const auto& action : data.actions) {
                std::int64_t end = 0;
                if (!add(action.start, action.duration, end)) {
                    fail(error, "action の終端が overflow です");
                    return std::nullopt;
                }
                if (action.state == result.state && result.localFrame >= action.start &&
                    result.localFrame < end)
                    result.activeAction = action.id;
            }
        }
        return result;
    }
    return std::nullopt;
}

bool remapEquationSequenceIds(EquationSequenceClipData& data,
                              const std::function<std::string()>& generate, std::string& error) {
    if (!validateEquationSequence(data, std::numeric_limits<int>::max(), error))
        return false;
    auto candidate = data;
    std::unordered_set<std::string> reserved;
    for (const auto& s : data.states) {
        reserved.insert(s.id.value);
        for (const auto& p : s.parts)
            reserved.insert(p.id.value);
    }
    for (const auto& t : data.transitions)
        reserved.insert(t.id.value);
    for (const auto& a : data.actions) {
        reserved.insert(a.id.value);
        reserved.insert(a.target.value);
    }
    auto fresh = [&]() -> std::optional<std::string> {
        if (!generate)
            return std::nullopt;
        for (int i = 0; i < 128; ++i) {
            auto id = generate();
            if (!id.empty() && reserved.insert(id).second)
                return id;
        }
        return std::nullopt;
    };
    std::unordered_map<std::string, std::string> states, parts;
    for (auto& s : candidate.states) {
        auto id = fresh();
        if (!id)
            return fail(error, "内部 ID を発行できません");
        states[s.id.value] = *id;
        s.id.value = *id;
        for (auto& p : s.parts) {
            id = fresh();
            if (!id)
                return fail(error, "内部 ID を発行できません");
            parts[p.id.value] = *id;
            p.id.value = *id;
        }
    }
    for (auto& t : candidate.transitions) {
        auto id = fresh();
        if (!id)
            return fail(error, "内部 ID を発行できません");
        t.id.value = *id;
        if (!states.contains(t.from.value) || !states.contains(t.to.value))
            return fail(error, "辺の状態が存在しません");
        t.from.value = states.at(t.from.value);
        t.to.value = states.at(t.to.value);
        for (auto& pair : t.correspondence) {
            if (!parts.contains(pair.from.value) || !parts.contains(pair.to.value))
                return fail(error, "対応の部分式が存在しません");
            pair.from.value = parts.at(pair.from.value);
            pair.to.value = parts.at(pair.to.value);
        }
    }
    for (auto& a : candidate.actions) {
        auto id = fresh();
        if (!id)
            return fail(error, "内部 ID を発行できません");
        a.id.value = *id;
        if (!states.contains(a.state.value))
            return fail(error, "action の状態が存在しません");
        a.state.value = states.at(a.state.value);
        if (!parts.contains(a.target.value)) {
            if (a.targetStatus != EquationTargetStatus::Missing)
                return fail(error, "action の対象が存在しません");
            id = fresh();
            if (!id)
                return fail(error, "欠落 ID を発行できません");
            parts[a.target.value] = *id;
        }
        a.target.value = parts.at(a.target.value);
    }
    data = std::move(candidate);
    return true;
}

bool insertEquationState(EquationSequenceClipData& data, std::size_t pos, EquationState state,
                         const std::vector<EquationStepTransition>& edges, int height,
                         std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        if (pos > c.states.size() || edges.size() != (pos > 0 && pos < c.states.size() ? 2u : 1u))
            return fail(error, "挿入する状態と新しい辺の数が不正です");
        if (pos > 0 && pos < c.states.size())
            c.transitions.erase(c.transitions.begin() + static_cast<std::ptrdiff_t>(pos - 1));
        c.states.insert(c.states.begin() + static_cast<std::ptrdiff_t>(pos), state);
        c.transitions.insert(c.transitions.begin() +
                                 static_cast<std::ptrdiff_t>(pos == 0 ? 0 : pos - 1),
                             edges.begin(), edges.end());
        return true;
    });
}

bool deleteEquationState(EquationSequenceClipData& data, StateId id,
                         const std::optional<EquationStepTransition>& edge, int height,
                         std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        const auto index = stateIndex(c, id);
        if (!index || c.states.size() == 1)
            return fail(error, "最後の状態または存在しない状態は削除できません");
        const auto i = *index;
        const bool middle = i > 0 && i + 1 < c.states.size();
        if (middle != edge.has_value() || (edge && !edge->correspondence.empty()))
            return fail(error, "新しい隣接辺は対応を持たず明示的に作成してください");
        std::erase_if(c.actions, [&](const auto& a) { return a.state == id; });
        std::erase_if(c.transitions, [&](const auto& t) { return t.from == id || t.to == id; });
        c.states.erase(c.states.begin() + static_cast<std::ptrdiff_t>(i));
        if (edge)
            c.transitions.insert(c.transitions.begin() + static_cast<std::ptrdiff_t>(i - 1), *edge);
        return true;
    });
}

bool changeEquationHold(EquationSequenceClipData& data, StateId id, std::int64_t frames, int height,
                        std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        const auto i = stateIndex(c, id);
        if (!i)
            return fail(error, "状態が存在しません");
        c.states[*i].holdFrames = frames;
        return true;
    });
}

bool changeEquationTransition(EquationSequenceClipData& data, TransitionId id, std::int64_t frames,
                              int height, std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        for (auto& t : c.transitions)
            if (t.id == id) {
                t.frames = frames;
                return true;
            }
        return fail(error, "辺が存在しません");
    });
}

bool replaceEquationSource(EquationSequenceClipData& data, StateId id, std::string source,
                           std::string revision, int height, std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        const auto i = stateIndex(c, id);
        if (!i)
            return fail(error, "状態が存在しません");
        auto& s = c.states[*i];
        s.equation.source = source;
        s.revision = revision;
        for (auto& p : s.parts)
            p.binding.status = BindingStatus::Invalid;
        for (auto& t : c.transitions)
            if (t.from == id || t.to == id)
                t.correspondence.clear();
        return true;
    });
}

bool deleteEquationPart(EquationSequenceClipData& data, StateId state, PartId id, int height,
                        std::string& error) {
    return edit(data, height, error, [&](auto& c) {
        const auto i = stateIndex(c, state);
        if (!i || !part(c.states[*i], id))
            return fail(error, "部分式が存在しません");
        std::erase_if(c.states[*i].parts, [&](const auto& p) { return p.id == id; });
        for (auto& a : c.actions)
            if (a.state == state && a.target == id)
                a.targetStatus = EquationTargetStatus::Missing;
        for (auto& t : c.transitions)
            std::erase_if(t.correspondence,
                          [&](const auto& p) { return p.from == id || p.to == id; });
        return true;
    });
}
} // namespace mvm::project
