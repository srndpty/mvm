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
    require(audioBefore.success && audioBefore.layers.size() == 1, "audio clipを取得できません");

    project.videoTracks[1].muted = true;
    const auto muted = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(muted.success && muted.layers.size() == 1 && muted.layers[0].videoTrackIndex == 0,
            "mute した video track を layer から外していません");

    project.audioTracks[0].muted = true;
    const auto audioMuted = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(audioMuted.success && audioMuted.layers.empty(),
            "mute した audio track を preview 対象から外していません");
}

// 現在の構成の layer 上限を超えたら成功にしない。
void testLayerLimit() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    require(mvm::project::addTrack(project, mvm::project::TrackKind::Video).success,
            "V3を追加できません");
    project.timelineClips = {clip("v1", 0, 0, 0, 100), clip("v2", 1, 0, 0, 100),
                             clip("v3", 2, 0, 0, 100)};
    const auto tooMany = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(!tooMany.success && tooMany.layers.empty(),
            "configured layer数を超えたframeを成功にしました");

    // 3本目を mute すれば 2 layer に収まる。
    project.videoTracks[2].muted = true;
    const auto withinLimit = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(withinLimit.success && withinLimit.layers.size() == 2,
            "mute で layer 数が上限内に収まりません");
}

// 重なったaudioをA1から順にすべてmix対象へ載せる。
void testAudioOverlapSelectsA1() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    require(mvm::project::addTrack(project, mvm::project::TrackKind::Audio).success,
            "A2を追加できません");
    auto second = audioClip("a2", 0, 0, 100, 60, 1);
    second.track = mvm::project::TrackRef{mvm::project::TrackKind::Audio, 1};
    project.timelineClips = {audioClip("a1", 0, 0, 100, 60, 1), second};
    const auto overlapped = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(overlapped.success && overlapped.layers.size() == 2 &&
                overlapped.layers[0].clipId == "a1" && overlapped.layers[1].clipId == "a2",
            "重なったaudioをtrack順のmix対象にできません");

    project.audioTracks[0].muted = true;
    const auto a1Muted = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(a1Muted.success && a1Muted.layers.size() == 1 && a1Muted.layers[0].clipId == "a2",
            "muteされたA1を飛ばしてA2を選択できません");
}

void testCrossRateVideoMapping() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto value = clip("23976", 0, 10, 100, 240);
    value.sourceFpsNum = 24000;
    value.sourceFpsDen = 1001;
    project.timelineClips = {value};

    // 書き出し (MLT) と同じ四捨五入。R = 60 * 1001 / 24000 = 2.5025、in = 100 の原点は
    // ceil(250.25) = 251。timeline 70 は位置 311 で 311 / 2.5025 = 124.28 -> 124。
    const auto atOneSecond = mvm::app::mapTimelinePreviewFrame(project, 70);
    require(atOneSecond.success && atOneSecond.layers.size() == 1 &&
                atOneSecond.layers[0].sourceFrameNumber == 124,
            "60fps timelineから23.976fps素材へframe換算できません");
    // 位置 251 -> 100.30、252 -> 100.70、253 -> 101.10。
    const auto first = mvm::app::mapTimelinePreviewFrame(project, 10);
    const auto second = mvm::app::mapTimelinePreviewFrame(project, 11);
    const auto repeated = mvm::app::mapTimelinePreviewFrame(project, 12);
    require(first.success && first.layers[0].sourceFrameNumber == 100 && second.success &&
                second.layers[0].sourceFrameNumber == 101 && repeated.success &&
                repeated.layers[0].sourceFrameNumber == 101,
            "高fps timelineで素材frameを四捨五入で選んでいません");
}

