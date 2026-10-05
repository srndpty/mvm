// 数式 clip の Project model と JSON (schema 19) の契約。
//
// - 数式 clip は Project の値として往復し、描画方針 (renderer) を持たない
// - 値の形の検証は描けるかどうかと無関係 (描けない式も保存できる。検証は形だけ)
// - 読める版は 19・18・17・16 で、読み込み後は 19。数式 clip は 18 以上、
//   math_animation は 19 の file にだけ現れてよい
// - 自動音量調整の field の欠落は 16 の file だけに許す (17 で欠けていたら拒否する)
// - Write の尺は clip の尺に収め、分割の右側は Write を持たない (fade in と同じ)
//
// 負例は、読める対照 (保存したままの JSON) から 1 か所だけ変えて作る。

#include "project/project.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

using namespace mvm::project;

const char* kProjectPath = "math-test.mvm";

TimelineClip mathClip(const std::string& id, const std::string& source) {
    TimelineClip clip;
    clip.kind = TimelineClipKind::Math;
    clip.name = source;
    clip.id = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 300;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = 300;
    clip.timelineStartFrame = 0;
    clip.track = {TrackKind::Video, 1};
    clip.math.source = source;
    return clip;
}

Project projectWithMath() {
    Project project = createDefaultProject();
    auto first = mathClip("math-1", "ax^2 + bx + c = 0");
    // 描けない式も Project の値としては正しい (検証は形だけを見る)。
    auto second = mathClip("math-2", "\\fracc{\"引用\"}\n{改行}");
    second.timelineStartFrame = 300;
    second.math.fontSize = 120;
    second.math.color = "#FF00FFCC";
    second.math.backgroundColor = "#80000000";
    second.effects.positionYPercent = -20;
    second.mathAnimation = {MathIntroKind::Write, 90};
    project.timelineClips = {first, second};
    return project;
}

std::string serialize(const Project& project) {
    const auto result = serializeProjectJson(project, kProjectPath);
    check(result.success, "保存できる: " + result.error);
    return result.json;
}

bool loads(const std::string& json) {
    return parseProjectJsonText(json, kProjectPath).success;
}

// text の最初の from を to にする。見つからなければ検査の失敗として扱い、空を返す。
std::string replaced(std::string text, const std::string& from, const std::string& to,
                     const std::string& what) {
    const auto at = text.find(from);
    check(at != std::string::npos, what + ": 置き換える位置がある (" + from + ")");
    if (at == std::string::npos)
        return {};
    return text.replace(at, from.size(), to);
}

void testRoundTrip() {
    const Project project = projectWithMath();
    const auto validated = validateTimeline(project);
    check(validated.success,
          "数式 clip を含む Project は timeline の検証を通る: " + validated.error);
    const std::string json = serialize(project);
    check(json.find("\"kind\": \"math\"") != std::string::npos, "kind は math で保存する");
    check(json.find("\"math\": {") != std::string::npos, "math object を保存する");
    check(json.find("renderer") == std::string::npos,
          "数式 clip は描画方針 (renderer) を保存しない");
    check(json.find("\"schema_version\": 19") != std::string::npos, "schema 19 で保存する");
    check(json.find("\"math_animation\": { \"intro\": \"write\", \"intro_frames\": 90 }") !=
              std::string::npos,
          "Write の intro を math_animation として保存する");
    const auto firstAnimation = json.find("\"math_animation\"");
    check(firstAnimation != std::string::npos &&
              json.find("\"math_animation\"", firstAnimation + 1) == std::string::npos,
          "intro が無い数式 clip は math_animation を書かない");
    const auto parsed = parseProjectJsonText(json, kProjectPath);
    check(parsed.success, "保存した JSON を読める: " + parsed.error);
    check(parsed.success && parsed.project == project,
          "式 (引用符・改行・日本語を含む)・文字サイズ・色・背景・ClipEffects・Write が往復する");
    check(parsed.success && parsed.project.timelineClips[0].mathAnimation == MathClipAnimation{} &&
              parsed.project.timelineClips[1].mathAnimation ==
                  MathClipAnimation{MathIntroKind::Write, 90},
          "省略した math_animation は intro 無しとして読む");
    check(approximateProjectBytes(project) > approximateProjectBytes(createDefaultProject()) +
                                                 project.timelineClips[1].math.source.size(),
          "Undo の memory 見積もりに式の長さを含める");
}

