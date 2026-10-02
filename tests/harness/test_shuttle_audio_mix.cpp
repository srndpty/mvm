// シャトル音声の sample 対応を、WASAPI と decoder を使わずに検査する。
// 素材 sample の供給元を差し替え、どの出力 sample にどの素材 sample が置かれたかを
// 値から読み戻す。期待値は実装の式を使わず、この test 内で直接書く。

#include "project/project.h"
#include "shuttle_audio_mix.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// 素材 sample 番号をそのまま振幅へ符号化する。L と R で符号を変え、channel の取り違えも見る。
float encodeSample(std::int64_t sourceSample) {
    return static_cast<float>(static_cast<double>(sourceSample) * 1e-6);
}

struct ReadCall {
    std::size_t clipIndex = 0;
    std::int64_t first = 0;
    std::int64_t count = 0;
};

mvm::app::ShuttleSourceReader encodingReader(std::vector<ReadCall>& calls) {
    return [&calls](std::size_t clipIndex, std::int64_t first, std::int64_t count,
                    std::vector<float>& pcm, std::string&) {
        calls.push_back({clipIndex, first, count});
        pcm.clear();
        for (std::int64_t i = 0; i < count; ++i) {
            pcm.push_back(encodeSample(first + i));
            pcm.push_back(-encodeSample(first + i));
        }
        return true;
    };
}

mvm::project::TimelineClip audioClip(const char* id, int trackIndex, std::int64_t timelineStart,
                                     std::int64_t sourceIn, std::int64_t sourceOut) {
    mvm::project::TimelineClip clip;
    clip.kind = mvm::project::TimelineClipKind::Audio;
    clip.id = id;
    clip.name = id;
    clip.mediaPath = std::string(id) + ".wav";
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 600;
    clip.sourceInFrame = sourceIn;
    clip.sourceOutFrame = sourceOut;
    clip.timelineStartFrame = timelineStart;
    clip.track = {mvm::project::TrackKind::Audio, trackIndex};
    return clip;
}

// 60fps / 48kHz なので 1 frame = 800 sample。
// A1: timeline frame [30, 270)、素材 frame 60 から。
//     timeline sample [24000, 216000)、素材 sample = timeline sample + 24000。
mvm::project::Project singleClipProject() {
    auto project = mvm::project::createDefaultProject();
    project.timelineClips.push_back(audioClip("a1", 0, 30, 60, 300));
    return project;
}

void testPlan() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 2, 40, plan, error), "planを作れません");
    check(plan.baseSample == 32000 && plan.endSample == 216000 && plan.rate == 2,
          "planのtimeline範囲が期待と一致しません");
    check(plan.clips.size() == 1 && plan.clips[0].timelineStartSample == 24000 &&
              plan.clips[0].timelineEndSample == 216000 && plan.clips[0].sourceOffset == 24000 &&
              plan.clips[0].path == "a1.wav",
          "clipのsample範囲または素材offsetが期待と一致しません");

    for (const int rate : {0, 3, 8, 16, -8}) {
        mvm::app::ShuttleAudioPlan rejected;
        std::string rateError;
        check(!mvm::app::planShuttleAudio(project, rate, 40, rejected, rateError) &&
                  !rateError.empty(),
              "音声経路の無い速度を拒否しません");
    }
    {
        auto disabled = project;
        disabled.timelineClips[0].enabled = false;
        mvm::app::ShuttleAudioPlan rejected;
        std::string disabledError;
        check(mvm::app::planShuttleAudio(disabled, 2, 40, rejected, disabledError) &&
                  rejected.clips.empty() && !mvm::app::hasShuttleAudibleClip(disabled),
              "無効にしたclipをシャトルで鳴らします");
    }
    mvm::app::ShuttleAudioPlan outside;
    std::string outsideError;
    check(!mvm::app::planShuttleAudio(project, 1, 270, outside, outsideError),
          "timeline終端以降の開始位置を拒否しません");
}

