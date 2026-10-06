#include "math_equation_sequence_artifact.h"

#include "app/math_clip_render.h"
#include "util/mvm_atomic_write.h"
#include "util/mvm_long_path.h"
#include "util/mvm_sha256.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace mvm::app {
namespace {

using math::EquationBackendFailure;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

bool writeAtomically(const std::filesystem::path& path, const std::string& bytes,
                     std::string& error) {
    char buffer[512] = {};
    const int code = mvm_atomic_write_file(path.c_str(), bytes.data(), bytes.size(), buffer,
                                           sizeof(buffer));
    error = buffer;
    return code == 0;
}

std::string sha256Hex(const void* data, std::size_t size) {
    char hex[MVM_SHA256_HEX_SIZE] = {};
    if (mvm_sha256_hex(data, size, hex) != 0)
        return {};
    return hex;
}

bool readExactly(const std::filesystem::path& path, std::uintmax_t expected,
                 std::vector<std::uint8_t>& bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size != expected)
        return false;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes.resize(static_cast<std::size_t>(expected));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::uintmax_t>(input.gcount()) == expected;
}

std::string argbText(std::uint32_t argb) {
    char text[16] = {};
    std::snprintf(text, sizeof text, "#%08X", static_cast<unsigned>(argb));
    return text;
}

std::string frameName(std::int64_t index) {
    char name[32] = {};
    std::snprintf(name, sizeof name, "%05lld.a8", static_cast<long long>(index));
    return name;
}

std::string transitionFrameName(std::size_t transition, std::int64_t index) {
    return "t" + std::to_string(transition) + "/" + frameName(index);
}

std::string actionFrameName(std::size_t action, std::int64_t index) {
    return "a" + std::to_string(action) + "/" + frameName(index);
}

std::string actionBaseName(std::size_t action) {
    return "a" + std::to_string(action) + "/base.a8";
}

// ---- 合成の色 (spec から mvm が決める。provenance に記録して照合する) ----

struct ExpectedColors {
    std::vector<std::vector<std::uint32_t>> transitions;
    std::vector<std::uint32_t> actionBase;
    std::vector<std::vector<std::uint32_t>> actionAccent;
};

bool expectedColors(const math::EquationSequenceRenderSpec& spec, ExpectedColors& colors) {
    for (const auto& item : spec.transitions) {
        std::vector<std::uint32_t> frames;
        for (std::int64_t i = 0; i < item.frames; ++i) {
            std::uint32_t argb = 0;
            if (!math::mathTransformColorAt(spec.states[item.fromState].foregroundArgb,
                                            spec.states[item.toState].foregroundArgb, i,
                                            item.frames, argb))
                return false;
            frames.push_back(argb);
        }
        colors.transitions.push_back(std::move(frames));
    }
    for (const auto& item : spec.actions) {
        const auto foreground = spec.states[item.state].foregroundArgb;
        colors.actionBase.push_back(foreground);
        std::vector<std::uint32_t> frames;
        for (std::int64_t i = 0; i < item.duration; ++i) {
            if (item.operation == math::EquationRenderOperation::Outline) {
                frames.push_back(math::kEquationActionAccentArgb);
                continue;
            }
            std::int64_t numerator = 0;
            std::int64_t denominator = 1;
            std::uint32_t argb = 0;
            if (!math::equationPulseWeight(i, item.duration, numerator, denominator) ||
                !math::mathTransformColorAt(foreground, math::kEquationActionAccentArgb,
                                            numerator, denominator, argb))
                return false;
            frames.push_back(argb);
        }
        colors.actionAccent.push_back(std::move(frames));
    }
    return true;
}

// ---- provenance ----

struct FrameRecord {
    std::uintmax_t bytes = 0;
    std::string sha;
};

// 区間 1 つ。transition は (ax, ay) = source、(bx, by) = target の切り出した座標での左上。
// action は (ax, ay) = 状態の静止の左上。
struct IntervalRecord {
    int canvasWidth = 0;
    int canvasHeight = 0;
    math::MathRect rect;
    int ax = 0;
    int ay = 0;
    int bx = 0;
    int by = 0;
    FrameRecord base; // action だけ
    std::vector<FrameRecord> frames;
};

struct Provenance {
    std::vector<math::EquationSegmentOwnership> ownership;
    std::vector<IntervalRecord> transitions;
    std::vector<IntervalRecord> actions;
};

std::string rectText(const math::MathRect& rect) {
    return std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
           std::to_string(rect.width) + "," + std::to_string(rect.height);
}

std::string pairText(int a, char separator, int b) {
    return std::to_string(a) + separator + std::to_string(b);
}

std::string setText(const std::vector<std::int64_t>& set) {
    if (set.empty())
        return "-";
    std::string text;
    for (std::size_t at = 0; at < set.size(); ++at)
        text += (at > 0 ? "," : "") + std::to_string(set[at]);
    return text;
}

