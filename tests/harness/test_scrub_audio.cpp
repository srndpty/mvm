#include "project/project.h"
#include "project/timeline_edit.h"
#include "scrub_audio_playback.h"

#include <algorithm>
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
    // nonSilentSamples は endpoint へ渡す前の PCM で数える。実際に render されたかは
    // sink の meter peak で見る。meter は render した PCM から取り、Windows の session volume
    // (ここでは 0 にして無音で走らせている) より前なので、無音設定でも値が出る。
    float peakBeforeDrag = 0.0F;
    float peakDuringDrag = 0.0F;
    const auto observePeak = [&playback](float& peak) {
        const auto sink = playback.sinkSnapshot();
        peak = std::max({peak, sink.meterPeakLeft, sink.meterPeakRight});
    };
    // 位置が来る前は grain を作らない。
    for (int i = 0; i < 20; ++i) {
        observePeak(peakBeforeDrag);
        sleepMs(10);
    }
    const auto beforeDrag = playback.nonSilentSamples();
    const auto renderedBeforeDrag = playback.sinkSnapshot().audioRenderedSamples;
    // drag: 40 ms ごとに位置を進める (controller の scrub timer と同じ間隔)。
    for (std::int64_t frame = 30; frame < 90; frame += 6) {
        playback.setTarget(frame);
        for (int i = 0; i < 4; ++i) {
            observePeak(peakDuringDrag);
            sleepMs(10);
        }
    }
    for (int i = 0; i < 30; ++i) {
        observePeak(peakDuringDrag);
        sleepMs(10);
    }
    const auto afterDrag = playback.nonSilentSamples();
    const auto grainsAfterDrag = playback.grainCount();
    const auto renderedAfterDrag = playback.sinkSnapshot().audioRenderedSamples;
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
    // 後ろ (続きから読めず seek が要る) へ戻した直後に離す。decode 待ちで release
    // が引っ掛からないこと。
    const auto seekWaitsBefore = playback.seekWaitCount();
    playback.setTarget(5);
    // stop より先に scheduler が位置を拾わないと、decode を一度も通らずに PASS してしまう。
    // seek の完了待ちへ入ったことを観測してから止める。入らなければ失敗にする。
    // 待ちは回数ではなく経過時間で打ち切る。1 ms 未満の sleep は即座に返ることがあり、
    // 回数で数えると scheduler が動く前に打ち切ってしまう。
    bool enteredSeekWait = false;
    const auto seekDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!enteredSeekWait && std::chrono::steady_clock::now() < seekDeadline) {
        enteredSeekWait = playback.seekWaitCount() > seekWaitsBefore;
        if (!enteredSeekWait)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto stopBegin = std::chrono::steady_clock::now();
    playback.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stopBegin)
                            .count();

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
    if (peakBeforeDrag != 0.0F || peakDuringDrag < 0.01F ||
        renderedAfterDrag <= renderedBeforeDrag) {
        std::fprintf(stderr,
                     "endpointへscrub音声がrenderされていません: peak before=%.4f during=%.4f "
                     "rendered %llu -> %llu\n",
                     static_cast<double>(peakBeforeDrag), static_cast<double>(peakDuringDrag),
                     static_cast<unsigned long long>(renderedBeforeDrag),
                     static_cast<unsigned long long>(renderedAfterDrag));
        return 1;
    }
    if (!enteredSeekWait) {
        std::fprintf(stderr, "停止前にscrub音声のseek待ちへ入りませんでした\n");
        return 1;
    }
    if (stopMs > 100) {
        std::fprintf(stderr, "scrub音声の停止に時間がかかっています: %lld ms\n",
                     static_cast<long long>(stopMs));
        return 1;
    }
    std::printf("scrub音声: grains=%llu audible=%llu peak=%.4f stop=%lldms PASS\n",
                static_cast<unsigned long long>(grainsAfterDrag),
                static_cast<unsigned long long>(afterDrag), static_cast<double>(peakDuringDrag),
                static_cast<long long>(stopMs));
    return 0;
}
