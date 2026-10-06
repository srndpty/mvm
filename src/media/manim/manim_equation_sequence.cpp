#include "media/manim/manim_equation_sequence.h"

#include "media/manim/manim_math_tex.h"
#include "media/manim/manim_scene.h"
#include "util/mvm_long_path.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace mvm::manim {
namespace {

using math::EquationBackendFailure;

// Equation Sequence の scene。kScriptPrelude の後に置く。
// - 各状態は mvm が分けた segment のまま MathTex(*segments) で作る。
// - structure の段階は構造の事実を structure.txt に書くだけで何も描かない。
// - render の段階は同じ報告を書いてから、区間ごとに Animation.interpolate(alpha) を直接呼び、
//   自前の Camera で 1 枚ずつ PNG にする (再生の履歴に依存しない直接の標本化)。
// - 変形の対は mvm の pairs・unmatched_* だけで組み、TransformMatchingTex を使わない。
// - action は区間の後の式を描き直して照合用に書く (base の式は copy で変更しない)。
//   action を変形より先に描くので、action が状態の object を変えれば、続く変形の frame 0 と
//   静止の照合でも見つかる。
constexpr char kEquationScene[] = R"PY(import logging

try:
    from manim import (ORIGIN, RIGHT, SMALL_BUFF, UP, WHITE, AnimationGroup, FadeIn, FadeOut,
                       Indicate, ReplacementTransform, ShowPassingFlash, SurroundingRectangle,
                       linear, logger, smooth)
    from manim.camera.camera import Camera
    from PIL import Image
except Exception as error:
    fail("other", "manim の Equation Sequence の部品を import できません: " + repr(error))


# Manim は部分の SVG group が見つからないと error を log に出し、式全体の group で代用する。
class FallbackLog(logging.Handler):
    def __init__(self):
        super().__init__(logging.ERROR)
        self.messages = []

    def emit(self, record):
        text = record.getMessage()
        if "Could not find SVG group" in text:
            self.messages.append(text)


FALLBACK_LOG = FallbackLog()
logger.addHandler(FALLBACK_LOG)
PHASE = REQUEST["phase"]


def build(state):
    try:
        return MathTex(*state["segments"], font_size=state["manim_font_size"])
    except ValueError as error:
        fail("latex" if "latex error" in str(error).lower() else "other", str(error))
    except Exception as error:
        fail("other", repr(error))


STATES = [build(state) for state in REQUEST["states"]]


def hex_text(text):
    return "x" + text.encode("utf-8").hex()


def point_bearing(mobject):
    return [item for item in mobject.get_family() if len(item.points) > 0]


def ids(items):
    return ",".join(format(id(item), "x") for item in items) or "-"


def buffers(items):
    return ",".join(format(item.points.__array_interface__["data"][0], "x")
                    for item in items) or "-"


LINES = ["fallback " + hex_text(message) for message in FALLBACK_LOG.messages]
for index, tex in enumerate(STATES):
    leaves = point_bearing(tex)
    LINES.append(" ".join(["state", str(index), str(len(tex.submobjects)), str(len(leaves)),
                           ids(leaves), buffers(leaves)]))
    for number, part in enumerate(tex.submobjects):
        text = getattr(part, "tex_string", None)
        owned = point_bearing(part)
        LINES.append(" ".join(["part", str(index), str(number), type(part).__name__,
                               "none" if text is None else hex_text(text),
                               str(len(part.submobjects)), str(len(part.get_family()) - 1),
                               str(len(owned)), ids(owned), buffers(owned)]))
(HERE / "structure.txt").write_text("\n".join(LINES) + "\n", encoding="utf-8")

# scene 自体の最後の 1 枚 (-s) は使わない。小さい canvas にしておく。
config.pixel_width = 16
config.pixel_height = 16
config.frame_width = 16 / PX_PER_UNIT
config.frame_height = 16 / PX_PER_UNIT
(HERE / "info.txt").write_text("16 16", encoding="utf-8")

if PHASE == "render":
    # 検証済みの構造で描く。mvm が検証した後でも、部分の数と代用を描く前に確かめる。
    for index, tex in enumerate(STATES):
        if FALLBACK_LOG.messages or len(tex.submobjects) != len(REQUEST["states"][index]["segments"]):
            fail("structure", "検証した構造と違う式は描きません")

OUT = HERE / "sequence"


def camera_for(size):
    width, height = size
    config.pixel_width = width
    config.pixel_height = height
    config.frame_width = width / PX_PER_UNIT
    config.frame_height = height / PX_PER_UNIT
    return Camera(background_opacity=0)


def placed(index, shift):
    tex = STATES[index].copy()
    tex.set_color(WHITE)
    tex.move_to(ORIGIN)
    tex.shift(RIGHT * (shift[0] / PX_PER_UNIT) + UP * (shift[1] / PX_PER_UNIT))
    return tex


def capture(camera, items, path):
    camera.reset()
    camera.capture_mobjects(items)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(camera.pixel_array).save(path)


def ratio(value):
    return value[0] / value[1]


class MvmEquationSequence(Scene):
    def construct(self):
        if PHASE != "render":
            return
        for index, state in enumerate(REQUEST["states"]):
            camera = camera_for(state["canvas_px"])
            capture(camera, [placed(index, state["manim_shift_px"])], OUT / f"s{index}.png")
        for number, item in enumerate(REQUEST["actions"]):
            camera = camera_for(item["canvas_px"])
            tex = placed(item["state"], item["shift_px"])
            folder = OUT / f"a{number}"
            target = tex[item["segment"]]
            if item["operation"] == "outline":
                capture(camera, [tex], folder / "base.png")
                shape = SurroundingRectangle(target, color=WHITE, buff=SMALL_BUFF)
                animation = ShowPassingFlash(shape, time_width=0.3, rate_func=smooth)
            elif item["operation"] == "pulse":
                others = [part for index, part in enumerate(tex.submobjects)
                          if index != item["segment"]]
                capture(camera, others, folder / "base.png")
                animation = Indicate(target.copy(), scale_factor=1.2, color=WHITE,
                                     rate_func=linear)
            else:
                fail("other", "未知の operation です")
            animation.begin()
            for frame, alpha in enumerate(item["alpha"]):
                animation.interpolate(ratio(alpha))
                capture(camera, [animation.mobject], folder / f"{frame:05d}.png")
            capture(camera, [tex], folder / "after.png")
        for number, item in enumerate(REQUEST["transitions"]):
            camera = camera_for(item["canvas_px"])
            source = placed(item["from"], item["source_shift_px"])
            target = placed(item["to"], item["target_shift_px"])
            animations = [ReplacementTransform(source[i], target[j], rate_func=smooth)
                          for i, j in item["pairs"]]
            animations += [FadeOut(source[i], rate_func=smooth) for i in item["unmatched_source"]]
            animations += [FadeIn(target[j], rate_func=smooth) for j in item["unmatched_target"]]
            group = AnimationGroup(*animations)
            group.begin()
            shown = [animation.mobject for animation in animations]
            for frame, alpha in enumerate(item["alpha"]):
                group.interpolate(ratio(alpha))
                capture(camera, shown, OUT / f"t{number}" / f"{frame:05d}.png")
)PY";

