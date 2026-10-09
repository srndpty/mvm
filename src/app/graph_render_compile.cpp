#include "app/graph_render_compile.h"

#include "project/project.h"

#include <algorithm>

namespace mvm::app {
graph::SpecResult compileGraphRender(const project::GraphClipData& data, std::int64_t length,
                                     int width, int height) {
    using namespace graph;
    const auto validation = project::validateGraph(data, length);
    if (validation != project::GraphValidationStatus::Valid)
        return Error{validation == project::GraphValidationStatus::InvalidViewport
                         ? Failure::InvalidViewport
                         : Failure::InvalidGraph,
                     0, "Graph の構造が不正です"};
    if (width < 1 || height < 1 || width > Limits::dimension || height > Limits::dimension ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > Limits::pixels)
        return Error{Failure::ResourceLimit, 0, "canvas の上限を超えています"};
    GraphRenderSpec spec;
    spec.width = width;
    spec.height = height;
    spec.xMin = data.viewport.xMin;
    spec.xMax = data.viewport.xMax;
    spec.yMin = data.viewport.yMin;
    spec.yMax = data.viewport.yMax;
    spec.plot = {width / 8.0, height / 8.0, width * .75, height * .75};
    spec.drawFrames = data.intro.frames;
    const double scale = strokeScale(width, height);
    auto line = [&](Point a, Point b, std::uint32_t color, double stroke) {
        spec.background.push_back({{a, b}, color, stroke * scale});
    };
    if (data.axes.showGrid) {
        for (int i = 0; i <= 10; ++i) {
            const double x = spec.plot.left + spec.plot.width * (i / 10.0);
            const double y = spec.plot.top + spec.plot.height * (i / 10.0);
            line({x, spec.plot.top}, {x, spec.plot.top + spec.plot.height}, 0xFF333A44, 1);
            line({spec.plot.left, y}, {spec.plot.left + spec.plot.width, y}, 0xFF333A44, 1);
        }
    }
    if (data.axes.showAxes) {
        if (spec.yMin <= 0 && spec.yMax >= 0)
            line(mapPoint(spec, {spec.xMin, 0}), mapPoint(spec, {spec.xMax, 0}), 0xFFFFFFFF, 2);
        if (spec.xMin <= 0 && spec.xMax >= 0)
            line(mapPoint(spec, {0, spec.yMin}), mapPoint(spec, {0, spec.yMax}), 0xFFFFFFFF, 2);
    }
    spec.labels.push_back(
        {data.axes.xLabel, {spec.plot.left, height * .875, spec.plot.width, height * .125}, false});
    spec.labels.push_back(
        {data.axes.yLabel, {0, spec.plot.top, width * .125, spec.plot.height}, true});
    for (std::size_t i = 0; i < data.functions.size(); ++i) {
        const auto& f = data.functions[i];
        const auto compiled = compile(f.expression);
        if (compiled.status != CompileStatus::Success)
            return Error{compiled.status == CompileStatus::UnsupportedExpression
                             ? Failure::UnsupportedExpression
                             : Failure::InvalidExpression,
                         i, "Graph の式をコンパイルできません"};
        const auto sampled = sample(
            compiled.expression,
            {f.domainMin.value_or(spec.xMin), f.domainMax.value_or(spec.xMax), spec.xMin, spec.xMax,
             spec.yMin, spec.yMax, spec.plot.width, spec.plot.height, f.id.value});
        if (sampled.status != SamplingStatus::Success &&
            sampled.status != SamplingStatus::ValidEmptyCurve)
            return Error{sampled.status == SamplingStatus::NoFiniteSamples
                             ? Failure::NoFiniteSamples
                         : sampled.status == SamplingStatus::SamplingBudgetExceeded
                             ? Failure::ResourceLimit
                             : Failure::EvaluationFailure,
                         i, "Graph の点列を生成できません"};
        std::uint32_t color = 0;
        if (!project::parseArgbColor(f.color, color))
            return Error{Failure::InvalidGraph, i, "曲線の色が不正です"};
        spec.curves.push_back({compiled.expression.canonical(), sampled.geometry, color,
                               f.strokeWidth, f.strokeWidth * scale});
        const double bandWidth = spec.plot.width / static_cast<double>(data.functions.size());
        spec.labels.push_back(
            {f.label,
             {spec.plot.left + static_cast<double>(i) * bandWidth, 0, bandWidth, height * .125},
             false});
    }
    for (auto& label : spec.labels)
        label.content = labelContentRectangle(label.band);
    if (auto checked = validateSpec(spec); std::holds_alternative<Error>(checked))
        return std::get<Error>(checked);
    return spec;
}
} // namespace mvm::app
