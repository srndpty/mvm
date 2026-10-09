#include "project/graph_clip.h"

#include "project/project.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace mvm::project {
bool validGraphFunctionId(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

GraphValidationStatus validateGraph(const GraphClipData& data, std::int64_t length) {
    using S = GraphValidationStatus;
    const auto& v = data.viewport;
    if (!std::isfinite(v.xMin) || !std::isfinite(v.xMax) || !std::isfinite(v.yMin) ||
        !std::isfinite(v.yMax) || !(v.xMin < v.xMax) || !(v.yMin < v.yMax) ||
        !std::isfinite(v.xMax - v.xMin) || !std::isfinite(v.yMax - v.yMin))
        return S::InvalidViewport;
    if (length < 1 || data.functions.empty() || data.functions.size() > 3 ||
        data.axes.xLabel.size() > 4096 || data.axes.yLabel.size() > 4096)
        return S::InvalidGraph;
    std::unordered_set<std::string> ids;
    for (const auto& f : data.functions) {
        const double lo = f.domainMin.value_or(v.xMin), hi = f.domainMax.value_or(v.xMax);
        std::uint32_t color = 0;
        if (!validGraphFunctionId(f.id.value) || !ids.insert(f.id.value).second ||
            !std::isfinite(lo) || !std::isfinite(hi) || !(lo < hi) ||
            !(std::max(lo, v.xMin) < std::min(hi, v.xMax)) || !std::isfinite(f.strokeWidth) ||
            !(f.strokeWidth > 0 && f.strokeWidth <= 64) || f.expression.size() > 4096 ||
            f.label.size() > 4096 || !parseArgbColor(f.color, color))
            return S::InvalidGraph;
    }
    if (data.intro.kind == GraphIntroKind::None)
        return data.intro.frames == 0 ? S::Valid : S::InvalidGraph;
    if (data.intro.kind != GraphIntroKind::Draw || data.intro.frames < 1 ||
        data.intro.frames > 10000 || data.intro.frames > length)
        return S::InvalidGraph;
    return S::Valid;
}

GraphClipData defaultGraph(GraphFunctionId id) {
    GraphClipData data;
    GraphFunction f;
    f.id = std::move(id);
    data.functions.push_back(std::move(f));
    return data;
}

namespace {
template<class Edit>
bool change(GraphClipData& data, std::int64_t length, Edit edit) {
    if (validateGraph(data, length) != GraphValidationStatus::Valid)
        return false;
    auto candidate = data;
    if (!edit(candidate) || validateGraph(candidate, length) != GraphValidationStatus::Valid)
        return false;
    data = std::move(candidate);
    return true;
}

auto findFunction(GraphClipData& data, const GraphFunctionId& id) {
    return std::find_if(data.functions.begin(), data.functions.end(),
                        [&](const auto& f) { return f.id == id; });
}
} // namespace

bool addGraphFunction(GraphClipData& data, GraphFunction f, std::int64_t length) {
    return change(data, length, [&](auto& candidate) {
        candidate.functions.push_back(std::move(f));
        return true;
    });
}

bool deleteGraphFunction(GraphClipData& data, GraphFunctionId id, std::int64_t length) {
    return change(data, length, [&](auto& candidate) {
        auto it = findFunction(candidate, id);
        if (it == candidate.functions.end())
            return false;
        candidate.functions.erase(it);
        return true;
    });
}

bool moveGraphFunction(GraphClipData& data, GraphFunctionId id, std::size_t index,
                       std::int64_t length) {
    return change(data, length, [&](auto& candidate) {
        auto it = findFunction(candidate, id);
        if (it == candidate.functions.end() || index >= candidate.functions.size())
            return false;
        auto f = *it;
        candidate.functions.erase(it);
        candidate.functions.insert(candidate.functions.begin() + static_cast<std::ptrdiff_t>(index),
                                   std::move(f));
        return true;
    });
}

bool updateGraphFunction(GraphClipData& data, const GraphFunction& f, std::int64_t length) {
    return change(data, length, [&](auto& candidate) {
        auto it = findFunction(candidate, f.id);
        if (it == candidate.functions.end())
            return false;
        *it = f;
        return true;
    });
}

bool remapGraphIds(GraphClipData& data, const std::function<std::string()>& fresh,
                   std::string& error) {
    if (!fresh) {
        error = "Graph の ID 発行関数がありません";
        return false;
    }
    auto candidate = data;
    std::unordered_set<std::string> reserved;
    for (const auto& f : data.functions)
        reserved.insert(f.id.value);
    for (auto& f : candidate.functions) {
        bool found = false;
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto id = fresh();
            if (validGraphFunctionId(id) && reserved.insert(id).second) {
                f.id.value = std::move(id);
                found = true;
                break;
            }
        }
        if (!found) {
            error = "Graph の一意な ID を発行できません";
            return false;
        }
    }
    data = std::move(candidate);
    return true;
}
} // namespace mvm::project