math::EquationSequenceRenderResult failed(math::MathRenderStatus status, std::string message,
                                          std::string log = {}) {
    math::EquationSequenceRenderResult result;
    result.status = status;
    result.message = std::move(message);
    result.log = std::move(log);
    return result;
}

math::EquationSequenceRenderResult invalid(EquationBackendFailure failure, std::string message,
                                           std::string log = {}, std::size_t state = 0,
                                           std::size_t segment = 0) {
    auto result = failed(math::MathRenderStatus::Failed, message, std::move(log));
    result.validation.failure = failure;
    result.validation.state = state;
    result.validation.segment = segment;
    result.validation.detail = std::move(message);
    return result;
}

void appendPair(std::string& json, const char* name, double first, double second) {
    json += '"';
    json += name;
    json += "\": [" + detail::jsonNumber(first) + ", " + detail::jsonNumber(second) + "]";
}

void appendIndices(std::string& json, const std::vector<std::size_t>& indices) {
    json += '[';
    for (std::size_t at = 0; at < indices.size(); ++at) {
        if (at > 0)
            json += ", ";
        json += std::to_string(indices[at]);
    }
    json += ']';
}

using ProgressFunction = bool (*)(std::int64_t, std::int64_t, std::int64_t&, std::int64_t&);

// frame ごとの alpha (整数の分子・分母)。withEnd なら照合用の進み具合 1 (N/N) を足す。
void appendAlpha(std::string& json, std::int64_t frames, ProgressFunction progress, bool withEnd) {
    json += "\"alpha\": [";
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        std::int64_t numerator = 0;
        std::int64_t denominator = 1;
        progress(frame, frames, numerator, denominator);
        if (frame > 0)
            json += ", ";
        json += '[' + std::to_string(numerator) + ", " + std::to_string(denominator) + ']';
    }
    if (withEnd)
        json += ", [" + std::to_string(frames) + ", " + std::to_string(frames) + ']';
    json += ']';
}

void appendShift(std::string& json, const char* name, const math::MathEndpointPlacement& at) {
    appendPair(json, name, at.shiftX, manimShiftUpFor(at.shiftY));
}

