#include "project/project.h"
#include "project/timeline_edit.h"
#include "scrub_audio_playback.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {

void sleepMs(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::exists(argv[1])) {
        std::fprintf(stderr, "音声テスト素材を指定してください\n");
        return 2;
    }
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip clip;
    clip.kind = mvm::project::TimelineClipKind::Audio;
    clip.mediaPath = argv[1];
    clip.name = "Scrub audio";
    clip.id = "scrub-audio";
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 300;
    clip.sourceInFrame = 0;
    clip.sourceOutFrame = 300;
    clip.timelineStartFrame = 0;
    clip.track = {mvm::project::TrackKind::Audio, 0};
    project.timelineClips.push_back(clip);
    if (!mvm::project::validateTimeline(project).success) {
        std::fprintf(stderr, "音声テスト用timelineが不正です\n");
        return 2;
    }
    {
        // 鳴らす clip が無ければ WASAPI を開かずに失敗する。
        auto muted = project;
        muted.audioTracks[0].muted = true;
        mvm::app::ScrubAudioPlayback playback;
        std::string error;
        if (playback.start(muted, 0.0F, error) || error.empty()) {
            std::fprintf(stderr, "mute trackだけのscrub音声を拒否しません\n");
            return 1;
        }
    }

    mvm::app::ScrubAudioPlayback playback;
    std::string error;
    if (!playback.start(project, 0.0F, error)) {
        std::fprintf(stderr, "scrub音声を開始できません: %s\n", error.c_str());
        return 1;
    }
    // 位置が来る前は grain を作らない。
    sleepMs(200);
    const auto beforeDrag = playback.nonSilentSamples();
    // drag: 40 ms ごとに位置を進める (controller の scrub timer と同じ間隔)。
    for (std::int64_t frame = 30; frame < 90; frame += 6) {
        playback.setTarget(frame);
        sleepMs(40);
    }
    sleepMs(300);
    const auto afterDrag = playback.nonSilentSamples();
    const auto grainsAfterDrag = playback.grainCount();
    // drag を止めて同じ位置が来続けても、新しい grain は鳴らさない。
    for (int i = 0; i < 5; ++i) {
        playback.setTarget(84);
        sleepMs(40);
    }
    sleepMs(300);
    const auto afterHold = playback.nonSilentSamples();
    const auto grainsAfterHold = playback.grainCount();
    // 供給が途切れた (underflow) 場合も error になる。
    error = playback.error();
    playback.stop();

    if (!error.empty()) {
        std::fprintf(stderr, "scrub音声が失敗しました: %s\n", error.c_str());
        return 1;
    }
    if (beforeDrag != 0) {
        std::fprintf(stderr, "drag前に音が出ています: %llu\n",
                     static_cast<unsigned long long>(beforeDrag));
        return 1;
    }
    if (grainsAfterDrag == 0 || afterDrag <= beforeDrag) {
        std::fprintf(stderr, "drag中に音が出ません: grains=%llu audible=%llu\n",
                     static_cast<unsigned long long>(grainsAfterDrag),
                     static_cast<unsigned long long>(afterDrag));
        return 1;
    }
    if (grainsAfterHold != grainsAfterDrag || afterHold != afterDrag) {
        std::fprintf(
            stderr, "drag停止後も音が出ています: grains %llu -> %llu, audible %llu -> %llu\n",
            static_cast<unsigned long long>(grainsAfterDrag),
            static_cast<unsigned long long>(grainsAfterHold),
            static_cast<unsigned long long>(afterDrag), static_cast<unsigned long long>(afterHold));
        return 1;
    }
    std::printf("scrub音声: grains=%llu audible=%llu PASS\n",
                static_cast<unsigned long long>(grainsAfterDrag),
                static_cast<unsigned long long>(afterDrag));
    return 0;
}
