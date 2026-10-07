#include "project/equation_binding_edit.h"

#include "core/text_offsets.h"

#include <algorithm>
#include <limits>

namespace mvm::project {
bool editEquationSourceTrusted(EquationSequenceClipData& data, StateId id,
                               const TrustedEquationEdit& delta, std::string revision, int height,
                               std::string& error) {
    if (!validateEquationSequence(data, height, error))
        return false;
    auto candidate = data;
    auto state = std::find_if(candidate.states.begin(), candidate.states.end(),
                              [&](const auto& s) { return s.id == id; });
    const auto begin = core::utf16ToUtf8Offset(delta.oldSource, delta.beginUtf16);
    const auto end = core::utf16ToUtf8Offset(delta.oldSource, delta.endUtf16);
    if (state == candidate.states.end() || state->equation.source != delta.oldSource || !begin ||
        !end || *begin > *end || !core::utf8ToUtf16Offset(delta.replacement, 0) ||
        delta.newSource !=
            delta.oldSource.substr(0, *begin) + delta.replacement + delta.oldSource.substr(*end) ||
        revision.empty() || revision == state->revision ||
        delta.newSource.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        error = "信頼済み編集の境界・旧式・新式・revision が一致しません";
        return false;
    }
    const auto shift = static_cast<std::int64_t>(delta.replacement.size()) -
                       static_cast<std::int64_t>(*end - *begin);
    for (auto& p : state->parts) {
        auto& b = p.binding;
        if (b.status != BindingStatus::Bound)
            continue;
        if (*end <= static_cast<std::size_t>(b.begin)) {
            b.begin += shift;
            b.end += shift;
        } else if (*begin < static_cast<std::size_t>(b.end)) {
            b.status = BindingStatus::Invalid;
            continue;
        }
        b.revision = revision;
    }
    state->equation.source = delta.newSource;
    state->revision = std::move(revision);
    // 無効になった参照だけを落とし、対応を自動で作り直さない。
    for (auto& t : candidate.transitions)
        std::erase_if(t.correspondence, [&](const auto& pair) {
            return std::any_of(state->parts.begin(), state->parts.end(), [&](const auto& p) {
                return p.binding.status == BindingStatus::Invalid &&
                       ((t.from == id && pair.from == p.id) || (t.to == id && pair.to == p.id));
            });
        });
    if (!validateEquationSequence(candidate, height, error))
        return false;
    data = std::move(candidate);
    return true;
}

bool rebindEquationPart(EquationSequenceClipData& data, StateId stateId, PartId partId,
                        SourceBinding binding, int height, std::string& error) {
    if (!validateEquationSequence(data, height, error))
        return false;
    auto candidate = data;
    for (auto& s : candidate.states) {
        if (s.id != stateId)
            continue;
        for (auto& p : s.parts) {
            if (p.id != partId)
                continue;
            if (!equationBindingMatchesSource(s, binding)) {
                error = "再 binding の revision・UTF-8 境界・証人が不正です";
                return false;
            }
            p.binding = std::move(binding);
            if (!validateEquationSequence(candidate, height, error))
                return false;
            data = std::move(candidate);
            return true;
        }
    }
    error = "再 binding の状態または PartId が存在しません";
    return false;
}
} // namespace mvm::project