// 静止の大きさと canvas の中の左上 (raster の座標)。
void appendStatic(std::string& json, const std::string& prefix, int width, int height,
                  const math::MathEndpointPlacement& at) {
    appendPair(json, (prefix + "static_px").c_str(), width, height);
    json += ", ";
    appendPair(json, (prefix + "placement_px").c_str(), at.left, at.top);
}

// ---- structure.txt ----

struct ReportPart {
    std::string type;
    bool hasText = false;
    std::string text;
    long long children = -1;
    long long descendants = -1;
    long long pointBearing = -1;
    std::vector<std::string> ids;
    std::vector<std::string> buffers;
};

struct ReportState {
    long long parts = -1;
    long long leafCount = -1;
    std::vector<std::string> ids;
    std::vector<std::string> buffers;
    std::map<long long, ReportPart> partByIndex;
};

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> items;
    if (text == "-")
        return items;
    std::istringstream input(text);
    std::string item;
    while (std::getline(input, item, ','))
        items.push_back(item);
    return items;
}

bool hexToken(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool listToken(const std::string& text, std::vector<std::string>& items) {
    items = splitList(text);
    return std::all_of(items.begin(), items.end(), hexToken);
}

math::EquationBackendValidation validationFailure(EquationBackendFailure failure,
                                                  std::string detail, std::size_t state = 0,
                                                  std::size_t segment = 0) {
    math::EquationBackendValidation validation;
    validation.failure = failure;
    validation.detail = std::move(detail);
    validation.state = state;
    validation.segment = segment;
    return validation;
}

std::string at(std::size_t state, std::size_t segment) {
    return "状態 " + std::to_string(state) + " の segment " + std::to_string(segment);
}

// ---- 出力の検査 ----

std::filesystem::path sequenceDirectory(const std::filesystem::path& job) {
    return job / L"sequence";
}

std::wstring frameName(std::int64_t frame) {
    wchar_t name[32] = {};
    std::swprintf(name, std::size(name), L"%05lld.png", static_cast<long long>(frame));
    return name;
}

// directory 直下の PNG の数。260 文字を超える path でも数える (extended-length)。
std::size_t countPngs(const std::filesystem::path& directory) {
    std::size_t count = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator it(util::extendedLengthPath(directory), error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == L".png")
            ++count;
    }
    return count;
}

struct FrameCheck {
    EquationBackendFailure failure = EquationBackendFailure::None;
    std::string message;
    math::MathCoverage coverage;
};

FrameCheck loadCanvasFrame(const math::MathCoverageLoader& loader,
                           const std::filesystem::path& path, int width, int height,
                           const std::string& name) {
    FrameCheck check;
    std::string error;
    if (!loader(util::extendedLengthPath(path), check.coverage, error) ||
        !math::mathCoverageValid(check.coverage)) {
        check.failure = EquationBackendFailure::CorruptFrame;
        check.message = name + " を読めません: " + error;
        return check;
    }
    if (check.coverage.width != width || check.coverage.height != height) {
        check.failure = EquationBackendFailure::FrameSizeMismatch;
        check.message = name + " の大きさが canvas と違います (" +
                        std::to_string(check.coverage.width) + "x" +
                        std::to_string(check.coverage.height) + ")";
        return check;
    }
    // 縁に触れなければ外周は全て alpha 0 (透明な背景) でもある。
    if (math::mathRectTouchesEdge(math::mathCoverageBounds(check.coverage), width, height)) {
        check.failure = EquationBackendFailure::EdgeContact;
        check.message = name + " が一時的な canvas の縁に触れました (はみ出した可能性があります)";
    }
    return check;
}

} // namespace