void testValidation() {
    auto expectInvalid = [](Project project, const std::string& what) {
        check(!validateTimeline(project).success, what + " を拒否する");
    };
    auto base = projectWithMath();
    check(validateTimeline(base).success, "対照: 正しい Project は通る");

    auto p = base;
    p.timelineClips[0].math.source = "";
    expectInvalid(p, "空の式");
    p = base;
    p.timelineClips[0].math.source = " \t\n";
    expectInvalid(p, "空白だけの式");
    p = base;
    p.timelineClips[0].math.syntax = "typst";
    expectInvalid(p, "未対応の記法");
    p = base;
    p.timelineClips[0].math.fontSize = 0;
    expectInvalid(p, "文字サイズ 0");
    p = base;
    p.timelineClips[0].math.fontSize = p.outputHeight + 1;
    expectInvalid(p, "出力の高さを超える文字サイズ");
    p = base;
    p.timelineClips[0].math.color = "#FFF";
    expectInvalid(p, "#AARRGGBB でない色");
    p = base;
    p.timelineClips[0].math.backgroundColor = "#GG000000";
    expectInvalid(p, "16 進でない背景色");
    p = base;
    p.timelineClips[0].mediaPath = "x.png";
    expectInvalid(p, "media path を持つ数式 clip");
    p = base;
    p.timelineClips[0].speedNum = 2;
    expectInvalid(p, "速度を持つ数式 clip");
    p = base;
    p.timelineClips[0].kind = TimelineClipKind::Text;
    expectInvalid(p, "数式のデータを持つ数式以外の clip");

    p = base;
    p.timelineClips[0].math.fontSize = p.outputHeight;
    check(validateTimeline(p).success, "出力の高さちょうどの文字サイズは通る");

    // Write の尺。clip は 300 frame。
    p = base;
    p.timelineClips[0].mathAnimation = {MathIntroKind::None, 30};
    expectInvalid(p, "intro が無いのに尺を持つ数式 clip");
    p = base;
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 0};
    expectInvalid(p, "尺 0 の Write");
    p = base;
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 301};
    expectInvalid(p, "clip の尺を超える Write");
    // 描画の方式 (連番の枚数・memory) の上限は Project の値に持ち込まない。60 fps で 5 分の
    // Write も Project としては正しい (描けるかは描画が判断し、未対応なら描画が失敗する)。
    p = base;
    p.timelineClips[0].sourceFrameCount = p.timelineClips[0].sourceOutFrame = 18000;
    p.timelineClips[1].timelineStartFrame = 18000;
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 18000};
    check(validateTimeline(p).success, "描画の上限を超える長さの Write も Project として正しい");
    {
        const auto saved = serializeProjectJson(p, kProjectPath);
        const auto reloaded =
            saved.success ? parseProjectJsonText(saved.json, kProjectPath) : ProjectLoadResult{};
        check(saved.success && reloaded.success && reloaded.project == p,
              "長い Write を保存して変えずに読み直せる");
    }
    p = base;
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 300};
    check(validateTimeline(p).success, "clip の尺ちょうどの Write は通る");
    p = base;
    p.timelineClips[0].kind = TimelineClipKind::Text;
    p.timelineClips[0].math = {};
    p.timelineClips[0].text.content = "文字";
    check(validateTimeline(p).success, "対照: 数式のデータを外した文字 clip は通る");
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 30};
    expectInvalid(p, "数式の animation を持つ数式以外の clip");
}

