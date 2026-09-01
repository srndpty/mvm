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
            60,
            1,
            60,
            0,
            60,
            0};
}

mvm::project::Project threeClips() {
    mvm::project::Project project;
    project.timelineClips = {clip("A", "a"), clip("Manim", "manim"), clip("B", "b")};
    const auto recomputed = mvm::project::recomputeTimelineStarts(project);
    check(recomputed.success, "テストtimelineを構築できません");
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
    check(!mvm::app::timelinePreviewCompatible(project),
          "29.97fpsを含むtimelineを再生可能にしました");
    check(!mvm::app::timelineCanPlay(project, false, false, 0, 180),
          "非対応clipを含むtimelineでcanPlayがtrueです");
}

} // namespace

int main() {
    testClockMapping();
    testSegmentedTransitions();
    testCompatibility();
    if (failures != 0) {
        std::fprintf(stderr, "M6b timeline playback: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("M6b timeline playback: PASS");
    return 0;
}