std::string frameLine(const char* kind, const std::string& name, const FrameRecord& frame,
                      const char* role, std::uint32_t color) {
    std::string line = std::string(kind) + " path=" + name + " bytes=" +
                       std::to_string(frame.bytes) + " sha256=" + frame.sha;
    if (role)
        line += std::string(" role=") + role;
    return line + " color=" + argbText(color) + "\n";
}

// identity (key・template・版・静止の key と大きさ・色・区間の構成・toolchain) は job から、
// 幾何・frame の byte 数と SHA-256・所有は p から組む。読むときは p だけを file から取り、
// 組み直した全文と byte 単位で比べる。
std::string provenanceText(const EquationSequenceJob& job, const ExpectedColors& colors,
                           const Provenance& p) {
    const auto& spec = job.spec;
    std::string text = std::string(kEquationSequenceArtifactFormat) + "\n";
    text += "key=" + job.key + "\n";
    text += "template=" + job.backend.equationSequenceTemplate + "\n";
    text += "compiler=" + spec.compilerVersion + "\n";
    text += std::string("raster=") + math::kEquationSequenceRasterVersion + "\n";
    text += std::string("progress=") + math::kEquationSequenceProgressVersion + "\n";
    text += "accent=" + argbText(math::kEquationActionAccentArgb) + "\n";
    text += "states=" + std::to_string(spec.states.size()) + "\n";
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        const auto& still = job.stateStatics[s];
        text += "state index=" + std::to_string(s) + " static_key=" + job.stateStaticKeys[s] +
                " static=" + pairText(still.width, 'x', still.height) +
                " foreground=" + argbText(spec.states[s].foregroundArgb) + "\n";
    }
    for (const auto& o : p.ownership)
        text += "ownership state=" + std::to_string(o.state) +
                " segment=" + std::to_string(o.segment) + " type=" + o.topLevelType +
                " children=" + std::to_string(o.children) +
                " descendants=" + std::to_string(o.descendants) +
                " point_bearing=" + std::to_string(o.pointBearing) +
                " set=" + setText(o.ownership) + "\n";
    text += "transitions=" + std::to_string(spec.transitions.size()) + "\n";
    for (std::size_t t = 0; t < spec.transitions.size() && t < p.transitions.size(); ++t) {
        const auto& item = spec.transitions[t];
        const auto& r = p.transitions[t];
        text += "transition index=" + std::to_string(t) +
                " from=" + std::to_string(item.fromState) + " to=" + std::to_string(item.toState) +
                " frames=" + std::to_string(item.frames) +
                " size=" + pairText(r.rect.width, 'x', r.rect.height) +
                " canvas=" + pairText(r.canvasWidth, 'x', r.canvasHeight) +
                " rect=" + rectText(r.rect) + " source=" + pairText(r.ax, ',', r.ay) +
                " target=" + pairText(r.bx, ',', r.by) + "\n";
        for (std::size_t i = 0; i < r.frames.size(); ++i)
            text += frameLine("frame", transitionFrameName(t, static_cast<std::int64_t>(i)),
                              r.frames[i], nullptr,
                              i < colors.transitions[t].size() ? colors.transitions[t][i] : 0);
    }
    text += "actions=" + std::to_string(spec.actions.size()) + "\n";
    for (std::size_t a = 0; a < spec.actions.size() && a < p.actions.size(); ++a) {
        const auto& item = spec.actions[a];
        const auto& r = p.actions[a];
        text += "action index=" + std::to_string(a) + " state=" + std::to_string(item.state) +
                " segment=" + std::to_string(item.segment) +
                " operation=" + math::equationRenderOperationName(item.operation) +
                " start=" + std::to_string(item.start) +
                " frames=" + std::to_string(item.duration) +
                " size=" + pairText(r.rect.width, 'x', r.rect.height) +
                " canvas=" + pairText(r.canvasWidth, 'x', r.canvasHeight) +
                " rect=" + rectText(r.rect) + " static=" + pairText(r.ax, ',', r.ay) + "\n";
        text += frameLine("layer", actionBaseName(a), r.base, "base", colors.actionBase[a]);
        for (std::size_t i = 0; i < r.frames.size(); ++i)
            text += frameLine("frame", actionFrameName(a, static_cast<std::int64_t>(i)),
                              r.frames[i], "accent",
                              i < colors.actionAccent[a].size() ? colors.actionAccent[a][i] : 0);
    }
    text += "toolchain:\n" + job.backend.fingerprint.canonical;
    return text;
}

std::map<std::string, std::string> tokens(const std::string& line) {
    std::map<std::string, std::string> result;
    std::istringstream fields(line);
    std::string field;
    fields >> field; // 種類
    while (fields >> field) {
        const auto equals = field.find('=');
        if (equals == std::string::npos)
            return {};
        result[field.substr(0, equals)] = field.substr(equals + 1);
    }
    return result;
}