bool planManimEquationSequence(const math::EquationSequenceRenderSpec& spec,
                               const std::vector<math::MathCoverage>& stateStatics,
                               ManimEquationPlan& plan, std::string& error) {
    if (!math::validateEquationSequenceRenderSpec(spec, error))
        return false;
    if (stateStatics.size() != spec.states.size()) {
        error = "状態の静止の数が状態の数と違います";
        return false;
    }
    constexpr int kMaximumSide = 1 << 20;
    for (const auto& coverage : stateStatics) {
        if (!math::mathCoverageValid(coverage) || coverage.width > kMaximumSide ||
            coverage.height > kMaximumSide) {
            error = "状態の静止の画像が不正です";
            return false;
        }
    }
    ManimEquationPlan result;
    const auto endpoint = [&](const math::MathCoverage& still, ManimEquationPlan::Endpoint& out) {
        out.staticWidth = still.width;
        out.staticHeight = still.height;
        out.canvasWidth = still.width + 2 * kEquationCanvasPadding;
        out.canvasHeight = still.height + 2 * kEquationCanvasPadding;
        return math::mathEndpointPlacement(still.width, still.height, out.canvasWidth,
                                           out.canvasHeight, out.placement);
    };
    for (const auto& still : stateStatics) {
        ManimEquationPlan::Endpoint state;
        if (!endpoint(still, state)) {
            error = "状態の静止を canvas に置けません";
            return false;
        }
        result.states.push_back(state);
    }
    for (const auto& item : spec.transitions) {
        if (item.frames > kMaximumEquationIntervalFrames) {
            error = "変形の枚数がこの backend の上限を超えています";
            return false;
        }
        const auto& source = stateStatics[item.fromState];
        const auto& target = stateStatics[item.toState];
        ManimEquationPlan::Transition transition;
        transition.sourceWidth = source.width;
        transition.sourceHeight = source.height;
        transition.targetWidth = target.width;
        transition.targetHeight = target.height;
        transition.canvasWidth = std::max(source.width, target.width) + 2 * kEquationCanvasPadding;
        transition.canvasHeight =
            std::max(source.height, target.height) + 2 * kEquationCanvasPadding;
        if (!math::mathTransformPlacement(source.width, source.height, target.width, target.height,
                                          transition.canvasWidth, transition.canvasHeight,
                                          transition.placement)) {
            error = "変形の端点を canvas に置けません";
            return false;
        }
        result.transitions.push_back(transition);
    }
    for (const auto& item : spec.actions) {
        if (item.duration > kMaximumEquationIntervalFrames) {
            error = "action の枚数がこの backend の上限を超えています";
            return false;
        }
        ManimEquationPlan::Endpoint action;
        if (!endpoint(stateStatics[item.state], action)) {
            error = "action の状態を canvas に置けません";
            return false;
        }
        result.actions.push_back(action);
    }
    plan = std::move(result);
    return true;
}

std::string manimEquationSequenceRequestJson(const math::EquationSequenceRenderSpec& spec,
                                             const ManimEquationPlan& plan, const char* phase) {
    std::string json = "{\"phase\": ";
    detail::appendJsonText(json, phase);
    json += ", \"states\": [";
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        const auto& state = spec.states[s];
        if (s > 0)
            json += ", ";
        json += "{\"segments\": [";
        for (std::size_t k = 0; k < state.segments.size(); ++k) {
            if (k > 0)
                json += ", ";
            detail::appendJsonText(json, state.segments[k].text);
        }
        json += "], \"manim_font_size\": " +
                detail::jsonNumber(manimFontSizeFor(state.still.fontSize)) + ", ";
        appendPair(json, "canvas_px", plan.states[s].canvasWidth, plan.states[s].canvasHeight);
        json += ", ";
        appendShift(json, "manim_shift_px", plan.states[s].placement);
        json += ", ";
        appendStatic(json, "", plan.states[s].staticWidth, plan.states[s].staticHeight,
                     plan.states[s].placement);
        json += '}';
    }
    json += "], \"transitions\": [";
    for (std::size_t t = 0; t < spec.transitions.size(); ++t) {
        const auto& item = spec.transitions[t];
        const auto& place = plan.transitions[t];
        if (t > 0)
            json += ", ";
        json += "{\"from\": " + std::to_string(item.fromState) +
                ", \"to\": " + std::to_string(item.toState) + ", ";
        appendPair(json, "canvas_px", place.canvasWidth, place.canvasHeight);
        json += ", ";
        appendShift(json, "source_shift_px", place.placement.source);
        json += ", ";
        appendShift(json, "target_shift_px", place.placement.target);
        json += ", ";
        appendStatic(json, "source_", place.sourceWidth, place.sourceHeight,
                     place.placement.source);
        json += ", ";
        appendStatic(json, "target_", place.targetWidth, place.targetHeight,
                     place.placement.target);
        json += ", \"pairs\": [";
        for (std::size_t at = 0; at < item.matching.pairs.size(); ++at) {
            if (at > 0)
                json += ", ";
            json += '[' + std::to_string(item.matching.pairs[at].source) + ", " +
                    std::to_string(item.matching.pairs[at].target) + ']';
        }
        json += "], \"unmatched_source\": ";
        appendIndices(json, item.matching.unmatchedSource);
        json += ", \"unmatched_target\": ";
        appendIndices(json, item.matching.unmatchedTarget);
        json += ", ";
        appendAlpha(json, item.frames, math::equationTransitionProgress, true);
        json += '}';
    }
    json += "], \"actions\": [";
    for (std::size_t a = 0; a < spec.actions.size(); ++a) {
        const auto& item = spec.actions[a];
        const auto& place = plan.actions[a];
        if (a > 0)
            json += ", ";
        json += "{\"state\": " + std::to_string(item.state) +
                ", \"segment\": " + std::to_string(item.segment) + ", \"operation\": ";
        detail::appendJsonText(json, math::equationRenderOperationName(item.operation));
        json += ", ";
        appendPair(json, "canvas_px", place.canvasWidth, place.canvasHeight);
        json += ", ";
        appendShift(json, "shift_px", place.placement);
        json += ", ";
        appendStatic(json, "", place.staticWidth, place.staticHeight, place.placement);
        json += ", ";
        appendAlpha(json, item.duration,
                    item.operation == math::EquationRenderOperation::Pulse
                        ? math::equationPulseWeight
                        : math::equationOutlineProgress,
                    false);
        json += '}';
    }
    json += "]}";
    return json;
}