void testForwardMapping() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 2, 40, plan, error), "2倍速のplanを作れません");
    std::vector<ReadCall> calls;
    std::vector<float> pcm;
    check(mvm::app::mixShuttleBlock(plan, 100, 500, encodingReader(calls), pcm, error),
          "2倍速のblockを合成できません");
    // 出力 i -> timeline 32000 + 2 * (100 + i) -> 素材 +24000
    bool mapped = pcm.size() == 1000;
    for (std::int64_t i = 0; mapped && i < 500; ++i) {
        const auto source = 32000 + 2 * (100 + i) + 24000;
        mapped = pcm[static_cast<std::size_t>(i) * 2] == encodeSample(source) &&
                 pcm[static_cast<std::size_t>(i) * 2 + 1] == -encodeSample(source);
    }
    check(mapped, "2倍速で出力sampleと素材sampleの対応が崩れています");
    check(calls.size() == 1 && calls[0].first == 56200 && calls[0].count == 999,
          "2倍速で必要な素材範囲だけを1回で読んでいません");
}

void testReverseStopsAtClipStart() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, -1, 40, plan, error), "逆再生のplanを作れません");
    std::vector<ReadCall> calls;
    std::vector<float> pcm;
    // 出力 7990 + i -> timeline 24010 - i。clip 先頭 (24000) は i = 10。
    check(mvm::app::mixShuttleBlock(plan, 7990, 20, encodingReader(calls), pcm, error),
          "逆再生のblockを合成できません");
    bool mapped = pcm.size() == 40;
    for (std::int64_t i = 0; mapped && i <= 10; ++i)
        mapped = pcm[static_cast<std::size_t>(i) * 2] == encodeSample(24010 - i + 24000);
    for (std::int64_t i = 11; mapped && i < 20; ++i)
        mapped = pcm[static_cast<std::size_t>(i) * 2] == 0.0F &&
                 pcm[static_cast<std::size_t>(i) * 2 + 1] == 0.0F;
    check(mapped, "逆再生が素材を降順に並べないか、clip先頭より前を無音にしません");
    check(calls.size() == 1 && calls[0].first == 48000 && calls[0].count == 11,
          "逆再生で必要な素材範囲を読んでいません");
}

void testForwardStopsAtTimelineEnd() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 4, 260, plan, error), "4倍速のplanを作れません");
    std::vector<ReadCall> calls;
    std::vector<float> pcm;
    // 出力 i -> timeline 208000 + 4i。終端 216000 は i = 2000。
    check(mvm::app::mixShuttleBlock(plan, 0, 2010, encodingReader(calls), pcm, error),
          "4倍速のblockを合成できません");
    check(pcm[1999 * 2] == encodeSample(208000 + 4 * 1999 + 24000) && pcm[2000 * 2] == 0.0F &&
              pcm[2009 * 2] == 0.0F,
          "timeline終端より後ろを無音にしません");
}

