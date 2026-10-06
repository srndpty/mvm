// 数式の変形 (TransitionKind::MathTransform) の Project / timeline の契約 (docs/math-clips.md の
// P2)。
//
// - 隣り合う 2 つの数式 clip の編集点に置く。式は両 clip を ID で参照し、写さない
// - 条件は Project の意味だけで決め、描けるかどうかは見ない:
//   数式 clip どうし・同じ track で接している・後ろに Write が無い・前の Write と重ならない・
//   両方の背景が透明・区間 (と cut の両側の frame) で ClipEffects の見た目が一定で等しい
// - 区間分けでは cut として扱う (延ばさない・重ねない)
// - 編集では既存のトランジションの規則 (接していなければ消え、尺が足りなければ縮む) に従い、
//   前の clip の Write も先頭側の使用として数える
// - schema 20 で kind を保存し、19 以前の file には現れない
//
// 期待値は clip の尺・区間を手で数えた値である (実装の式を呼ばない)。負例は対照から 1
// か所だけ変える。

#include "app/math_clip_render.h"
#include "project/project.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "project/timeline_render.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

const char* kProjectPath = "math-transform-test.mvm";
const TrackRef kTrack{TrackKind::Video, 0};

// 60 fps で置いた frames frame の数式 clip。
TimelineClip mathClip(const std::string& id, const std::string& source, std::int64_t start,
                      std::int64_t frames) {
    TimelineClip clip;
    clip.kind = TimelineClipKind::Math;
    clip.name = id;
    clip.id = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = frames;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = frames;
    clip.timelineStartFrame = start;
    clip.track = kTrack;
    clip.math.source = source;
    return clip;
}

// A [0,300) と B [300,600) (二次方程式の導出の 1 段)。
Project base() {
    Project project = createDefaultProject();
    project.timelineClips = {mathClip("A", "x^2 + \\frac{b}{a}x = -\\frac{c}{a}", 0, 300),
                             mathClip("B",
                                      "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
                                      "-\\frac{c}{a} + \\left(\\frac{b}{2a}\\right)^2",
                                      300, 300)};
    return project;
}

Project withTransform(std::int64_t before, std::int64_t after) {
    auto project = base();
    project.timelineTransitions = {{"t1", "A", "B", before, after, TransitionKind::MathTransform}};
    return project;
}

TimelineClip& clipOf(Project& project, const std::string& id) {
    return *std::find_if(project.timelineClips.begin(), project.timelineClips.end(),
                         [&](const TimelineClip& clip) { return clip.id == id; });
}

int indexOf(const Project& project, const std::string& id) {
    for (std::size_t index = 0; index < project.timelineClips.size(); ++index)
        if (project.timelineClips[index].id == id)
            return static_cast<int>(index);
    return -1;
}

std::function<std::string()> idGenerator() {
    auto counter = std::make_shared<int>(0);
    return [counter] { return "gen-" + std::to_string(++*counter); };
}

bool valid(const Project& project) {
    return validateTimeline(project).success;
}

// 検証が fragment を含む理由で失敗すること (別の理由の失敗で通らない)。
bool failsWith(const Project& project, const std::string& fragment) {
    const auto result = validateTimeline(project);
    if (result.success || result.error.find(fragment) == std::string::npos) {
        std::fprintf(stderr, "  期待: %s / 実際: %s\n", fragment.c_str(),
                     result.success ? "(成功)" : result.error.c_str());
        return false;
    }
    return true;
}

bool hasOnly(const Project& project, std::int64_t before, std::int64_t after) {
    return project.timelineTransitions.size() == 1 &&
           project.timelineTransitions[0].kind == TransitionKind::MathTransform &&
           project.timelineTransitions[0].framesBeforeCut == before &&
           project.timelineTransitions[0].framesAfterCut == after;
}