std::string manimEquationSequenceScript() {
    return std::string(detail::manimScriptPrelude()) + kEquationScene;
}

math::EquationBackendValidation
checkManimEquationStructure(const std::string& report,
                            const math::EquationSequenceRenderSpec& spec) {
    std::map<long long, ReportState> states;
    std::vector<std::string> fallbacks;
    std::istringstream lines(report);
    std::string line;
    bool any = false;
    while (std::getline(lines, line)) {
        line = detail::trimText(line);
        if (line.empty())
            continue;
        any = true;
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        const auto malformed = [&] {
            return validationFailure(EquationBackendFailure::StructureReportMalformed,
                                     "Manim の構造の報告を読めません: " + line);
        };
        if (kind == "fallback") {
            std::string token;
            std::string message;
            fields >> token;
            if (!detail::decodeHexToken(token, message))
                message = "(log を読めません)";
            fallbacks.push_back(message);
        } else if (kind == "state") {
            long long index = -1;
            ReportState state;
            std::string ids;
            std::string buffers;
            std::string rest;
            if (!(fields >> index >> state.parts >> state.leafCount >> ids >> buffers) ||
                (fields >> rest) || index < 0 || state.parts < 0 || state.leafCount < 0 ||
                !listToken(ids, state.ids) || !listToken(buffers, state.buffers) ||
                static_cast<long long>(state.ids.size()) != state.leafCount ||
                state.buffers.size() != state.ids.size() || states.count(index))
                return malformed();
            states[index] = std::move(state);
        } else if (kind == "part") {
            long long stateIndex = -1;
            long long index = -1;
            ReportPart part;
            std::string text;
            std::string ids;
            std::string buffers;
            std::string rest;
            if (!(fields >> stateIndex >> index >> part.type >> text >> part.children >>
                  part.descendants >> part.pointBearing >> ids >> buffers) ||
                (fields >> rest) || stateIndex < 0 || index < 0 || part.children < 0 ||
                part.descendants < 0 || part.pointBearing < 0 || !listToken(ids, part.ids) ||
                !listToken(buffers, part.buffers) ||
                static_cast<long long>(part.ids.size()) != part.pointBearing ||
                part.buffers.size() != part.ids.size())
                return malformed();
            if (text != "none") {
                if (!detail::decodeHexToken(text, part.text))
                    return malformed();
                part.hasText = true;
            }
            const auto found = states.find(stateIndex);
            if (found == states.end() || found->second.partByIndex.count(index))
                return malformed();
            found->second.partByIndex[index] = std::move(part);
        } else {
            return malformed();
        }
    }
    if (!any)
        return validationFailure(EquationBackendFailure::StructureReportMissing,
                                 "Manim が部分の構造を報告しませんでした");
    if (!fallbacks.empty())
        return validationFailure(EquationBackendFailure::GroupingFallback,
                                 "Manim が式の部分を見つけられず、式全体で代用しました: " +
                                     fallbacks.front());
    if (states.size() != spec.states.size())
        return validationFailure(EquationBackendFailure::StructureReportMalformed,
                                 "Manim が報告した状態の数が要求と違います");

    math::EquationBackendValidation validation;
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        const auto found = states.find(static_cast<long long>(s));
        if (found == states.end())
            return validationFailure(EquationBackendFailure::StructureReportMalformed,
                                     "状態 " + std::to_string(s) + " の構造の報告がありません", s);
        const auto& state = found->second;
        const auto& segments = spec.states[s].segments;
        if (state.parts != static_cast<long long>(segments.size()))
            return validationFailure(EquationBackendFailure::SegmentCountMismatch,
                                     "状態 " + std::to_string(s) +
                                         " の top-level の部分の数が segment の数と違います "
                                         "(Manim " +
                                         std::to_string(state.parts) + "、mvm " +
                                         std::to_string(segments.size()) + ")",
                                     s);
        // 式全体の点を持つ object の並び。番号が所有の集合の正準な表し方になる。
        std::map<std::string, std::int64_t> ordinal;
        std::set<std::string> buffers;
        for (std::size_t at = 0; at < state.ids.size(); ++at) {
            if (!ordinal.emplace(state.ids[at], static_cast<std::int64_t>(at)).second)
                return validationFailure(
                    EquationBackendFailure::SharedDescendant,
                    "状態 " + std::to_string(s) + " の式の木に同じ object が 2 回現れます", s);
            if (!buffers.insert(state.buffers[at]).second)
                return validationFailure(
                    EquationBackendFailure::AliasedPointData,
                    "状態 " + std::to_string(s) + " の別の object が同じ点列を共有します", s);
        }
        std::vector<int> claimed(state.ids.size(), 0);
        for (std::size_t k = 0; k < segments.size(); ++k) {
            const auto part = state.partByIndex.find(static_cast<long long>(k));
            if (part == state.partByIndex.end())
                return validationFailure(EquationBackendFailure::MissingSegmentObject,
                                         at(s, k) + " に対応する Manim の部分がありません", s, k);
            const auto& p = part->second;
            if (p.type != "MathTexPart" || !p.hasText)
                return validationFailure(
                    EquationBackendFailure::SegmentTypeMismatch,
                    at(s, k) + " を Manim が部分として作りませんでした (" + p.type + ")", s, k);
            if (p.text != segments[k].text)
                return validationFailure(EquationBackendFailure::SegmentTextMismatch,
                                         at(s, k) + " の文字列が分けた部分と違います (Manim \"" +
                                             p.text + "\"、mvm \"" + segments[k].text + "\")",
                                         s, k);
            math::EquationSegmentOwnership ownership;
            ownership.state = s;
            ownership.segment = k;
            ownership.topLevelType = p.type;
            ownership.children = p.children;
            ownership.descendants = p.descendants;
            ownership.pointBearing = p.pointBearing;
            ownership.nonEmpty = p.pointBearing > 0;
            for (std::size_t at2 = 0; at2 < p.ids.size(); ++at2) {
                const auto index = ordinal.find(p.ids[at2]);
                if (index == ordinal.end())
                    return validationFailure(EquationBackendFailure::UnclaimedDescendant,
                                             at(s, k) + " の object が式全体の木にありません", s,
                                             k);
                if (claimed[static_cast<std::size_t>(index->second)]++)
                    return validationFailure(EquationBackendFailure::SharedDescendant,
                                             at(s, k) + " の object を別の handle も所有します", s,
                                             k);
                if (state.buffers[static_cast<std::size_t>(index->second)] != p.buffers[at2])
                    return validationFailure(EquationBackendFailure::AliasedPointData,
                                             at(s, k) + " の点列が式全体の木と違います", s, k);
                ownership.ownership.push_back(index->second);
            }
            std::sort(ownership.ownership.begin(), ownership.ownership.end());
            validation.segments.push_back(std::move(ownership));
        }
        if (state.partByIndex.size() != segments.size())
            return validationFailure(
                EquationBackendFailure::SegmentCountMismatch,
                "状態 " + std::to_string(s) + " に segment の数より多い部分が報告されました", s);
        for (std::size_t at2 = 0; at2 < claimed.size(); ++at2)
            if (!claimed[at2])
                return validationFailure(EquationBackendFailure::UnclaimedDescendant,
                                         "状態 " + std::to_string(s) +
                                             " にどの handle にも属さない glyph があります",
                                         s);
    }
    const auto ownershipOf = [&](std::size_t state,
                                 std::size_t segment) -> const math::EquationSegmentOwnership& {
        std::size_t offset = 0;
        for (std::size_t s = 0; s < state; ++s)
            offset += spec.states[s].segments.size();
        return validation.segments[offset + segment];
    };
    const auto withSegments = [&](math::EquationBackendValidation failure) {
        failure.segments = validation.segments;
        return failure;
    };
    for (const auto& action : spec.actions) {
        if (!ownershipOf(action.state, action.segment).nonEmpty)
            return withSegments(
                validationFailure(EquationBackendFailure::EmptyActionTarget,
                                  at(action.state, action.segment) +
                                      " は描画される glyph を持たないので強調できません",
                                  action.state, action.segment));
    }
    for (const auto& transition : spec.transitions) {
        for (const auto& pair : transition.matching.pairs) {
            const bool source = ownershipOf(transition.fromState, pair.source).nonEmpty;
            const bool target = ownershipOf(transition.toState, pair.target).nonEmpty;
            if (source != target)
                return withSegments(
                    validationFailure(EquationBackendFailure::EmptyTransitionHandle,
                                      at(source ? transition.toState : transition.fromState,
                                         source ? pair.target : pair.source) +
                                          " は glyph を持たず、対の相手は glyph を持ちます",
                                      source ? transition.toState : transition.fromState,
                                      source ? pair.target : pair.source));
        }
    }
    validation.failure = EquationBackendFailure::None;
    return validation;
}

