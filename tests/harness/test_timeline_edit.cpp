#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    if (!wide)
        return {};
    std::filesystem::path result(wide);
    mvm_str_free(wide);
    return result;
}

mvm::project::TimelineClip
clip(const char* name,
     mvm::project::TimelineClipKind kind = mvm::project::TimelineClipKind::Video) {
    return {kind, std::filesystem::path(name) += ".mp4",
            name, std::string("id-") + name,
            60,   1,
            300,  0,
            300,  0};
}

mvm::project::Project threeClips() {
    mvm::project::Project project;
    project.timelineClips = {clip("A"), clip("Manim", mvm::project::TimelineClipKind::Manim),
                             clip("B")};
    return project;
}

void testMove() {
    auto project = threeClips();
    const auto left = mvm::project::moveTimelineClip(project, 1, -1);
    check(left.success && left.selectedIndex == 0, "左移動後の選択 index が違います");
    check(project.timelineClips[0].name == "Manim" && project.timelineClips[1].name == "A",
          "左移動で clip が交換されません");

    const auto right = mvm::project::moveTimelineClip(project, 0, 1);
    check(right.success && right.selectedIndex == 1, "右移動後の選択 index が違います");
    check(project.timelineClips[0].name == "A" && project.timelineClips[1].name == "Manim",
          "右移動で元の順序へ戻りません");

    const auto before = project.timelineClips;
    check(!mvm::project::moveTimelineClip(project, 0, -1).success,
          "先頭 clip の左移動を拒否しません");
    check(!mvm::project::moveTimelineClip(project, 2, 1).success,
          "末尾 clip の右移動を拒否しません");
    check(!mvm::project::moveTimelineClip(project, -1, 1).success,
          "不正 index の移動を拒否しません");
    check(project.timelineClips == before, "拒否した移動で timeline が変化しました");
}

void testFrameConversions() {
    const std::pair<std::int64_t, std::int64_t> rates[] = {
        {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {60, 1}};
    for (const auto [numerator, denominator] : rates) {
        for (std::int64_t timelineFrame = 0; timelineFrame < 240; ++timelineFrame) {
            const auto source = mvm::project::timelineBoundaryToSourceBoundary(
                timelineFrame, numerator, denominator, 60, 1);
            check(source.success, "timeline境界をsource境界へ変換できません");
            if (!source.success)
                continue;
            const auto at = mvm::project::sourceBoundaryToTimelineBoundary(source.frame, numerator,
                                                                           denominator, 60, 1);
            const auto next = mvm::project::sourceBoundaryToTimelineBoundary(
                source.frame + 1, numerator, denominator, 60, 1);
            check(at.success && at.frame <= timelineFrame,
                  "逆変換がdrag位置を越えないsource境界へsnapしません");
            check(next.success && next.frame > timelineFrame,
                  "逆変換が最新のsource境界を選びません");
        }
    }
    const auto overflow = mvm::project::sourceBoundaryToTimelineBoundary(
        std::numeric_limits<std::int64_t>::max(), 1, 1, std::numeric_limits<std::int64_t>::max(),
        1);
    check(!overflow.success, "overflowするframe変換を拒否しません");

    mvm::project::Project project;
    auto normalized = clip("normalized");
    auto equivalent = normalized;
    equivalent.sourceFpsNum = 120;
    equivalent.sourceFpsDen = 2;
    check(mvm::project::sourceRateMatchesTimelineRate(project, normalized) &&
              mvm::project::sourceRateMatchesTimelineRate(project, equivalent),
          "同値な60fps rateをPreview対応として判定できません");
}

void testTrimAndLookup() {
    mvm::project::Project project;
    project.timelineClips.push_back({mvm::project::TimelineClipKind::Video, "fractional.mp4",
                                     "fractional", "fractional-id", 30000, 1001, 300, 0, 300, 0});
    const auto recomputed = mvm::project::recomputeTimelineStarts(project);
    check(recomputed.success && recomputed.totalFrames == 601,
          "29.97fps clipのtimeline durationが違います");
    check(mvm::project::timelineClipIndexAtFrame(project, 0) == 0, "clip先頭をlookupできません");
    check(mvm::project::timelineClipIndexAtFrame(project, 600) == 0,
          "clip末尾frameをlookupできません");
    check(mvm::project::timelineClipIndexAtFrame(project, 601) == -1,
          "exclusive timeline終端をclip内と判定しました");

    const auto left =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Left, 3);
    check(left.success && project.timelineClips.front().sourceInFrame == 1,
          "left trimをsource-native境界へsnapできません");
    const auto beforeInvalid = project;
    const auto invalid = mvm::project::trimTimelineClip(project, "fractional-id",
                                                        mvm::project::TrimEdge::Right, -10000);
    check(!invalid.success, "sourceOut <= sourceInになるtrimを拒否しません");
    project = beforeInvalid;
}

void testValidationFailures() {
    auto duplicate = threeClips();
    duplicate.timelineClips[1].id = duplicate.timelineClips[0].id;
    check(!mvm::project::validateTimeline(duplicate).success, "重複clip IDを拒否しません");

    auto badRate = threeClips();
    badRate.timelineClips[0].sourceFpsNum = 0;
    check(!mvm::project::validateTimeline(badRate).success, "不正source FPSを拒否しません");

    auto emptyRange = threeClips();
    emptyRange.timelineClips[0].sourceOutFrame = emptyRange.timelineClips[0].sourceInFrame;
    check(!mvm::project::validateTimeline(emptyRange).success, "duration 0のclipを拒否しません");

    auto gap = threeClips();
    gap.timelineClips[1].timelineStartFrame = 301;
    check(!mvm::project::validateTimeline(gap).success, "timeline gapを拒否しません");
}

