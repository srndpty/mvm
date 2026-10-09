#include "project/graph_edit.h"
#include "project/project_json.h"

#include <cstdio>
#include <limits>
using namespace mvm::project;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "失敗: %s\n", message);
    }
}

std::string replace(std::string text, const std::string& from, const std::string& to) {
    auto pos = text.find(from);
    check(pos != std::string::npos, "変異対象の存在");
    if (pos != std::string::npos)
        text.replace(pos, from.size(), to);
    return text;
}
} // namespace

int main() {
    auto data = defaultGraph({"f1"});
    check(validateGraph(data, 300) == GraphValidationStatus::Valid, "既定 Graph");
    auto valid = [&](GraphClipData d) {
        return validateGraph(d, 300) == GraphValidationStatus::Valid;
    };
    auto bad = data;
    bad.functions.clear();
    check(!valid(bad), "0 関数");
    for (int count : {1, 3, 4}) {
        auto d = data;
        d.functions.clear();
        for (int i = 0; i < count; ++i) {
            auto f = data.functions[0];
            f.id.value = "f" + std::to_string(i);
            d.functions.push_back(f);
        }
        check(valid(d) == (count <= 3), "関数数境界");
    }
    for (auto id : {"", "f!", "日本語", "f.1"}) {
        bad = data;
        bad.functions[0].id.value = id;
        check(!valid(bad), "ID 不正");
    }
    bad = data;
    bad.functions.push_back(bad.functions[0]);
    check(!valid(bad), "ID 重複");
    bad = data;
    bad.functions[0].id.value = std::string(64, 'a');
    check(valid(bad), "ID 64 byte");
    bad.functions[0].id.value += 'a';
    check(!valid(bad), "ID 65 byte");
    bad = data;
    bad.functions[0].expression = std::string(4096, 'x');
    check(valid(bad), "source 構造 4096 byte");
    bad.functions[0].expression += 'x';
    check(!valid(bad), "source 構造 4097 byte");
    bad = data;
    bad.functions[0].label = std::string(4096, 'a');
    check(valid(bad), "label 4096 byte");
    bad.functions[0].label += 'a';
    check(!valid(bad), "label 4097 byte");
    bad = data;
    bad.axes.xLabel = std::string(4097, 'a');
    check(!valid(bad), "axis label 上限");
    bad = data;
    bad.functions[0].strokeWidth = 64;
    check(valid(bad), "線幅 64");
    bad = data;
    bad.functions[0].domainMin = std::numeric_limits<double>::infinity();
    check(!valid(bad), "domain 非有限");
    for (double width : {0.0, -1.0, 64.01, std::numeric_limits<double>::infinity()}) {
        bad = data;
        bad.functions[0].strokeWidth = width;
        check(!valid(bad), "線幅不正");
    }
    bad = data;
    bad.viewport.xMin = 5;
    check(!valid(bad), "viewport 順序");
    bad = data;
    bad.viewport.xMax = std::numeric_limits<double>::quiet_NaN();
    check(!valid(bad), "viewport 非有限");
    bad = data;
    bad.functions[0].domainMin = 6;
    bad.functions[0].domainMax = 7;
    check(!valid(bad), "domain 空交差");
    for (std::string expression : {"", "sin(", "未知", "__import__('os')"}) {
        bad = data;
        bad.functions[0].expression = expression;
        check(valid(bad), "編集可能な不正式");
    }
    bad = data;
    bad.intro.frames = 1;
    check(!valid(bad), "None frames");
    for (auto n : {0, 1, 300, 301, 10001}) {
        bad = data;
        bad.intro = {GraphIntroKind::Draw, n};
        check(valid(bad) == (n >= 1 && n <= 300), "Draw frames");
    }
    auto p = createDefaultProject();
    check(addGraph(p, "clip", {"f1"}, "グラフ", {TrackKind::Video, 0}, 0).success, "作成");
    check(p.timelineClips[0].sourceFrameCount ==
              defaultStillClipFrames(p.timelineFpsNum, p.timelineFpsDen),
          "既定 5 秒");
    const auto before = p;
    check(!editGraph(
               p, "clip",
               [](auto& g, auto&) {
                   g.functions.clear();
                   return true;
               }).success &&
              p == before,
          "拒否編集の原子性");
    check(editGraph(p, "clip",
                    [](auto& g, auto&) {
                        g.functions[0].expression = "sin(";
                        g.axes.xLabel = "横軸";
                        return true;
                    })
              .success,
          "不正式の編集");
    const auto id = p.timelineClips[0].graph.functions[0].id;
    check(id == GraphFunctionId{"f1"}, "編集 ID 保持");
    auto serialized = serializeProjectJson(p, "graph.mvm");
    auto loaded = parseProjectJsonText(serialized.json, "graph.mvm");
    check(serialized.success && loaded.success && loaded.project == p, "schema22 exact 往復");
    for (int fields = 0; fields < 4; ++fields) {
        auto optional = p;
        auto& g = optional.timelineClips[0].graph;
        if (fields & 1)
            g.functions[0].domainMin = -3;
        if (fields & 2)
            g.functions[0].domainMax = 3;
        g.axes.showAxes = false;
        g.axes.showGrid = false;
        g.axes.yLabel = "縦軸";
        g.functions[0].label = "関数";
        g.functions[0].color = "#00FF6655";
        auto json = serializeProjectJson(optional, "graph.mvm");
        auto round = parseProjectJsonText(json.json, "graph.mvm");
        check(json.success && round.success && round.project == optional,
              "省略/単独/両端 domain と透明色往復");
    }
    for (auto mutation : {std::pair{"\"x_min\":", "\"unknown\": 1, \"x_min\":"},
                          std::pair{"\"show_grid\":", "\"show_grid\": false, \"show_grid\":"},
                          std::pair{"\"stroke_width\":", "\"domain_min\": null, \"stroke_width\":"},
                          std::pair{"\"schema_version\": 22", "\"schema_version\": 21"},
                          std::pair{"\"schema_version\": 22", "\"schema_version\": 23"},
                          std::pair{"\"kind\": \"graph\"", "\"kind\": \"math\""}})
        check(!parseProjectJsonText(replace(serialized.json, mutation.first, mutation.second),
                                    "graph.mvm")
                   .success,
              "schema 負例");
    auto old = createDefaultProject();
    auto oldJson = serializeProjectJson(old, "old.mvm").json;
    loaded = parseProjectJsonText(
        replace(oldJson, "\"schema_version\": 22", "\"schema_version\": 21"), "old.mvm");
    check(loaded.success && loaded.project == old, "schema21 migration");
    data = defaultGraph({"f1"});
    auto f = data.functions[0];
    f.id = {"f2"};
    check(addGraphFunction(data, f, 300), "関数追加");
    check(moveGraphFunction(data, {"f2"}, 0, 300) && data.functions[0].id == f.id,
          "reorder identity");
    f.expression = "bad(";
    check(updateGraphFunction(data, f, 300) && data.functions[0].id == f.id, "関数更新 identity");
    check(deleteGraphFunction(data, {"f1"}, 300) && !deleteGraphFunction(data, {"f2"}, 300),
          "最後の削除拒否");
    int serial = 0;
    auto fresh = [&] { return "new-" + std::to_string(++serial); };
    TimelineClip copied;
    std::string error;
    check(copyGraphClip(p.timelineClips[0], copied, fresh, error) &&
              copied.id != p.timelineClips[0].id && copied.graph.functions[0].id != id,
          "コピー所有 ID");
    p.timelineClips[0].graph.intro = {GraphIntroKind::Draw, 3};
    for (auto n : {0, 1, 2, 3}) {
        auto frame =
            evaluateGraphClip(p.timelineClips[0], {p.timelineFpsNum, p.timelineFpsDen}, n, error);
        check(frame && frame->sourceFrame == n && frame->progressNumerator == (n < 3 ? n : 1) &&
                  frame->progressDenominator == (n < 3 ? 3 : 1),
              "N3 mapping");
    }
    check(splitTimelineClips(p, {"clip"}, 1, fresh, LinkMode::Single).success, "Draw 途中 split");
    check(p.timelineClips.size() == 2 && p.timelineClips[0].graph.functions[0].id == id &&
              p.timelineClips[1].graph.functions[0].id != id,
          "split 所有 ID");
    auto right =
        evaluateGraphClip(p.timelineClips[1], {p.timelineFpsNum, p.timelineFpsDen}, 0, error);
    check(right && right->sourceFrame == 1 && right->progressNumerator == 1 &&
              right->progressDenominator == 3,
          "右片 Draw 継続");
    const auto split = p;
    check(!setGraphSourceDuration(p, p.timelineClips[1].id, 2).success && p == split, "N>L 拒否");
    auto extension = createDefaultProject();
    check(addGraph(extension, "extended", {"f"}, "延長", {TrackKind::Video, 0}, 0).success,
          "duration 対照");
    extension.timelineClips[0].graph.intro = {GraphIntroKind::Draw, 3};
    check(setGraphSourceDuration(extension, "extended", 400).success &&
              extension.timelineClips[0].sourceOutFrame == 400 &&
              extension.timelineClips[0].graph.intro.frames == 3,
          "source 延長は intro 不変");
    check(setGraphSourceDuration(extension, "extended", 3).success, "N=L 短縮");
    check(!setGraphSourceDuration(extension, "extended", 2).success, "source 短縮 N>L");
    extension.timelineClips[0].sourceFrameCount = 10;
    extension.timelineClips[0].sourceOutFrame = 10;
    const auto full = extension.timelineClips[0].graph;
    check(trimTimelineClip(extension, "extended", TrimEdge::Left, 1, LinkMode::Single).success &&
              extension.timelineClips[0].graph == full &&
              extension.timelineClips[0].sourceInFrame == 1,
          "trim は full data と所有 ID 保持");
    auto trimmedFrame = evaluateGraphClip(extension.timelineClips[0], {60, 1}, 0, error);
    check(trimmedFrame && trimmedFrame->progressNumerator == 1 &&
              trimmedFrame->progressDenominator == 3,
          "trim は Draw を再開しない");
    auto one = copied;
    one.sourceInFrame = 2;
    one.sourceOutFrame = 3;
    one.graph.intro = {GraphIntroKind::Draw, 3};
    auto lastIntro = evaluateGraphClip(one, {60, 1}, 0, error);
    check(lastIntro && lastIntro->sourceFrame == 2 && lastIntro->progressNumerator == 2,
          "一 frame 区間の最後の intro");
    for (auto fps : {mvm::core::FrameRate{24, 1}, {24000, 1001}, {30000, 1001}, {60, 1}}) {
        auto c = copied;
        c.sourceFpsNum = fps.num;
        c.sourceFpsDen = fps.den;
        c.graph.intro = {GraphIntroKind::Draw, 3};
        auto frame = evaluateGraphClip(c, {60, 1}, 2, error);
        const auto expected = 2 * fps.num / (60 * fps.den);
        check(frame && frame->sourceFrame == expected, "独立整数式による有理数始点 mapping");
    }
    std::printf("検査 %d 件、失敗 %d 件\n", checks, failures);
    return failures ? 1 : 0;
}