math::EquationSequenceRenderResult
renderManimEquationSequence(const std::filesystem::path& manimExecutablePath,
                            const math::EquationSequenceRenderRequest& request,
                            const math::MathCoverageLoader& loader,
                            const std::atomic<bool>* cancel) {
    const auto& spec = request.spec;
    if (!loader)
        return invalid(EquationBackendFailure::InvalidRequest, "画像の loader がありません");
    ManimEquationPlan plan;
    std::string planError;
    if (!planManimEquationSequence(spec, request.stateStatics, plan, planError))
        return invalid(EquationBackendFailure::InvalidRequest,
                       "Equation Sequence の描画要求が不正です: " + planError);

    const auto script = manimEquationSequenceScript();
    const auto structurePath = request.jobDirectory / L"structure.txt";
    const auto output = sequenceDirectory(request.jobDirectory);
    std::error_code error;
    const auto runPhase = [&](const char* phase) {
        // 前の段階・前の実行の報告と出力を読まないよう、起動の前に消す。
        std::filesystem::remove(structurePath, error);
        util::removeTree(output, error);
        return detail::runManimScene(
            manimExecutablePath, request.jobDirectory, script, L"MvmEquationSequence",
            manimEquationSequenceRequestJson(spec, plan, phase), true, request.timeout, cancel);
    };
    const auto processFailure = [](const detail::ManimSceneRun& run) {
        return failed(run.status, run.message, run.log);
    };

    // 1. 構造だけを報告させ、描く前に検証する。
    const auto structureRun = runPhase("structure");
    if (structureRun.status != math::MathRenderStatus::Ok)
        return processFailure(structureRun);
    const auto first = checkManimEquationStructure(detail::readTextFile(structurePath), spec);
    if (!first.ready()) {
        auto result =
            invalid(first.failure, first.detail, structureRun.log, first.state, first.segment);
        result.validation = first;
        return result;
    }
    if (cancel && cancel->load())
        return failed(math::MathRenderStatus::Cancelled, "数式の描画を中断しました");

    // 2. 検証済みの構造で描く。報告は 1 と同じ所有でなければならない。
    const auto renderRun = runPhase("render");
    if (renderRun.status != math::MathRenderStatus::Ok)
        return processFailure(renderRun);
    const auto second = checkManimEquationStructure(detail::readTextFile(structurePath), spec);
    if (!second.ready() || second.segments != first.segments) {
        auto result = invalid(EquationBackendFailure::StructureChangedBetweenPhases,
                              "検証の段階と描画の段階で Manim の構造が違います", renderRun.log);
        result.validation.segments = first.segments;
        return result;
    }
    const std::string& log = renderRun.log;
    const auto cancelled = [&] { return cancel && cancel->load(); };

    // 状態: 通常の静止の描画と全画素で一致する (同じ配置の規則で置いたとき)。
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        if (cancelled())
            return failed(math::MathRenderStatus::Cancelled, "数式の描画を中断しました", log);
        const auto& place = plan.states[s];
        const std::string name = "状態 " + std::to_string(s) + " の描画";
        auto frame = loadCanvasFrame(loader, output / (L"s" + std::to_wstring(s) + L".png"),
                                     place.canvasWidth, place.canvasHeight, name);
        if (frame.failure != EquationBackendFailure::None)
            return invalid(frame.failure, frame.message, log, s);
        const auto different = math::mathEndpointDifference(
            frame.coverage, request.stateStatics[s], place.placement.left, place.placement.top);
        if (different != 0)
            return invalid(EquationBackendFailure::StaticMismatch,
                           name + "が通常の静止の描画と一致しません (違う画素 " +
                               std::to_string(different) + ")",
                           log, s);
    }

    math::EquationSequenceRenderResult result;
    for (std::size_t t = 0; t < spec.transitions.size(); ++t) {
        const auto& item = spec.transitions[t];
        const auto& place = plan.transitions[t];
        const auto folder = output / (L"t" + std::to_wstring(t));
        const std::string label = "変形 " + std::to_string(t);
        const auto expected = static_cast<std::size_t>(item.frames) + 1;
        if (countPngs(folder) != expected)
            return invalid(EquationBackendFailure::FrameCountMismatch,
                           label + " の PNG が " + std::to_string(expected) +
                               " 枚ではありません (件数=" + std::to_string(countPngs(folder)) + ")",
                           log);
        const auto& source = request.stateStatics[item.fromState];
        const auto& target = request.stateStatics[item.toState];
        math::EquationTransitionRaster raster;
        raster.placement = place.placement;
        raster.interval.canvasWidth = place.canvasWidth;
        raster.interval.canvasHeight = place.canvasHeight;
        raster.interval.artifact = math::mathRectUnion(
            {place.placement.source.left, place.placement.source.top, source.width, source.height},
            {place.placement.target.left, place.placement.target.top, target.width, target.height});
        for (std::size_t index = 0; index < expected; ++index) {
            if (cancelled())
                return failed(math::MathRenderStatus::Cancelled, "数式の描画を中断しました", log);
            const auto path = folder / frameName(static_cast<std::int64_t>(index));
            const std::string name = label + " の frame " + std::to_string(index);
            auto frame = loadCanvasFrame(loader, path, place.canvasWidth, place.canvasHeight, name);
            if (frame.failure != EquationBackendFailure::None)
                return invalid(frame.failure, frame.message, log);
            raster.interval.artifact = math::mathRectUnion(
                raster.interval.artifact, math::mathCoverageBounds(frame.coverage));
            const bool last = index + 1 == expected;
            if (index == 0 || last) {
                const auto& mask = last ? target : source;
                const auto& endpoint = last ? place.placement.target : place.placement.source;
                const auto different =
                    math::mathEndpointDifference(frame.coverage, mask, endpoint.left, endpoint.top);
                if (different != 0)
                    return invalid(EquationBackendFailure::EndpointMismatch,
                                   label +
                                       (last ? " の終状態が変形後" : " の最初の frame が変形前") +
                                       "の式の静止の描画と一致しません (違う画素 " +
                                       std::to_string(different) + ")",
                                   log);
            }
            if (!last)
                raster.interval.frames.push_back(path);
        }
        result.transitions.push_back(std::move(raster));
    }

    for (std::size_t a = 0; a < spec.actions.size(); ++a) {
        const auto& item = spec.actions[a];
        const auto& place = plan.actions[a];
        const auto folder = output / (L"a" + std::to_wstring(a));
        const std::string label = "action " + std::to_string(a);
        const auto& still = request.stateStatics[item.state];
        // base・N 枚・after。
        const auto expected = static_cast<std::size_t>(item.duration) + 2;
        if (countPngs(folder) != expected)
            return invalid(EquationBackendFailure::FrameCountMismatch,
                           label + " の PNG が " + std::to_string(expected) +
                               " 枚ではありません (件数=" + std::to_string(countPngs(folder)) + ")",
                           log);
        math::EquationActionRaster raster;
        raster.placement = place.placement;
        raster.base = folder / L"base.png";
        raster.interval.canvasWidth = place.canvasWidth;
        raster.interval.canvasHeight = place.canvasHeight;
        raster.interval.artifact = {place.placement.left, place.placement.top, still.width,
                                    still.height};
        auto base = loadCanvasFrame(loader, raster.base, place.canvasWidth, place.canvasHeight,
                                    label + " の base");
        if (base.failure != EquationBackendFailure::None)
            return invalid(base.failure, base.message, log);
        if (item.operation == math::EquationRenderOperation::Outline) {
            // outline の base は状態そのもの。
            const auto different = math::mathEndpointDifference(
                base.coverage, still, place.placement.left, place.placement.top);
            if (different != 0)
                return invalid(EquationBackendFailure::StaticMismatch,
                               label + " の base が状態の静止と一致しません (違う画素 " +
                                   std::to_string(different) + ")",
                               log, item.state);
        }
        raster.interval.artifact =
            math::mathRectUnion(raster.interval.artifact, math::mathCoverageBounds(base.coverage));
        for (std::int64_t index = 0; index < item.duration; ++index) {
            if (cancelled())
                return failed(math::MathRenderStatus::Cancelled, "数式の描画を中断しました", log);
            const auto path = folder / frameName(index);
            auto frame = loadCanvasFrame(loader, path, place.canvasWidth, place.canvasHeight,
                                         label + " の frame " + std::to_string(index));
            if (frame.failure != EquationBackendFailure::None)
                return invalid(frame.failure, frame.message, log);
            raster.interval.artifact = math::mathRectUnion(
                raster.interval.artifact, math::mathCoverageBounds(frame.coverage));
            raster.interval.frames.push_back(path);
        }
        auto after = loadCanvasFrame(loader, folder / L"after.png", place.canvasWidth,
                                     place.canvasHeight, label + " の後");
        if (after.failure != EquationBackendFailure::None)
            return invalid(after.failure, after.message, log);
        const auto different = math::mathEndpointDifference(
            after.coverage, still, place.placement.left, place.placement.top);
        if (different != 0)
            return invalid(EquationBackendFailure::ActionMutatedState,
                           label + " の後の式が状態の静止と一致しません (違う画素 " +
                               std::to_string(different) + ")",
                           log, item.state);
        result.actions.push_back(std::move(raster));
    }

    result.status = math::MathRenderStatus::Ok;
    result.validation = first;
    result.log = log;
    return result;
}

} // namespace mvm::manim