bool readInts(const std::string& text, char separator, std::size_t count,
              std::vector<long long>& out) {
    out.clear();
    std::istringstream values(text);
    std::string value;
    while (std::getline(values, value, separator)) {
        try {
            std::size_t used = 0;
            out.push_back(std::stoll(value, &used));
            if (used != value.size())
                return false;
        } catch (...) {
            return false;
        }
    }
    return out.size() == count;
}

bool readIntsInto(const std::map<std::string, std::string>& map, const char* name, char separator,
                  std::initializer_list<int*> out) {
    const auto found = map.find(name);
    std::vector<long long> numbers;
    if (found == map.end() || !readInts(found->second, separator, out.size(), numbers))
        return false;
    std::size_t at = 0;
    for (int* target : out) {
        if (numbers[at] < -(1LL << 30) || numbers[at] > (1LL << 30))
            return false;
        *target = static_cast<int>(numbers[at++]);
    }
    return true;
}

bool readFrame(const std::map<std::string, std::string>& map, FrameRecord& frame) {
    const auto bytes = map.find("bytes");
    const auto sha = map.find("sha256");
    std::vector<long long> numbers;
    if (bytes == map.end() || sha == map.end() || !readInts(bytes->second, ',', 1, numbers) ||
        numbers[0] < 0)
        return false;
    frame.bytes = static_cast<std::uintmax_t>(numbers[0]);
    frame.sha = sha->second;
    return true;
}

// file から幾何・frame・所有だけを取り出す。形の厳密さは組み直した全文の比較で確かめる。
bool parseProvenance(const std::string& text, Provenance& p) {
    std::istringstream lines(text);
    std::string line;
    enum class Section { Header, Transitions, Actions } section = Section::Header;
    while (std::getline(lines, line)) {
        if (line == "toolchain:")
            return true;
        const auto kind = line.substr(0, line.find(' '));
        const auto map = tokens(line);
        if (line.rfind("transitions=", 0) == 0) {
            section = Section::Transitions;
        } else if (line.rfind("actions=", 0) == 0) {
            section = Section::Actions;
        } else if (kind == "ownership") {
            math::EquationSegmentOwnership o;
            int state = 0;
            int segment = 0;
            int children = 0;
            int descendants = 0;
            int pointBearing = 0;
            const auto type = map.find("type");
            const auto set = map.find("set");
            if (type == map.end() || set == map.end() ||
                !readIntsInto(map, "state", ',', {&state}) ||
                !readIntsInto(map, "segment", ',', {&segment}) ||
                !readIntsInto(map, "children", ',', {&children}) ||
                !readIntsInto(map, "descendants", ',', {&descendants}) ||
                !readIntsInto(map, "point_bearing", ',', {&pointBearing}) || state < 0 ||
                segment < 0 || children < 0 || descendants < 0 || pointBearing < 0)
                return false;
            o.state = static_cast<std::size_t>(state);
            o.segment = static_cast<std::size_t>(segment);
            o.topLevelType = type->second;
            o.children = children;
            o.descendants = descendants;
            o.pointBearing = pointBearing;
            o.nonEmpty = pointBearing > 0;
            if (set->second != "-") {
                std::istringstream items(set->second);
                std::string item;
                while (std::getline(items, item, ',')) {
                    std::vector<long long> number;
                    if (!readInts(item, ',', 1, number) || number[0] < 0)
                        return false;
                    o.ownership.push_back(number[0]);
                }
            }
            p.ownership.push_back(std::move(o));
        } else if (kind == "transition" && section == Section::Transitions) {
            IntervalRecord r;
            if (!readIntsInto(map, "canvas", 'x', {&r.canvasWidth, &r.canvasHeight}) ||
                !readIntsInto(map, "rect", ',', {&r.rect.x, &r.rect.y, &r.rect.width, &r.rect.height}) ||
                !readIntsInto(map, "source", ',', {&r.ax, &r.ay}) ||
                !readIntsInto(map, "target", ',', {&r.bx, &r.by}))
                return false;
            p.transitions.push_back(std::move(r));
        } else if (kind == "action" && section == Section::Actions) {
            IntervalRecord r;
            if (!readIntsInto(map, "canvas", 'x', {&r.canvasWidth, &r.canvasHeight}) ||
                !readIntsInto(map, "rect", ',', {&r.rect.x, &r.rect.y, &r.rect.width, &r.rect.height}) ||
                !readIntsInto(map, "static", ',', {&r.ax, &r.ay}))
                return false;
            p.actions.push_back(std::move(r));
        } else if (kind == "layer" && section == Section::Actions && !p.actions.empty()) {
            if (!readFrame(map, p.actions.back().base))
                return false;
        } else if (kind == "frame") {
            auto* records = section == Section::Transitions ? &p.transitions
                            : section == Section::Actions   ? &p.actions
                                                            : nullptr;
            FrameRecord frame;
            if (!records || records->empty() || !readFrame(map, frame))
                return false;
            records->back().frames.push_back(std::move(frame));
        }
    }
    return false; // toolchain の節が無い
}

