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

constexpr mvm::project::TrackRef kV1{mvm::project::TrackKind::Video, 0};
constexpr mvm::project::TrackRef kV2{mvm::project::TrackKind::Video, 1};
constexpr mvm::project::TrackRef kA1{mvm::project::TrackKind::Audio, 0};

mvm::project::TimelineClip
clip(const char* name, mvm::project::TimelineClipKind kind = mvm::project::TimelineClipKind::Video,
     mvm::project::TrackRef track = kV1) {
    mvm::project::TimelineClip value;
    value.kind = kind;
    value.mediaPath = std::filesystem::path(name) += ".mp4";
    value.name = name;
    value.id = std::string("id-") + name;
    value.sourceFpsNum = 60;
    value.sourceFpsDen = 1;
    value.sourceFrameCount = 300;
    value.sourceInFrame = 0;
    value.sourceOutFrame = 300;
    value.timelineStartFrame = 0;
    value.track = track;
    return value;
}

mvm::project::Project threeClips() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {clip("A"), clip("Manim", mvm::project::TimelineClipKind::Manim),
                             clip("B")};
    project.timelineClips[0].timelineStartFrame = 0;
    project.timelineClips[1].timelineStartFrame = 300;
    project.timelineClips[2].timelineStartFrame = 600;
    return project;
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

    mvm::project::Project project = mvm::project::createDefaultProject();
    auto normalized = clip("normalized");
    auto equivalent = normalized;
    equivalent.sourceFpsNum = 120;
    equivalent.sourceFpsDen = 2;
    check(mvm::project::sourceRateMatchesTimelineRate(project, normalized) &&
              mvm::project::sourceRateMatchesTimelineRate(project, equivalent),
          "同値な60fps rateをPreview対応として判定できません");
}

// Project の timeline fps を 60 以外へ変えられること。対応表の外は拒否すること。
void testTimelineFrameRates() {
    check(mvm::project::isSupportedTimelineFrameRate(30, 1) &&
              mvm::project::isSupportedTimelineFrameRate(30000, 1001) &&
              mvm::project::isSupportedTimelineFrameRate(24, 1),
          "対応 rate を対応外と判定しました");
    check(!mvm::project::isSupportedTimelineFrameRate(48, 1) &&
              !mvm::project::isSupportedTimelineFrameRate(0, 1) &&
              !mvm::project::isSupportedTimelineFrameRate(60, 0),
          "対応外 rate を受理しました");
    check(mvm::project::isSupportedTimelineFrameRate(120, 2),
          "約分すれば対応 rate になる値を拒否しました");

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineFpsNum = 30;
    auto thirty = clip("thirty");
    thirty.sourceFpsNum = 30;
    project.timelineClips.push_back(thirty);
    const auto valid = mvm::project::validateTimeline(project);
    check(valid.success && valid.totalFrames == 300, "30fps Projectのtimelineを検証できません");

    project.timelineFpsNum = 48;
    check(!mvm::project::validateTimeline(project).success,
          "対応外 timeline fps の Project を受理しました");
}

