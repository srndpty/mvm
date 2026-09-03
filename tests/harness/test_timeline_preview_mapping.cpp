#include "app/timeline_preview_mapping.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

mvm::project::TimelineClip clip(std::string id, int videoTrackIndex, std::int64_t start,
                                std::int64_t sourceIn, std::int64_t duration) {
    mvm::project::TimelineClip value;
    value.kind = mvm::project::TimelineClipKind::Video;
    value.id = std::move(id);
    value.name = value.id;
    value.mediaPath = value.id + ".mp4";
    value.sourceFpsNum = 60;
    value.sourceFpsDen = 1;
    value.sourceFrameCount = sourceIn + duration;
    value.sourceInFrame = sourceIn;
    value.sourceOutFrame = sourceIn + duration;
    value.timelineStartFrame = start;
    value.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, videoTrackIndex};
    return value;
}

mvm::project::TimelineClip audioClip(std::string id, std::int64_t start, std::int64_t sourceIn,
                                     std::int64_t duration, std::int64_t fpsNum,
                                     std::int64_t fpsDen) {
    mvm::project::TimelineClip value;
    value.kind = mvm::project::TimelineClipKind::Audio;
    value.id = std::move(id);
    value.name = value.id;
    value.mediaPath = value.id + ".wav";
    value.sourceFpsNum = fpsNum;
    value.sourceFpsDen = fpsDen;
    value.sourceFrameCount = sourceIn + duration;
    value.sourceInFrame = sourceIn;
    value.sourceOutFrame = sourceIn + duration;
    value.timelineStartFrame = start;
    value.track = mvm::project::TrackRef{mvm::project::TrackKind::Audio, 0};
    return value;
}

// audio の offset は source domain と timeline domain を別 timebase で sample 化する。
// 両方を Project fps で換算すると、素材固有 fps が違う clip で素材内位置がずれる。
void testAudioPreviewSampleOffset() {
    constexpr std::int64_t kSampleRate = 48000;

    // 60fps Project、素材も 60fps。sourceIn=60 (1秒)、start=120 (2秒)。
    // offset = 1秒 - 2秒 = -48000 sample。
    mvm::project::Project project = mvm::project::createDefaultProject();
    const auto sameRate = audioClip("voice", 120, 60, 300, 60, 1);
    const auto offset = mvm::app::audioPreviewSampleOffset(project, sameRate);
    require(offset.success && offset.sampleOffset == -kSampleRate,
            "同一rateのaudio offsetが違います");

    // clip を動かすと offset が変わる。identity に timelineStartFrame を含める根拠。
    auto moved = sameRate;
    moved.timelineStartFrame = 180; // 3秒
    const auto movedOffset = mvm::app::audioPreviewSampleOffset(project, moved);
    require(movedOffset.success && movedOffset.sampleOffset == -2 * kSampleRate,
            "clipを動かしてもaudio offsetが変わりません");

    // 左 trim しても offset が変わる。identity に sourceInFrame を含める根拠。
    auto trimmed = sameRate;
    trimmed.sourceInFrame = 120; // 2秒
    const auto trimmedOffset = mvm::app::audioPreviewSampleOffset(project, trimmed);
    require(trimmedOffset.success && trimmedOffset.sampleOffset == 0,
            "左trimしてもaudio offsetが変わりません");

    // **素材固有 fps が Project fps と違っても、素材内位置は素材の timebase で解釈する。**
    // sourceIn=60 が 60fps 素材なら 1 秒。Project を 24fps にしても 1 秒のままでなければ
    // ならない (Project fps で換算すると 2.5 秒になってしまう)。
    mvm::project::Project twentyFour = mvm::project::createDefaultProject();
    twentyFour.timelineFpsNum = 24;
    const auto crossRate = audioClip("cross", 0, 60, 300, 60, 1);
    const auto crossOffset = mvm::app::audioPreviewSampleOffset(twentyFour, crossRate);
    require(crossOffset.success && crossOffset.sampleOffset == kSampleRate,
            "素材固有fpsではなくProject fpsでsourceInFrameを換算しています");

    // timeline 側は Project timebase で読む。24fps で start=24 は 1 秒。
    const auto crossStart = audioClip("cross-start", 24, 0, 300, 60, 1);
    const auto crossStartOffset = mvm::app::audioPreviewSampleOffset(twentyFour, crossStart);
    require(crossStartOffset.success && crossStartOffset.sampleOffset == -kSampleRate,
            "timelineStartFrameをProject timebaseで換算していません");

    require(!mvm::app::audioPreviewSampleOffset(project, audioClip("bad", 0, 0, 10, 0, 1)).success,
            "不正なsource fpsを受理しました");
}