bool containsRect(int width, int height, int x, int y, int innerWidth, int innerHeight) {
    return x >= 0 && y >= 0 && innerWidth > 0 && innerHeight > 0 &&
           static_cast<long long>(x) + innerWidth <= width &&
           static_cast<long long>(y) + innerHeight <= height;
}

bool frameBytesMatch(const IntervalRecord& r) {
    const auto bytes = static_cast<std::uintmax_t>(r.rect.width) *
                       static_cast<std::uintmax_t>(r.rect.height);
    return std::all_of(r.frames.begin(), r.frames.end(),
                       [&](const FrameRecord& f) { return f.bytes == bytes; });
}

// 数値どうしが矛盾しないか。端点の位置は、静止と canvas の大きさから中立な配置の規則
// (mathTransformPlacement / mathEndpointPlacement) で決まる値を切り出した座標へ移したもの。
bool geometryValid(const EquationSequenceJob& job, const Provenance& p) {
    const auto& spec = job.spec;
    if (p.transitions.size() != spec.transitions.size() || p.actions.size() != spec.actions.size())
        return false;
    for (std::size_t t = 0; t < spec.transitions.size(); ++t) {
        const auto& item = spec.transitions[t];
        const auto& r = p.transitions[t];
        const auto& source = job.stateStatics[item.fromState];
        const auto& target = job.stateStatics[item.toState];
        math::MathTransformPlacement placement;
        if (static_cast<std::int64_t>(r.frames.size()) != item.frames ||
            !math::mathTransformPlacement(source.width, source.height, target.width,
                                          target.height, r.canvasWidth, r.canvasHeight,
                                          placement) ||
            r.ax != placement.source.left - r.rect.x || r.ay != placement.source.top - r.rect.y ||
            r.bx != placement.target.left - r.rect.x || r.by != placement.target.top - r.rect.y ||
            !containsRect(r.canvasWidth, r.canvasHeight, r.rect.x, r.rect.y, r.rect.width,
                          r.rect.height) ||
            !containsRect(r.rect.width, r.rect.height, r.ax, r.ay, source.width, source.height) ||
            !containsRect(r.rect.width, r.rect.height, r.bx, r.by, target.width, target.height) ||
            !frameBytesMatch(r))
            return false;
    }
    for (std::size_t a = 0; a < spec.actions.size(); ++a) {
        const auto& item = spec.actions[a];
        const auto& r = p.actions[a];
        const auto& still = job.stateStatics[item.state];
        math::MathEndpointPlacement placement;
        const auto bytes = static_cast<std::uintmax_t>(r.rect.width) *
                           static_cast<std::uintmax_t>(r.rect.height);
        if (static_cast<std::int64_t>(r.frames.size()) != item.duration ||
            !math::mathEndpointPlacement(still.width, still.height, r.canvasWidth,
                                         r.canvasHeight, placement) ||
            r.ax != placement.left - r.rect.x || r.ay != placement.top - r.rect.y ||
            !containsRect(r.canvasWidth, r.canvasHeight, r.rect.x, r.rect.y, r.rect.width,
                          r.rect.height) ||
            !containsRect(r.rect.width, r.rect.height, r.ax, r.ay, still.width, still.height) ||
            r.base.bytes != bytes || !frameBytesMatch(r))
            return false;
    }
    return true;
}

// 所有の集合が状態ごとに交わらず、segment の数と順に合い、action の対象が空でないか。
bool ownershipValid(const math::EquationSequenceRenderSpec& spec,
                    const std::vector<math::EquationSegmentOwnership>& ownership) {
    std::size_t at = 0;
    std::vector<std::size_t> offsets;
    for (std::size_t s = 0; s < spec.states.size(); ++s) {
        offsets.push_back(at);
        std::set<std::int64_t> claimed;
        for (std::size_t k = 0; k < spec.states[s].segments.size(); ++k, ++at) {
            if (at >= ownership.size())
                return false;
            const auto& o = ownership[at];
            if (o.state != s || o.segment != k ||
                static_cast<std::int64_t>(o.ownership.size()) != o.pointBearing ||
                o.nonEmpty != (o.pointBearing > 0))
                return false;
            for (const auto item : o.ownership)
                if (!claimed.insert(item).second)
                    return false;
        }
    }
    if (at != ownership.size())
        return false;
    return std::all_of(spec.actions.begin(), spec.actions.end(), [&](const auto& action) {
        return ownership[offsets[action.state] + action.segment].nonEmpty;
    });
}