void testTrimAndLookup() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto fractional = clip("fractional");
    fractional.id = "fractional-id";
    fractional.sourceFpsNum = 30000;
    fractional.sourceFpsDen = 1001;
    project.timelineClips.push_back(fractional);

    const auto validated = mvm::project::validateTimeline(project);
    check(validated.success && validated.totalFrames == 601,
          "29.97fps clipのtimeline durationが違います");
    check(mvm::project::timelineClipIndexAt(project, kV1, 0) == 0, "clip先頭をlookupできません");
    check(mvm::project::timelineClipIndexAt(project, kV1, 600) == 0,
          "clip末尾frameをlookupできません");
    check(mvm::project::timelineClipIndexAt(project, kV1, 601) == -1,
          "exclusive timeline終端をclip内と判定しました");
    check(mvm::project::timelineClipIndexAt(project, kV2, 0) == -1,
          "別trackのframeをclip内と判定しました");

    const auto left =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Left, 3);
    check(left.success && project.timelineClips.front().sourceInFrame == 1,
          "left trimをsource-native境界へsnapできません");
    const auto beforeInvalid = project;
    const auto invalid = mvm::project::trimTimelineClip(project, "fractional-id",
                                                        mvm::project::TrimEdge::Right, -10000);
    check(!invalid.success, "sourceOut <= sourceInになるtrimを拒否しません");
    project = beforeInvalid;

    auto unrelated = clip("unrelated", mvm::project::TimelineClipKind::Video, kV2);
    unrelated.timelineStartFrame = 77;
    unrelated.effects.opacityPercent = 42.0;
    project.timelineClips.push_back(unrelated);
    const auto beforeUnrelated = project.timelineClips.back();
    const auto beforeRightStart = project.timelineClips.front().timelineStartFrame;
    const auto right =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Right, -3);
    check(right.success && project.timelineClips.front().timelineStartFrame == beforeRightStart,
          "right trimでclip startが変化しました");
    check(project.timelineClips.back() == beforeUnrelated,
          "trimで無関係clipのtrack/start/effectsが変化しました");

    const auto oldEnd =
        project.timelineClips.front().timelineStartFrame +
        mvm::project::timelineClipDuration(project, project.timelineClips.front()).frame;
    const auto secondLeft =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Left, 4);
    const auto newEnd =
        project.timelineClips.front().timelineStartFrame +
        mvm::project::timelineClipDuration(project, project.timelineClips.front()).frame;
    check(secondLeft.success && oldEnd == newEnd, "left trimがclipの右端を維持しません");
    check(project.timelineClips.back() == beforeUnrelated,
          "left trimで無関係clipの配置が変化しました");
}

void testPlacementHelpers() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto base = clip("base");
    base.timelineStartFrame = 120;
    project.timelineClips.push_back(base);

    auto added = clip("added");
    added.timelineStartFrame = 999;
    const auto video = mvm::project::appendTimelineClip(project, added, kV1);
    check(video.success && project.timelineClips.back().track == kV1 &&
              project.timelineClips.back().timelineStartFrame == 420,
          "動画追加がV1の最大endへ配置されません");

    mvm::project::ManimAsset asset;
    asset.sceneName = "Overlay";
    asset.generatedVideoPath = "overlay.mp4";
    const auto manim =
        mvm::project::appendManimTimelineClipAt(project, asset, "overlay-id", 60, 1, 120, 180, kV2);
    check(manim.success && project.timelineClips.back().track == kV2 &&
              project.timelineClips.back().timelineStartFrame == 180,
          "Manim clipがplayhead指定位置のV2へ配置されません");
    check(!mvm::project::appendManimTimelineClipAt(project, asset, "overlay-id-2", 60, 1, 120, 400,
                                                   kV2)
               .success,
          "同じ Manim asset の重複配置を拒否しません");

    const auto beforeRejected = project;
    const auto rejected = mvm::project::moveClip(project, "id-added", kV1, 200);
    check(!rejected.success && project.timelineClips == beforeRejected.timelineClips,
          "same-track overlap拒否時にProjectが変化しました");

    const auto cross = mvm::project::moveClip(project, "id-added", kV2, 420);
    check(cross.success && project.timelineClips[1].track == kV2 &&
              project.timelineClips[1].timelineStartFrame == 420,
          "V1からV2へのcross-track移動ができません");
    const auto horizontal = mvm::project::moveClip(project, "id-added", kV2, 500);
    check(horizontal.success && project.timelineClips[1].timelineStartFrame == 500,
          "clipを同じtrack内で水平移動できません");
    const auto back = mvm::project::moveClip(project, "id-added", kV1, 420);
    check(back.success && project.timelineClips[1].track == kV1, "V2からV1へ移動できません");

    // video clip を audio track へ落とせない。track の種別が守られること。
    check(!mvm::project::moveClip(project, "id-added", kA1, 0).success,
          "video clipのaudio trackへの移動を拒否しません");
}