void testCreateAndRender() {
    auto p = base();
    const auto ids = idGenerator();
    const auto created = applyMathTransformTransition(p, "A", "B", 60, ids);
    check(created.success, "隣り合う数式 clip に変形を置ける: " + created.error);
    check(hasOnly(p, 0, 60) && p.timelineTransitions[0].outgoingClipId == "A" &&
              p.timelineTransitions[0].incomingClipId == "B" &&
              p.timelineTransitions[0].id == created.transitionId,
          "既定は cut から始まる 60 frame (前 0・後 60)");
    check(valid(p), "置いた変形は検証を通る");

    // 区間分けでは cut として扱う (延ばさない・重ねない・dissolve の envelope を作らない)。
    std::vector<TimelineRenderSegment> segments;
    std::string error;
    check(timelineRenderSegments(p, TrackKind::Video, segments, error) && segments.size() == 2,
          "描画区間は 2 つ: " + error);
    for (const auto& segment : segments) {
        const auto expectedStart = segment.original.id == "A" ? 0 : 300;
        check(segment.lane == 0 && !segment.fadeIn && !segment.fadeOut &&
                  segment.clip.timelineStartFrame == expectedStart &&
                  segment.clip.sourceOutFrame - segment.clip.sourceInFrame == 300,
              "変形の clip は延ばさず lane 0 のまま: " + segment.original.id);
    }
    check(!hasRenderedTransitions(p, TrackKind::Video), "変形は混合のトランジションとして描かない");

    // 置き直し: 既存の変形を置き換え、両端のフェードを消す (Blend と同じ規則)。
    auto again = base();
    clipOf(again, "A").effects.fadeOutFrames = 10;
    check(valid(again), "対照: 前の clip の末尾にフェードがある");
    check(applyMathTransformTransition(again, "A", "B", 60, ids).success &&
              clipOf(again, "A").effects.fadeOutFrames == 0,
          "変形を置くと前の clip の末尾のフェードを消す");
    const auto replaced = applyMathTransformTransition(again, "A", "B", 30, ids);
    check(replaced.success && hasOnly(again, 0, 30) &&
              again.timelineTransitions[0].id == replaced.transitionId,
          "同じ編集点に置き直すと 1 つだけ残る");

    // 後ろの clip が短ければ、その尺まで縮める。
    auto shortB = base();
    auto& b = clipOf(shortB, "B");
    b.sourceFrameCount = b.sourceOutFrame = 40;
    check(applyMathTransformTransition(shortB, "A", "B", 60, ids).success && hasOnly(shortB, 0, 40),
          "後ろの clip が 40 frame なら 40 frame に縮める");

    // 長さの変更・削除は既存のトランジションの操作で行い、種類を保つ。
    auto span = setTimelineTransitionSpan(p, created.transitionId, 30, 30, LinkMode::Single);
    check(span.success && hasOnly(p, 30, 30), "長さと配置を変えられる: " + span.error);
    const auto limits = transitionSpanLimits(p, created.transitionId, LinkMode::Single);
    check(limits.success && limits.maxBefore == 300 && limits.maxAfter == 300,
          "上限は両 clip の尺 (余白は使わない)");
    check(deleteTimelineTransition(p, created.transitionId).success &&
              p.timelineTransitions.empty(),
          "変形を削除できる");

    // 数式 clip の編集点にはクロスディゾルブ (Blend) を置けない (従来どおり)。
    auto blend = base();
    const auto before = blend;
    check(!applyDefaultEditTransition(blend, "A", "B", 60, LinkMode::Single, ids).success &&
              blend == before,
          "数式 clip の編集点の Blend は拒否する");
}

void testApplyRejects() {
    const auto ids = idGenerator();
    const auto rejects = [&](Project p, const std::string& fragment, const std::string& what) {
        const auto before = p;
        const auto result = applyMathTransformTransition(p, "A", "B", 60, ids);
        check(!result.success && result.error.find(fragment) != std::string::npos && p == before,
              what + " は理由付きで拒否し Project を変えない (実際: " + result.error + ")");
    };
    auto p = base();
    auto& a = clipOf(p, "A");
    a.kind = TimelineClipKind::Text;
    a.math = {};
    a.text.content = "文字";
    check(valid(p), "対照: 文字 clip と数式 clip の並び");
    rejects(p, "数式 clip どうし", "数式でない clip への変形");

    p = base();
    clipOf(p, "B").timelineStartFrame = 310;
    rejects(p, "接していません", "離れた clip への変形");

    p = base();
    clipOf(p, "B").track = {TrackKind::Video, 1};
    rejects(p, "同じ track", "別の track の clip への変形");

    p = base();
    clipOf(p, "B").mathAnimation = {MathIntroKind::Write, 30};
    rejects(p, "Write を付けられません", "後ろに Write のある変形");

    p = base();
    clipOf(p, "A").math.backgroundColor = "#80000000";
    rejects(p, "背景を透明", "背景のある変形");

    p = base();
    clipOf(p, "B").effects.positionXPercent = 10;
    rejects(p, "位置・拡大・回転", "位置の違う変形");

    // 前の clip の Write は clip 全体でも、cut から始まる変形とは重ならない。
    p = base();
    clipOf(p, "A").mathAnimation = {MathIntroKind::Write, 300};
    check(applyMathTransformTransition(p, "A", "B", 60, ids).success && hasOnly(p, 0, 60),
          "前の clip の Write は cut から始まる変形と両立する");
}