EquationSequenceArtifact artifactFrom(const EquationSequenceJob& job, const ExpectedColors& colors,
                                      const Provenance& p) {
    EquationSequenceArtifact artifact;
    const auto root = equationSequenceDirectory(job.directory, job.key);
    artifact.stateStaticKeys = job.stateStaticKeys;
    artifact.ownership = p.ownership;
    for (std::size_t t = 0; t < p.transitions.size(); ++t) {
        const auto& r = p.transitions[t];
        EquationTransitionArtifact item;
        item.width = r.rect.width;
        item.height = r.rect.height;
        item.sourceX = r.ax;
        item.sourceY = r.ay;
        item.targetX = r.bx;
        item.targetY = r.by;
        for (std::size_t i = 0; i < r.frames.size(); ++i)
            item.frames.push_back(
                {root / transitionFrameName(t, static_cast<std::int64_t>(i)), r.frames[i].sha,
                 colors.transitions[t][i]});
        artifact.transitions.push_back(std::move(item));
    }
    for (std::size_t a = 0; a < p.actions.size(); ++a) {
        const auto& r = p.actions[a];
        EquationActionArtifact item;
        item.operation = job.spec.actions[a].operation;
        item.width = r.rect.width;
        item.height = r.rect.height;
        item.staticX = r.ax;
        item.staticY = r.ay;
        item.base = {root / actionBaseName(a), r.base.sha, colors.actionBase[a]};
        for (std::size_t i = 0; i < r.frames.size(); ++i)
            item.accent.push_back({root / actionFrameName(a, static_cast<std::int64_t>(i)),
                                   r.frames[i].sha, colors.actionAccent[a][i]});
        artifact.actions.push_back(std::move(item));
    }
    return artifact;
}

void removeArtifact(const std::filesystem::path& directory, const std::string& key) {
    std::error_code error;
    std::filesystem::remove(equationSequenceProvenancePath(directory, key), error);
    util::removeTree(equationSequenceDirectory(directory, key), error);
}

// cache directory を変える操作 (消す・確定する) を、取消と排他に行う。
bool underGate(const EquationSequenceJob& job, const std::atomic<bool>* cancel,
               const std::function<void()>& action) {
    std::lock_guard lock(*job.publishGate);
    if (cancel->load())
        return false;
    action();
    return true;
}

bool jobShapeValid(const EquationSequenceJob& job) {
    std::string error;
    return math::validateEquationSequenceRenderSpec(job.spec, error) &&
           job.stateStatics.size() == job.spec.states.size() &&
           job.stateStaticKeys.size() == job.spec.states.size() &&
           std::all_of(job.stateStatics.begin(), job.stateStatics.end(),
                       [](const math::MathCoverage& c) { return math::mathCoverageValid(c); });
}

EquationSequenceOutcome cancelledOutcome() {
    EquationSequenceOutcome outcome;
    outcome.cancelled = true;
    outcome.status = math::MathRenderStatus::Cancelled;
    return outcome;
}

EquationSequenceOutcome failedOutcome(math::MathRenderStatus status, EquationBackendFailure failure,
                                      std::string message, std::string log = {}) {
    EquationSequenceOutcome outcome;
    outcome.status = status;
    outcome.backendFailure = failure;
    outcome.message = std::move(message);
    outcome.log = std::move(log);
    return outcome;
}

} // namespace

std::filesystem::path equationSequenceProvenancePath(const std::filesystem::path& directory,
                                                     const std::string& key) {
    return directory / L"equation-sequence" / (key + ".txt");
}

std::filesystem::path equationSequenceDirectory(const std::filesystem::path& directory,
                                                const std::string& key) {
    return directory / L"equation-sequence" / key;
}

bool loadEquationArtifactFrame(const EquationArtifactFrame& frame, int width, int height,
                               std::vector<std::uint8_t>& coverage, std::string& error) {
    if (width <= 0 || height <= 0) {
        error = "Equation Sequence の frame の大きさが不正です";
        return false;
    }
    const auto bytes = static_cast<std::uintmax_t>(width) * static_cast<std::uintmax_t>(height);
    std::vector<std::uint8_t> read;
    if (!readExactly(frame.path, bytes, read)) {
        error = "Equation Sequence の frame を読めないか、大きさが provenance と違います";
        return false;
    }
    if (sha256Hex(read.data(), read.size()) != frame.sha256) {
        error = "Equation Sequence の frame の中身が provenance と違います";
        return false;
    }
    coverage = std::move(read);
    return true;
}

