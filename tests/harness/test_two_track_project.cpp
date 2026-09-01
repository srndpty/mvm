#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "util/mvm_win_utf8.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using mvm::project::Project;
using mvm::project::TimelineClip;
using mvm::project::TimelineClipKind;
using mvm::project::VideoTrack;

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

TimelineClip clip(const char* id, VideoTrack track, std::int64_t start) {
    TimelineClip result;
    result.kind = TimelineClipKind::Video;
    result.mediaPath = std::string(id) + ".mp4";
    result.name = id;
    result.id = id;
    result.sourceFpsNum = 60;
    result.sourceFpsDen = 1;
    result.sourceFrameCount = 100;
    result.sourceInFrame = 0;
    result.sourceOutFrame = 100;
    result.timelineStartFrame = start;
    result.videoTrack = track;
    return result;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    check(output.good(), "テストJSONを書き込めません");
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string oneClipJson(const std::string& videoTrackField) {
    return R"JSON({"schema_version":2,"timeline_fps_num":60,"timeline_fps_den":1,"manim_assets":[],"timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"A","id":"a","source_fps_num":60,"source_fps_den":1,"source_frame_count":100,"source_in_frame":0,"source_out_frame":100,"timeline_start_frame":25)JSON" +
           videoTrackField + "}]}";
}

void testSchemaCompatibility(const std::filesystem::path& root) {
    const auto oldPath = root / "old-schema-2.json";
    writeText(oldPath, oneClipJson(""));
    const auto oldLoaded = mvm::project::loadProjectJson(oldPath);
    check(oldLoaded.success && oldLoaded.project.timelineClips.size() == 1 &&
              oldLoaded.project.timelineClips.front().videoTrack == VideoTrack::V1,
          "video_track欠損schema 2をV1として読めません");

    const auto invalidPath = root / "invalid-track.json";
    writeText(invalidPath, oneClipJson(",\"video_track\":2"));
    check(!mvm::project::loadProjectJson(invalidPath).success,
          "schema 2の不正video_trackを拒否しません");
}

void testValidationAndLookup() {
    Project arbitrary;
    arbitrary.timelineClips = {clip("late", VideoTrack::V1, 300),
                               clip("overlay", VideoTrack::V2, 50),
                               clip("early", VideoTrack::V1, 0)};
    const auto arbitraryResult = mvm::project::validateTimeline(arbitrary);
    check(arbitraryResult.success && arbitraryResult.totalFrames == 400,
          "vector順に依存せず任意startとgapを受理できません");

    auto negative = arbitrary;
    negative.timelineClips[0].timelineStartFrame = -1;
    check(!mvm::project::validateTimeline(negative).success, "負のstartを拒否しません");

    auto invalidTrack = arbitrary;
    invalidTrack.timelineClips[0].videoTrack = static_cast<VideoTrack>(2);
    check(!mvm::project::validateTimeline(invalidTrack).success,
          "モデル上の不正video trackを拒否しません");

    Project sameTrack;
    sameTrack.timelineClips = {clip("a", VideoTrack::V1, 0), clip("b", VideoTrack::V1, 99)};
    check(!mvm::project::validateTimeline(sameTrack).success,
          "同一trackのhalf-open interval重複を拒否しません");

    Project crossTrack;
    crossTrack.timelineClips = {clip("a", VideoTrack::V1, 0), clip("b", VideoTrack::V2, 50)};
    check(mvm::project::validateTimeline(crossTrack).success, "track間の重複を受理しません");

    Project touching;
    touching.timelineClips = {clip("a", VideoTrack::V1, 0), clip("b", VideoTrack::V1, 100)};
    check(mvm::project::validateTimeline(touching).success,
          "同一trackで終端と始端が接するclipを受理しません");

    const auto at75 = mvm::project::activeClipsAt(arbitrary, 75);
    check(at75[0] && at75[0]->id == "early" && at75[1] && at75[1]->id == "overlay",
          "V1/V2のactive clipを同時に取得できません");
    check(mvm::project::activeClipAt(arbitrary, VideoTrack::V2, 150) == nullptr,
          "half-open終端をactiveと判定しました");
    const auto gap = mvm::project::activeClipsAt(arbitrary, 200);
    check(!gap[0] && !gap[1], "timeline gapでactive clipを返しました");

    const auto end = mvm::project::timelineEndFrame(arbitrary);
    check(end.success && end.frame == 400, "両trackの最大終端を計算できません");

    Project v2EndsLast;
    v2EndsLast.timelineClips = {clip("bottom", VideoTrack::V1, 0),
                                clip("top", VideoTrack::V2, 500)};
    const auto v2End = mvm::project::timelineEndFrame(v2EndsLast);
    check(v2End.success && v2End.frame == 600, "V2が最も遅い場合のtimeline終端を計算できません");
}

