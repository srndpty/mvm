#include "project/project.h"
#include "project/timeline_edit.h"
#include "shuttle_audio_playback.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::exists(argv[1])) {
        std::fprintf(stderr, "音声テスト素材を指定してください\n");
        return 2;
    }
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip clip;
    clip.kind = mvm::project::TimelineClipKind::Audio;
    clip.mediaPath = argv[1];
    clip.name = "Shuttle audio";
    clip.id = "shuttle-audio";
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
        mvm::app::ShuttleAudioPlayback unsupported;
        std::string error;
        if (unsupported.start(project, 8, 120, 0.0F, error) || error.empty()) {
            std::fprintf(stderr, "8倍速の音声経路を拒否しません\n");
            return 1;
        }
    }
    for (const int rate : {1, 2, 4, -1, -2, -4}) {
        mvm::app::ShuttleAudioPlayback playback;
        std::string error;
        if (!playback.start(project, rate, 120, 0.0F, error)) {
            std::fprintf(stderr, "%d倍速の音声開始に失敗しました: %s\n", rate, error.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        const auto elapsed = playback.elapsedSamples();
        const auto audible = playback.nonSilentSamples();
        error = playback.error();
        playback.stop();
        if (elapsed <= 12000 || audible <= 24000 || !error.empty()) {
            std::fprintf(stderr, "%d倍速の音声が進行しません: elapsed=%lld audible=%llu %s\n", rate,
                         static_cast<long long>(elapsed), static_cast<unsigned long long>(audible),
                         error.c_str());
            return 1;
        }
    }
    std::puts("シャトル音声の6速度: PASS");
    return 0;
}