void testStrictJson() {
    const std::string good = serialize(projectWithMath());
    check(loads(good), "対照: 保存したままの JSON は読める");

    const auto mathAt = good.find("\"math\": {");
    if (mathAt == std::string::npos) {
        check(false, "math object の位置がある");
        return;
    }
    const auto mathEnd = good.find('}', mathAt);
    // "math" object を丸ごと消す (直前の ",\n      " も一緒に)。
    const auto objectStart = good.rfind(",\n", mathAt);
    std::string noMath = good;
    noMath.erase(objectStart, mathEnd + 1 - objectStart);
    check(!loads(noMath), "kind が math なのに math object が無い file を拒否する");

    std::string withRenderer = good;
    withRenderer.insert(mathAt + std::string("\"math\": {").size(),
                        "\n        \"renderer\": \"manim-mathtex\",");
    check(!loads(withRenderer), "math object の renderer (未知の field) を拒否する");

    std::string duplicated = good;
    duplicated.insert(mathAt + std::string("\"math\": {").size(), "\n        \"font_size\": 50,");
    check(!loads(duplicated), "math object の重複した field を拒否する");

    const auto fontAt = good.find("\"font_size\": ", mathAt);
    std::string missing = good;
    missing.erase(fontAt, good.find('\n', fontAt) + 1 - fontAt);
    check(!loads(missing), "math object の欠けた field を拒否する");

    const std::string kindKey = "\"kind\": \"math\",\n      \"media_path\": \"\"";
    check(!loads(replaced(good, kindKey, "\"kind\": \"math\",\n      \"media_path\": \"x.png\"",
                          "media_path")),
          "media_path を持つ数式 clip を拒否する");

    check(!loads(replaced(good, "\"syntax\": \"latex\"", "\"syntax\": \"typst\"", "syntax")),
          "未対応の記法を読み込みでも拒否する");

    // math_animation の中は厳格に読む。負例は保存したままの object を 1 か所だけ変える。
    const std::string animation =
        "\"math_animation\": { \"intro\": \"write\", \"intro_frames\": 90 }";
    check(good.find(animation) != std::string::npos, "math_animation の位置がある");
    check(!loads(replaced(good, animation,
                          "\"math_animation\": { \"intro\": \"write\", \"intro_frames\": 90, "
                          "\"rate_func\": \"linear\" }",
                          "math_animation")),
          "math_animation の未知の field を拒否する");
    check(!loads(replaced(good, animation,
                          "\"math_animation\": { \"intro\": \"write\", \"intro_frames\": 90, "
                          "\"intro_frames\": 30 }",
                          "math_animation")),
          "math_animation の重複した field を拒否する");
    check(!loads(replaced(good, animation, "\"math_animation\": { \"intro\": \"write\" }",
                          "math_animation")),
          "math_animation の欠けた field を拒否する");
    check(!loads(replaced(good, animation,
                          "\"math_animation\": { \"intro\": \"unwrite\", \"intro_frames\": 90 }",
                          "math_animation")),
          "未知の intro を拒否する");
    check(!loads(replaced(good, animation,
                          "\"math_animation\": { \"intro\": \"write\", \"intro_frames\": 301 }",
                          "math_animation")),
          "clip の尺を超える Write を読み込みでも拒否する");
    check(!loads(replaced(good, animation, animation + ",\n      " + animation, "math_animation")),
          "math_animation の重複を拒否する");
    check(loads(replaced(good, animation,
                         "\"math_animation\": { \"intro\": \"none\", \"intro_frames\": 0 }",
                         "math_animation")),
          "対照: 明示した intro 無しは読める");

    // 文字 clip に math_animation を付けた file は拒否する。
    Project text = createDefaultProject();
    text.timelineClips = {mathClip("text-1", "x")};
    text.timelineClips[0].kind = TimelineClipKind::Text;
    text.timelineClips[0].math = {};
    text.timelineClips[0].text.content = "文字";
    const std::string textJson = serialize(text);
    check(loads(textJson), "対照: 文字 clip の file は読める");
    const std::string textClose = "\"background_color\": \"#00000000\"\n      }";
    check(!loads(replaced(textJson, textClose,
                          textClose + ",\n      \"math_animation\": { \"intro\": \"none\", "
                                      "\"intro_frames\": 0 }",
                          "text clip")),
          "数式以外の clip の math_animation を拒否する");
}