void testValidation() {
    const auto control = withTransform(30, 30);
    check(valid(control), "対照: 前後 30 frame の変形は通る");

    auto p = control;
    p.timelineTransitions[0].kind = TransitionKind::Blend;
    check(failsWith(p, "クロスディゾルブ"), "同じ編集点の Blend は数式 clip に置けない");

    p = control;
    clipOf(p, "A").kind = TimelineClipKind::Text;
    clipOf(p, "A").math = {};
    clipOf(p, "A").text.content = "文字";
    check(failsWith(p, "数式 clip どうし"), "数式でない clip の変形を拒否する");

    p = control;
    clipOf(p, "B").timelineStartFrame = 301;
    check(failsWith(p, "接していません"), "離れた clip の変形を拒否する");

    p = control;
    clipOf(p, "B").mathAnimation = {MathIntroKind::Write, 30};
    check(failsWith(p, "Write を付けられません"), "後ろの clip の Write を拒否する");

    // 前の clip の Write: 300 frame の clip に 270 frame の Write なら、cut の前は 30 まで。
    p = control;
    clipOf(p, "A").mathAnimation = {MathIntroKind::Write, 270};
    check(valid(p), "Write 270 と cut の前 30 は接するだけで重ならない");
    p.timelineTransitions[0].framesBeforeCut = 31;
    check(failsWith(p, "Write と重なっています"), "Write 270 と cut の前 31 は重なる");
    // 30 fps で置いた clip (150 frame = 5 秒) の Write 135 frame (4.5 秒)。60 fps の timeline の
    // local frame k が見せる素材 frame は k/2 の四捨五入 (MLT・Write の表示と同じ) なので、
    // k = 268 (134) までが Write、k = 269 (134.5 → 135) から書き終えた式。Write の表示は
    // 269 frame で終わり、cut の前は 300 - 269 = 31 frame まで使える。
    p = control;
    auto& a30 = clipOf(p, "A");
    a30.sourceFpsNum = 30;
    a30.sourceFrameCount = a30.sourceOutFrame = 150;
    a30.mathAnimation = {MathIntroKind::Write, 135};
    p.timelineTransitions[0].framesBeforeCut = 31;
    check(valid(p), "30 fps の Write が表示を終えた frame 269 から変形を始められる");
    p.timelineTransitions[0].framesBeforeCut = 32;
    check(failsWith(p, "Write と重なっています"),
          "30 fps の Write は表示の frame で数えて重なりを検出する (frame 268 は Write)");

    p = control;
    clipOf(p, "B").math.backgroundColor = "#01000000";
    check(failsWith(p, "背景を透明"), "わずかでも不透明な背景を拒否する");
    p = control;
    clipOf(p, "A").math.backgroundColor = "#00FF8800";
    clipOf(p, "B").math.backgroundColor = "#00000000";
    check(valid(p), "alpha 0 の背景は色が違っても透明として通る");

    // ClipEffects の見た目。
    p = control;
    clipOf(p, "B").effects.rotationDegrees = 5;
    check(failsWith(p, "位置・拡大・回転"), "回転の違う両端を拒否する");
    p = control;
    for (auto* clip : {&clipOf(p, "A"), &clipOf(p, "B")}) {
        clip->effects.positionXPercent = 10;
        clip->effects.scaleXPercent = clip->effects.scaleYPercent = 80;
        clip->effects.opacityPercent = 50;
    }
    check(valid(p), "両端で等しい位置・拡大・不透明度は通る");
    p = control;
    clipOf(p, "A").effects.positionXKeys = {{200, 0}, {290, 10}};
    clipOf(p, "B").effects.positionXPercent = 10;
    check(failsWith(p, "位置・拡大・回転"), "区間の中 (A の 270〜299) で動く位置の key を拒否する");
    p = control;
    clipOf(p, "A").effects.positionXKeys = {{0, 0}, {100, 10}};
    clipOf(p, "B").effects.positionXPercent = 10;
    check(valid(p), "区間の前で終わる key は通る (区間では一定で B と等しい)");
    // フェード: A の fade in が区間に掛かると不透明度が変わる。
    p = withTransform(0, 30);
    clipOf(p, "A").effects.fadeInFrames = 290;
    check(valid(p), "cut の前を使わなければ A の fade in (290 frame) は掛からない");
    p.timelineTransitions[0].framesBeforeCut = 30;
    check(failsWith(p, "位置・拡大・回転"), "区間に掛かる fade in を拒否する");
    p = control;
    clipOf(p, "A").effects.fadeOutFrames = 10;
    check(failsWith(p, "フェード"), "変形の端のフェードを拒否する (既存の規則)");

    p = control;
    p.timelineTransitions[0].framesAfterCut = 301;
    check(failsWith(p, "尺を超えて"), "clip の尺を超える変形を拒否する");
}