// 50% の clip。60fps 素材を 60fps へ置くと実効 fps は 30、R = 2。in = 10 の原点は 20 で、
// timeline t は位置 20 + t を 2 で割って四捨五入した frame を出す (書き出しの timewarp と同じ)。
//   t: 0 -> 10.0, 1 -> 10.5 -> 11, 2 -> 11.0, 3 -> 11.5 -> 12
void testSlowedClipMapping() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto slowed = clip("slowed", 0, 0, 10, 100);
    slowed.speedNum = 1;
    slowed.speedDen = 2;
    project.timelineClips = {slowed};
    const std::int64_t expected[] = {10, 11, 11, 12};
    for (std::int64_t t = 0; t < 4; ++t) {
        const auto mapped = mvm::app::mapTimelinePreviewFrame(project, t);
        require(mapped.success && mapped.layers.size() == 1 &&
                    mapped.layers[0].sourceFrameNumber == expected[t],
                "50%のclipの素材frameが四捨五入の対応と違います");
    }
    // 尺は 2 倍 (100 素材 frame -> 200)。
    require(mvm::app::mapTimelinePreviewFrame(project, 199).success &&
                mvm::app::mapTimelinePreviewFrame(project, 199).layers.size() == 1 &&
                mvm::app::mapTimelinePreviewFrame(project, 200).layers.empty(),
            "50%のclipの尺が2倍になりません");
    // 速度が違えば同じ素材・同じ原点でも source を使い回さない。
    auto normal = slowed;
    normal.speedDen = 1;
    require(!mvm::app::previewVideoMappingCovers(project, mvm::app::previewVideoMappingOf(normal),
                                                 slowed),
            "速度の違うclipのsourceを使い回しました");

    // audio は伸縮した時間軸の sample で数える。実効 30fps で素材 in = 10 は 10 / 30 秒 =
    // 16000 sample。timeline 0 に置いたので offset は 16000。
    auto sound = audioClip("slowed-audio", 0, 10, 100, 60, 1);
    sound.speedNum = 1;
    sound.speedDen = 2;
    const auto offset = mvm::app::audioPreviewSampleOffset(project, sound);
    require(offset.success && offset.sampleOffset == 16000,
            "50%のaudio clipのsample offsetが伸縮した時間軸になっていません");
}

