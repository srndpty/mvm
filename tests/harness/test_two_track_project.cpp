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
using mvm::project::TrackKind;
using mvm::project::TrackRef;

constexpr TrackRef kV1{TrackKind::Video, 0};
constexpr TrackRef kV2{TrackKind::Video, 1};
constexpr TrackRef kA1{TrackKind::Audio, 0};

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

TimelineClip clip(const char* id, TrackRef track, std::int64_t start) {
    TimelineClip result;
    result.kind =
        track.kind == TrackKind::Audio ? TimelineClipKind::Audio : TimelineClipKind::Video;
    result.mediaPath = std::string(id) + ".mp4";
    result.name = id;
    result.id = id;
    result.sourceFpsNum = 60;
    result.sourceFpsDen = 1;
    result.sourceFrameCount = 100;
    result.sourceInFrame = 0;
    result.sourceOutFrame = 100;
    result.timelineStartFrame = start;
    result.track = track;
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

// schema 3 は track 配列と clip の track_kind / track_index を必須にする。
// 欠けたファイルを暗黙の既定値で読まないことを確認する。
void testSchemaIsFailClosed(const std::filesystem::path& root) {
    const std::string header =
        R"JSON({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,"timeline_fps_den":1,)JSON"
        R"JSON("video_tracks":[{"name":"V1","muted":false}],"audio_tracks":[],"manim_assets":[],)JSON";
    const std::string clipHead =
        R"JSON("timeline_clips":[{"kind":"video","media_path":"a.mp4","name":"A","id":"a",)JSON"
        R"JSON("source_fps_num":60,"source_fps_den":1,"source_frame_count":100,"source_in_frame":0,)JSON"
        R"JSON("source_out_frame":100,"timeline_start_frame":25)JSON";

    const auto complete = root / "complete.mvm";
    writeText(complete, header + clipHead + R"JSON(,"track_kind":"video","track_index":0}]})JSON");
    const auto loaded = mvm::project::loadProjectJson(complete);
    check(loaded.success && loaded.project.timelineClips.size() == 1 &&
              loaded.project.timelineClips.front().track == kV1,
          "schema 3のclip trackを読めません");

    const auto missingTrack = root / "missing-track.mvm";
    writeText(missingTrack, header + clipHead + "}]}");
    check(!mvm::project::loadProjectJson(missingTrack).success,
          "track_kind / track_index が欠けたclipを既定値で受理しました");

    const auto missingTracks = root / "missing-tracks.mvm";
    writeText(missingTracks,
              R"JSON({"schema_version":3,"format":"mvm-project","timeline_fps_num":60,)JSON"
              R"JSON("timeline_fps_den":1,"manim_assets":[],"timeline_clips":[]})JSON");
    check(!mvm::project::loadProjectJson(missingTracks).success,
          "track配列が無いProjectを受理しました");

    const auto unknownTrack = root / "unknown-track.mvm";
    writeText(unknownTrack,
              header + clipHead + R"JSON(,"track_kind":"video","track_index":3}]})JSON");
    check(!mvm::project::loadProjectJson(unknownTrack).success,
          "存在しないtrack indexを指すclipを受理しました");

    const auto oldSchema = root / "old-schema-2.mvm";
    writeText(oldSchema,
              R"JSON({"schema_version":2,"timeline_fps_num":60,"timeline_fps_den":1,)JSON"
              R"JSON("manim_assets":[],"timeline_clips":[]})JSON");
    check(!mvm::project::loadProjectJson(oldSchema).success,
          "schema 2のProjectを黙って読み込みました");
}

void testValidationAndLookup() {
    Project arbitrary = mvm::project::createDefaultProject();
    arbitrary.timelineClips = {clip("late", kV1, 300), clip("overlay", kV2, 50),
                               clip("early", kV1, 0)};
    const auto arbitraryResult = mvm::project::validateTimeline(arbitrary);
    check(arbitraryResult.success && arbitraryResult.totalFrames == 400,
          "vector順に依存せず任意startとgapを受理できません");

    auto negative = arbitrary;
    negative.timelineClips[0].timelineStartFrame = -1;
    check(!mvm::project::validateTimeline(negative).success, "負のstartを拒否しません");

    auto invalidTrack = arbitrary;
    invalidTrack.timelineClips[0].track = TrackRef{TrackKind::Video, 2};
    check(!mvm::project::validateTimeline(invalidTrack).success,
          "モデル上の不正track indexを拒否しません");

    Project sameTrack = mvm::project::createDefaultProject();
    sameTrack.timelineClips = {clip("a", kV1, 0), clip("b", kV1, 99)};
    check(!mvm::project::validateTimeline(sameTrack).success,
          "同一trackのhalf-open interval重複を拒否しません");

    Project crossTrack = mvm::project::createDefaultProject();
    crossTrack.timelineClips = {clip("a", kV1, 0), clip("b", kV2, 50)};
    check(mvm::project::validateTimeline(crossTrack).success, "track間の重複を受理しません");

    Project crossKind = mvm::project::createDefaultProject();
    crossKind.timelineClips = {clip("a", kV1, 0), clip("voice", kA1, 50)};
    check(mvm::project::validateTimeline(crossKind).success,
          "video/audio間で時間が重なるclipを拒否しました");

    Project touching = mvm::project::createDefaultProject();
    touching.timelineClips = {clip("a", kV1, 0), clip("b", kV1, 100)};
    check(mvm::project::validateTimeline(touching).success,
          "同一trackで終端と始端が接するclipを受理しません");

    const auto at75 = mvm::project::activeClipsAt(arbitrary, TrackKind::Video, 75);
    check(at75.size() == 2 && at75[0] && at75[0]->id == "early" && at75[1] &&
              at75[1]->id == "overlay",
          "V1/V2のactive clipを同時に取得できません");
    check(mvm::project::activeClipAt(arbitrary, kV2, 150) == nullptr,
          "half-open終端をactiveと判定しました");
    const auto gap = mvm::project::activeClipsAt(arbitrary, TrackKind::Video, 200);
    check(!gap[0] && !gap[1], "timeline gapでactive clipを返しました");

    const auto end = mvm::project::timelineEndFrame(arbitrary);
    check(end.success && end.frame == 400, "全trackの最大終端を計算できません");

    Project topEndsLast = mvm::project::createDefaultProject();
    topEndsLast.timelineClips = {clip("bottom", kV1, 0), clip("top", kV2, 500)};
    const auto topEnd = mvm::project::timelineEndFrame(topEndsLast);
    check(topEnd.success && topEnd.frame == 600,
          "上位trackが最も遅い場合のtimeline終端を計算できません");

    // track を 3 本以上へ増やしても同じ規則で扱えること。
    Project many = mvm::project::createDefaultProject();
    check(mvm::project::addTrack(many, TrackKind::Video).success, "video trackを追加できません");
    many.timelineClips = {clip("v1", kV1, 0), clip("v2", kV2, 0),
                          clip("v3", TrackRef{TrackKind::Video, 2}, 0)};
    check(mvm::project::validateTimeline(many).success, "3本のvideo trackを受理しません");
    const auto manyActive = mvm::project::activeClipsAt(many, TrackKind::Video, 10);
    check(manyActive.size() == 3 && manyActive[2] && manyActive[2]->id == "v3",
          "3本目のtrackのactive clipを引けません");
}

void testTransactionalMove() {
    Project project = mvm::project::createDefaultProject();
    auto first = clip("first", kV1, 0);
    first.sourceInFrame = 10;
    first.sourceOutFrame = 90;
    first.effects.positionXPercent = 12.5;
    first.effects.scalePercent = 60.0;
    first.effects.opacityPercent = 55.0;
    first.effects.fadeInFrames = 7;
    project.timelineClips = {first, clip("second", kV1, 200)};

    const auto original = project.timelineClips.front();
    const auto horizontal = mvm::project::moveClip(project, "first", kV1, 100);
    check(horizontal.success && project.timelineClips.front().timelineStartFrame == 100,
          "clipを水平方向へ移動できません");
    auto expected = original;
    expected.timelineStartFrame = 100;
    check(project.timelineClips.front() == expected,
          "水平移動でID、trim、effect、source metadataが変化しました");

    const auto beforeRejectedMove = project;
    check(!mvm::project::moveClip(project, "first", kV1, 150).success,
          "同一trackへ重なる移動を拒否しません");
    check(project.timelineClips == beforeRejectedMove.timelineClips,
          "拒否したmoveClipがProjectを部分変更しました");

    check(mvm::project::moveClip(project, "first", kV2, 200).success &&
              project.timelineClips.front().track == kV2,
          "clipをV1からV2へ移動できません");
    check(mvm::project::moveClip(project, "first", kV1, 100).success &&
              project.timelineClips.front().track == kV1,
          "clipをV2からV1へ移動できません");
}

void testRoundTrip(const std::filesystem::path& root) {
    Project project = mvm::project::createDefaultProject();
    check(mvm::project::addTrack(project, TrackKind::Video).success, "V3を追加できません");
    project.videoTracks[1].muted = true;
    auto bottom = clip("bottom", kV1, 120);
    auto top = clip("top", TrackRef{TrackKind::Video, 2}, 40);
    auto voice = clip("voice", kA1, 5);
    bottom.mediaPath = root / "bottom.mp4";
    top.mediaPath = root / "top.mp4";
    voice.mediaPath = root / "voice.wav";
    top.kind = TimelineClipKind::Manim;
    top.effects.positionYPercent = -18.0;
    top.effects.scalePercent = 60.0;
    top.effects.rotationDegrees = 22.0;
    top.effects.opacityPercent = 70.0;
    top.effects.cropLeftPercent = 5.0;
    top.effects.fadeInFrames = 8;
    top.effects.fadeOutFrames = 9;
    project.timelineClips = {bottom, top, voice};

    const auto path = root / "multi-track.mvm";
    check(mvm::project::saveProjectJson(project, path).success,
          "multi-track Projectを保存できません");
    const std::string savedText = readText(path);
    check(savedText.find("\"format\": \"mvm-project\"") != std::string::npos,
          "保存ファイルにformat markerが書かれていません");
    check(savedText.find("\"track_kind\": \"video\"") != std::string::npos &&
              savedText.find("\"track_kind\": \"audio\"") != std::string::npos &&
              savedText.find("\"track_index\": 2") != std::string::npos,
          "保存JSONが全clipのtrack_kind / track_indexを書きません");
    check(savedText.find("\"video_tracks\"") != std::string::npos &&
              savedText.find("\"audio_tracks\"") != std::string::npos &&
              savedText.find("\"muted\": true") != std::string::npos,
          "保存JSONがtrack構成とmuteを書きません");
    const auto loaded = mvm::project::loadProjectJson(path);
    check(loaded.success && loaded.project.timelineClips == project.timelineClips &&
              loaded.project.videoTracks == project.videoTracks &&
              loaded.project.audioTracks == project.audioTracks,
          "track構成、mute、任意start、ClipEffectsがJSON round-tripしません");
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

    testSchemaIsFailClosed(root);
    testValidationAndLookup();
    testTransactionalMove();
    testRoundTrip(root);
    if (failures) {
        std::fprintf(stderr, "multi-track Project: %d件失敗\n", failures);
        return 1;
    }
    std::puts("multi-track Project: PASS");
    return 0;
}