std::string replacedOnce(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "置き換える位置がある: " + from);
    return at == std::string::npos ? std::string{} : text.replace(at, from.size(), to);
}

void testJson() {
    const auto project = withTransform(0, 60);
    const auto saved = serializeProjectJson(project, kProjectPath);
    check(saved.success, "変形を含む Project を保存できる: " + saved.error);
    const std::string kind = "\"kind\": \"math_transform\"";
    check(saved.json.find(kind) != std::string::npos, "kind は math_transform で保存する");
    check(saved.json.find("matching") == std::string::npos &&
              saved.json.find("segments") == std::string::npos &&
              saved.json.find("Transform") == std::string::npos,
          "照合・分け方・Manim の値を保存しない");
    const auto loaded = parseProjectJsonText(saved.json, kProjectPath);
    check(loaded.success && loaded.project == project, "変形が往復する: " + loaded.error);

    const auto schema19 =
        replacedOnce(saved.json, "\"schema_version\": 20", "\"schema_version\": 19");
    check(!parseProjectJsonText(schema19, kProjectPath).success,
          "schema 19 の file の math_transform を拒否する");
    // kind を消した 19 の file は Blend として読むので、数式 clip の編集点では検証が拒否する
    // (19 以前の file から変形は生まれない)。
    const auto noKind19 = replacedOnce(schema19, ", " + kind, "");
    const auto old = parseProjectJsonText(noKind19, kProjectPath);
    check(!old.success && old.error.find("クロスディゾルブ") != std::string::npos,
          "schema 19 の数式の編集点のトランジションは Blend として読み、検証が拒否する: " +
              old.error);
    check(!parseProjectJsonText(replacedOnce(saved.json, kind, "\"kind\": \"Math_Transform\""),
                                kProjectPath)
               .success,
          "大文字小文字の違う kind を拒否する");
    // 条件を満たさない変形の file は読み込みで拒否する (reconcile で直さない)。
    auto writeOnB = saved.json;
    const auto bAt = writeOnB.find("\"id\": \"B\"");
    const auto bEnd = bAt == std::string::npos ? std::string::npos : writeOnB.find("\n    }", bAt);
    check(bEnd != std::string::npos, "B の clip の位置がある");
    if (bEnd != std::string::npos) {
        writeOnB.insert(bEnd, ",\n      \"math_animation\": { \"intro\": \"write\", "
                              "\"intro_frames\": 30 }");
        const auto rejected = parseProjectJsonText(writeOnB, kProjectPath);
        check(!rejected.success && rejected.error.find("Write") != std::string::npos,
              "後ろの clip に Write のある変形の file を拒否する: " + rejected.error);
    }
}