// レーザーで分割した直後の連続した clip は、左半分の source のまま表示できる。
// 再生中に clip 境界で source を作り直すと、そこで一瞬止まる。
void testSplitClipReusesPreviewSource() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto whole = clip("whole", 0, 10, 100, 120);
    project.timelineClips = {whole};
    int counter = 0;
    require(mvm::project::splitTimelineClips(
                project, {"whole"}, 70, [&counter] { return "right-" + std::to_string(++counter); },
                mvm::project::LinkMode::Linked)
                    .success &&
                project.timelineClips.size() == 2,
            "mapping検査用のclipを分割できません");
    const auto& left = project.timelineClips[0];
    const auto& right = project.timelineClips[1];
    const auto leftSource = mvm::app::previewVideoMappingOf(left);
    require(mvm::app::previewVideoMappingCovers(project, leftSource, right),
            "分割直後の右半分を左半分のsourceで表示できると判定しません");
    // source は in より前の素材 frame を写せない。逆向きの使い回しは拒否する。
    require(
        !mvm::app::previewVideoMappingCovers(project, mvm::app::previewVideoMappingOf(right), left),
        "右半分のsourceでinより前の左半分を表示できると判定しました");

    // 右半分を 1 frame でも動かせば素材との対応が変わるので使い回さない。
    auto moved = right;
    moved.timelineStartFrame += 1;
    require(!mvm::app::previewVideoMappingCovers(project, leftSource, moved),
            "動かした右半分を左半分のsourceで表示できると判定しました");
    auto otherMedia = right;
    otherMedia.mediaPath = "other.mp4";
    require(!mvm::app::previewVideoMappingCovers(project, leftSource, otherMedia),
            "別素材のclipを同じsourceで表示できると判定しました");

    // 素材の絶対位置のずらし量 start - ceil(in R) が違えば使い回さない。60fps で分割した
    // in / start に 29.97fps を当てると、10 - ceil(200.2) と 70 - ceil(320.32) で一致しない。
    auto ntscLeft = left;
    auto ntscRight = right;
    ntscLeft.sourceFpsNum = ntscRight.sourceFpsNum = 30000;
    ntscLeft.sourceFpsDen = ntscRight.sourceFpsDen = 1001;
    require(!mvm::app::previewVideoMappingCovers(project, mvm::app::previewVideoMappingOf(ntscLeft),
                                                 ntscRight),
            "fpsが違う素材で原点の違うsourceを使い回しました");

    // 60fps timeline 上の 30fps 素材は timeline 2 frame = 素材 1 frame なので、分割直後の
    // 右半分も左半分の source のまま表示できる。以前は fps 一致を要求していて境界で組み直していた。
    mvm::project::Project halfRate = mvm::project::createDefaultProject();
    auto thirty = clip("thirty", 0, 0, 0, 300);
    thirty.sourceFpsNum = 30;
    halfRate.timelineClips = {thirty};
    require(mvm::project::splitTimelineClips(
                halfRate, {"thirty"}, 241,
                [&counter] { return "half-" + std::to_string(++counter); },
                mvm::project::LinkMode::Linked)
                    .success &&
                halfRate.timelineClips.size() == 2,
            "30fps素材のclipを分割できません");
    require(mvm::app::previewVideoMappingCovers(
                halfRate, mvm::app::previewVideoMappingOf(halfRate.timelineClips[0]),
                halfRate.timelineClips[1]),
            "60fps timeline上の30fps素材の分割直後を同じsourceで表示できると判定しません");
    // 29.97fps 素材も、実際に分割した右半分は左半分の source のまま表示できる。
    // 対応を素材の絶対位置で数えるので、trim の境界 ceil(in R) と必ず揃う。
    mvm::project::Project ntscRate = mvm::project::createDefaultProject();
    auto ntsc = clip("ntsc", 0, 0, 0, 300);
    ntsc.sourceFpsNum = 30000;
    ntsc.sourceFpsDen = 1001;
    ntscRate.timelineClips = {ntsc};
    require(mvm::project::splitTimelineClips(
                ntscRate, {"ntsc"}, 101, [&counter] { return "ntsc-" + std::to_string(++counter); },
                mvm::project::LinkMode::Linked)
                    .success &&
                ntscRate.timelineClips.size() == 2,
            "29.97fps素材のclipを分割できません");
    require(mvm::app::previewVideoMappingCovers(
                ntscRate, mvm::app::previewVideoMappingOf(ntscRate.timelineClips[0]),
                ntscRate.timelineClips[1]),
            "29.97fps素材の分割直後を同じsourceで表示できると判定しません");

    auto shiftedHalf = halfRate.timelineClips[1];
    shiftedHalf.timelineStartFrame += 1;
    require(!mvm::app::previewVideoMappingCovers(
                halfRate, mvm::app::previewVideoMappingOf(halfRate.timelineClips[0]), shiftedHalf),
            "1 frameずらした30fps素材の右半分を同じsourceで表示できると判定しました");

    // audio は offset が同じなら同じ source として扱える。
    const auto leftAudio = audioClip("take", 10, 100, 60, 60, 1);
    const auto rightAudio = audioClip("take", 70, 160, 60, 60, 1);
    const auto leftOffset = mvm::app::audioPreviewSampleOffset(project, leftAudio);
    const auto rightOffset = mvm::app::audioPreviewSampleOffset(project, rightAudio);
    require(leftOffset.success && rightOffset.success &&
                leftOffset.sampleOffset == rightOffset.sampleOffset,
            "分割直後のaudio clipのsample offsetが一致しません");
}

} // namespace

int main() {
    testSplitClipReusesPreviewSource();
    testSlowedClipMapping();
    testAudioPreviewSampleOffset();
    testAudioSourceFrameCount();
    testMutedTracks();
    testLayerLimit();
    testAudioOverlapSelectsA1();
    testCrossRateVideoMapping();

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