// track を任意に増減できること。clip の載った track を暗黙に消さないこと。
void testTrackEditing() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    check(project.videoTracks.size() == 2 && project.audioTracks.size() == 1,
          "既定Projectのtrack構成が違います");

    const auto addedVideo = mvm::project::addTrack(project, mvm::project::TrackKind::Video);
    check(addedVideo.success && project.videoTracks.size() == 3 &&
              project.videoTracks.back().name == "V3",
          "video trackを追加できません");
    const auto addedAudio = mvm::project::addTrack(project, mvm::project::TrackKind::Audio);
    check(addedAudio.success && project.audioTracks.size() == 2 &&
              project.audioTracks.back().name == "A2",
          "audio trackを追加できません");

    const auto muted = mvm::project::setTrackMuted(
        project, mvm::project::TrackRef{mvm::project::TrackKind::Video, 1}, true);
    check(muted.success && project.videoTracks[1].muted, "trackをミュートできません");

    // V3 に clip を置くと V3 は消せない。clip を勝手に消さないことの検査。
    auto onTop = clip("onTop", mvm::project::TimelineClipKind::Video,
                      mvm::project::TrackRef{mvm::project::TrackKind::Video, 2});
    project.timelineClips.push_back(onTop);
    check(mvm::project::validateTimeline(project).success, "V3上のclipを検証できません");
    const auto beforeRemove = project;
    check(!mvm::project::removeTrack(project,
                                     mvm::project::TrackRef{mvm::project::TrackKind::Video, 2})
               .success,
          "clipが載ったtrackの削除を拒否しません");
    check(project.videoTracks == beforeRemove.videoTracks,
          "拒否したtrack削除でProjectが変化しました");

    // 空の V2 を消すと V3 の clip が V2 へ繰り上がる。
    const auto removed = mvm::project::removeTrack(
        project, mvm::project::TrackRef{mvm::project::TrackKind::Video, 1});
    check(removed.success && project.videoTracks.size() == 2 && project.videoTracks[1].name == "V2",
          "空のvideo trackを削除できません");
    check(project.timelineClips.back().track.index == 1,
          "track削除後に後続trackのclip indexが詰められません");

    // video track は最低 1 本。0 本の Project を作らせない。
    mvm::project::Project single = mvm::project::createDefaultProject();
    single.videoTracks.resize(1);
    check(!mvm::project::removeTrack(single, kV1).success, "最後のvideo trackの削除を拒否しません");
}

// audio clip は audio track にだけ載る。逆も拒否する。
void testAudioClipPlacement() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto audio = clip("voice", mvm::project::TimelineClipKind::Audio, kA1);
    const auto placed = mvm::project::appendTimelineClip(project, audio, kA1);
    check(placed.success && project.timelineClips.back().track == kA1,
          "audio clipをaudio trackへ追加できません");
    check(mvm::project::validateTimeline(project).success, "audio clipを含むtimelineが不正です");

    auto misplaced = clip("misplaced", mvm::project::TimelineClipKind::Audio, kA1);
    check(!mvm::project::appendTimelineClip(project, misplaced, kV1).success,
          "audio clipのvideo trackへの追加を拒否しません");

    auto broken = project;
    broken.timelineClips.back().track = kV1;
    check(!mvm::project::validateTimeline(broken).success,
          "video trackに載ったaudio clipを検証で拒否しません");

    const auto activeAudio =
        mvm::project::activeClipsAt(project, mvm::project::TrackKind::Audio, 10);
    check(activeAudio.size() == 1 && activeAudio[0] != nullptr,
          "audio trackのactive clipを引けません");
    const auto activeVideo =
        mvm::project::activeClipsAt(project, mvm::project::TrackKind::Video, 10);
    check(activeVideo.size() == 2 && activeVideo[0] == nullptr && activeVideo[1] == nullptr,
          "video trackにclipが無いのにactive clipを返しました");
}

