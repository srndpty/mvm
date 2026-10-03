#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

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

mvm::project::TimelineClip audioClip(std::string id, int audioTrackIndex) {
    auto value = clip(std::move(id), 0, 0, 0, 100);
    value.kind = mvm::project::TimelineClipKind::Audio;
    value.mediaPath = value.id + ".wav";
    value.track = mvm::project::TrackRef{mvm::project::TrackKind::Audio, audioTrackIndex};
    return value;
}

// 書き出しの計画が選んだ clip の track と、preview が frame 10 に出す track の集合。
// 全 clip が [0, 100) にあるので、両者は同じ集合でなければならない。
using TrackSet = std::set<std::pair<int, int>>;

TrackSet exportTracks(const mvm::project::Project& project,
                      const mvm::app::TimelineExportPlan& plan) {
    TrackSet tracks;
    for (const auto& mapped : plan.clips) {
        const auto& track =
            project.timelineClips[static_cast<std::size_t>(mapped.projectClipIndex)].track;
        tracks.insert({static_cast<int>(track.kind), track.index});
    }
    return tracks;
}

TrackSet previewTracks(const mvm::project::Project& project) {
    TrackSet tracks;
    const auto video = mvm::app::mapTimelinePreviewFrame(project, 10);
    const auto audio = mvm::app::mapTimelinePreviewAudio(project, 10);
    require(video.success && audio.success, "前提: preview の mapping を作れません");
    for (const auto& layer : video.layers)
        tracks.insert({static_cast<int>(mvm::project::TrackKind::Video), layer.videoTrackIndex});
    for (const auto& layer : audio.layers)
        tracks.insert({static_cast<int>(mvm::project::TrackKind::Audio), layer.audioTrackIndex});
    return tracks;
}

// 目玉で隠した video track、ミュート・他 track のソロで鳴らない audio track は書き出さない。
// 見聞きしたもの (preview) と書き出しが同じ track を選ぶことを比べる。
void testTrackOutputMatchesPreview(const mvm::app::TimelineExportRequest& request) {
    using mvm::project::TrackKind;
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.audioTracks.push_back({"A2", false});
    project.timelineClips = {clip("v1", 0, 0, 0, 100), clip("v2", 1, 0, 0, 100), audioClip("a1", 0),
                             audioClip("a2", 1)};
    const auto compare = [&](const char* message, std::size_t expectedClips) {
        const auto plan = mvm::app::mapTimelineExportPlan(project, request);
        require(plan.success && plan.clips.size() == expectedClips &&
                    plan.totalDurationFrames == 100 &&
                    exportTracks(project, plan) == previewTracks(project),
                message);
        return plan;
    };
    compare("対照: 全 track を書き出しません", 4);

    project.videoTracks[1].muted = true;
    const auto hidden = compare("隠した V2 を書き出しの対象にしています", 3);
    require(hidden.backend == mvm::app::TimelineExportResult::Backend::Tractor,
            "隠した track の穴を埋める tractor を選びません");
    project.videoTracks[1].muted = false;

    project.audioTracks[1].solo = true;
    compare("ソロでない A1 を書き出しの対象にしています", 3);

    project.audioTracks[1].muted = true;
    compare("ミュートしたソロの A2 を書き出しの対象にしています", 2);

    // 何も出力しない状態は、黙って空の書き出しにしない。
    project.videoTracks[0].muted = project.videoTracks[1].muted = true;
    const auto none = mvm::app::mapTimelineExportPlan(project, request);
    require(!none.success && none.error.find("有効なclip") != std::string::npos,
            "出力する track が無いのに書き出す計画を作りました");
}

} // namespace