EquationSequenceDiskLoad loadEquationSequenceArtifact(const EquationSequenceJob& job,
                                                      const std::atomic<bool>* cancel,
                                                      EquationSequenceArtifact& artifact,
                                                      bool removeInvalid) {
    const std::string text = readFile(equationSequenceProvenancePath(job.directory, job.key));
    if (text.empty())
        return EquationSequenceDiskLoad::Missing; // 確定の印が無い
    ExpectedColors colors;
    Provenance parsed;
    bool valid = jobShapeValid(job) && expectedColors(job.spec, colors) &&
                 parseProvenance(text, parsed) && geometryValid(job, parsed) &&
                 ownershipValid(job.spec, parsed.ownership) &&
                 provenanceText(job, colors, parsed) == text;
    if (valid) {
        artifact = artifactFrom(job, colors, parsed);
        std::vector<std::uint8_t> bytes;
        std::string error;
        for (std::size_t t = 0; valid && t < artifact.transitions.size(); ++t) {
            const auto& item = artifact.transitions[t];
            for (std::size_t i = 0; valid && i < item.frames.size(); ++i) {
                if (cancel->load())
                    return EquationSequenceDiskLoad::Cancelled;
                valid = loadEquationArtifactFrame(item.frames[i], item.width, item.height, bytes,
                                                  error);
                // frame 0 は今の前の状態の静止と、記録した位置で全画素一致する。
                if (valid && i == 0)
                    valid = math::mathEndpointDifference(
                                {item.width, item.height, bytes},
                                job.stateStatics[job.spec.transitions[t].fromState], item.sourceX,
                                item.sourceY) == 0;
            }
        }
        for (std::size_t a = 0; valid && a < artifact.actions.size(); ++a) {
            const auto& item = artifact.actions[a];
            if (cancel->load())
                return EquationSequenceDiskLoad::Cancelled;
            valid = loadEquationArtifactFrame(item.base, item.width, item.height, bytes, error);
            // outline の base は状態の静止そのもの。
            if (valid && item.operation == math::EquationRenderOperation::Outline)
                valid = math::mathEndpointDifference({item.width, item.height, bytes},
                                                     job.stateStatics[job.spec.actions[a].state],
                                                     item.staticX, item.staticY) == 0;
            for (std::size_t i = 0; valid && i < item.accent.size(); ++i)
                valid = loadEquationArtifactFrame(item.accent[i], item.width, item.height, bytes,
                                                  error);
        }
    }
    if (valid)
        return EquationSequenceDiskLoad::Ready;
    if (removeInvalid && !underGate(job, cancel, [&] { removeArtifact(job.directory, job.key); }))
        return EquationSequenceDiskLoad::Cancelled;
    return EquationSequenceDiskLoad::Missing;
}