// 空白の ripple delete。詰める対象が無い場所は成功にしない。
void testRippleDelete() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto first = clip("first");
    first.timelineStartFrame = 0;
    auto second = clip("second");
    second.timelineStartFrame = 500; // 300..500 が空白
    auto third = clip("third");
    third.timelineStartFrame = 900; // 800..900 が空白
    auto other = clip("other", mvm::project::TimelineClipKind::Video, kV2);
    other.timelineStartFrame = 400;
    project.timelineClips = {first, second, third, other};
    check(mvm::project::validateTimeline(project).success, "ripple用のtimelineが不正です");

    const auto gap = mvm::project::gapAt(project, kV1, 400);
    check(gap.found && gap.start == 300 && gap.end == 500, "空白区間を正しく求められません");
    check(!mvm::project::gapAt(project, kV1, 100).found, "clip上をgapと判定しました");
    check(!mvm::project::gapAt(project, kV1, 1300).found,
          "後続clipが無い終端の空白をgapと判定しました");

    const auto rippled = mvm::project::rippleDeleteGap(project, kV1, 400);
    check(rippled.success, "空白をripple削除できません");
    check(project.timelineClips[0].timelineStartFrame == 0 &&
              project.timelineClips[1].timelineStartFrame == 300 &&
              project.timelineClips[2].timelineStartFrame == 700,
          "ripple削除で後続clipが詰められません");
    check(project.timelineClips[3].timelineStartFrame == 400,
          "ripple削除が他trackのclipまで動かしました");

    const auto beforeReject = project;
    check(!mvm::project::rippleDeleteGap(project, kV1, 100).success,
          "clip上のripple削除を拒否しません");
    check(project.timelineClips == beforeReject.timelineClips,
          "拒否したripple削除でProjectが変化しました");
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

    auto missingTrack = threeClips();
    missingTrack.timelineClips[0].track = mvm::project::TrackRef{mvm::project::TrackKind::Video, 7};
    check(!mvm::project::validateTimeline(missingTrack).success,
          "存在しないtrackを指すclipを拒否しません");

    auto noVideoTrack = threeClips();
    noVideoTrack.videoTracks.clear();
    check(!mvm::project::validateTimeline(noVideoTrack).success,
          "video trackが0本のProjectを拒否しません");

    auto gap = threeClips();
    gap.timelineClips[0].timelineStartFrame = 0;
    gap.timelineClips[1].timelineStartFrame = 400;
    gap.timelineClips[2].timelineStartFrame = 800;
    const auto gapResult = mvm::project::validateTimeline(gap);
    check(gapResult.success && gapResult.totalFrames == 1100, "timeline gapを受理しません");
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

    mvm::project::Project only = mvm::project::createDefaultProject();
    only.timelineClips.push_back(clip("only"));
    const auto onlyResult = mvm::project::deleteTimelineClip(only, 0);
    check(onlyResult.success && onlyResult.selectedIndex == -1 && only.timelineClips.empty(),
          "最後の clip 削除後が未選択になりません");

    const auto before = middle.timelineClips;
    check(!mvm::project::deleteTimelineClip(middle, 9).success, "不正 index の削除を拒否しません");
    check(middle.timelineClips == before, "拒否した削除で timeline が変化しました");
}