// 素材全体を含む clip を作るので ceil。floor だと必ず短くなる方向へ bias する。
void testAudioSourceFrameCount() {
    const auto exact = mvm::app::audioSourceFrameCount(1.0, 24, 1);
    require(exact.success && exact.frameCount == 24, "境界ちょうどの尺が違います");

    // 1.02 秒 @24fps = 24.48 frame。floor だと 24 frame になり末尾 20ms が消える。
    const auto partial = mvm::app::audioSourceFrameCount(1.02, 24, 1);
    require(partial.success && partial.frameCount == 25, "端数frameを切り捨てています");

    // 1 frame 未満の素材も 1 frame の clip として持てること。
    const auto tiny = mvm::app::audioSourceFrameCount(0.001, 24, 1);
    require(tiny.success && tiny.frameCount == 1, "1 frame未満の音声を保持できません");

    // 1001 分母でも同じ規則。
    const auto ntsc = mvm::app::audioSourceFrameCount(1.0, 30000, 1001);
    require(ntsc.success && ntsc.frameCount == 30, "29.97fpsの尺換算が違います");

    require(!mvm::app::audioSourceFrameCount(0.0, 24, 1).success, "尺0を受理しました");
    require(!mvm::app::audioSourceFrameCount(-1.0, 24, 1).success, "負の尺を受理しました");
    require(!mvm::app::audioSourceFrameCount(1.0, 0, 1).success, "fps 0を受理しました");
}

// mute した track を layer / audio から外すこと。
void testMutedTracks() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {clip("v1", 0, 0, 0, 100), clip("v2", 1, 0, 0, 100),
                             audioClip("a1", 0, 0, 100, 60, 1)};
    const auto before = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(before.success && before.layers.size() == 2, "2 layerを取得できません");
    const auto audioBefore = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(audioBefore.success && audioBefore.hasAudio, "audio clipを取得できません");

    project.videoTracks[1].muted = true;
    const auto muted = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(muted.success && muted.layers.size() == 1 && muted.layers[0].videoTrackIndex == 0,
            "mute した video track を layer から外していません");

    project.audioTracks[0].muted = true;
    const auto audioMuted = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(audioMuted.success && !audioMuted.hasAudio,
            "mute した audio track を preview 対象から外していません");
}

// preview が qualify している layer 数を超えたら成功にしない。
void testLayerLimit() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    require(mvm::project::addTrack(project, mvm::project::TrackKind::Video).success,
            "V3を追加できません");
    project.timelineClips = {clip("v1", 0, 0, 0, 100), clip("v2", 1, 0, 0, 100),
                             clip("v3", 2, 0, 0, 100)};
    const auto tooMany = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(!tooMany.success && tooMany.layers.empty(),
            "qualified layer数を超えたframeを成功にしました");

    // 3本目を mute すれば 2 layer に収まる。
    project.videoTracks[2].muted = true;
    const auto withinLimit = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(withinLimit.success && withinLimit.layers.size() == 2,
            "mute で layer 数が上限内に収まりません");
}

// 同一 frame に複数 audio が載ったら成功にしない (engine は 1 件しか受理しない)。
void testAudioOverlapRejected() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    require(mvm::project::addTrack(project, mvm::project::TrackKind::Audio).success,
            "A2を追加できません");
    auto second = audioClip("a2", 0, 0, 100, 60, 1);
    second.track = mvm::project::TrackRef{mvm::project::TrackKind::Audio, 1};
    project.timelineClips = {audioClip("a1", 0, 0, 100, 60, 1), second};
    const auto overlapped = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(!overlapped.success && !overlapped.hasAudio, "重なった audio を成功にしました");
}

} // namespace

int main() {
    testAudioPreviewSampleOffset();
    testAudioSourceFrameCount();
    testMutedTracks();
    testLayerLimit();
    testAudioOverlapRejected();

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {
        clip("v1-a", 0, 0, 100, 10),
        clip("v2-a", 1, 5, 20, 10),
        clip("v1-b", 0, 30, 200, 10),
    };

    const auto v1Only = mvm::app::mapTimelinePreviewFrame(project, 2);
    require(v1Only.success && v1Only.layers.size() == 1 && v1Only.layers[0].videoTrackIndex == 0 &&
                v1Only.layers[0].sourceFrameNumber == 102,
            "V1-only mappingが不正です");

    const auto both = mvm::app::mapTimelinePreviewFrame(project, 7);
    require(both.success && both.outputFrameNumber == 7 && both.layers.size() == 2,
            "V1+V2 mappingが不正です");
    require(both.layers[0].videoTrackIndex == 0 && both.layers[1].videoTrackIndex == 1,
            "composition orderがV1 bottom/V2 topではありません");
    require(both.layers[0].sourceFrameNumber == 107 && both.layers[1].sourceFrameNumber == 22,
            "共通output identityから異なるsource frameを計算できません");

    const auto v2Only = mvm::app::mapTimelinePreviewFrame(project, 12);
    require(v2Only.layers.size() == 1 && v2Only.layers[0].videoTrackIndex == 1,
            "V2-only mappingが不正です");
    const auto gap = mvm::app::mapTimelinePreviewFrame(project, 25);
    require(gap.success && gap.layers.empty() && gap.outputFrameNumber == 25,
            "gapを成功するempty mappingにしていません");
    const auto next = mvm::app::mapTimelinePreviewFrame(project, 30);
    require(next.layers.size() == 1 && next.layers[0].clipId == "v1-b" &&
                !mvm::app::sameTimelinePreviewSourceSet(gap, next),
            "source-set boundary changeを検出できません");
    require(
        mvm::app::sameTimelinePreviewSourceSet(both, mvm::app::mapTimelinePreviewFrame(project, 8)),
        "同一active source setを境界変更と誤認しました");
    require(!mvm::app::mapTimelinePreviewFrame(project, -1).success,
            "negative timeline frameを受理しました");
    return 0;
}
