#include "app/timeline_preview_mapping.h"
#include "project/timeline_edit.h"

#include <cmath>
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

// 映像 track を count 本にする (既定の Project は V1/V2)。
void ensureVideoTracks(mvm::project::Project& project, int count) {
    while (static_cast<int>(project.videoTracks.size()) < count)
        require(mvm::project::addTrack(project, mvm::project::TrackKind::Video).success,
                "映像 track を追加できません");
}

// 現在の構成の layer 上限を超えたら成功にしない。
// 上限は決め打ちの 8 本。期待値は実装の定数を参照せずに書く。
void testLayerLimit() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    ensureVideoTracks(project, 9);
    project.timelineClips.clear();
    for (int track = 0; track < 8; ++track)
        project.timelineClips.push_back(clip("v" + std::to_string(track + 1), track, 0, 0, 100));
    const auto eight = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(eight.success && eight.layers.size() == 8, "映像 8 本の重なりを拒否しました");
    for (std::size_t index = 0; index < eight.layers.size(); ++index)
        require(eight.layers[index].videoTrackIndex == static_cast<int>(index),
                "映像 8 本の layer が track 昇順ではありません");

    project.timelineClips.push_back(clip("v9", 8, 0, 0, 100));
    const auto tooMany = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(!tooMany.success && tooMany.layers.empty(),
            "configured layer数を超えたframeを成功にしました");

    // 9本目を mute すれば 8 layer に収まる。
    project.videoTracks[8].muted = true;
    const auto withinLimit = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(withinLimit.success && withinLimit.layers.size() == 8,
            "mute で layer 数が上限内に収まりません");
}

mvm::project::TimelineClip textClip(std::string id, int videoTrackIndex, std::int64_t duration) {
    mvm::project::TimelineClip value;
    value.kind = mvm::project::TimelineClipKind::Text;
    value.id = std::move(id);
    value.name = value.id;
    value.sourceFpsNum = 60;
    value.sourceFpsDen = 1;
    value.sourceFrameCount = duration;
    value.sourceOutFrame = duration;
    value.track = mvm::project::TrackRef{mvm::project::TrackKind::Video, videoTrackIndex};
    value.text.content = value.id;
    return value;
}

// 文字 clip は decode source ではなく stillLayers に入り、合成順は track index だけで決まる。
// 書き出しと同じく、文字より上の track の映像が文字を隠す順序になること。
void testTextLayerStack() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    require(mvm::project::addTrack(project, mvm::project::TrackKind::Video).success,
            "V3を追加できません");
    project.timelineClips = {clip("v3", 2, 0, 0, 100), textClip("t2", 1, 100),
                             clip("v1", 0, 0, 0, 100)};
    const auto mapped = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(mapped.success && mapped.layers.size() == 2 && mapped.stillLayers.size() == 1 &&
                mapped.stillLayers[0].clipId == "t2" && mapped.stillLayers[0].clipIndex == 1,
            "文字 clip を stillLayers として取り出せません");
    const auto stack = mvm::app::previewLayerStack(mapped);
    const std::vector<mvm::app::TimelinePreviewStackEntry> expected{
        {false, 0, 0}, {true, 0, 1}, {false, 1, 2}};
    require(stack == expected, "V1 映像 / V2 文字 / V3 映像の合成順が track 順ではありません");

    // 文字を最上段へ動かすと、文字が最前面になる (対照)。
    project.timelineClips[0].track.index = 1;
    project.timelineClips[1].track.index = 2;
    const auto moved = mvm::app::mapTimelinePreviewFrame(project, 10);
    const std::vector<mvm::app::TimelinePreviewStackEntry> textOnTop{
        {false, 0, 0}, {false, 1, 1}, {true, 0, 2}};
    require(moved.success && mvm::app::previewLayerStack(moved) == textOnTop,
            "V3 の文字が最前面になりません");

    // mute した track の文字は合成しない。
    project.videoTracks[2].muted = true;
    const auto muted = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(muted.success && muted.stillLayers.empty() && muted.layers.size() == 2,
            "mute した track の文字 clip を合成対象に残しました");
}

// 文字は decode source の上限 (8) ではなく合成 layer の上限 (16) で数える。
void testTextLayerLimit() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    ensureVideoTracks(project, 17);
    // 映像 8 本 (V1-V8) + 文字 8 枚 (V9-V16) = 16 layer。文字は V4 以上にも置ける。
    project.timelineClips.clear();
    for (int track = 0; track < 8; ++track)
        project.timelineClips.push_back(clip("v" + std::to_string(track + 1), track, 0, 0, 100));
    for (int track = 8; track < 16; ++track)
        project.timelineClips.push_back(textClip("t" + std::to_string(track + 1), track, 100));
    require(mvm::project::validateTimeline(project).success,
            "前提: V9-V16 文字の Project が不正です");
    const auto sixteen = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(sixteen.success && sixteen.layers.size() == 8 && sixteen.stillLayers.size() == 8,
            "映像 8 本と文字 8 枚の 16 layer を拒否しました");

    // 17 枚目の文字で合成 layer の上限を超える。
    project.timelineClips.push_back(textClip("t17", 16, 100));
    require(mvm::project::validateTimeline(project).success,
            "前提: 17 track の Project が不正です");
    const auto seventeen = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(!seventeen.success && seventeen.layers.empty() && seventeen.stillLayers.empty(),
            "合成 layer の上限を超えた frame を成功にしました");

    // 映像の無い frame の文字も engine が静止画 layer として合成するので、同じ上限で数える。
    project.timelineClips.clear();
    for (int track = 0; track < 16; ++track)
        project.timelineClips.push_back(textClip("t" + std::to_string(track + 1), track, 100));
    require(mvm::project::validateTimeline(project).success,
            "前提: 文字 16 枚の Project が不正です");
    const auto textOnly = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(textOnly.success && textOnly.layers.empty() && textOnly.stillLayers.size() == 16,
            "映像の無い frame の文字 16 枚を拒否しました");
    project.timelineClips.push_back(textClip("t17", 16, 100));
    const auto textOnlyOver = mvm::app::mapTimelinePreviewFrame(project, 10);
    require(!textOnlyOver.success && textOnlyOver.stillLayers.empty(),
            "映像の無い frame で合成 layer の上限を超えた文字を成功にしました");
}