void testTrimAndReconcile() {
    // 後ろの clip の終端を縮めると、cut の後ろを縮める。
    auto p = withTransform(60, 120);
    auto trimmed = trimTimelineClip(p, "B", TrimEdge::Right, -200, LinkMode::Single);
    check(trimmed.success && hasOnly(p, 60, 100),
          "B を 100 frame にすると後ろは 100: " + trimmed.error);
    trimmed = trimTimelineClip(p, "B", TrimEdge::Right, -99, LinkMode::Single);
    check(trimmed.success && hasOnly(p, 60, 1), "B を 1 frame にすると後ろは 1");
    // 前の clip の先頭を縮めると、cut の前を縮める。
    p = withTransform(60, 120);
    trimmed = trimTimelineClip(p, "A", TrimEdge::Left, 250, LinkMode::Single);
    check(trimmed.success && hasOnly(p, 50, 120),
          "A を 50 frame にすると前は 50: " + trimmed.error);

    // cut を動かす (rolling) と接したまま。前の clip の Write が先頭を使う分は変形に充てない。
    p = withTransform(60, 0);
    clipOf(p, "A").mathAnimation = {MathIntroKind::Write, 240};
    check(valid(p), "対照: Write 240 と cut の前 60 は接する");
    auto rolled = rollTimelineEdit(p, "A", TrimEdge::Right, -30, LinkMode::Single);
    check(rolled.success && hasOnly(p, 30, 0),
          "A を 270 frame にすると Write 240 の後の 30 frame に縮める: " + rolled.error);
    rolled = rollTimelineEdit(p, "A", TrimEdge::Right, -40, LinkMode::Single);
    check(rolled.success && p.timelineTransitions.empty(),
          "Write の後に余りが無く、cut の後ろも 0 なら変形は消える: " + rolled.error);
    p = withTransform(60, 30);
    clipOf(p, "A").mathAnimation = {MathIntroKind::Write, 240};
    rolled = rollTimelineEdit(p, "A", TrimEdge::Right, -70, LinkMode::Single);
    check(rolled.success && hasOnly(p, 0, 30) &&
              clipOf(p, "A").mathAnimation == MathClipAnimation{MathIntroKind::Write, 230},
          "Write が clip 全体になっても cut の後ろの 30 frame は残る: " + rolled.error);

    // 長さの変更は Write の分を上限にする。
    p = withTransform(30, 0);
    clipOf(p, "A").mathAnimation = {MathIntroKind::Write, 240};
    const auto limits = transitionSpanLimits(p, "t1", LinkMode::Single);
    check(limits.success && limits.maxBefore == 60, "cut の前の上限は Write の後の 60 frame");
    const auto before = p;
    check(!setTimelineTransitionSpan(p, "t1", 61, 0, LinkMode::Single).success && p == before,
          "Write に掛かる長さは設定を拒否し Project を変えない");
    check(setTimelineTransitionSpan(p, "t1", 60, 0, LinkMode::Single).success && hasOnly(p, 60, 0),
          "Write の直後までは設定できる");
    const auto fit =
        nearestTransitionSpan(p, "t1", 100, 0, SpanFitMode::EachSide, LinkMode::Single);
    check(fit.success && fit.framesBeforeCut == 60 && fit.framesAfterCut == 0,
          "吸着は Write の直後 (60) に止める");
    // 区間の見た目の条件も吸着で守る: B の 20 frame 目から位置が動くなら後ろは 20 まで。
    p = withTransform(0, 10);
    clipOf(p, "B").effects.positionXKeys = {{20, 0}, {40, 10}};
    check(valid(p), "対照: 位置の key は区間の後ろで動き始める");
    const auto lookFit =
        nearestTransitionSpan(p, "t1", 0, 100, SpanFitMode::EachSide, LinkMode::Single);
    check(lookFit.success && lookFit.framesAfterCut == 21,
          "後ろは位置が動き始める前 (key 20 の frame まで一定なので 21) に止める");
    check(!setTimelineTransitionSpan(p, "t1", 0, 22, LinkMode::Single).success,
          "位置が動く frame に掛かる長さは拒否する");

    // fps の変更は既存の換算で秒を保つ。
    p = withTransform(30, 60);
    const auto fps = setTimelineFrameRate(p, 30, 1);
    check(fps.success && hasOnly(p, 15, 30) && valid(p),
          "30 fps にすると前 15・後 30: " + fps.error);
}