EquationSequenceOutcome renderEquationSequenceJob(const EquationSequenceJob& job,
                                                  const std::atomic<bool>* cancel) {
    if (!jobShapeValid(job))
        return failedOutcome(math::MathRenderStatus::Failed, EquationBackendFailure::InvalidRequest,
                             "Equation Sequence の描画要求が不正です");
    ExpectedColors colors;
    if (!expectedColors(job.spec, colors))
        return failedOutcome(math::MathRenderStatus::Failed, EquationBackendFailure::InvalidRequest,
                             "Equation Sequence の合成の色を決められません");
    {
        EquationSequenceArtifact artifact;
        switch (loadEquationSequenceArtifact(job, cancel, artifact)) {
        case EquationSequenceDiskLoad::Ready: {
            EquationSequenceOutcome outcome;
            outcome.ready = true;
            outcome.status = math::MathRenderStatus::Ok;
            outcome.backendFailure = EquationBackendFailure::None;
            outcome.artifact = std::move(artifact);
            return outcome;
        }
        case EquationSequenceDiskLoad::Cancelled:
            return cancelledOutcome();
        case EquationSequenceDiskLoad::Missing:
            break;
        }
    }
    if (cancel->load())
        return cancelledOutcome();
    if (!job.backend.renderEquationSequence)
        return failedOutcome(math::MathRenderStatus::Failed, EquationBackendFailure::InvalidRequest,
                             "数式の描画 backend は Equation Sequence を描けません");

    math::EquationSequenceRenderRequest request;
    request.spec = job.spec;
    request.stateStatics = job.stateStatics;
    request.timeout = job.timeout;
    request.jobDirectory =
        job.jobs / (job.key + "-equation-sequence-" + std::to_string(job.ticket));
    std::error_code error;
    util::removeTree(request.jobDirectory, error);
    std::filesystem::create_directories(request.jobDirectory, error);
    if (error)
        return failedOutcome(math::MathRenderStatus::Failed, EquationBackendFailure::InvalidRequest,
                             "数式の作業 directory を作成できません: " + error.message());
    const auto rendered = job.backend.renderEquationSequence(request, loadMathCoverage, cancel);
    const auto cleanup = [&] {
        std::error_code ignored;
        util::removeTree(request.jobDirectory, ignored);
    };
    if (rendered.status == math::MathRenderStatus::Cancelled || cancel->load()) {
        cleanup();
        return cancelledOutcome();
    }
    if (rendered.status != math::MathRenderStatus::Ok) {
        cleanup();
        return failedOutcome(rendered.status, rendered.validation.failure, rendered.message,
                             rendered.log);
    }
    // backend の成功の報告だけを信じない。構造検証と区間の数・枚数・幾何を照らし直す。
    if (!rendered.validation.ready() || !ownershipValid(job.spec, rendered.validation.segments)) {
        cleanup();
        return failedOutcome(math::MathRenderStatus::Failed,
                             rendered.validation.ready() ? EquationBackendFailure::InvalidRequest
                                                         : rendered.validation.failure,
                             "backend の構造検証が完了していません", rendered.log);
    }
    Provenance p;
    p.ownership = rendered.validation.segments;
    bool shape = rendered.transitions.size() == job.spec.transitions.size() &&
                 rendered.actions.size() == job.spec.actions.size();
    const auto intervalRecord = [](const math::EquationIntervalRaster& interval) {
        IntervalRecord r;
        r.canvasWidth = interval.canvasWidth;
        r.canvasHeight = interval.canvasHeight;
        r.rect = interval.artifact;
        const auto bytes = static_cast<std::uintmax_t>(std::max(interval.artifact.width, 0)) *
                           static_cast<std::uintmax_t>(std::max(interval.artifact.height, 0));
        r.frames.assign(interval.frames.size(), FrameRecord{bytes, std::string(64, '0')});
        r.base = {bytes, std::string(64, '0')};
        return r;
    };
    for (std::size_t t = 0; shape && t < rendered.transitions.size(); ++t) {
        const auto& item = rendered.transitions[t];
        auto r = intervalRecord(item.interval);
        r.ax = item.placement.source.left - r.rect.x;
        r.ay = item.placement.source.top - r.rect.y;
        r.bx = item.placement.target.left - r.rect.x;
        r.by = item.placement.target.top - r.rect.y;
        p.transitions.push_back(std::move(r));
    }
    for (std::size_t a = 0; shape && a < rendered.actions.size(); ++a) {
        const auto& item = rendered.actions[a];
        auto r = intervalRecord(item.interval);
        r.ax = item.placement.left - r.rect.x;
        r.ay = item.placement.top - r.rect.y;
        p.actions.push_back(std::move(r));
    }
    if (!shape || !geometryValid(job, p)) {
        cleanup();
        return failedOutcome(math::MathRenderStatus::Failed,
                             EquationBackendFailure::FrameCountMismatch,
                             "Equation Sequence の区間の数・枚数・artifact の矩形・端点の配置が"
                             "要求と合いません",
                             rendered.log);
    }

    // 古い provenance を消し、frame の directory を空にしてから書く。
    const auto root = equationSequenceDirectory(job.directory, job.key);
    const auto provenancePath = equationSequenceProvenancePath(job.directory, job.key);
    if (!underGate(job, cancel, [&] {
            std::filesystem::remove(provenancePath, error);
            util::removeTree(root, error);
            if (!error)
                std::filesystem::create_directories(root, error);
        })) {
        cleanup();
        return cancelledOutcome();
    }
    const auto fail = [&](EquationBackendFailure failure, const std::string& message) {
        cleanup();
        underGate(job, cancel, [&] { removeArtifact(job.directory, job.key); });
        return failedOutcome(math::MathRenderStatus::Failed, failure, message, rendered.log);
    };
    if (error)
        return fail(EquationBackendFailure::InvalidRequest,
                    "Equation Sequence を cache へ保存できません: " + error.message());

    // canvas の 1 枚を読み、artifact の矩形で切り出す。
    const auto cropFrame = [&](const std::filesystem::path& canvasPath, const IntervalRecord& r,
                               const std::string& name, std::vector<std::uint8_t>& cropped,
                               std::string& message) -> EquationBackendFailure {
        math::MathCoverage canvas;
        std::string loadError;
        if (!loadMathCoverage(canvasPath, canvas, loadError)) {
            message = name + " を読めません: " + loadError;
            return EquationBackendFailure::CorruptFrame;
        }
        if (canvas.width != r.canvasWidth || canvas.height != r.canvasHeight) {
            message = name + " の大きさが canvas と違います";
            return EquationBackendFailure::FrameSizeMismatch;
        }
        const auto bounds = math::mathCoverageBounds(canvas);
        if (math::mathRectTouchesEdge(bounds, canvas.width, canvas.height)) {
            message = name + " が一時的な canvas の縁に触れました";
            return EquationBackendFailure::EdgeContact;
        }
        // artifact の矩形の外に被覆があれば、切り出すと画素を失う。黙って切らない。
        if (!bounds.empty() && math::mathRectUnion(bounds, r.rect) != r.rect) {
            message = name + " の式が artifact の矩形の外にあります";
            return EquationBackendFailure::EdgeContact;
        }
        cropped.assign(static_cast<std::size_t>(r.rect.width) *
                           static_cast<std::size_t>(r.rect.height),
                       0);
        for (int y = 0; y < r.rect.height; ++y)
            std::copy_n(canvas.alpha.begin() +
                            static_cast<std::ptrdiff_t>(r.rect.y + y) * canvas.width + r.rect.x,
                        r.rect.width,
                        cropped.begin() + static_cast<std::ptrdiff_t>(y) * r.rect.width);
        return EquationBackendFailure::None;
    };
    // 切り出して書く。
    const auto publishFrame = [&](const std::filesystem::path& canvasPath,
                                  const IntervalRecord& r, const std::string& name,
                                  FrameRecord& record, std::vector<std::uint8_t>& cropped,
                                  std::string& message) -> EquationBackendFailure {
        const auto failure = cropFrame(canvasPath, r, name, cropped, message);
        if (failure != EquationBackendFailure::None)
            return failure;
        record.sha = sha256Hex(cropped.data(), cropped.size());
        std::string writeError;
        if (record.sha.empty() ||
            !writeAtomically(root / name, std::string(cropped.begin(), cropped.end()),
                             writeError)) {
            message = name + " を cache へ保存できません: " + writeError;
            return EquationBackendFailure::CorruptFrame;
        }
        return EquationBackendFailure::None;
    };

    std::vector<std::uint8_t> cropped;
    std::string message;
    for (std::size_t t = 0; t < p.transitions.size(); ++t) {
        std::filesystem::create_directories(root / ("t" + std::to_string(t)), error);
        auto& r = p.transitions[t];
        for (std::size_t i = 0; i < r.frames.size(); ++i) {
            if (cancel->load()) {
                cleanup(); // provenance が無いので、書きかけの frame は使われない
                return cancelledOutcome();
            }
            const auto name = transitionFrameName(t, static_cast<std::int64_t>(i));
            const auto failure = publishFrame(rendered.transitions[t].interval.frames[i], r, name,
                                              r.frames[i], cropped, message);
            if (failure != EquationBackendFailure::None)
                return fail(failure, message);
            if (i == 0) {
                const auto different = math::mathEndpointDifference(
                    {r.rect.width, r.rect.height, cropped},
                    job.stateStatics[job.spec.transitions[t].fromState], r.ax, r.ay);
                if (different != 0)
                    return fail(EquationBackendFailure::EndpointMismatch,
                                "切り出した変形 " + std::to_string(t) +
                                    " の最初の frame が前の状態の静止と一致しません (違う画素 " +
                                    std::to_string(different) + ")");
            }
        }
    }
    for (std::size_t a = 0; a < p.actions.size(); ++a) {
        std::filesystem::create_directories(root / ("a" + std::to_string(a)), error);
        auto& r = p.actions[a];
        auto failure =
            publishFrame(rendered.actions[a].base, r, actionBaseName(a), r.base, cropped, message);
        if (failure != EquationBackendFailure::None)
            return fail(failure, message);
        if (job.spec.actions[a].operation == math::EquationRenderOperation::Outline) {
            const auto different = math::mathEndpointDifference(
                {r.rect.width, r.rect.height, cropped},
                job.stateStatics[job.spec.actions[a].state], r.ax, r.ay);
            if (different != 0)
                return fail(EquationBackendFailure::StaticMismatch,
                            "切り出した action " + std::to_string(a) +
                                " の base が状態の静止と一致しません");
        } else {
            // pulse の base は「対象を除いた状態」。backend が作業 directory に残した通常の大きさ・
            // 位置の対象の層を同じ矩形で切り出し、P3-4 と同じ合成規則で base の上に重ねると、
            // 状態の静止と全画素一致しなければならない。対象の層は artifact に保存しない。
            const std::string name = "action " + std::to_string(a) + " の通常の対象";
            if (rendered.actions[a].normalTarget.empty())
                return fail(EquationBackendFailure::PulseBaseMismatch,
                            name + "の層を backend が返しませんでした");
            std::vector<std::uint8_t> target;
            failure = cropFrame(rendered.actions[a].normalTarget, r, name, target, message);
            if (failure != EquationBackendFailure::None)
                return fail(failure, message);
            math::MathCoverage composed;
            const auto different =
                math::composeEquationCoverage({r.rect.width, r.rect.height, cropped},
                                              {r.rect.width, r.rect.height, target}, composed)
                    ? math::mathEndpointDifference(
                          composed, job.stateStatics[job.spec.actions[a].state], r.ax, r.ay)
                    : -1;
            if (different != 0)
                return fail(EquationBackendFailure::PulseBaseMismatch,
                            "切り出した action " + std::to_string(a) +
                                " の base と通常の対象を合成しても状態の静止と一致しません "
                                "(違う画素 " +
                                std::to_string(different) + ")");
        }
        for (std::size_t i = 0; i < r.frames.size(); ++i) {
            if (cancel->load()) {
                cleanup();
                return cancelledOutcome();
            }
            failure = publishFrame(rendered.actions[a].interval.frames[i], r,
                                   actionFrameName(a, static_cast<std::int64_t>(i)), r.frames[i],
                                   cropped, message);
            if (failure != EquationBackendFailure::None)
                return fail(failure, message);
        }
    }

    if (job.beforePublish)
        job.beforePublish(provenancePath);
    std::string writeError;
    bool written = false;
    if (!underGate(job, cancel, [&] {
            written = writeAtomically(provenancePath, provenanceText(job, colors, p), writeError);
        })) {
        cleanup();
        return cancelledOutcome();
    }
    if (!written)
        return fail(EquationBackendFailure::InvalidRequest,
                    "Equation Sequence の provenance を cache へ保存できません: " + writeError);
    cleanup();
    EquationSequenceOutcome outcome;
    outcome.ready = true;
    outcome.status = math::MathRenderStatus::Ok;
    outcome.backendFailure = EquationBackendFailure::None;
    outcome.log = rendered.log;
    outcome.artifact = artifactFrom(job, colors, p);
    return outcome;
}

} // namespace mvm::app