// 文字の不透明度は書き出しと同じ effects (値・key・fade) を frame ごとに評価する。
// 期待値は key の直線補間を手で計算した値。
void testTextLayerOpacity() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto text = textClip("t1", 1, 60);
    text.timelineStartFrame = 10;
    text.effects.opacityKeys = {{0, 100.0}, {20, 0.0}};
    project.timelineClips = {clip("v1", 0, 0, 0, 100), text};
    require(mvm::project::validateTimeline(project).success,
            "前提: opacity key 付き文字が不正です");
    const auto at = [&](std::int64_t frame) {
        const auto mapped = mvm::app::mapTimelinePreviewFrame(project, frame);
        require(mapped.success && mapped.stillLayers.size() == 1, "文字を取り出せません");
        return mapped.stillLayers[0].opacity;
    };
    require(std::abs(at(10) - 1.0) < 1e-9, "key 0 (100%) の不透明度が 1 ではありません");
    require(std::abs(at(20) - 0.5) < 1e-9, "key の中間 (50%) を補間していません");
    require(std::abs(at(30)) < 1e-9, "key 20 (0%) の不透明度が 0 ではありません");

    // 対照: key の無い文字は 1。
    project.timelineClips[1].effects.opacityKeys.clear();
    require(std::abs(at(20) - 1.0) < 1e-9, "対照: key の無い文字の不透明度が 1 ではありません");
}

// 文字・画像は置いたときの fps を素材 frame domain として持ち、Project の fps を変えても
// 振り直さない。fade は素材 frame で数えるので、fps が違っても秒単位で同じ位置に効くこと。
// 期待値は clipFadeFactor の定義 (距離 / (fade frame 数 - 1)) から手で計算する。
void testStillFadeAcrossFrameRates() {
    using mvm::project::TimelineClipKind;

    struct Case {
        const char* name;
        std::int64_t projectFps;
        std::int64_t sourceFps;
        std::int64_t fadeFrames; // 素材 frame で 1 秒
        std::int64_t halfSecond; // timeline の 0.5 秒
        double halfExpected;     // 0.5 秒の fade in
        std::int64_t oneSecond;
        std::int64_t last;   // timeline の最終 frame
        double lastExpected; // 最終 frame の fade out
    };

    // 60fps で作った 5 秒 (300 frame) を 30fps の Project へ: timeline 15 = 素材 30 -> 30/59。
    // 最終 frame 149 = 素材 298 -> 末尾まで 1 frame -> 1/59。
    // 30fps で作った 5 秒 (150 frame) を 60fps の Project へ: timeline 30 = 素材 15 -> 15/29。
    // 最終 frame 299 = 素材 149 -> 末尾まで 0 frame -> 0。
    const Case cases[] = {
        {"60fps の clip を 30fps の Project で", 30, 60, 60, 15, 30.0 / 59.0, 30, 149, 1.0 / 59.0},
        {"30fps の clip を 60fps の Project で", 60, 30, 30, 30, 15.0 / 29.0, 60, 299, 0.0},
    };
    for (const auto& entry : cases) {
        for (const auto kind : {TimelineClipKind::Text, TimelineClipKind::Image}) {
            mvm::project::Project project = mvm::project::createDefaultProject();
            project.timelineFpsNum = entry.projectFps;
            project.timelineFpsDen = 1;
            auto still = textClip("still", 0, entry.sourceFps * 5);
            still.sourceFpsNum = entry.sourceFps;
            if (kind == TimelineClipKind::Image) {
                still.kind = TimelineClipKind::Image;
                still.text = {};
                still.mediaPath = "still.png";
            }
            const auto opacityAt = [&](std::int64_t frame) {
                const auto mapped = mvm::app::mapTimelinePreviewFrame(project, frame);
                require(mapped.success && mapped.stillLayers.size() == 1,
                        "fade の検査で静止画を取り出せません");
                return mapped.stillLayers[0].opacity;
            };
            const std::string label =
                std::string(entry.name) + (kind == TimelineClipKind::Image ? " (画像)" : " (文字)");

            still.effects.fadeInFrames = entry.fadeFrames;
            project.timelineClips = {still};
            require(mvm::project::validateTimeline(project).success,
                    ("前提: fade 付きの静止画が不正です: " + label).c_str());
            require(std::abs(opacityAt(entry.halfSecond) - entry.halfExpected) < 1e-9,
                    ("0.5 秒の fade in が素材 frame で評価されていません: " + label).c_str());
            require(std::abs(opacityAt(entry.oneSecond) - 1.0) < 1e-9,
                    ("1 秒で fade in が終わっていません: " + label).c_str());

            still.effects.fadeInFrames = 0;
            still.effects.fadeOutFrames = entry.fadeFrames;
            project.timelineClips = {still};
            require(std::abs(opacityAt(entry.last) - entry.lastExpected) < 1e-9,
                    ("最終 frame の fade out が素材 frame で評価されていません: " + label).c_str());
        }
    }
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
    testTextLayerStack();
    testTextLayerLimit();
    testTextLayerOpacity();
    testStillFadeAcrossFrameRates();
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