void testEditsThatRemove() {
    const auto ids = idGenerator();
    auto p = withTransform(30, 30);
    check(moveClip(p, "B", kTrack, 400).success && p.timelineTransitions.empty(),
          "後ろの clip を離すと変形は消える");

    p = withTransform(30, 30);
    check(deleteTimelineClip(p, indexOf(p, "A")).success && p.timelineTransitions.empty(),
          "前の clip を削除すると変形は消える");
    p = withTransform(30, 30);
    check(deleteTimelineClip(p, indexOf(p, "B")).success && p.timelineTransitions.empty(),
          "後ろの clip を削除すると変形は消える");

    // 上書きで cut に別の clip を置くと、A と B の間に入るので消える。
    p = withTransform(30, 30);
    p.timelineClips.push_back(mathClip("C", "y", 600, 40));
    check(valid(p), "対照: C は B の後ろ");
    const auto overwritten = moveClips(p, {"C"}, "C", kTrack, 280, LinkMode::Single, ids);
    check(overwritten.success && p.timelineTransitions.empty(),
          "上書きで間に clip が入ると変形は消える: " + overwritten.error);

    // リップルで A を縮めると B が詰まり、接したままなので残る (式は変わらない)。
    p = withTransform(30, 30);
    const auto ripple = rippleTrimTimelineClip(p, "A", TrimEdge::Right, -60, LinkMode::Single);
    check(ripple.success && hasOnly(p, 30, 30) && clipOf(p, "B").timelineStartFrame == 240,
          "リップルで詰めても接したままなら変形は残る: " + ripple.error);

    // 無効にしても変形は Project に残り、描画区間には出さない (Blend と同じ)。
    p = withTransform(30, 30);
    check(toggleClipsEnabled(p, {"A"}).success && hasOnly(p, 30, 30),
          "clip を無効にしても変形は残る");
    std::vector<TimelineRenderSegment> segments;
    std::string error;
    check(timelineRenderSegments(p, TrackKind::Video, segments, error) && segments.size() == 1 &&
              segments[0].original.id == "B" && segments[0].clip.timelineStartFrame == 300,
          "無効な clip は区間に含めず、後ろの clip は cut から出す");
}

void testSplit() {
    const auto ids = idGenerator();
    // 前の clip を区間の外で分けると、右側が前の clip になる (同じ式)。
    auto p = withTransform(30, 60);
    check(splitTimelineClips(p, {"A"}, 100, ids, LinkMode::Single).success &&
              p.timelineClips.size() == 3,
          "A を 100 で分けられる");
    const auto& transition = p.timelineTransitions.at(0);
    check(hasOnly(p, 30, 60) && transition.outgoingClipId != "A" &&
              transition.incomingClipId == "B",
          "A の分割後は右側が前の clip");
    const int right = indexOf(p, transition.outgoingClipId);
    check(right >= 0 &&
              p.timelineClips[static_cast<std::size_t>(right)].math == clipOf(p, "A").math,
          "前の clip になった右側は同じ式");
    // 区間の中で分けると、右側の尺まで縮める。
    p = withTransform(30, 60);
    check(splitTimelineClips(p, {"A"}, 285, ids, LinkMode::Single).success && hasOnly(p, 15, 60),
          "A を 285 で分けると前は 15");
    // 後ろの clip を分けると、左側 (元の ID) が後ろの clip のまま。
    p = withTransform(30, 60);
    check(splitTimelineClips(p, {"B"}, 330, ids, LinkMode::Single).success && hasOnly(p, 30, 30) &&
              p.timelineTransitions[0].incomingClipId == "B",
          "B を 330 で分けると後ろは 30");
    p = withTransform(30, 60);
    check(splitTimelineClips(p, {"B"}, 500, ids, LinkMode::Single).success && hasOnly(p, 30, 60),
          "B を区間の外で分けても変わらない");
}

