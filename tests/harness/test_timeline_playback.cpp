#include "app/timeline_playback.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <limits>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

mvm::project::TimelineClip clip(const char* name, const char* id) {
    return {mvm::project::TimelineClipKind::Video,
            std::string(name) + ".mp4",
            name,
            id,
            {},
            60,
            1,
            60,
            0,
            60,
            0,
            {},
            {},
            {}};
}

mvm::project::Project threeClips() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {clip("A", "a"), clip("Manim", "manim"), clip("B", "b")};
    // V1 上に隙間なく並べる。start は helper に推測させず、ここで明示する。
    std::int64_t start = 0;
    for (auto& value : project.timelineClips) {
        value.timelineStartFrame = start;
        start += mvm::project::timelineClipDuration(project, value).frame;
    }
    check(mvm::project::validateTimeline(project).success, "テストtimelineを構築できません");
    return project;
}

void testClockMapping() {
    const auto atZero = mvm::app::timelineFrameFromElapsed(10, 0, 60, 1);
    const auto at16ms = mvm::app::timelineFrameFromElapsed(10, 16'000'000, 60, 1);
    const auto at17ms = mvm::app::timelineFrameFromElapsed(10, 17'000'000, 60, 1);
    const auto atSecond = mvm::app::timelineFrameFromElapsed(10, 1'000'000'000, 60, 1);
    check(atZero.success && atZero.frame == 10, "elapsed 0のbase frameが違います");
    check(at16ms.success && at16ms.frame == 10, "16msで早くframeを進めました");
    check(at17ms.success && at17ms.frame == 11, "17msで1frame進みません");
    check(atSecond.success && atSecond.frame == 70, "1秒で60frame進みません");

    const auto paused = mvm::app::timelineFrameFromElapsed(30, 500'000'000, 60, 1);
    const auto resumed = mvm::app::timelineFrameFromElapsed(paused.frame, 250'000'000, 60, 1);
    check(paused.success && resumed.success && paused.frame == 60 && resumed.frame == 75,
          "pause位置からのresume mappingが違います");

    check(!mvm::app::timelineFrameFromElapsed(-1, 0, 60, 1).success,
          "負のbase frameを拒否しません");
    check(!mvm::app::timelineFrameFromElapsed(0, -1, 60, 1).success, "負のelapsedを拒否しません");
    check(!mvm::app::timelineFrameFromElapsed(std::numeric_limits<std::int64_t>::max(),
                                              1'000'000'000, 60, 1)
               .success,
          "clock mappingのoverflowを拒否しません");
}

void testSegmentedTransitions() {
    const auto project = threeClips();
    const auto beforeAEnd = mvm::app::evaluateTimelinePlayback(project, 0, 59);
    check(beforeAEnd.success &&
              beforeAEnd.transition == mvm::app::TimelinePlaybackTransition::StayInClip &&
              beforeAEnd.frame == 59 && beforeAEnd.clipIndex == 0,
          "A終端直前でclipを切り替えました");

    const auto toManim = mvm::app::evaluateTimelinePlayback(project, 0, 60);
    check(toManim.success &&
              toManim.transition == mvm::app::TimelinePlaybackTransition::SwitchClip &&
              toManim.frame == 60 && toManim.clipIndex == 1,
          "A終端でManim先頭へ切り替わりません");

    const auto discardedOverrun = mvm::app::evaluateTimelinePlayback(project, 1, 155);
    check(discardedOverrun.success &&
              discardedOverrun.transition == mvm::app::TimelinePlaybackTransition::SwitchClip &&
              discardedOverrun.frame == 120 && discardedOverrun.clipIndex == 2,
          "境界overrunを破棄してB先頭へrebaseしません");

    const auto finished = mvm::app::evaluateTimelinePlayback(project, 2, 180);
    check(finished.success &&
              finished.transition == mvm::app::TimelinePlaybackTransition::Finished &&
              finished.frame == 180,
          "timeline exclusive endで停止しません");
    check(!mvm::app::evaluateTimelinePlayback(project, -1, 0).success,
          "不正active clipを拒否しません");
    check(!mvm::app::evaluateTimelinePlayback(project, 1, 59).success,
          "active clip先頭より前のclock frameを拒否しません");
}

void testCompatibility() {
    auto project = threeClips();
    check(mvm::app::timelinePreviewCompatible(project), "60fps timelineを再生不可にしました");
    check(mvm::app::timelineCanPlay(project, false, false, 0, 180),
          "再生可能なtimelineでcanPlayがfalseです");
    check(!mvm::app::timelineCanPlay(project, true, false, 0, 180), "busy中にcanPlayがtrueです");
    check(!mvm::app::timelineCanPlay(project, false, true, 0, 180), "再生中にcanPlayがtrueです");
    check(!mvm::app::timelineCanPlay(project, false, false, 180, 180),
          "timeline終端でcanPlayがtrueです");
    project.timelineClips[1].sourceFpsNum = 120;
    project.timelineClips[1].sourceFpsDen = 2;
    check(mvm::app::timelinePreviewCompatible(project), "120/2の同値rateを再生不可にしました");
    project.timelineClips[1].sourceFpsNum = 30000;
    project.timelineClips[1].sourceFpsDen = 1001;
    check(mvm::app::timelinePreviewCompatible(project),
          "異なるsource fpsを持つtimelineを再生不可にしました");
    check(mvm::app::timelineCanPlay(project, false, false, 0, 180),
          "異なるsource fpsを持つtimelineでcanPlayがfalseです");
}

void testShuttleClockAndEditPoints() {
    using mvm::app::adjacentTimelineEditPoint;
    using mvm::app::nextShuttleRate;
    using mvm::app::timelineShuttleFrameFromElapsed;
    check(nextShuttleRate(0, 1) == 1 && nextShuttleRate(1, 1) == 2 && nextShuttleRate(2, 1) == 4 &&
              nextShuttleRate(4, 1) == 8 && nextShuttleRate(8, 1) == 16 &&
              nextShuttleRate(16, 1) == 16,
          "右シャトルの加速段階が違います");
    check(nextShuttleRate(16, -1) == 8 && nextShuttleRate(4, -1) == 2 &&
              nextShuttleRate(1, -1) == 0 && nextShuttleRate(0, -1) == -1 &&
              nextShuttleRate(-1, -1) == -2 && nextShuttleRate(-2, -1) == -4,
          "左シャトルの減速・逆再生段階が違います");
    check(!nextShuttleRate(3, 1) && !nextShuttleRate(17, 1) && !nextShuttleRate(0, 0),
          "不正なシャトル入力を受理しました");
    const auto forward = timelineShuttleFrameFromElapsed(40, 250'000'000, 60, 1, 2, 179);
    const auto reverse = timelineShuttleFrameFromElapsed(40, 250'000'000, 60, 1, -4, 179);
    const auto atStart = timelineShuttleFrameFromElapsed(3, 1'000'000'000, 60, 1, -4, 179);
    const auto atEnd = timelineShuttleFrameFromElapsed(175, 1'000'000'000, 60, 1, 4, 179);
    check(forward.success && forward.frame == 70, "2倍速の進行位置が違います");
    check(reverse.success && reverse.frame == 0, "逆方向4倍速を先頭で止めません");
    check(atStart.success && atStart.frame == 0, "逆方向シャトルが先頭を越えました");
    check(atEnd.success && atEnd.frame == 179, "正方向シャトルが末尾を越えました");
    check(!timelineShuttleFrameFromElapsed(0, 0, 60, 1, 0, 179).success,
          "速度0のシャトルを受理しました");
    check(timelineShuttleFrameFromElapsed(0, 250'000'000, 60, 1, 16, 179).frame == 179,
          "16倍速の進行位置が違います");
    check(!timelineShuttleFrameFromElapsed(0, 0, 60, 1, 17, 179).success,
          "上限を超えるシャトルを受理しました");
    check(!timelineShuttleFrameFromElapsed(0, 0, 60, 1, 3, 179).success,
          "段階にないシャトル速度を受理しました");
    using mvm::app::timelineShuttleSampleAt;
    check(timelineShuttleSampleAt(100, 1, 2) == 102 && timelineShuttleSampleAt(100, 2, 2) == 104 &&
              timelineShuttleSampleAt(100, 4, 2) == 108,
          "正方向の音声sample間隔が違います");
    check(timelineShuttleSampleAt(100, -1, 2) == 98 && timelineShuttleSampleAt(100, -2, 2) == 96 &&
              timelineShuttleSampleAt(100, -4, 2) == 92,
          "逆再生の音声sample順序が違います");
    check(!timelineShuttleSampleAt(0, -1, 1) && !timelineShuttleSampleAt(100, 8, 1) &&
              !timelineShuttleSampleAt(100, 0, 1) &&
              !timelineShuttleSampleAt(std::numeric_limits<std::int64_t>::max(), 4, 1),
          "音声sample変換が範囲外の入力を受理しました");

    auto project = threeClips();
    auto upper = clip("Upper", "upper");
    upper.track = {mvm::project::TrackKind::Video, 1};
    upper.timelineStartFrame = 30;
    project.timelineClips.push_back(upper);
    const auto next = adjacentTimelineEditPoint(project, 0, 1, 179);
    const auto afterUpperStart = adjacentTimelineEditPoint(project, 30, 1, 179);
    const auto previous = adjacentTimelineEditPoint(project, 90, -1, 179);
    const auto finalEdge = adjacentTimelineEditPoint(project, 120, 1, 179);
    check(next.success && next.frame == 30, "別trackのclip先頭を飛ばしました");
    check(afterUpperStart.success && afterUpperStart.frame == 60,
          "現在位置と同じ編集点へ留まりました");
    check(previous.success && previous.frame == 60, "前の編集点を選べません");
    check(finalEdge.success && finalEdge.frame == 179, "末尾clipの終端へ移動できません");
    check(!adjacentTimelineEditPoint(project, 0, -1, 179).success,
          "先頭より前に編集点を見つけました");
    check(!adjacentTimelineEditPoint(project, 90, 0, 179).success, "方向0を受理しました");
    project.timelineMarkers = {45, 200};
    check(adjacentTimelineEditPoint(project, 30, 1, 200).frame == 45 &&
              adjacentTimelineEditPoint(project, 60, -1, 200).frame == 45 &&
              adjacentTimelineEditPoint(project, 180, 1, 200).frame == 200,
          "マーカーを前後の編集点として探索できません");
    check(!adjacentTimelineEditPoint(project, 200, 1, 200).success,
          "最終マーカーの先に編集点を見つけました");
    project.timelineClips.clear();
    check(adjacentTimelineEditPoint(project, 0, 1, 200).frame == 45,
          "clipが無いとマーカーへ移動できません");
    project.timelineMarkers = {201};
    check(!adjacentTimelineEditPoint(project, 0, 1, 200).success, "範囲外のマーカーを受理しました");
    project = threeClips();
    check(adjacentTimelineEditPoint(project, 60, 1, 179).frame == 120,
          "字幕の無い対照で次の編集点が違います");
    project.subtitles.emplace();
    project.subtitles->cues = {{"a", 70, 80, "前", {}}, {"b", 100, 200, "後", {}}};
    check(adjacentTimelineEditPoint(project, 60, 1, 179).frame == 70 &&
              adjacentTimelineEditPoint(project, 70, 1, 179).frame == 80 &&
              adjacentTimelineEditPoint(project, 80, 1, 179).frame == 100 &&
              adjacentTimelineEditPoint(project, 100, -1, 179).frame == 80 &&
              adjacentTimelineEditPoint(project, 60, -1, 179).frame == 0,
          "字幕の開始・終了を編集点として探索できません");
    check(adjacentTimelineEditPoint(project, 120, 1, 179).frame == 179 &&
              !adjacentTimelineEditPoint(project, 179, 1, 179).success,
          "末尾を越える字幕の終了を末尾へ丸めません");
    // lastFrame より先で始まる字幕も、結果を範囲の外へ出さない。全位置・両方向で確かめる。
    project.subtitles->cues = {{"a", 70, 80, "前", {}}, {"far", 250, 300, "先", {}}};
    check(adjacentTimelineEditPoint(project, 120, 1, 179).frame == 179,
          "lastFrame より先の字幕の開始を末尾へ丸めません");
    bool inRange = true;
    int compared = 0;
    for (std::int64_t at = 0; at <= 179; ++at)
        for (const int direction : {-1, 1}) {
            const auto point = adjacentTimelineEditPoint(project, at, direction, 179);
            if (!point.success)
                continue;
            ++compared;
            inRange = inRange && point.frame >= 0 && point.frame <= 179 &&
                      (direction > 0 ? point.frame > at : point.frame < at);
        }
    check(inRange && compared >= 300, "字幕の編集点が timeline の範囲外を返しました");
    project.subtitles->cues = {{"bad", 70, 70, "空", {}}};
    check(!adjacentTimelineEditPoint(project, 60, 1, 179).success,
          "不正な字幕の編集点を黙って飛ばしました");
    project = threeClips();
    project.timelineClips[0].sourceOutFrame = 0;
    check(!adjacentTimelineEditPoint(project, 30, 1, 179).success,
          "不正clipの編集点を黙って飛ばしました");
}

} // namespace

int main() {
    testClockMapping();
    testSegmentedTransitions();
    testCompatibility();
    testShuttleClockAndEditPoints();
    if (failures != 0) {
        std::fprintf(stderr, "M6b timeline playback: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("M6b timeline playback: PASS");
    return 0;
}