void testTransactionalMove() {
    Project project;
    auto first = clip("first", VideoTrack::V1, 0);
    first.sourceInFrame = 10;
    first.sourceOutFrame = 90;
    first.effects.positionXPercent = 12.5;
    first.effects.scalePercent = 60.0;
    first.effects.opacityPercent = 55.0;
    first.effects.fadeInFrames = 7;
    project.timelineClips = {first, clip("second", VideoTrack::V1, 200)};

    const auto original = project.timelineClips.front();
    const auto horizontal = mvm::project::moveClip(project, "first", VideoTrack::V1, 100);
    check(horizontal.success && project.timelineClips.front().timelineStartFrame == 100,
          "clipを水平方向へ移動できません");
    auto expected = original;
    expected.timelineStartFrame = 100;
    check(project.timelineClips.front() == expected,
          "水平移動でID、trim、effect、source metadataが変化しました");

    const auto beforeRejectedMove = project;
    check(!mvm::project::moveClip(project, "first", VideoTrack::V1, 150).success,
          "同一trackへ重なる移動を拒否しません");
    check(project.timelineClips == beforeRejectedMove.timelineClips,
          "拒否したmoveClipがProjectを部分変更しました");

    check(mvm::project::moveClip(project, "first", VideoTrack::V2, 200).success &&
              project.timelineClips.front().videoTrack == VideoTrack::V2,
          "clipをV1からV2へ移動できません");
    check(mvm::project::moveClip(project, "first", VideoTrack::V1, 100).success &&
              project.timelineClips.front().videoTrack == VideoTrack::V1,
          "clipをV2からV1へ移動できません");
}

void testRoundTrip(const std::filesystem::path& root) {
    Project project;
    auto bottom = clip("bottom", VideoTrack::V1, 120);
    auto top = clip("top", VideoTrack::V2, 40);
    bottom.mediaPath = root / "bottom.mp4";
    top.mediaPath = root / "top.mp4";
    top.kind = TimelineClipKind::Manim;
    top.effects.positionYPercent = -18.0;
    top.effects.scalePercent = 60.0;
    top.effects.rotationDegrees = 22.0;
    top.effects.opacityPercent = 70.0;
    top.effects.cropLeftPercent = 5.0;
    top.effects.fadeInFrames = 8;
    top.effects.fadeOutFrames = 9;
    project.timelineClips = {bottom, top};

    const auto path = root / "two-track.json";
    check(mvm::project::saveProjectJson(project, path).success,
          "two-track Projectを保存できません");
    const std::string savedText = readText(path);
    check(savedText.find("\"video_track\": 0") != std::string::npos &&
              savedText.find("\"video_track\": 1") != std::string::npos,
          "保存JSONが全clipのvideo_trackを書きません");
    const auto loaded = mvm::project::loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips == project.timelineClips,
          "V1/V2、任意start、ClipEffectsがJSON round-tripしません");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_two_track_project <work-directory>\n");
        return 2;
    }
    const auto root = std::filesystem::absolute(fromUtf8(argv[1]));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error) {
        std::fprintf(stderr, "テストdirectoryを作成できません\n");
        return 2;
    }

    testSchemaCompatibility(root);
    testValidationAndLookup();
    testTransactionalMove();
    testRoundTrip(root);
    if (failures) {
        std::fprintf(stderr, "M7b-1 two-track Project: %d件失敗\n", failures);
        return 1;
    }
    std::puts("M7b-1 two-track Project: PASS");
    return 0;
}