// Write の終わりの境界は 1 つの正 (mathIntroSourceFrameAt) が決める。preview・書き出しが見せる
// frame (mathIntroFrameAt) を 1 frame ずつ走査して求めた境界と、変形の条件・上限・reconcile が
// 使う境界 (mathIntroTimelineFrames) が一致し、どちらも手で数えた値になることを確かめる。
void testSharedWriteBoundary() {
    struct Case {
        const char* what;
        std::int64_t timelineFps;
        std::int64_t clipFps;
        std::int64_t clipFrames;       // 素材 frame
        std::int64_t introFrames;      // 素材 frame
        std::int64_t timelineFrames;   // clip の timeline 上の尺
        std::int64_t expectedBoundary; // 手で数えた Write の表示の終わり (timeline local frame)
    };

    // 30 fps の Write 135 は 60 fps の local k に素材 round(k/2) を見せ、k = 269 で 135 になる。
    // 30 fps の Write 1 は k = 1 で round(0.5) = 1 になり、1 frame だけ見える。
    // 60 fps の Write 45 は 30 fps の local k に素材 2k を見せ、k = 23 で 46 になる。
    const Case cases[] = {
        {"60 fps の Write 90", 60, 60, 300, 90, 300, 90},
        {"60 fps の timeline の 30 fps の Write 135", 60, 30, 150, 135, 300, 269},
        {"60 fps の timeline の 30 fps の Write 1", 60, 30, 150, 1, 300, 1},
        {"30 fps の timeline の 60 fps の Write 45", 30, 60, 600, 45, 300, 23},
        {"clip 全体の Write", 60, 60, 300, 300, 300, 300},
    };
    for (const auto& c : cases) {
        const std::string what = c.what;
        auto p = withTransform(0, 30);
        p.timelineFpsNum = c.timelineFps;
        auto& a = clipOf(p, "A");
        a.sourceFpsNum = c.clipFps;
        a.sourceFrameCount = a.sourceOutFrame = c.clipFrames;
        a.mathAnimation = {MathIntroKind::Write, c.introFrames};
        auto& b = clipOf(p, "B");
        b.timelineStartFrame = c.timelineFrames;
        b.sourceFpsNum = c.timelineFps;
        b.sourceFrameCount = b.sourceOutFrame = 300;
        check(valid(p), what + ": 対照の Project が通る: " + validateTimeline(p).error);

        // preview・書き出しの frame の選び方を 1 frame ずつ走査する。
        std::int64_t previewBoundary = -1;
        bool contiguous = true;
        for (std::int64_t local = 0; local < c.timelineFrames; ++local) {
            const auto shown = mvm::app::mathIntroFrameAt(a, c.timelineFps, 1, local);
            if (!shown) {
                contiguous = false;
                break;
            }
            if (*shown < 0 && previewBoundary < 0)
                previewBoundary = local;
            if (*shown >= 0 && previewBoundary >= 0)
                contiguous = false;
        }
        if (previewBoundary < 0)
            previewBoundary = c.timelineFrames;
        check(contiguous && previewBoundary == c.expectedBoundary,
              what + ": preview は local " + std::to_string(c.expectedBoundary) +
                  " から書き終えた式を見せる (実際 " + std::to_string(previewBoundary) + ")");
        const auto shared = mathIntroTimelineFrames(p, a);
        check(shared.success && shared.frame == previewBoundary,
              what + ": 変形の境界 (mathIntroTimelineFrames) が preview と同じ");

        // 変形は preview が Write を見せ終えた frame から始められ、1 frame 手前からは始められない。
        const auto fits = c.timelineFrames - c.expectedBoundary;
        p.timelineTransitions[0].framesBeforeCut = fits;
        check(valid(p), what + ": cut の前 " + std::to_string(fits) + " は Write と重ならない");
        const auto limits = transitionSpanLimits(p, "t1", LinkMode::Single);
        check(limits.success && limits.maxBefore == fits,
              what + ": cut の前の上限が preview の境界と同じ");
        if (fits < c.timelineFrames) {
            p.timelineTransitions[0].framesBeforeCut = fits + 1;
            check(failsWith(p, "Write と重なっています"), what + ": cut の前 " +
                                                              std::to_string(fits + 1) +
                                                              " は preview の Write に掛かる");
        }
    }
}

} // namespace

// 変形の描画要求と cache key (P2-2) は両端の式・文字サイズと区間の frame 数だけで決まる。
// 色・背景・ClipEffects・clip / トランジションの ID・timeline の fps は合成の側の値で、key
// を変えない。
std::string transformKeyOf(const Project& project) {
    const auto& transition = project.timelineTransitions.at(0);
    const auto outgoing = indexOf(project, transition.outgoingClipId);
    const auto incoming = indexOf(project, transition.incomingClipId);
    if (outgoing < 0 || incoming < 0)
        return "(clip が無い)";
    const auto spec = mvm::app::mathTransformSpecFor(
        transition, project.timelineClips[static_cast<std::size_t>(outgoing)],
        project.timelineClips[static_cast<std::size_t>(incoming)]);
    if (!spec)
        return "(描画要求が無い)";
    const mvm::math::MathToolchainFingerprint toolchain{"manim-mathtex", "manim=M\n"};
    return mvm::math::mathTransformKey(*spec, toolchain, "manim-transform/1");
}