void testMuteAndOverlap() {
    auto project = singleClipProject();
    project.audioTracks.push_back({"A2", false});
    project.audioTracks.push_back({"A3", true});
    // A2: timeline frame [0, 120) -> sample [0, 96000)。A1 と [24000, 96000) で重なる。
    project.timelineClips.push_back(audioClip("a2", 1, 0, 0, 120));
    project.timelineClips.push_back(audioClip("a3", 2, 0, 0, 300));
    check(mvm::app::hasShuttleAudibleClip(project), "鳴らせるclipを検出しません");

    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 1, 100, plan, error), "重なりのplanを作れません");
    check(plan.clips.size() == 2 && plan.clips[0].path == "a1.wav" &&
              plan.clips[1].path == "a2.wav",
          "muteされたtrackのclipを鳴らす対象に含めています");

    // clip ごとに一定値を返し、加算と clamp を見る。
    const auto constantReader = [](std::size_t clipIndex, std::int64_t, std::int64_t count,
                                   std::vector<float>& pcm, std::string&) {
        pcm.assign(static_cast<std::size_t>(count) * 2, clipIndex == 0 ? 0.7F : 0.6F);
        return true;
    };
    std::vector<float> pcm;
    // 出力 i -> timeline 80000 + 15990 + i。A2 の終端 96000 は i = 10。
    check(mvm::app::mixShuttleBlock(plan, 15990, 20, constantReader, pcm, error),
          "重なったclipを合成できません");
    check(pcm[0] == 1.0F && pcm[9 * 2 + 1] == 1.0F && pcm[10 * 2] == 0.7F &&
              pcm[19 * 2 + 1] == 0.7F,
          "重なったclipの加算・clampまたはclip終端の扱いが崩れています");

    // A2 だけを solo にすると A1 は鳴らさない。
    project.audioTracks[1].solo = true;
    mvm::app::ShuttleAudioPlan soloPlan;
    check(mvm::app::planShuttleAudio(project, 1, 100, soloPlan, error) &&
              soloPlan.clips.size() == 1 && soloPlan.clips[0].path == "a2.wav",
          "soloでないtrackのclipを鳴らす対象に含めています");
    project.audioTracks[1].solo = false;

    project.audioTracks[0].muted = true;
    project.audioTracks[1].muted = true;
    check(!mvm::app::hasShuttleAudibleClip(project), "全trackがmuteでも鳴らせる扱いにします");
    mvm::app::ShuttleAudioPlan silent;
    check(mvm::app::planShuttleAudio(project, 2, 100, silent, error) && silent.clips.empty(),
          "鳴らすclipが無いときのplanが空になりません");
}

void testVideoOnlyHasNoAudibleClip() {
    auto project = mvm::project::createDefaultProject();
    mvm::project::TimelineClip video;
    video.id = "video";
    video.name = "video";
    video.mediaPath = "video.mp4";
    video.sourceFpsNum = 60;
    video.sourceFpsDen = 1;
    video.sourceFrameCount = 120;
    video.sourceOutFrame = 120;
    project.timelineClips.push_back(video);
    check(!mvm::app::hasShuttleAudibleClip(project), "videoだけのtimelineを鳴らせる扱いにします");
}

void testRejectsInvalidSampleCount() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 1, 40, plan, error), "planを作れません");
    for (const std::int64_t sampleCount :
         {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(),
          std::numeric_limits<std::int64_t>::max()}) {
        std::vector<ReadCall> calls;
        std::vector<float> pcm;
        std::string countError;
        check(!mvm::app::mixShuttleBlock(plan, 0, sampleCount, encodingReader(calls), pcm,
                                         countError) &&
                  !countError.empty() && calls.empty() && pcm.empty(),
              "不正なblock sample数を拒否しないか、拒否する前に素材を読みます");
    }
}

void testReaderFailure() {
    const auto project = singleClipProject();
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 1, 40, plan, error), "planを作れません");
    std::vector<float> pcm;
    const auto failing = [](std::size_t, std::int64_t, std::int64_t, std::vector<float>&,
                            std::string& readError) {
        readError = "fixture decode failure";
        return false;
    };
    check(!mvm::app::mixShuttleBlock(plan, 0, 100, failing, pcm, error) &&
              error == "fixture decode failure",
          "素材読み取りの失敗を伝えません");
    const auto shortRead = [](std::size_t, std::int64_t, std::int64_t count,
                              std::vector<float>& source, std::string&) {
        source.assign(static_cast<std::size_t>(count), 0.1F);
        return true;
    };
    error.clear();
    check(!mvm::app::mixShuttleBlock(plan, 0, 100, shortRead, pcm, error) && !error.empty(),
          "要求より短い素材sampleを受け入れます");
}