std::string withSchema(const std::string& json, int version) {
    return replaced(json, "\"schema_version\": 19",
                    "\"schema_version\": " + std::to_string(version), "schema_version");
}

// clip の effects から自動音量調整の field (17 で加わった) を消す。
std::string withoutAudioAdjustmentFields(std::string json) {
    for (const char* field :
         {"\"normalization_gain_db\"", "\"ducking_db\"", "\"audio_adjustment_settings\"",
          "\"audio_adjustment_fingerprint\"", "\"ducking_keys\""}) {
        for (auto at = json.find(field); at != std::string::npos; at = json.find(field)) {
            const auto lineStart = json.rfind('\n', at) + 1;
            json.erase(lineStart, json.find('\n', at) + 1 - lineStart);
        }
    }
    // 消した行の前の行が "," で終わっていれば、effects の最後の要素の後に "," が残る。
    for (auto at = json.find(",\n      }"); at != std::string::npos; at = json.find(",\n      }"))
        json.erase(at, 1);
    return json;
}

void testSchemaVersions() {
    Project plain = createDefaultProject();
    plain.timelineClips = {mathClip("math-1", "x")};
    plain.timelineClips[0].kind = TimelineClipKind::Text;
    plain.timelineClips[0].math = {};
    plain.timelineClips[0].text.content = "文字";
    const std::string current = serialize(plain);
    const std::string withMath = serialize(projectWithMath());
    // 数式 clip はあるが Write の無い file (18 の file として正しい形)。
    Project staticMath = projectWithMath();
    staticMath.timelineClips[1].mathAnimation = {};
    const std::string withStaticMath = serialize(staticMath);
    check(withStaticMath.find("math_animation") == std::string::npos,
          "負例の準備: Write の無い数式の file は math_animation を持たない");

    for (const int version : {18, 17, 16}) {
        std::string old = withSchema(current, version);
        if (version == 16)
            old = withoutAudioAdjustmentFields(old);
        const auto parsed = parseProjectJsonText(old, kProjectPath);
        check(parsed.success, "schema " + std::to_string(version) + " を読める: " + parsed.error);
        check(parsed.success && parsed.project.schemaVersion == kProjectSchemaVersion,
              "schema " + std::to_string(version) + " は読み込み後に現行版 (19) になる");
        check(!loads(withSchema(withMath, version)),
              "schema " + std::to_string(version) + " の file の math_animation を拒否する");
        if (version < 18)
            check(!loads(withSchema(withStaticMath, version)),
                  "schema " + std::to_string(version) + " の file の数式 clip を拒否する");
    }
    const auto staticMath18 = parseProjectJsonText(withSchema(withStaticMath, 18), kProjectPath);
    check(staticMath18.success && staticMath18.project == staticMath,
          "schema 18 の静止の数式 clip は値を変えずに読める (P0 の Project)");
    for (const int version : {15, 20}) {
        check(!loads(withSchema(current, version)),
              "schema " + std::to_string(version) + " を拒否する");
    }

    // 自動音量調整の field の欠落は 16 だけに許す。
    const std::string stripped = withoutAudioAdjustmentFields(current);
    check(stripped.find("normalization_gain_db") == std::string::npos,
          "負例の準備: 自動音量調整の field を消せた");
    check(loads(withSchema(stripped, 16)), "対照: 16 の file は自動音量調整の field が無くてよい");
    check(!loads(withSchema(stripped, 17)),
          "17 の file で自動音量調整の field が欠けていたら拒否する");
    check(!loads(stripped), "19 の file で自動音量調整の field が欠けていたら拒否する");
}