void testTransformSpec() {
    const auto project = withTransform(12, 18);
    const auto& transition = project.timelineTransitions[0];
    const auto spec = mvm::app::mathTransformSpecFor(transition, project.timelineClips[0],
                                                     project.timelineClips[1]);
    check(spec &&
              spec->source ==
                  mvm::math::MathRenderSpec{"latex", project.timelineClips[0].math.source, 96} &&
              spec->target ==
                  mvm::math::MathRenderSpec{"latex", project.timelineClips[1].math.source, 96} &&
              spec->frames == 30,
          "変形の描画要求は両 clip の式・文字サイズと区間の 12 + 18 frame");

    const std::string key = transformKeyOf(project);
    check(key.size() == 64, "変形の key を作れる: " + key);
    auto same = [&](const Project& changed, const std::string& what) {
        check(transformKeyOf(changed) == key, what + " を変えても変形の key は変わらない");
    };
    auto changed = project;
    clipOf(changed, "A").math.color = "#FF00FF00";
    clipOf(changed, "B").math.color = "#8000FFFF";
    same(changed, "両端の文字色");
    changed = project;
    clipOf(changed, "A").math.backgroundColor = "#FF000000";
    same(changed, "背景色");
    changed = project;
    for (auto& clip : changed.timelineClips) {
        clip.effects.positionXPercent = 10.0;
        clip.effects.scaleXPercent = 150.0;
        clip.effects.rotationDegrees = 30.0;
        clip.effects.opacityPercent = 50.0;
    }
    same(changed, "ClipEffects");
    changed = project;
    clipOf(changed, "A").id = "A-renamed";
    clipOf(changed, "B").id = "B-renamed";
    changed.timelineTransitions[0].outgoingClipId = "A-renamed";
    changed.timelineTransitions[0].incomingClipId = "B-renamed";
    same(changed, "clip の ID");
    changed = project;
    changed.timelineTransitions[0].id = "t-other";
    same(changed, "トランジションの ID");
    changed = project;
    changed.timelineFpsNum = 30;
    for (auto& clip : changed.timelineClips)
        clip.sourceFpsNum = 30;
    same(changed, "timeline の fps (frame 数は同じ)");
    changed = project;
    changed.timelineTransitions[0].framesBeforeCut = 0;
    changed.timelineTransitions[0].framesAfterCut = 30;
    same(changed, "cut に対する区間の位置 (frame 数は同じ)");
    changed = project;
    for (auto& clip : changed.timelineClips)
        clip.timelineStartFrame += 600;
    same(changed, "timeline の位置");

    // 対照: 描画を変える値は key を変える (上の「変わらない」が空振りでないこと)。
    changed = project;
    clipOf(changed, "A").math.source = "x^2 = 1";
    check(transformKeyOf(changed) != key, "前の clip の式を変えると変形の key が変わる");
    changed = project;
    clipOf(changed, "B").math.fontSize = 97;
    check(transformKeyOf(changed) != key, "後ろの clip の文字サイズを変えると変形の key が変わる");
    changed = project;
    changed.timelineTransitions[0].framesAfterCut = 19;
    check(transformKeyOf(changed) != key, "区間の frame 数を変えると変形の key が変わる");

    // 描画要求を作らない入力。
    auto blend = transition;
    blend.kind = TransitionKind::Blend;
    check(
        !mvm::app::mathTransformSpecFor(blend, project.timelineClips[0], project.timelineClips[1]),
        "Blend のトランジションには変形の描画要求を作らない");
    check(!mvm::app::mathTransformSpecFor(transition, project.timelineClips[1],
                                          project.timelineClips[0]),
          "トランジションの参照と違う clip には作らない");
    auto video = project.timelineClips[1];
    video.kind = TimelineClipKind::Video;
    check(!mvm::app::mathTransformSpecFor(transition, project.timelineClips[0], video),
          "数式でない clip には作らない");
}

int main() {
    testCreateAndRender();
    testApplyRejects();
    testValidation();
    testJson();
    testTrimAndReconcile();
    testEditsThatRemove();
    testSplit();
    testSharedWriteBoundary();
    testTransformSpec();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