int main() {
    mvm::app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;

    mvm::project::Project contiguous = mvm::project::createDefaultProject();
    contiguous.timelineClips = {clip("later", 0, 10, 0, 10), clip("first", 0, 0, 5, 10)};
    const auto sequential = mvm::app::mapTimelineExportPlan(contiguous, request);
    require(sequential.success &&
                sequential.backend == mvm::app::TimelineExportResult::Backend::Sequential,
            "contiguous V1-onlyがsequential fast pathではありません");
    require(sequential.clips[0].projectClipIndex == 1 && sequential.clips[1].projectClipIndex == 0,
            "shuffled Project vectorをtimeline startで解決していません");

    {
        mvm::project::Project withTransition = mvm::project::createDefaultProject();
        auto outgoing = clip("out", 0, 0, 0, 50);
        outgoing.sourceFrameCount = 100;
        auto incoming = clip("in", 0, 50, 50, 50);
        withTransition.timelineClips = {outgoing, incoming};
        require(mvm::app::mapTimelineExportPlan(withTransition, request).success,
                "対照: トランジションの無い2clipを書き出せません");
        withTransition.timelineTransitions = {{"t", "out", "in", 10, 10}};
        require(mvm::project::validateTimeline(withTransition).success,
                "前提: 書き出し試験のトランジションが不正です");
        // cut = 50、区間 [40, 60)。期待値は手で数えた値である。
        //   layer 0: out を [0, 60) に延ばす (素材 0..60)、in の残りを [60, 100) (素材 60..100)
        //   layer 1: in の頭を [40, 60) (素材 40..60) に重ね、不透明度を (k + 0.5) / 20 で上げる
        const auto plan = mvm::app::mapTimelineExportPlan(withTransition, request);
        require(plan.success && plan.backend == mvm::app::TimelineExportResult::Backend::Tractor &&
                    plan.clips.size() == 3 && plan.totalDurationFrames == 100,
                "ディゾルブを3区間のtractorへmappingしません");
        if (plan.clips.size() == 3) {
            const auto& base = plan.clips[0];
            const auto& rest = plan.clips[1];
            const auto& over = plan.clips[2];
            require(base.projectClipIndex == 0 && base.videoTrackIndex == 0 &&
                        base.timelineStartFrame == 0 && base.timelineDurationFrames == 60 &&
                        base.renderClip.sourceInFrame == 0 && base.renderClip.sourceOutFrame == 60,
                    "outgoingを尻の余白へ延ばしていません");
            require(rest.projectClipIndex == 1 && rest.videoTrackIndex == 0 &&
                        rest.timelineStartFrame == 60 && rest.timelineDurationFrames == 40 &&
                        rest.renderClip.sourceInFrame == 60 &&
                        rest.renderClip.sourceOutFrame == 100,
                    "incomingの残りをlayer 0の区間の後ろへ置いていません");
            require(over.projectClipIndex == 1 && over.videoTrackIndex == 1 &&
                        over.timelineStartFrame == 40 && over.timelineDurationFrames == 20 &&
                        over.renderClip.sourceInFrame == 40 && over.renderClip.sourceOutFrame == 60,
                    "incomingの頭の区間をlayer 1へ重ねていません");
            require(over.opacityKeys.size() == 20 &&
                        std::abs(over.opacityKeys.front().opacity - 0.025) < 1e-12 &&
                        std::abs(over.opacityKeys[10].opacity - 0.525) < 1e-12 &&
                        std::abs(over.opacityKeys.back().opacity - 0.975) < 1e-12,
                    "incomingの不透明度が区間の中で上がりません");
            // 余白を黒で埋めて重ねるのは incoming の頭の区間だけ。
            require(over.opaqueBackdrop && !base.opaqueBackdrop && !rest.opaqueBackdrop,
                    "incomingの頭の区間だけを余白を黒で埋めて重ねていません");
        }
        // V2 は incoming を重ねる layer の上へずれる (V1 の lane 1 = layer 1、V2 = layer 2)。
        auto withUpper = withTransition;
        withUpper.timelineClips.push_back(clip("upper", 1, 0, 0, 10));
        const auto upperPlan = mvm::app::mapTimelineExportPlan(withUpper, request);
        require(upperPlan.success && upperPlan.clips.size() == 4 &&
                    upperPlan.clips.back().projectClipIndex == 2 &&
                    upperPlan.clips.back().videoTrackIndex == 2,
                "ディゾルブのあるtrackより上の映像trackをlayerの上へ置きません");
        // 2 倍速の outgoing は素材を 2 倍進めて延ばす (timeline 10 frame = 素材 20 frame)。
        auto fast = withTransition;
        fast.timelineClips[0].speedNum = 2;
        fast.timelineClips[0].sourceFrameCount = 200;
        fast.timelineClips[0].sourceOutFrame = 100;
        const auto fastPlan = mvm::app::mapTimelineExportPlan(fast, request);
        require(fastPlan.success && fastPlan.clips.size() == 3 &&
                    fastPlan.clips[0].timelineDurationFrames == 60 &&
                    fastPlan.clips[0].renderClip.sourceOutFrame == 120,
                "2倍速のoutgoingを素材の速度で延ばしません");
    }
    {
        // 音声のクロスフェード: 2 clip を重ねて等パワーの gain で加算する。
        mvm::project::Project crossfade = mvm::project::createDefaultProject();
        auto outgoing = clip("a-out", 0, 0, 0, 50);
        outgoing.sourceFrameCount = 100;
        auto incoming = clip("a-in", 0, 50, 50, 50);
        for (auto* value : {&outgoing, &incoming}) {
            value->kind = mvm::project::TimelineClipKind::Audio;
            value->track = {mvm::project::TrackKind::Audio, 0};
        }
        crossfade.timelineClips = {outgoing, incoming};
        crossfade.timelineTransitions = {{"ta", "a-out", "a-in", 10, 10}};
        const auto plan = mvm::app::mapTimelineExportPlan(crossfade, request);
        require(plan.success && plan.clips.size() == 2 && plan.clips[0].audio &&
                    plan.clips[0].timelineDurationFrames == 60 &&
                    plan.clips[1].timelineStartFrame == 40 &&
                    plan.clips[1].timelineDurationFrames == 60,
                "クロスフェードの2clipを延ばして重ねません");
        if (plan.success && plan.clips.size() == 2) {
            const auto& fadingOut = plan.clips[0].gainKeys;
            const auto& fadingIn = plan.clips[1].gainKeys;
            require(fadingOut[39].gain == 1.0 && fadingIn[20].gain == 1.0,
                    "クロスフェードの区間の外でgainを変えました");
            // frame 50 (p = 10.5 / 20): cos(0.525 * pi / 2) = 0.678801 (手で計算)。
            require(std::abs(fadingOut[50].gain - 0.678801) < 1e-5,
                    "outgoingのgainが等パワーで下がりません");
            bool powerKept = true;
            for (int frame = 40; frame < 60; ++frame) {
                const double outGain = fadingOut[static_cast<std::size_t>(frame)].gain;
                const double inGain = fadingIn[static_cast<std::size_t>(frame - 40)].gain;
                powerKept =
                    powerKept && std::abs(outGain * outGain + inGain * inGain - 1.0) < 1e-12;
            }
            require(powerKept, "クロスフェードの区間で2clipのgainの二乗和が1になりません");
        }
    }
    {
        // 無効にした clip は書き出さない。尺は timeline 全体のまま (穴は tractor が埋める)。
        mvm::project::Project disabledProject = mvm::project::createDefaultProject();
        disabledProject.timelineClips = {clip("first", 0, 0, 0, 10), clip("second", 0, 10, 0, 10)};
        const auto control = mvm::app::mapTimelineExportPlan(disabledProject, request);
        require(control.success && control.clips.size() == 2 &&
                    control.backend == mvm::app::TimelineExportResult::Backend::Sequential,
                "対照: 有効な2clipをsequentialで書き出せません");
        disabledProject.timelineClips[1].enabled = false;
        const auto disabled = mvm::app::mapTimelineExportPlan(disabledProject, request);
        require(disabled.success && disabled.clips.size() == 1 &&
                    disabled.clips[0].projectClipIndex == 0 && disabled.totalDurationFrames == 20 &&
                    disabled.backend == mvm::app::TimelineExportResult::Backend::Tractor,
                "無効clipを外し、timelineの尺のままtractorで書き出す計画になりません");
        disabledProject.timelineClips[0].enabled = false;
        const auto none = mvm::app::mapTimelineExportPlan(disabledProject, request);
        require(!none.success && none.error.find("有効なclip") != std::string::npos,
                "全clipが無効なのに書き出す計画を作りました");
    }

    auto gapProject = contiguous;
    gapProject.timelineClips[0].timelineStartFrame = 12;
    const auto gap = mvm::app::mapTimelineExportPlan(gapProject, request);
    require(gap.success && gap.backend == mvm::app::TimelineExportResult::Backend::Tractor,
            "V1-only gapがtractorを選びません");

    mvm::project::Project overlay = mvm::project::createDefaultProject();
    auto bottom = clip("bottom", 0, 0, 0, 100);
    bottom.effects.scaleXPercent = bottom.effects.scaleYPercent = 80;
    auto topLate = clip("top-late", 1, 60, 20, 20);
    auto topEarly = clip("top-early", 1, 10, 30, 20);
    topEarly.effects.opacityPercent = 50;
    topEarly.effects.cropLeftPercent = 10;
    topEarly.effects.fadeInFrames = 5;
    topEarly.effects.fadeOutFrames = 5;
    overlay.timelineClips = {topLate, bottom, topEarly};
    const auto tractor = mvm::app::mapTimelineExportPlan(overlay, request);
    require(tractor.success && tractor.backend == mvm::app::TimelineExportResult::Backend::Tractor,
            "V1+V2がtractorを選びません");
    require(tractor.clips.size() == 3 && tractor.clips[0].videoTrackIndex == 0 &&
                tractor.clips[1].timelineStartFrame == 10 &&
                tractor.clips[2].timelineStartFrame == 60,
            "track/start順のmappingが不正です");
    require(tractor.clips[0].effectsEnabled, "V1 M7a effectsを失いました");
    const auto& mappedTop = tractor.clips[1];
    require(mappedTop.projectClipIndex == 2 && mappedTop.timelineStartFrame == 10 &&
                mappedTop.timelineDurationFrames == 20,
            "V2 source-in/timeline start mappingが不正です");
    require(mappedTop.cropLeft == 32 && mappedTop.opacityKeys.front().localFrame == 0 &&
                mappedTop.opacityKeys.back().localFrame == 19,
            "V2 cropまたはtransition-local key domainが不正です");
    require(mappedTop.opacityKeys.front().opacity == 0.0 &&
                mappedTop.opacityKeys.back().opacity == 0.0,
            "clipFadeFactor由来のfade端値が不正です");
    require(tractor.clips[2].opacityKeys.front().localFrame == 0 &&
                tractor.clips[2].opacityKeys.back().localFrame == 19,
            "default V2にも全尺overlay transition mappingがありません");
    require(tractor.clips[1].timelineStartFrame + tractor.clips[1].timelineDurationFrames <
                tractor.clips[2].timelineStartFrame,
            "two V2 clip間gapのmapping fixtureが成立していません");

    // 回転の補正: 縦横の倍率が同じなら MLT の回転をそのまま使い (shear 0)、違えば shear で
    // 剛体回転に直す。実画素の一致は image_preview_parity が見る。
    {
        mvm::project::Project rotatedProject = mvm::project::createDefaultProject();
        auto uniform = clip("uniform", 0, 0, 0, 10);
        uniform.effects.scaleXPercent = uniform.effects.scaleYPercent = 70;
        uniform.effects.rotationDegrees = 30;
        rotatedProject.timelineClips = {uniform};
        const auto uniformPlan = mvm::app::mapTimelineExportPlan(rotatedProject, request);
        require(uniformPlan.success && uniformPlan.clips.size() == 1 &&
                    std::abs(uniformPlan.clips[0].rotationDegrees - 30.0) < 1e-9 &&
                    std::abs(uniformPlan.clips[0].shearDegrees) < 1e-9,
                "縦横同じ倍率の回転に shear の補正が掛かりました");
        rotatedProject.timelineClips[0].effects.scaleYPercent = 35;
        const auto stretchedPlan = mvm::app::mapTimelineExportPlan(rotatedProject, request);
        require(stretchedPlan.success && stretchedPlan.clips.size() == 1 &&
                    std::abs(stretchedPlan.clips[0].shearDegrees) > 1.0 &&
                    std::abs(stretchedPlan.clips[0].rotationDegrees - 30.0) > 1.0,
                "縦横別の倍率の回転に shear の補正が掛かりません");
    }

    // 30fps素材を60fps timelineのV2へ置くと、最終素材frameはtimeline 2 frameに跨る。
    // 端keyはclip末尾(timeline-local 19)に置き、最終素材frameのopacityを保持する。
    mvm::project::Project lowFps = mvm::project::createDefaultProject();
    auto lowFpsTop = clip("low-fps-top", 1, 0, 0, 10);
    lowFpsTop.sourceFpsNum = 30;
    lowFpsTop.effects.opacityPercent = 50;
    lowFpsTop.effects.fadeInFrames = 3;
    lowFpsTop.effects.fadeOutFrames = 3;
    lowFps.timelineClips = {clip("low-fps-bottom", 0, 0, 0, 20), lowFpsTop};
    const auto lowFpsPlan = mvm::app::mapTimelineExportPlan(lowFps, request);
    require(lowFpsPlan.success && lowFpsPlan.clips.size() == 2 &&
                lowFpsPlan.clips[1].timelineDurationFrames == 20,
            "30fps V2 fixtureのmappingが成立していません");
    const auto& lowFpsKeys = lowFpsPlan.clips[1].opacityKeys;
    require(lowFpsKeys.size() >= 2 && lowFpsKeys.front().localFrame == 0 &&
                lowFpsKeys.back().localFrame == 19,
            "timelineより低fpsのV2で端keyがclip末尾に置かれません");
    // 最終素材frame(source-local 9)はtimeline-local 18から始まり、19まで同じopacityを保つ。
    require(lowFpsKeys[lowFpsKeys.size() - 2].localFrame == 18 &&
                lowFpsKeys[lowFpsKeys.size() - 2].opacity == lowFpsKeys.back().opacity &&
                lowFpsKeys.back().opacity == 0.0,
            "最終素材frameのopacityを末尾まで保持しません");

    mvm::project::Project linkedAv = mvm::project::createDefaultProject();
    auto linkedVideo = clip("linked-video", 0, 0, 0, 100);
    linkedVideo.linkGroupId = "linked-pair";
    auto linkedAudio = linkedVideo;
    linkedAudio.id = "linked-audio";
    linkedAudio.name = "linked-audio";
    linkedAudio.kind = mvm::project::TimelineClipKind::Audio;
    linkedAudio.track = {mvm::project::TrackKind::Audio, 0};
    linkedAv.timelineClips = {linkedVideo, linkedAudio};
    const auto linkedPlan = mvm::app::mapTimelineExportPlan(linkedAv, request);
    require(linkedPlan.success &&
                linkedPlan.backend == mvm::app::TimelineExportResult::Backend::Tractor &&
                linkedPlan.clips.size() == 2 && !linkedPlan.clips[0].audio &&
                linkedPlan.clips[1].audio,
            "linked audioを独立audio trackへmappingできません");

    auto mismatchedLinkedAv = linkedAv;
    mismatchedLinkedAv.timelineClips[1].sourceInFrame = 1;
    mismatchedLinkedAv.timelineClips[1].timelineStartFrame = 10;
    const auto mismatchedLinkedPlan = mvm::app::mapTimelineExportPlan(mismatchedLinkedAv, request);
    require(mismatchedLinkedPlan.success && mismatchedLinkedPlan.clips.size() == 2 &&
                mismatchedLinkedPlan.clips[1].audio &&
                mismatchedLinkedPlan.clips[1].timelineStartFrame == 10,
            "videoとstart/trimが異なるlinked audioをmappingできません");

    auto standaloneAudio = linkedAudio;
    standaloneAudio.linkGroupId.clear();
    mvm::project::Project audioOnly = mvm::project::createDefaultProject();
    audioOnly.timelineClips = {standaloneAudio};
    const auto audioOnlyPlan = mvm::app::mapTimelineExportPlan(audioOnly, request);
    require(audioOnlyPlan.success && audioOnlyPlan.clips.size() == 1 &&
                audioOnlyPlan.clips[0].audio,
            "単独audio clipを独立trackへmappingできません");
    mvm::project::Project automated = mvm::project::createDefaultProject();
    auto picture = clip("automated-video", 0, 0, 0, 11);
    picture.effects.opacityKeys = {{0, 100}, {10, 0}};
    picture.effects.fadeInFrames = 3;
    auto sound = picture;
    sound.id = "automated-audio";
    sound.kind = mvm::project::TimelineClipKind::Audio;
    sound.track = {mvm::project::TrackKind::Audio, 0};
    sound.effects = {};
    sound.effects.volumeKeys = {{0, 0}, {10, 200}};
    automated.timelineClips = {picture, sound};
    const auto automationPlan = mvm::app::mapTimelineExportPlan(automated, request);
    require(automationPlan.success && automationPlan.clips.size() == 2 &&
                automationPlan.clips[0].opacityKeys.size() == 11 &&
                automationPlan.clips[0].opacityKeys[0].opacity == 0.0 &&
                automationPlan.clips[0].opacityKeys[5].opacity == 0.5 &&
                automationPlan.clips[1].gainKeys.size() == 11 &&
                automationPlan.clips[1].gainKeys[0].gain == 0.0 &&
                automationPlan.clips[1].gainKeys[5].gain == 1.0 &&
                automationPlan.clips[1].gainKeys[10].gain == 2.0,
            "手動カーブとフェードを出力フレームへ反映できません");
    picture.effects = {};
    picture.effects.positionXKeys = {{0, -20, mvm::project::KeyInterpolation::EaseIn}, {10, 20}};
    picture.effects.scaleXKeys = {{0, 50}, {10, 100}};
    automated.timelineClips = {picture};
    const auto motionPlan = mvm::app::mapTimelineExportPlan(automated, request);
    require(motionPlan.success && motionPlan.clips[0].motionFrames.size() == 11,
            "モーションを全出力フレームへ評価しません");
    if (motionPlan.success && motionPlan.clips[0].motionFrames.size() == 11) {
        const auto& midpoint = motionPlan.clips[0].motionFrames[5];
        require(std::abs(midpoint.rectWidth - request.width * 0.75) < 1e-9 &&
                    std::abs(midpoint.rectX - request.width * 0.025) < 1e-9,
                "中間フレームの拡大とイーズの位置が独立した期待値と違います");
    }
    auto cancelledRequest = request;
    cancelledRequest.progress = [](long long, long long) { return true; };
    require(!mvm::app::mapTimelineExportPlan(automated, cancelledRequest).success,
            "モーション書き出し準備をキャンセルできません");
    testTrackOutputMatchesPreview(request);
    return 0;
}