void testDeleteSelection() {
    auto middle = threeClips();
    const auto middleResult = mvm::project::deleteTimelineClip(middle, 1);
    check(middleResult.success && middleResult.selectedIndex == 1,
          "中央削除後に右隣を選択しません");
    check(middle.timelineClips.size() == 2 && middle.timelineClips[1].name == "B",
          "中央 clip を削除できません");

    auto first = threeClips();
    const auto firstResult = mvm::project::deleteTimelineClip(first, 0);
    check(firstResult.success && firstResult.selectedIndex == 0, "先頭削除後に右隣を選択しません");

    auto last = threeClips();
    const auto lastResult = mvm::project::deleteTimelineClip(last, 2);
    check(lastResult.success && lastResult.selectedIndex == 1, "末尾削除後に左隣を選択しません");

    mvm::project::Project only;
    only.timelineClips.push_back(clip("only"));
    const auto onlyResult = mvm::project::deleteTimelineClip(only, 0);
    check(onlyResult.success && onlyResult.selectedIndex == -1 && only.timelineClips.empty(),
          "最後の clip 削除後が未選択になりません");

    const auto before = middle.timelineClips;
    check(!mvm::project::deleteTimelineClip(middle, 9).success, "不正 index の削除を拒否しません");
    check(middle.timelineClips == before, "拒否した削除で timeline が変化しました");
}

void testManimPlacement() {
    mvm::project::Project project;
    project.timelineClips.push_back(clip("A"));
    mvm::project::ManimAsset asset;
    asset.sceneName = "Scene";
    asset.generatedVideoPath = "scene.mp4";
    project.manimAssets.push_back(asset);

    const auto placed = mvm::project::appendManimTimelineClip(project, project.manimAssets.front(),
                                                              "id-manim", 60, 1, 300);
    check(placed.success && placed.selectedIndex == 1, "Manim clip を末尾へ配置できません");
    check(project.timelineClips.back().kind == mvm::project::TimelineClipKind::Manim,
          "配置した clip が Manim ではありません");
    check(!mvm::project::appendManimTimelineClip(project, project.manimAssets.front(), "id-manim-2",
                                                 60, 1, 300)
               .success,
          "同じ Manim asset の重複配置を拒否しません");

    const auto deleted = mvm::project::deleteTimelineClip(project, 1);
    check(deleted.success && project.manimAssets.size() == 1,
          "Manim clip の削除で asset まで削除されました");
}

void testPersistenceTransaction(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "テスト directory を作成できません");

    auto live = threeClips();
    auto candidate = live;
    const auto moved = mvm::project::moveTimelineClip(candidate, 1, 1);
    check(moved.success, "保存用 candidate を編集できません");
    candidate.timelineClips[1].sourceFpsNum = 30000;
    candidate.timelineClips[1].sourceFpsDen = 1001;
    candidate.timelineClips[1].sourceInFrame = 30;
    candidate.timelineClips[1].sourceOutFrame = 270;
    check(mvm::project::recomputeTimelineStarts(candidate).success,
          "保存用のsource-native trimを確定できません");
    const auto saved =
        mvm::project::saveProjectJsonTransaction(live, std::move(candidate), root / "project.json");
    check(saved.success, "編集済み Project を保存できません");
    const auto loaded = mvm::project::loadProjectJson(root / "project.json");
    check(loaded.success && loaded.project.timelineClips[2].name == "Manim",
          "編集順が reload 後に保持されません");
    bool timelineFieldsMatch = loaded.success && loaded.project.timelineFpsNum == 60 &&
                               loaded.project.timelineFpsDen == 1 &&
                               loaded.project.timelineClips.size() == live.timelineClips.size();
    if (timelineFieldsMatch) {
        for (std::size_t index = 0; index < live.timelineClips.size(); ++index) {
            const auto& actual = loaded.project.timelineClips[index];
            const auto& expected = live.timelineClips[index];
            timelineFieldsMatch = timelineFieldsMatch && actual.id == expected.id &&
                                  actual.sourceFpsNum == expected.sourceFpsNum &&
                                  actual.sourceFpsDen == expected.sourceFpsDen &&
                                  actual.sourceFrameCount == expected.sourceFrameCount &&
                                  actual.sourceInFrame == expected.sourceInFrame &&
                                  actual.sourceOutFrame == expected.sourceOutFrame &&
                                  actual.timelineStartFrame == expected.timelineStartFrame;
        }
    }
    check(timelineFieldsMatch, "schema 2のID・FPS・trim・timeline startがround-tripしません");

    const auto beforeFailure = live.timelineClips;
    auto failedCandidate = live;
    check(mvm::project::deleteTimelineClip(failedCandidate, 0).success,
          "失敗保存用 candidate を編集できません");
    std::ofstream blocker(root / "blocked", std::ios::binary);
    blocker << "file";
    blocker.close();
    const auto failed = mvm::project::saveProjectJsonTransaction(live, std::move(failedCandidate),
                                                                 root / "blocked" / "project.json");
    check(!failed.success, "保存不能 path への transaction が成功しました");
    check(live.timelineClips == beforeFailure, "保存失敗時に live Project が変化しました");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_timeline_edit <work-directory>\n");
        return 2;
    }
    testMove();
    testFrameConversions();
    testTrimAndLookup();
    testValidationFailures();
    testDeleteSelection();
    testManimPlacement();
    testPersistenceTransaction(fromUtf8(argv[1]));
    if (failures != 0) {
        std::fprintf(stderr, "M6a timeline edit: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("M6a timeline edit: PASS");
    return 0;
}