void testAutomationGain() {
    auto project = singleClipProject();
    project.timelineClips[0].effects.volumeKeys = {{0, 0}, {20, 200}};
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 1, 30, plan, error),
          "音量カーブ付きシャトルplanを作れません");
    std::vector<ReadCall> calls;
    std::vector<float> pcm;
    check(mvm::app::mixShuttleBlock(plan, 0, 1, encodingReader(calls), pcm, error) &&
              pcm[0] == 0.0F,
          "シャトル音声の0%が無音になりません");
    check(mvm::app::mixShuttleBlock(plan, 8000, 1, encodingReader(calls), pcm, error) &&
              pcm[0] == encodeSample(56000),
          "シャトル音声の中間100%が一致しません");
    check(mvm::app::mixShuttleBlock(plan, 16000, 1, encodingReader(calls), pcm, error) &&
              pcm[0] == encodeSample(64000) * 2,
          "シャトル音声の200%が増幅されません");
}

void testIntermediatePan() {
    const auto reader = [](std::size_t, std::int64_t, std::int64_t count, std::vector<float>& pcm,
                           std::string&) {
        pcm.resize(static_cast<std::size_t>(count) * 2);
        for (std::size_t i = 0; i < pcm.size(); i += 2) {
            pcm[i] = 0.4F;
            pcm[i + 1] = 0.2F;
        }
        return true;
    };
    for (const double pan : {-0.5, 0.5}) {
        auto project = singleClipProject();
        project.audioTracks[0].mixerPan = pan;
        mvm::app::ShuttleAudioPlan plan;
        std::string error;
        std::vector<float> pcm;
        check(mvm::app::planShuttleAudio(project, 1, 40, plan, error) &&
                  mvm::app::mixShuttleBlock(plan, 0, 32, reader, pcm, error) && pcm.size() == 64 &&
                  std::abs(pcm[0] - (pan > 0 ? 0.2F : 0.4F)) < 1e-6F &&
                  std::abs(pcm[1] - (pan > 0 ? 0.2F : 0.1F)) < 1e-6F,
              "シャトル・スクラブの中間パンが非対称PCMへ正しく適用されません");
    }
}
} // namespace

// クロスフェード: 2 clip を余白の分だけ延ばして重ね、等パワーの gain で加算する。
// 区間 [90, 111) の中央 frame 100 では p = 10.5 / 21 = 0.5 で、両方の gain が 1/sqrt(2)。
void testCrossfade() {
    auto project = mvm::project::createDefaultProject();
    project.timelineClips = {audioClip("a-out", 0, 0, 0, 100), audioClip("a-in", 0, 100, 100, 200)};
    project.timelineTransitions = {{"t", "a-out", "a-in", 10, 11}};
    mvm::app::ShuttleAudioPlan plan;
    std::string error;
    check(mvm::app::planShuttleAudio(project, 1, 0, plan, error),
          "クロスフェードのplanを作れません");
    check(plan.clips.size() == 2 && plan.clips[0].timelineEndSample == 111 * 800 &&
              plan.clips[1].timelineStartSample == 90 * 800,
          "クロスフェードの2clipを余白の分だけ延ばしていません");
    const auto constantReader = [](std::size_t, std::int64_t, std::int64_t count,
                                   std::vector<float>& pcm, std::string&) {
        pcm.assign(static_cast<std::size_t>(count) * 2, 0.5F);
        return true;
    };
    std::vector<float> pcm;
    check(mvm::app::mixShuttleBlock(plan, 100 * 800 + 400, 1, constantReader, pcm, error) &&
              pcm.size() == 2 && std::abs(pcm[0] - 0.70710678F) < 1e-5F,
          "クロスフェードの中央で等パワーのgainで加算しません");
}

int main() {
    testPlan();
    testAutomationGain();
    testIntermediatePan();
    testForwardMapping();
    testReverseStopsAtClipStart();
    testForwardStopsAtTimelineEnd();
    testMuteAndOverlap();
    testCrossfade();
    testVideoOnlyHasNoAudibleClip();
    testRejectsInvalidSampleCount();
    testReaderFailure();
    if (failures != 0)
        return 1;
    std::puts("シャトル音声のsample対応: PASS");
    return 0;
}