// Write の尺は clip の見えている先頭から数え、分割・trim で clip の尺に収める。
void testIntroEditRules() {
    Project base = createDefaultProject();
    auto clip = mathClip("math-1", "x");
    clip.mathAnimation = {MathIntroKind::Write, 90};
    clip.effects.fadeInFrames = 30;
    base.timelineClips = {clip};
    check(validateTimeline(base).success, "対照: Write 90 frame の 300 frame の clip は通る");
    const auto newId = [] { return std::string("math-1-right"); };

    // 分割: 左は Write を引き継ぎ (縮んだ尺に収める)、右は持たない。
    auto p = base;
    auto split = splitTimelineClips(p, {"math-1"}, 60, newId, LinkMode::Single);
    check(split.success, "Write の内側で分割できる: " + split.error);
    if (split.success && p.timelineClips.size() == 2) {
        const auto& left =
            p.timelineClips[0].id == "math-1" ? p.timelineClips[0] : p.timelineClips[1];
        const auto& right =
            p.timelineClips[0].id == "math-1" ? p.timelineClips[1] : p.timelineClips[0];
        check(left.mathAnimation == MathClipAnimation{MathIntroKind::Write, 60},
              "分割の左は Write を縮めた尺 (60) に収める");
        check(right.mathAnimation == MathClipAnimation{}, "分割の右は Write を持たない (式の続き)");
    }
    p = base;
    split = splitTimelineClips(p, {"math-1"}, 200, newId, LinkMode::Single);
    check(split.success && p.timelineClips.size() == 2 &&
              p.timelineClips[0].mathAnimation == MathClipAnimation{MathIntroKind::Write, 90},
          "Write の後で分割した左は Write の尺を変えない");

    // 左 trim: 見えている先頭から書き直す。尺は縮めた clip に収める。
    p = base;
    auto trimmed = trimTimelineClip(p, "math-1", TrimEdge::Left, 250, LinkMode::Single);
    check(trimmed.success &&
              p.timelineClips[0].mathAnimation == MathClipAnimation{MathIntroKind::Write, 50},
          "左 trim で 50 frame に縮めると Write も 50 frame になる: " + trimmed.error);
    // 右 trim: 尺が Write より短くなれば詰める。
    p = base;
    // (fade in 30 frame は既存の規則どおり尺に収まる必要があるので、40 frame までにする)
    trimmed = trimTimelineClip(p, "math-1", TrimEdge::Right, -260, LinkMode::Single);
    check(trimmed.success &&
              p.timelineClips[0].mathAnimation == MathClipAnimation{MathIntroKind::Write, 40},
          "右 trim で 40 frame に縮めると Write も 40 frame になる: " + trimmed.error);
    p = base;
    trimmed = trimTimelineClip(p, "math-1", TrimEdge::Right, -100, LinkMode::Single);
    check(trimmed.success &&
              p.timelineClips[0].mathAnimation == MathClipAnimation{MathIntroKind::Write, 90},
          "Write より長く残す右 trim は Write の尺を変えない");

    // 置いたときの fps と違う timeline で trim すると、同じ秒数へ換算する。
    p = base;
    p.timelineClips[0].sourceFpsNum = 30;
    p.timelineClips[0].sourceFrameCount = p.timelineClips[0].sourceOutFrame = 150;
    p.timelineClips[0].mathAnimation = {MathIntroKind::Write, 45};
    p.timelineClips[0].effects.fadeInFrames = 0;
    check(validateTimeline(p).success, "対照: 30 fps で置いた clip は 60 fps の timeline で通る");
    trimmed = trimTimelineClip(p, "math-1", TrimEdge::Right, -60, LinkMode::Single);
    check(trimmed.success && p.timelineClips[0].sourceFpsNum == 60 &&
              p.timelineClips[0].mathAnimation == MathClipAnimation{MathIntroKind::Write, 90},
          "30 fps の Write 45 frame は 60 fps へ揃えると 90 frame (同じ 1.5 秒): " + trimmed.error);
}

} // namespace

int main() {
    testRoundTrip();
    testValidation();
    testStrictJson();
    testSchemaVersions();
    testIntroEditRules();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