void testManimPlacement() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips.push_back(clip("A"));
    mvm::project::ManimAsset asset;
    asset.sceneName = "Scene";
    asset.generatedVideoPath = "scene.mp4";
    project.manimAssets.push_back(asset);

    const auto end = mvm::project::timelineTrackEndFrame(project, kV1);
    check(end.success && end.frame == 300, "track末尾frameが違います");
    const auto placed = mvm::project::appendManimTimelineClipAt(
        project, project.manimAssets.front(), "id-manim", 60, 1, 300, end.frame, kV1);
    check(placed.success && placed.selectedIndex == 1, "Manim clip を末尾へ配置できません");
    check(project.timelineClips.back().kind == mvm::project::TimelineClipKind::Manim,
          "配置した clip が Manim ではありません");
    check(!mvm::project::appendManimTimelineClipAt(project, project.manimAssets.front(),
                                                   "id-manim-2", 60, 1, 300, 600, kV1)
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
    candidate.timelineClips[1].sourceFpsNum = 30000;
    candidate.timelineClips[1].sourceFpsDen = 1001;
    candidate.timelineClips[1].sourceInFrame = 30;
    candidate.timelineClips[1].sourceOutFrame = 270;
    candidate.timelineClips[1].timelineStartFrame = 300;
    candidate.timelineClips[2].track = kV2;
    candidate.timelineClips[2].timelineStartFrame = 90;
    candidate.timelineClips[2].effects.scalePercent = 60.0;
    candidate.timelineClips[2].effects.opacityPercent = 55.0;
    check(mvm::project::addTrack(candidate, mvm::project::TrackKind::Audio).success,
          "保存用candidateへaudio trackを追加できません");
    candidate.audioTracks[0].muted = true;
    auto audio = clip("voice", mvm::project::TimelineClipKind::Audio, kA1);
    audio.timelineStartFrame = 42;
    candidate.timelineClips.push_back(audio);
    check(mvm::project::validateTimeline(candidate).success,
          "複数track配置とeffectsを持つ保存candidateが不正です");

    const auto projectFile = root / "project.mvm";
    const auto saved =
        mvm::project::saveProjectJsonTransaction(live, std::move(candidate), projectFile);
    check(saved.success, "編集済み Project を保存できません");
    const auto loaded = mvm::project::loadProjectJson(projectFile);
    check(loaded.success, "保存した .mvm を読み込めません");
    bool timelineFieldsMatch = loaded.success && loaded.project.schemaVersion == 3 &&
                               loaded.project.timelineFpsNum == 60 &&
                               loaded.project.timelineFpsDen == 1 &&
                               loaded.project.videoTracks == live.videoTracks &&
                               loaded.project.audioTracks == live.audioTracks &&
                               loaded.project.timelineClips.size() == live.timelineClips.size();
    if (timelineFieldsMatch) {
        for (std::size_t index = 0; index < live.timelineClips.size(); ++index) {
            const auto& actual = loaded.project.timelineClips[index];
            const auto& expected = live.timelineClips[index];
            timelineFieldsMatch =
                timelineFieldsMatch && actual.id == expected.id && actual.kind == expected.kind &&
                actual.sourceFpsNum == expected.sourceFpsNum &&
                actual.sourceFpsDen == expected.sourceFpsDen &&
                actual.sourceFrameCount == expected.sourceFrameCount &&
                actual.sourceInFrame == expected.sourceInFrame &&
                actual.sourceOutFrame == expected.sourceOutFrame &&
                actual.timelineStartFrame == expected.timelineStartFrame &&
                actual.track == expected.track && actual.effects == expected.effects;
        }
    }
    check(timelineFieldsMatch,
          "schema 3のtrack構成・mute・clip種別・trim・effectsがround-tripしません");

    // format marker が無いファイルは .mvm として受理しない。
    const auto strangerPath = root / "stranger.mvm";
    {
        std::ofstream stranger(strangerPath, std::ios::binary);
        stranger << R"({"schema_version": 3, "timeline_fps_num": 60, "timeline_fps_den": 1,)"
                 << R"("video_tracks": [{"name": "V1", "muted": false}], "audio_tracks": [],)"
                 << R"("manim_assets": [], "timeline_clips": []})";
    }
    check(!mvm::project::loadProjectJson(strangerPath).success,
          "format markerが無いファイルを .mvm として受理しました");

    const auto beforeFailure = live.timelineClips;
    auto failedCandidate = live;
    check(mvm::project::deleteTimelineClip(failedCandidate, 0).success,
          "失敗保存用 candidate を編集できません");
    std::ofstream blocker(root / "blocked", std::ios::binary);
    blocker << "file";
    blocker.close();
    const auto failed = mvm::project::saveProjectJsonTransaction(live, std::move(failedCandidate),
                                                                 root / "blocked" / "project.mvm");
    check(!failed.success, "保存不能 path への transaction が成功しました");
    check(live.timelineClips == beforeFailure, "保存失敗時に live Project が変化しました");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_timeline_edit <work-directory>\n");
        return 2;
    }
    testFrameConversions();
    testTimelineFrameRates();
    testTrimAndLookup();
    testPlacementHelpers();
    testTrackEditing();
    testAudioClipPlacement();
    testRippleDelete();
    testValidationFailures();
    testDeleteSelection();
    testManimPlacement();
    testPersistenceTransaction(fromUtf8(argv[1]));
    if (failures != 0) {
        std::fprintf(stderr, "timeline edit: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("timeline edit: PASS");
    return 0;
}
