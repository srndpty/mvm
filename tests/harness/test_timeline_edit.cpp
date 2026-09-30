#include "app/audio_source_set_transaction.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"
#include "test_media_fixture.h"
#include "util/mvm_win_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void testAudioSourceSetCompensation() {
    std::vector<int> current{1, 2, 3};
    const std::vector<int> desired{4};
    bool failedOnce = false;
    bool resetCalled = false;
    const auto result = mvm::app::replaceSourceSet(
        current, desired,
        [&](int value) {
            if (value == 2 && !failedOnce) {
                failedOnce = true;
                return false;
            }
            return true;
        },
        [](int requested, int& installed) {
            installed = requested;
            return true;
        },
        [&] {
            resetCalled = true;
            return true;
        });
    check(result == mvm::app::SourceSetReplaceResult::OperationFailedRestored &&
              current == std::vector<int>({1, 2, 3}) && !resetCalled,
          "途中remove失敗後にexact旧source setを再構築できません");

    current = {1, 2, 3};
    resetCalled = false;
    const auto resetResult = mvm::app::replaceSourceSet(
        current, desired, [](int value) { return value != 2; },
        [](int requested, int& installed) {
            installed = requested;
            return true;
        },
        [&] {
            resetCalled = true;
            return true;
        });
    check(resetResult == mvm::app::SourceSetReplaceResult::OperationFailedReset &&
              current.empty() && resetCalled,
          "旧source set再構築不能時にbackend resetでfail-closedにできません");
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    if (!wide)
        return {};
    std::filesystem::path result(wide);
    mvm_str_free(wide);
    return result;
}

constexpr mvm::project::TrackRef kV1{mvm::project::TrackKind::Video, 0};
constexpr mvm::project::TrackRef kV2{mvm::project::TrackKind::Video, 1};
constexpr mvm::project::TrackRef kV3{mvm::project::TrackKind::Video, 2};
constexpr mvm::project::TrackRef kA1{mvm::project::TrackKind::Audio, 0};

mvm::project::TimelineClip
clip(const char* name, mvm::project::TimelineClipKind kind = mvm::project::TimelineClipKind::Video,
     mvm::project::TrackRef track = kV1) {
    mvm::project::TimelineClip value;
    value.kind = kind;
    value.mediaPath = std::filesystem::path(name) += ".mp4";
    value.name = name;
    value.id = std::string("id-") + name;
    value.sourceFpsNum = 60;
    value.sourceFpsDen = 1;
    value.sourceFrameCount = 300;
    value.sourceInFrame = 0;
    value.sourceOutFrame = 300;
    value.timelineStartFrame = 0;
    value.track = track;
    return value;
}

mvm::project::Project threeClips() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {clip("A"), clip("Manim", mvm::project::TimelineClipKind::Manim),
                             clip("B")};
    project.timelineClips[0].timelineStartFrame = 0;
    project.timelineClips[1].timelineStartFrame = 300;
    project.timelineClips[2].timelineStartFrame = 600;
    return project;
}

void testSpeedDurationAndFrameHold() {
    using namespace mvm::project;
    Project project = threeClips();
    const Project before = project;
    ClipSpeedDurationEdit edit;
    edit.speedNum = 1;
    edit.speedDen = 2;
    check(!setClipSpeedDuration(project, "id-A", edit, LinkMode::Linked).success &&
              project == before,
          "リップル無効時の重なりを拒否し、Project を保てません");
    const auto overlapPreview = previewClipSpeedDuration(project, "id-A", edit, LinkMode::Linked);
    check(!overlapPreview.success && overlapPreview.overlapsFollowing,
          "後続 clip との重なりを上書き確認の対象として返しません");
    {
        // 50% で A は 0-600 になり、300-600 の Manim を丸ごと覆う。B (600-900) は触らない。
        Project overwritten = project;
        auto overwrite = edit;
        overwrite.overwrite = true;
        check(setClipSpeedDuration(overwritten, "id-A", overwrite, LinkMode::Linked).success &&
                  overwritten.timelineClips.size() == 2 &&
                  overwritten.timelineClips[0].id == "id-A" &&
                  overwritten.timelineClips[1].id == "id-B" &&
                  overwritten.timelineClips[1].timelineStartFrame == 600 &&
                  overwritten.timelineClips[1].sourceInFrame == 0,
              "覆われた clip を上書きで削除できません");
        // 40% で A は 0-750 になり、B の先頭 150 frame を削る (B の終端 900 は動かない)。
        overwritten = project;
        overwrite.speedNum = 2;
        overwrite.speedDen = 5;
        check(setClipSpeedDuration(overwritten, "id-A", overwrite, LinkMode::Linked).success &&
                  overwritten.timelineClips.size() == 2 &&
                  overwritten.timelineClips[1].timelineStartFrame == 750 &&
                  overwritten.timelineClips[1].sourceInFrame == 150 &&
                  overwritten.timelineClips[1].sourceOutFrame == 300,
              "はみ出す clip の左端を上書きで削れません");
        // 上書きの短縮は通常の左 trim と同じく effect を消さない。B (300 frame) を 150 frame へ
        // 縮める。fade in 100 は収まるので残す。fade in 200 + fade out 100 は収まらないので、
        // 末尾側の fade out 100 を残して fade in を 50 に詰める。
        const auto overwrittenFades = [&](std::int64_t fadeIn, std::int64_t fadeOut) {
            Project faded = project;
            faded.timelineClips[2].effects.fadeInFrames = fadeIn;
            faded.timelineClips[2].effects.fadeOutFrames = fadeOut;
            const bool ok =
                setClipSpeedDuration(faded, "id-A", overwrite, LinkMode::Linked).success;
            const auto b = std::find_if(faded.timelineClips.begin(), faded.timelineClips.end(),
                                        [](const auto& value) { return value.id == "id-B"; });
            return ok && b != faded.timelineClips.end()
                       ? std::pair{b->effects.fadeInFrames, b->effects.fadeOutFrames}
                       : std::pair<std::int64_t, std::int64_t>{-1, -1};
        };
        check(overwrittenFades(100, 0) == std::pair<std::int64_t, std::int64_t>{100, 0},
              "上書きで短縮した clip の fade in が消えました");
        check(overwrittenFades(200, 100) == std::pair<std::int64_t, std::int64_t>{50, 100},
              "上書きで短縮した clip の fade が縮めた尺へ収まりません");
        // 削除した clip のリンク相手は片方だけのリンクにならないよう未リンクにする。
        overwritten = project;
        overwritten.timelineClips[1].kind = TimelineClipKind::Video;
        overwritten.timelineClips[1].linkGroupId = "covered";
        auto partner = clip("partner", TimelineClipKind::Audio, kA1);
        partner.timelineStartFrame = 300;
        partner.linkGroupId = "covered";
        overwritten.timelineClips.push_back(partner);
        overwrite.speedNum = 1;
        overwrite.speedDen = 2;
        check(setClipSpeedDuration(overwritten, "id-A", overwrite, LinkMode::Linked).success &&
                  overwritten.timelineClips.size() == 3 &&
                  overwritten.timelineClips[2].id == "id-partner" &&
                  overwritten.timelineClips[2].linkGroupId.empty(),
              "上書きで消した clip のリンク相手を未リンクにできません");
    }
    edit.ripple = true;
    const auto changed = setClipSpeedDuration(project, "id-A", edit, LinkMode::Linked);
    check(changed.success && project.timelineClips[0].timelineStartFrame == 0 &&
              timelineClipDuration(project, project.timelineClips[0]).frame == 600 &&
              project.timelineClips[1].timelineStartFrame == 600 &&
              project.timelineClips[2].timelineStartFrame == 900,
          "50% のリップル速度変更が正しくありません");
    edit.speedNum = 1;
    edit.speedDen = 1;
    check(setClipSpeedDuration(project, "id-A", edit, LinkMode::Linked).success &&
              project == before,
          "速度を元に戻して後続 clip を詰められません");
    check(!setClipSpeedDuration(project, "id-A", edit, LinkMode::Linked).success &&
              project == before,
          "変化のない速度指定を拒否できません");
    {
        // 等速のピッチ保持は preview だけが stretcher を通す食い違いになるので持たせない。
        Project pitched = before;
        pitched.timelineClips[0].preservePitch = true;
        check(!validateTimeline(pitched).success, "等速のピッチ保持を受理しました");
        pitched = before;
        ClipSpeedDurationEdit pitch;
        pitch.speedNum = 1;
        pitch.speedDen = 2;
        pitch.preservePitch = true;
        pitch.ripple = true;
        check(setClipSpeedDuration(pitched, "id-A", pitch, LinkMode::Linked).success &&
                  pitched.timelineClips[0].preservePitch,
              "50% のピッチ保持を設定できません");
        pitch.speedDen = 1;
        check(setClipSpeedDuration(pitched, "id-A", pitch, LinkMode::Linked).success &&
                  !pitched.timelineClips[0].preservePitch && validateTimeline(pitched).success,
              "等速へ戻したときにピッチ保持を落とせません");
    }

    Project held = createDefaultProject();
    held.timelineClips = {clip("hold-source")};
    int id = 0;
    const auto inserted = insertFrameHold(held, "id-hold-source", 120, 120,
                                          [&] { return "new-" + std::to_string(++id); });
    check(inserted.success && held.timelineClips.size() == 3 &&
              held.timelineClips[0].sourceOutFrame == 120 &&
              held.timelineClips[1].timelineStartFrame == 240 &&
              held.timelineClips[1].sourceInFrame == 120 && held.timelineClips[2].frameHold &&
              held.timelineClips[2].frameHold->sourceFrame == 120 &&
              held.timelineClips[2].timelineStartFrame == 120 &&
              clipSourceFrameAt(held.timelineClips[2], 60, 1, 119).frame == 120,
          "フレーム保持の分割・配置・素材 frame が正しくありません");
    {
        // 保持は挿入位置の見た目 (不透明度) で止める。fade-in 240 frame の中央 (120) なら約 50%、
        // key 0:100% -> 200:0% の 120 なら 40%。automation は捨て、この値を基本値へ焼き込む。
        const auto heldOpacity = [&](const std::function<void(TimelineClip&)>& setup) {
            Project faded = createDefaultProject();
            faded.timelineClips = {clip("fade-source")};
            setup(faded.timelineClips[0]);
            int fadeId = 0;
            const auto result = insertFrameHold(faded, "id-fade-source", 120, 120,
                                                [&] { return "fade-" + std::to_string(++fadeId); });
            const auto hold = std::find_if(faded.timelineClips.begin(), faded.timelineClips.end(),
                                           [](const auto& value) { return value.frameHold; });
            return result.success && hold != faded.timelineClips.end() &&
                           hold->effects.opacityKeys.empty() && hold->effects.fadeInFrames == 0 &&
                           hold->effects.fadeOutFrames == 0
                       ? hold->effects.opacityPercent
                       : -1.0;
        };
        const double fadeHeld =
            heldOpacity([](TimelineClip& value) { value.effects.fadeInFrames = 240; });
        check(std::abs(fadeHeld - 50.0) <= 1.0,
              ("fade の途中の保持が挿入位置の不透明度になりません: " + std::to_string(fadeHeld))
                  .c_str());
        const double keyHeld = heldOpacity(
            [](TimelineClip& value) { value.effects.opacityKeys = {{0, 100.0}, {200, 0.0}}; });
        check(std::abs(keyHeld - 40.0) <= 1.0,
              ("key の途中の保持が挿入位置の不透明度になりません: " + std::to_string(keyHeld))
                  .c_str());
    }
    const Project heldBefore = held;
    check(!insertFrameHold(
               held, "id-hold-source", 0, 120,
               [&] {
                   return "bad-" + std::to_string(++id);
               }).success &&
              held == heldBefore,
          "clip の端へのフレーム保持を拒否できません");

    auto invalidHold = held;
    invalidHold.timelineClips[2].speedNum = 2;
    check(!validateTimeline(invalidHold).success, "保持 clip の速度変更を受理しました");
    invalidHold = held;
    invalidHold.timelineClips[2].frameHold->sourceFrame = 300;
    check(!validateTimeline(invalidHold).success, "素材範囲外の保持 frame を受理しました");
    invalidHold = held;
    invalidHold.timelineClips[2].preservePitch = true;
    check(!validateTimeline(invalidHold).success, "保持 clip の pitch flag を受理しました");
    invalidHold = held;
    invalidHold.timelineClips[2].linkGroupId = "bad";
    check(!validateTimeline(invalidHold).success, "保持 clip の link を受理しました");
    invalidHold = held;
    invalidHold.timelineClips[2].frameHold->sourceFrame = 1;
    invalidHold.timelineClips[2].frameHold->sourceFpsNum = 120;
    check(!frameHoldProducerPosition(invalidHold.timelineClips[2], 60, 1).success &&
              !validateTimeline(invalidHold).success,
          "出力位置へ換算できない素材 frame を受理しました");
    // 同じ 120 fps の frame 1 でも、保持元が 50% (実効 60 fps) なら timeline の 1 frame 目に出る。
    invalidHold.timelineClips[2].frameHold->speedNum = 1;
    invalidHold.timelineClips[2].frameHold->speedDen = 2;
    const auto slowPosition = frameHoldProducerPosition(invalidHold.timelineClips[2], 60, 1);
    check(slowPosition.success && slowPosition.frame == 1 && validateTimeline(invalidHold).success,
          "slow motion の保持元速度で素材 frame を出力位置へ換算できません");
    invalidHold.timelineClips[2].frameHold->speedDen = 1000;
    check(!validateTimeline(invalidHold).success, "範囲外の保持元速度を受理しました");
    {
        // 120 fps 素材を 50% にした clip の奇数 frame (素材 fps のままでは 60 fps timeline へ
        // 出せない) でも、表示中の frame を保持できる。
        Project slow = createDefaultProject();
        slow.timelineClips = {clip("slow")};
        auto& source = slow.timelineClips[0];
        source.sourceFpsNum = 120;
        source.sourceFrameCount = 600;
        source.sourceOutFrame = 600;
        source.speedNum = 1;
        source.speedDen = 2;
        int slowId = 0;
        const auto slowHeld = insertFrameHold(slow, "id-slow", 61, 120,
                                              [&] { return "slow-" + std::to_string(++slowId); });
        const auto hold = std::find_if(slow.timelineClips.begin(), slow.timelineClips.end(),
                                       [](const auto& value) { return value.frameHold; });
        check(slowHeld.success && hold != slow.timelineClips.end() &&
                  hold->frameHold->sourceFrame == 61 && hold->frameHold->speedNum == 1 &&
                  hold->frameHold->speedDen == 2 && validateTimeline(slow).success,
              "slow motion の高 fps 素材で表示中の frame を保持できません");
    }
    check(!insertFrameHold(
               held, held.timelineClips[2].id, 180, 120,
               [&] {
                   return "bad-" + std::to_string(++id);
               }).success &&
              held == heldBefore,
          "保持 clip からの再挿入を拒否できません");

    Project multi = createDefaultProject();
    check(addTrack(multi, TrackKind::Video).success, "V2 を追加できません");
    multi.timelineClips = {clip("main"), clip("overlay", TimelineClipKind::Video, kV2),
                           clip("music", TimelineClipKind::Audio, kA1)};
    const auto multiInserted = insertFrameHold(multi, "id-main", 120, 120,
                                               [&] { return "multi-" + std::to_string(++id); });
    check(multiInserted.success && multi.timelineClips.size() == 7,
          "全トラックを分割して保持区間を作れません");
    int shiftedVideo = 0, shiftedAudio = 0;
    for (const auto& piece : multi.timelineClips) {
        if (piece.id == "id-overlay" || piece.id == "id-music")
            continue;
        if (piece.frameHold)
            continue;
        if (piece.timelineStartFrame == 240 && piece.track == kV2)
            ++shiftedVideo;
        if (piece.timelineStartFrame == 240 && piece.track == kA1)
            ++shiftedAudio;
    }
    check(shiftedVideo == 1 && shiftedAudio == 1 && validateTimeline(multi).success,
          "保持区間の他トラックに映像・音声の空きを作れません");

    mvm::test::attachFixtureMedia(held);
    const auto holdJson = serializeProjectJson(held, "hold.mvm");
    check(holdJson.success, "保持 clip の JSON を出力できません");
    if (holdJson.success) {
        const auto restored = parseProjectJsonText(holdJson.json, "hold.mvm");
        check(restored.success && restored.project.timelineClips.size() == 3 &&
                  restored.project.timelineClips[2].frameHold == held.timelineClips[2].frameHold &&
                  restored.project.timelineClips[2].sourceInFrame == 0 &&
                  restored.project.timelineClips[2].sourceOutFrame == 120,
              "保持 clip の JSON が round-trip しません");
    }
}

void testFrameConversions() {
    const std::pair<std::int64_t, std::int64_t> rates[] = {
        {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {60, 1}};
    for (const auto [numerator, denominator] : rates) {
        for (std::int64_t timelineFrame = 0; timelineFrame < 240; ++timelineFrame) {
            const auto source = mvm::project::timelineBoundaryToSourceBoundary(
                timelineFrame, numerator, denominator, 60, 1);
            check(source.success, "timeline境界をsource境界へ変換できません");
            if (!source.success)
                continue;
            const auto at = mvm::project::sourceBoundaryToTimelineBoundary(source.frame, numerator,
                                                                           denominator, 60, 1);
            const auto next = mvm::project::sourceBoundaryToTimelineBoundary(
                source.frame + 1, numerator, denominator, 60, 1);
            check(at.success && at.frame <= timelineFrame,
                  "逆変換がdrag位置を越えないsource境界へsnapしません");
            check(next.success && next.frame > timelineFrame,
                  "逆変換が最新のsource境界を選びません");
        }
    }
    const auto overflow = mvm::project::sourceBoundaryToTimelineBoundary(
        std::numeric_limits<std::int64_t>::max(), 1, 1, std::numeric_limits<std::int64_t>::max(),
        1);
    check(!overflow.success, "overflowするframe変換を拒否しません");

    mvm::project::Project project = mvm::project::createDefaultProject();
    auto normalized = clip("normalized");
    auto equivalent = normalized;
    equivalent.sourceFpsNum = 120;
    equivalent.sourceFpsDen = 2;
    check(mvm::project::sourceRateMatchesTimelineRate(project, normalized) &&
              mvm::project::sourceRateMatchesTimelineRate(project, equivalent),
          "同値な60fps rateをPreview対応として判定できません");
}

// Project の timeline fps を 60 以外へ設定できること。表の外は拒否すること。
// configurable と measured を混ぜないこと。
void testTimelineFrameRates() {
    check(mvm::project::isConfigurableTimelineFrameRate(30, 1) &&
              mvm::project::isConfigurableTimelineFrameRate(30000, 1001) &&
              mvm::project::isConfigurableTimelineFrameRate(24, 1),
          "設定可能 rate を対応外と判定しました");
    check(!mvm::project::isConfigurableTimelineFrameRate(48, 1) &&
              !mvm::project::isConfigurableTimelineFrameRate(0, 1) &&
              !mvm::project::isConfigurableTimelineFrameRate(60, 0),
          "設定表に無い rate を受理しました");
    check(mvm::project::isConfigurableTimelineFrameRate(120, 2),
          "約分すれば設定可能になる値を拒否しました");

    // 設定できること != 計測済み。60/1 以外を measured にしない。
    check(mvm::project::isMeasuredTimelineFrameRate(60, 1), "60/1 が measured ではありません");
    check(!mvm::project::isMeasuredTimelineFrameRate(24, 1) &&
              !mvm::project::isMeasuredTimelineFrameRate(30000, 1001) &&
              !mvm::project::isMeasuredTimelineFrameRate(50, 1),
          "未計測 rate を measured として公開しました");

    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineFpsNum = 30;
    auto thirty = clip("thirty");
    thirty.sourceFpsNum = 30;
    project.timelineClips.push_back(thirty);
    const auto valid = mvm::project::validateTimeline(project);
    check(valid.success && valid.totalFrames == 300, "30fps Projectのtimelineを検証できません");

    project.timelineFpsNum = 48;
    check(!mvm::project::validateTimeline(project).success,
          "設定表に無い timeline fps の Project を受理しました");

    // 永続化する fps は canonical だけを authority にする。
    mvm::project::Project reducible = mvm::project::createDefaultProject();
    reducible.timelineFpsNum = 120;
    reducible.timelineFpsDen = 2;
    check(!mvm::project::validateTimeline(reducible).success,
          "約分されていない timeline fps の Project を受理しました");
}

// fps変更ではsource domainを維持し、timeline上のwall-clock位置だけを換算する。
void testTimelineFrameRateChange() {
    mvm::project::Project empty = mvm::project::createDefaultProject();
    const auto changed = mvm::project::setTimelineFrameRate(empty, 24, 1);
    check(changed.success && empty.timelineFpsNum == 24 && empty.timelineFpsDen == 1,
          "空 Project の frame rate を変更できません");

    const auto ntsc = mvm::project::setTimelineFrameRate(empty, 30000, 1001);
    check(ntsc.success && empty.timelineFpsNum == 30000 && empty.timelineFpsDen == 1001,
          "1001 分母の frame rate を設定できません");

    check(!mvm::project::setTimelineFrameRate(empty, 48, 1).success,
          "設定表に無い rate を受理しました");
    check(!mvm::project::setTimelineFrameRate(empty, 120, 2).success,
          "約分されていない pair を受理しました");

    mvm::project::Project withClip = mvm::project::createDefaultProject();
    auto audio = clip("voice", mvm::project::TimelineClipKind::Audio, kA1);
    audio.sourceInFrame = 60; // 左 trim 済み。source domain は取り込み時の fps
    audio.timelineStartFrame = 120;
    check(mvm::project::appendTimelineClip(withClip, audio, kA1).success,
          "audio clip を追加できません");
    withClip.timelineClips.front().timelineStartFrame = 120;
    const auto sourceBefore = withClip.timelineClips.front();
    const auto changedWithClip = mvm::project::setProjectVideoSettings(withClip, 1280, 720, 24, 1);
    check(changedWithClip.success && withClip.outputWidth == 1280 && withClip.outputHeight == 720 &&
              withClip.timelineFpsNum == 24 && withClip.timelineFpsDen == 1,
          "clipがあるProjectの映像設定を変更できません");
    check(withClip.timelineClips.front().timelineStartFrame == 48,
          "clip開始位置の秒位置を維持して新fpsへ換算できません");
    check(withClip.timelineClips.front().sourceFpsNum == sourceBefore.sourceFpsNum &&
              withClip.timelineClips.front().sourceFpsDen == sourceBefore.sourceFpsDen &&
              withClip.timelineClips.front().sourceInFrame == sourceBefore.sourceInFrame &&
              withClip.timelineClips.front().sourceOutFrame == sourceBefore.sourceOutFrame,
          "Project設定変更で素材側のframe domainを書き換えました");

    const auto beforeInvalid = withClip;
    check(!mvm::project::setProjectVideoSettings(withClip, 0, 720, 24, 1).success,
          "幅0のProject設定を受理しました");
    check(withClip.outputWidth == beforeInvalid.outputWidth &&
              withClip.outputHeight == beforeInvalid.outputHeight &&
              withClip.timelineFpsNum == beforeInvalid.timelineFpsNum &&
              withClip.timelineFpsDen == beforeInvalid.timelineFpsDen &&
              withClip.timelineClips == beforeInvalid.timelineClips,
          "拒否したProject設定でProjectが変化しました");

    // 同じ rate への設定は no-op として成功にする。
    check(mvm::project::setTimelineFrameRate(withClip, 24, 1).success,
          "同一 rate への設定を失敗にしました");
}

void testTrimAndLookup() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto fractional = clip("fractional");
    fractional.id = "fractional-id";
    fractional.sourceFpsNum = 30000;
    fractional.sourceFpsDen = 1001;
    project.timelineClips.push_back(fractional);

    const auto validated = mvm::project::validateTimeline(project);
    check(validated.success && validated.totalFrames == 601,
          "29.97fps clipのtimeline durationが違います");
    check(mvm::project::timelineClipIndexAt(project, kV1, 0) == 0, "clip先頭をlookupできません");
    check(mvm::project::timelineClipIndexAt(project, kV1, 600) == 0,
          "clip末尾frameをlookupできません");
    check(mvm::project::timelineClipIndexAt(project, kV1, 601) == -1,
          "exclusive timeline終端をclip内と判定しました");
    check(mvm::project::timelineClipIndexAt(project, kV2, 0) == -1,
          "別trackのframeをclip内と判定しました");

    const auto left = mvm::project::trimTimelineClip(
        project, "fractional-id", mvm::project::TrimEdge::Left, 3, mvm::project::LinkMode::Single);
    check(left.success && project.timelineClips.front().sourceInFrame == 1,
          "left trimをsource-native境界へsnapできません");
    const auto beforeInvalid = project;
    const auto invalid =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Right,
                                       -10000, mvm::project::LinkMode::Single);
    check(!invalid.success, "sourceOut <= sourceInになるtrimを拒否しません");
    project = beforeInvalid;

    auto unrelated = clip("unrelated", mvm::project::TimelineClipKind::Video, kV2);
    unrelated.timelineStartFrame = 77;
    unrelated.effects.opacityPercent = 42.0;
    project.timelineClips.push_back(unrelated);
    const auto beforeUnrelated = project.timelineClips.back();
    const auto beforeRightStart = project.timelineClips.front().timelineStartFrame;
    const auto right =
        mvm::project::trimTimelineClip(project, "fractional-id", mvm::project::TrimEdge::Right, -3,
                                       mvm::project::LinkMode::Single);
    check(right.success && project.timelineClips.front().timelineStartFrame == beforeRightStart,
          "right trimでclip startが変化しました");
    check(project.timelineClips.back() == beforeUnrelated,
          "trimで無関係clipのtrack/start/effectsが変化しました");

    const auto oldEnd =
        project.timelineClips.front().timelineStartFrame +
        mvm::project::timelineClipDuration(project, project.timelineClips.front()).frame;
    const auto secondLeft = mvm::project::trimTimelineClip(
        project, "fractional-id", mvm::project::TrimEdge::Left, 4, mvm::project::LinkMode::Single);
    const auto newEnd =
        project.timelineClips.front().timelineStartFrame +
        mvm::project::timelineClipDuration(project, project.timelineClips.front()).frame;
    check(secondLeft.success && oldEnd == newEnd, "left trimがclipの右端を維持しません");
    check(project.timelineClips.back() == beforeUnrelated,
          "left trimで無関係clipの配置が変化しました");
}

void testPlacementHelpers() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto base = clip("base");
    base.timelineStartFrame = 120;
    project.timelineClips.push_back(base);

    auto added = clip("added");
    added.timelineStartFrame = 999;
    const auto video = mvm::project::appendTimelineClip(project, added, kV1);
    check(video.success && project.timelineClips.back().track == kV1 &&
              project.timelineClips.back().timelineStartFrame == 420,
          "動画追加がV1の最大endへ配置されません");

    mvm::project::ManimAsset asset;
    asset.sceneName = "Overlay";
    asset.generatedVideoPath = "overlay.mp4";
    const auto manim =
        mvm::project::appendManimTimelineClipAt(project, asset, "overlay-id", 60, 1, 120, 180, kV2);
    check(manim.success && project.timelineClips.back().track == kV2 &&
              project.timelineClips.back().timelineStartFrame == 180,
          "Manim clipがplayhead指定位置のV2へ配置されません");
    check(!mvm::project::appendManimTimelineClipAt(project, asset, "overlay-id-2", 60, 1, 120, 400,
                                                   kV2)
               .success,
          "同じ Manim asset の重複配置を拒否しません");

    const auto beforeRejected = project;
    const auto rejected = mvm::project::moveClip(project, "id-added", kV1, 200);
    check(!rejected.success && project.timelineClips == beforeRejected.timelineClips,
          "same-track overlap拒否時にProjectが変化しました");

    const auto cross = mvm::project::moveClip(project, "id-added", kV2, 420);
    check(cross.success && project.timelineClips[1].track == kV2 &&
              project.timelineClips[1].timelineStartFrame == 420,
          "V1からV2へのcross-track移動ができません");
    const auto horizontal = mvm::project::moveClip(project, "id-added", kV2, 500);
    check(horizontal.success && project.timelineClips[1].timelineStartFrame == 500,
          "clipを同じtrack内で水平移動できません");
    const auto back = mvm::project::moveClip(project, "id-added", kV1, 420);
    check(back.success && project.timelineClips[1].track == kV1, "V2からV1へ移動できません");

    // video clip を audio track へ落とせない。track の種別が守られること。
    check(!mvm::project::moveClip(project, "id-added", kA1, 0).success,
          "video clipのaudio trackへの移動を拒否しません");
}

void testMultipleClipMove() {
    {
        auto project = mvm::project::createDefaultProject();
        check(mvm::project::addTrack(project, mvm::project::TrackKind::Video).success,
              "複数移動テスト用のV3を追加できません");
        auto first = clip("multi-first", mvm::project::TimelineClipKind::Video, kV1);
        auto second = clip("multi-second", mvm::project::TimelineClipKind::Video, kV2);
        first.timelineStartFrame = 100;
        second.timelineStartFrame = 500;
        project.timelineClips = {first, second};

        const auto moved = mvm::project::moveClips(project, {first.id, second.id}, first.id, kV2,
                                                   220, mvm::project::LinkMode::Linked);
        check(moved.success && project.timelineClips[0].track == kV2 &&
                  project.timelineClips[1].track == kV3 &&
                  project.timelineClips[0].timelineStartFrame == 220 &&
                  project.timelineClips[1].timelineStartFrame == 620,
              "複数clipを時間・track方向へ平行移動できません");

        auto blocker = clip("multi-blocker", mvm::project::TimelineClipKind::Video, kV2);
        blocker.timelineStartFrame = 600;
        project.timelineClips.push_back(blocker);
        const auto beforeFailure = project.timelineClips;
        const auto rejected = mvm::project::moveClips(project, {first.id, second.id}, first.id, kV2,
                                                      500, mvm::project::LinkMode::Linked);
        check(!rejected.success && project.timelineClips == beforeFailure,
              "複数移動の重なり拒否時にProjectの一部だけが変化しました");

        const auto missingAnchor = mvm::project::moveClips(project, {second.id}, first.id, kV1, 300,
                                                           mvm::project::LinkMode::Linked);
        check(!missingAnchor.success && project.timelineClips == beforeFailure,
              "選択外anchorを使う複数移動を拒否しません");
    }

    {
        auto project = mvm::project::createDefaultProject();
        auto first = clip("track-limit-first", mvm::project::TimelineClipKind::Video, kV1);
        auto second = clip("track-limit-second", mvm::project::TimelineClipKind::Video, kV2);
        second.timelineStartFrame = 400;
        project.timelineClips = {first, second};
        const auto beforeFailure = project.timelineClips;
        const auto rejected = mvm::project::moveClips(project, {first.id, second.id}, first.id, kV2,
                                                      100, mvm::project::LinkMode::Linked);
        check(!rejected.success && project.timelineClips == beforeFailure,
              "V3が無い複数track移動を全体rejectしません");
    }

    {
        auto project = mvm::project::createDefaultProject();
        auto video = clip("mixed-video", mvm::project::TimelineClipKind::Video, kV1);
        auto audio = clip("mixed-audio", mvm::project::TimelineClipKind::Audio, kA1);
        video.timelineStartFrame = 100;
        audio.timelineStartFrame = 500;
        project.timelineClips = {video, audio};
        const auto moved = mvm::project::moveClips(project, {video.id, audio.id}, video.id, kV2,
                                                   200, mvm::project::LinkMode::Linked);
        check(moved.success && project.timelineClips[0].track == kV2 &&
                  project.timelineClips[1].track == kA1 &&
                  project.timelineClips[0].timelineStartFrame == 200 &&
                  project.timelineClips[1].timelineStartFrame == 600,
              "video縦移動時にaudioのtrackを維持して時間だけ移動できません");
    }

    {
        auto project = mvm::project::createDefaultProject();
        auto video = clip("linked-video", mvm::project::TimelineClipKind::Video, kV1);
        auto audio = clip("linked-audio", mvm::project::TimelineClipKind::Audio, kA1);
        video.timelineStartFrame = 100;
        audio.timelineStartFrame = 100;
        video.linkGroupId = "move-link";
        audio.linkGroupId = "move-link";
        project.timelineClips = {video, audio};

        const auto moved = mvm::project::moveClips(project, {video.id}, video.id, kV2, 250,
                                                   mvm::project::LinkMode::Linked);
        check(moved.success && project.timelineClips[0].timelineStartFrame == 250 &&
                  project.timelineClips[1].timelineStartFrame == 250 &&
                  project.timelineClips[0].track == kV2 && project.timelineClips[1].track == kA1,
              "片方だけ指定したlinked clipを同じ時間差で移動できません");

        const auto movedWithDuplicateExpansion = mvm::project::moveClips(
            project, {video.id, audio.id}, video.id, kV1, 300, mvm::project::LinkMode::Linked);
        check(movedWithDuplicateExpansion.success &&
                  project.timelineClips[0].timelineStartFrame == 300 &&
                  project.timelineClips[1].timelineStartFrame == 300,
              "selectedとlinked展開が重なるclipを二重移動しました");
    }

    {
        auto project = mvm::project::createDefaultProject();
        auto anchor = clip("lower-bound-anchor", mvm::project::TimelineClipKind::Video, kV1);
        auto leftmost = clip("lower-bound-left", mvm::project::TimelineClipKind::Video, kV2);
        anchor.timelineStartFrame = 100;
        leftmost.timelineStartFrame = 20;
        project.timelineClips = {anchor, leftmost};
        const auto snapped = mvm::project::moveClips(project, {anchor.id, leftmost.id}, anchor.id,
                                                     kV1, 50, mvm::project::LinkMode::Linked);
        check(snapped.success && project.timelineClips[0].timelineStartFrame == 80 &&
                  project.timelineClips[1].timelineStartFrame == 0,
              "複数移動の最左clipを0秒へスナップできません");
    }

    {
        auto project = mvm::project::createDefaultProject();
        auto video = clip("legacy-linked-video", mvm::project::TimelineClipKind::Video, kV1);
        auto audio = clip("legacy-linked-audio", mvm::project::TimelineClipKind::Audio, kA1);
        video.timelineStartFrame = 100;
        audio.timelineStartFrame = 100;
        video.linkGroupId = "legacy-link";
        audio.linkGroupId = "legacy-link";
        project.timelineClips = {video, audio};
        const auto moved = mvm::project::moveClip(project, video.id, kV2, 180);
        check(moved.success && project.timelineClips[0].track == kV2 &&
                  project.timelineClips[1].track == kA1 &&
                  project.timelineClips[0].timelineStartFrame == 180 &&
                  project.timelineClips[1].timelineStartFrame == 180,
              "旧moveClipのlinked clip移動挙動が変わりました");

        const auto snapped = mvm::project::moveClip(project, video.id, kV1, 0);
        check(snapped.success && project.timelineClips[0].timelineStartFrame == 0 &&
                  project.timelineClips[1].timelineStartFrame == 0,
              "linked video/audioを0秒へ一体でスナップできません");
    }
}

// track を任意に増減できること。clip の載った track を暗黙に消さないこと。
void testTrackEditing() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    check(project.videoTracks.size() == 2 && project.audioTracks.size() == 1,
          "既定Projectのtrack構成が違います");

    const auto addedVideo = mvm::project::addTrack(project, mvm::project::TrackKind::Video);
    check(addedVideo.success && project.videoTracks.size() == 3 &&
              project.videoTracks.back().name == "V3",
          "video trackを追加できません");
    const auto addedAudio = mvm::project::addTrack(project, mvm::project::TrackKind::Audio);
    check(addedAudio.success && project.audioTracks.size() == 2 &&
              project.audioTracks.back().name == "A2",
          "audio trackを追加できません");

    const auto muted = mvm::project::setTrackMuted(
        project, mvm::project::TrackRef{mvm::project::TrackKind::Video, 1}, true);
    check(muted.success && project.videoTracks[1].muted, "trackをミュートできません");

    // V3 に clip を置くと V3 は消せない。clip を勝手に消さないことの検査。
    auto onTop = clip("onTop", mvm::project::TimelineClipKind::Video,
                      mvm::project::TrackRef{mvm::project::TrackKind::Video, 2});
    project.timelineClips.push_back(onTop);
    check(mvm::project::validateTimeline(project).success, "V3上のclipを検証できません");
    const auto beforeRemove = project;
    check(!mvm::project::removeTrack(project,
                                     mvm::project::TrackRef{mvm::project::TrackKind::Video, 2})
               .success,
          "clipが載ったtrackの削除を拒否しません");
    check(project.videoTracks == beforeRemove.videoTracks,
          "拒否したtrack削除でProjectが変化しました");

    // 空の V2 を消すと V3 の clip が V2 へ繰り上がる。
    const auto removed = mvm::project::removeTrack(
        project, mvm::project::TrackRef{mvm::project::TrackKind::Video, 1});
    check(removed.success && project.videoTracks.size() == 2 && project.videoTracks[1].name == "V2",
          "空のvideo trackを削除できません");
    check(project.timelineClips.back().track.index == 1,
          "track削除後に後続trackのclip indexが詰められません");

    // video track は最低 1 本。0 本の Project を作らせない。
    mvm::project::Project single = mvm::project::createDefaultProject();
    single.videoTracks.resize(1);
    check(!mvm::project::removeTrack(single, kV1).success, "最後のvideo trackの削除を拒否しません");
}

// audio clip は audio track にだけ載る。逆も拒否する。
void testAudioClipPlacement() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto audio = clip("voice", mvm::project::TimelineClipKind::Audio, kA1);
    const auto placed = mvm::project::appendTimelineClip(project, audio, kA1);
    check(placed.success && project.timelineClips.back().track == kA1,
          "audio clipをaudio trackへ追加できません");
    check(mvm::project::validateTimeline(project).success, "audio clipを含むtimelineが不正です");

    auto misplaced = clip("misplaced", mvm::project::TimelineClipKind::Audio, kA1);
    check(!mvm::project::appendTimelineClip(project, misplaced, kV1).success,
          "audio clipのvideo trackへの追加を拒否しません");

    auto broken = project;
    broken.timelineClips.back().track = kV1;
    check(!mvm::project::validateTimeline(broken).success,
          "video trackに載ったaudio clipを検証で拒否しません");

    const auto activeAudio =
        mvm::project::activeClipsAt(project, mvm::project::TrackKind::Audio, 10);
    check(activeAudio.size() == 1 && activeAudio[0] != nullptr,
          "audio trackのactive clipを引けません");
    const auto activeVideo =
        mvm::project::activeClipsAt(project, mvm::project::TrackKind::Video, 10);
    check(activeVideo.size() == 2 && activeVideo[0] == nullptr && activeVideo[1] == nullptr,
          "video trackにclipが無いのにactive clipを返しました");
}

// 空白の ripple delete。詰める対象が無い場所は成功にしない。
void testRippleDelete() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto first = clip("first");
    first.timelineStartFrame = 0;
    auto second = clip("second");
    second.timelineStartFrame = 500; // 300..500 が空白
    auto third = clip("third");
    third.timelineStartFrame = 900; // 800..900 が空白
    auto other = clip("other", mvm::project::TimelineClipKind::Video, kV2);
    other.timelineStartFrame = 400;
    project.timelineClips = {first, second, third, other};
    check(mvm::project::validateTimeline(project).success, "ripple用のtimelineが不正です");

    const auto gap = mvm::project::gapAt(project, kV1, 400);
    check(gap.found && gap.start == 300 && gap.end == 500, "空白区間を正しく求められません");
    check(!mvm::project::gapAt(project, kV1, 100).found, "clip上をgapと判定しました");
    check(!mvm::project::gapAt(project, kV1, 1300).found,
          "後続clipが無い終端の空白をgapと判定しました");

    const auto rippled = mvm::project::rippleDeleteGap(project, kV1, 400);
    check(rippled.success, "空白をripple削除できません");
    check(project.timelineClips[0].timelineStartFrame == 0 &&
              project.timelineClips[1].timelineStartFrame == 300 &&
              project.timelineClips[2].timelineStartFrame == 700,
          "ripple削除で後続clipが詰められません");
    check(project.timelineClips[3].timelineStartFrame == 400,
          "ripple削除が他trackのclipまで動かしました");

    const auto beforeReject = project;
    check(!mvm::project::rippleDeleteGap(project, kV1, 100).success,
          "clip上のripple削除を拒否しません");
    check(project.timelineClips == beforeReject.timelineClips,
          "拒否したripple削除でProjectが変化しました");
}

void testRippleDeleteWithLinkedClips() {
    // linked pair を ripple すると counterpart も同じ shift で動く。
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("linked-video");
    video.timelineStartFrame = 100;
    auto audio = clip("linked-audio", mvm::project::TimelineClipKind::Audio, kA1);
    audio.timelineStartFrame = 100;
    video.linkGroupId = "ripple-link";
    audio.linkGroupId = "ripple-link";
    project.timelineClips = {video, audio};
    check(mvm::project::validateTimeline(project).success, "linked ripple用のtimelineが不正です");

    const auto rippled = mvm::project::rippleDeleteGap(project, kV1, 50);
    check(rippled.success && project.timelineClips[0].timelineStartFrame == 0 &&
              project.timelineClips[1].timelineStartFrame == 0,
          "ripple削除でlinked counterpartが同期しません");

    // counterpart 側が衝突するなら ripple 全体を失敗させる (fail-closed)。
    mvm::project::Project blocked = mvm::project::createDefaultProject();
    auto blocker = clip("audio-blocker", mvm::project::TimelineClipKind::Audio, kA1);
    blocker.sourceOutFrame = 60;
    blocker.timelineStartFrame = 0;
    blocked.timelineClips = {video, audio, blocker};
    check(mvm::project::validateTimeline(blocked).success, "衝突テスト用のtimelineが不正です");
    const auto beforeBlocked = blocked.timelineClips;
    check(!mvm::project::rippleDeleteGap(blocked, kV1, 50).success &&
              blocked.timelineClips == beforeBlocked,
          "counterpartが衝突するripple削除で片側だけを動かしました");

    // unlink 後は対象 track だけを詰める。
    mvm::project::Project unlinked = mvm::project::createDefaultProject();
    unlinked.timelineClips = {video, audio};
    check(mvm::project::unlinkTimelineClip(unlinked, video.id).success,
          "ripple前のリンク解除に失敗しました");
    check(mvm::project::rippleDeleteGap(unlinked, kV1, 50).success &&
              unlinked.timelineClips[0].timelineStartFrame == 0 &&
              unlinked.timelineClips[1].timelineStartFrame == 100,
          "unlink後のripple削除が他trackのclipまで動かしました");
}

std::int64_t clipEnd(const mvm::project::Project& project,
                     const mvm::project::TimelineClip& value) {
    return value.timelineStartFrame + mvm::project::timelineClipDuration(project, value).frame;
}

const mvm::project::TimelineClip* findClip(const mvm::project::Project& project,
                                           const std::string& id) {
    for (const auto& value : project.timelineClips) {
        if (value.id == id)
            return &value;
    }
    return nullptr;
}

std::function<std::string()> sequentialIds() {
    auto counter = std::make_shared<int>(0);
    return [counter] { return "new-" + std::to_string(++*counter); };
}

// レート調整。期待値は手で計算した値である (実装の式を呼ばない)。
void testRateStretch() {
    using mvm::project::LinkMode;
    using mvm::project::TrimEdge;
    const auto duration = [](const mvm::project::Project& project, int index) {
        return mvm::project::timelineClipDuration(
                   project, project.timelineClips[static_cast<std::size_t>(index)])
            .frame;
    };

    // 60fps 素材 300 frame を 0 に置き、次の clip は 600 から。
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto target = clip("rate");
    auto next = clip("rate-next");
    next.timelineStartFrame = 600;
    project.timelineClips = {target, next};
    check(mvm::project::validateTimeline(project).success, "レート調整用のtimelineが不正です");

    // right 端を +300: 尺 600 = 50%。素材範囲と開始位置は変えない。
    check(mvm::project::rateStretchTimelineClip(project, target.id, TrimEdge::Right, 300,
                                                LinkMode::Single)
              .success,
          "right端のレート調整に失敗しました");
    const auto& slowed = project.timelineClips[0];
    check(slowed.speedNum == 1 && slowed.speedDen == 2 && duration(project, 0) == 600 &&
              slowed.timelineStartFrame == 0 && slowed.sourceInFrame == 0 &&
              slowed.sourceOutFrame == 300,
          "50%にしたclipの速度・尺・素材範囲が違います");

    // 次の clip に接したので、これ以上は伸ばせない。失敗しても Project は変わらない。
    const auto beforeBlocked = project;
    check(mvm::project::clampRateEdit(project, target.id, TrimEdge::Right, 10000, LinkMode::Single)
                  .frame == 0,
          "隣のclipで止まりません");
    check(!mvm::project::rateStretchTimelineClip(project, target.id, TrimEdge::Right, 10000,
                                                 LinkMode::Single)
                  .success &&
              project == beforeBlocked,
          "伸ばせないレート調整が成功した、またはProjectを変えました");

    // 縮める方向は 1000% (尺 30) で止まる。
    check(mvm::project::clampRateEdit(project, target.id, TrimEdge::Right, -1000, LinkMode::Single)
                  .frame == -570,
          "1000%で止まりません");
    check(mvm::project::rateStretchTimelineClip(project, target.id, TrimEdge::Right, -1000,
                                                LinkMode::Single)
                  .success &&
              project.timelineClips[0].speedNum == 10 && project.timelineClips[0].speedDen == 1 &&
              duration(project, 0) == 30,
          "1000%のclipになりません");

    // 10% は尺 3000。隣を遠ざけて確かめる。
    project.timelineClips[1].timelineStartFrame = 10000;
    check(mvm::project::clampRateEdit(project, target.id, TrimEdge::Right, 100000, LinkMode::Single)
                  .frame == 2970,
          "10%で止まりません");

    // left 端は終端を保って開始位置を動かす。timeline 先頭で止まる。
    mvm::project::Project leftProject = mvm::project::createDefaultProject();
    auto leftTarget = clip("rate-left");
    leftTarget.timelineStartFrame = 100;
    leftProject.timelineClips = {leftTarget};
    check(mvm::project::clampRateEdit(leftProject, leftTarget.id, TrimEdge::Left, -1000,
                                      LinkMode::Single)
                  .frame == -100,
          "left端がtimeline先頭で止まりません");
    check(mvm::project::rateStretchTimelineClip(leftProject, leftTarget.id, TrimEdge::Left, -1000,
                                                LinkMode::Single)
                  .success &&
              leftProject.timelineClips[0].timelineStartFrame == 0 &&
              duration(leftProject, 0) == 400 && leftProject.timelineClips[0].speedNum == 3 &&
              leftProject.timelineClips[0].speedDen == 4,
          "left端のレート調整で終端が保たれない、または速度が75%になりません");

    // 29.97fps 素材 100 frame (60fps で尺 ceil(200.2) = 201) を尺 250 にする。
    // 速度 = 100 * 60 * 1001 / (30000 * 250) = 1001 / 1250。尺はちょうど 250 になる。
    mvm::project::Project ntscProject = mvm::project::createDefaultProject();
    auto ntsc = clip("rate-ntsc");
    ntsc.sourceFpsNum = 30000;
    ntsc.sourceFpsDen = 1001;
    ntsc.sourceOutFrame = 100;
    ntscProject.timelineClips = {ntsc};
    check(duration(ntscProject, 0) == 201, "29.97fps素材の尺の前提が違います");
    check(mvm::project::rateStretchTimelineClip(ntscProject, ntsc.id, TrimEdge::Right, 49,
                                                LinkMode::Single)
                  .success &&
              ntscProject.timelineClips[0].speedNum == 1001 &&
              ntscProject.timelineClips[0].speedDen == 1250 && duration(ntscProject, 0) == 250,
          "29.97fps素材の尺がちょうど目標にならない、または速度が約分されていません");

    // key は内容に付いて伸縮する。端は端へ写る。150 -> round(150 * 599 / 299) = 301。
    mvm::project::Project keyProject = mvm::project::createDefaultProject();
    auto keyed = clip("rate-keys");
    keyed.effects.opacityKeys = {{0, 0.0}, {150, 50.0}, {299, 100.0}};
    keyProject.timelineClips = {keyed};
    check(mvm::project::rateStretchTimelineClip(keyProject, keyed.id, TrimEdge::Right, 300,
                                                LinkMode::Single)
              .success,
          "key付きclipのレート調整に失敗しました");
    const auto& keys = keyProject.timelineClips[0].effects.opacityKeys;
    check(keys.size() == 3 && keys[0].frame == 0 && keys[1].frame == 301 &&
              keys[1].valuePercent == 50.0 && keys[2].frame == 599,
          "keyが尺に合わせて伸縮しません");

    // 50% の clip の trim: timeline 100 frame = 素材 50 frame。
    check(mvm::project::trimTimelineClip(keyProject, keyed.id, TrimEdge::Right, -100,
                                         LinkMode::Single)
                  .success &&
              keyProject.timelineClips[0].sourceOutFrame == 250 && duration(keyProject, 0) == 500,
          "50%のclipのtrimが速度を考慮しません");
    // 分割した左右とも速度を引き継ぎ、尺の合計は変わらない。
    check(mvm::project::splitTimelineClips(keyProject, {keyed.id}, 200, sequentialIds(),
                                           LinkMode::Linked)
                  .success &&
              keyProject.timelineClips.size() == 2 && keyProject.timelineClips[1].speedNum == 1 &&
              keyProject.timelineClips[1].speedDen == 2 &&
              duration(keyProject, 0) + duration(keyProject, 1) == 500,
          "50%のclipを分割すると速度または尺が変わります");

    // リンク相手にも同じ速度を適用する。相手の track の隣の clip でも止まる。
    mvm::project::Project linked = mvm::project::createDefaultProject();
    auto video = clip("rate-video");
    auto audio = clip("rate-audio", mvm::project::TimelineClipKind::Audio, kA1);
    video.linkGroupId = audio.linkGroupId = "rate-pair";
    auto audioNext = clip("rate-audio-next", mvm::project::TimelineClipKind::Audio, kA1);
    audioNext.timelineStartFrame = 450;
    linked.timelineClips = {video, audio, audioNext};
    check(mvm::project::clampRateEdit(linked, video.id, TrimEdge::Right, 300, LinkMode::Linked)
                  .frame == 150,
          "リンク相手の隣のclipで止まりません");
    check(mvm::project::clampRateEdit(linked, video.id, TrimEdge::Right, 300, LinkMode::Single)
                  .frame == 300,
          "Altのレート調整がリンク相手の隣で止まりました");
    check(mvm::project::rateStretchTimelineClip(linked, video.id, TrimEdge::Right, 150,
                                                LinkMode::Linked)
                  .success &&
              linked.timelineClips[0].speedNum == 2 && linked.timelineClips[0].speedDen == 3 &&
              linked.timelineClips[1].speedNum == 2 && linked.timelineClips[1].speedDen == 3 &&
              duration(linked, 0) == 450 && duration(linked, 1) == 450,
          "リンク相手に同じ速度を適用しません");
    // Alt で片方だけ変えた後は、Linked のレート調整を拒否する。
    linked.timelineClips[2].timelineStartFrame = 5000;
    check(mvm::project::rateStretchTimelineClip(linked, video.id, TrimEdge::Right, -150,
                                                LinkMode::Single)
              .success,
          "Altで片方だけレート調整できません");
    const auto beforeMismatch = linked;
    check(!mvm::project::rateStretchTimelineClip(linked, video.id, TrimEdge::Right, 100,
                                                 LinkMode::Linked)
                  .success &&
              linked == beforeMismatch,
          "速度の違うリンク相手を一緒にレート調整しました");
}

// 尺の違うリンク相手 (Alt で片方だけ trim した後など) のレート調整。相手は同じ速度で
// 自分の尺になる。drag 中の表示 (previewRateStretch) と確定の結果が一致しなければならない。
// 以前の QML は相手へ同じ端の移動量を配っており、A を 390f / 61.54% と表示していた。
void testRateStretchLinkedDifferentDurations() {
    using mvm::project::LinkMode;
    using mvm::project::TrimEdge;
    const auto duration = [](const mvm::project::Project& project, int index) {
        return mvm::project::timelineClipDuration(
                   project, project.timelineClips[static_cast<std::size_t>(index)])
            .frame;
    };
    // V: 300f、A: 240f (素材 in 0..240)、どちらも 100%、0 から。
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("rate-v");
    auto audio = clip("rate-a", mvm::project::TimelineClipKind::Audio, kA1);
    audio.sourceOutFrame = 240;
    video.linkGroupId = audio.linkGroupId = "rate-unequal";
    project.timelineClips = {video, audio};
    check(mvm::project::validateTimeline(project).success, "尺の違うリンク対のtimelineが不正です");

    // V を +150 (450f)。速度 = 300 / 450 = 2/3。A は 240 x 3/2 = 360f。
    const auto preview =
        mvm::project::previewRateStretch(project, video.id, TrimEdge::Right, 150, LinkMode::Linked);
    const auto shownFor =
        [&](const std::string& id) -> const mvm::project::RateStretchPreviewClip* {
        for (const auto& shown : preview.clips) {
            if (shown.clipId == id)
                return &shown;
        }
        return nullptr;
    };
    const auto* shownVideo = shownFor(video.id);
    const auto* shownAudio = shownFor(audio.id);
    check(preview.success && preview.appliedDelta == 150 && preview.clips.size() == 2 &&
              shownVideo && shownAudio,
          "尺の違うリンク対のpreviewを作れません");
    if (shownVideo && shownAudio) {
        check(shownVideo->durationFrames == 450 && shownVideo->endDelta == 150 &&
                  shownVideo->startDelta == 0 && shownVideo->speedNum == 2 &&
                  shownVideo->speedDen == 3,
              "previewのVが450f・2/3になりません");
        check(shownAudio->durationFrames == 360 && shownAudio->endDelta == 120 &&
                  shownAudio->startDelta == 0 && shownAudio->speedNum == 2 &&
                  shownAudio->speedDen == 3,
              "previewのAが同じ速度で自分の尺 (360f) になりません");
    }
    check(mvm::project::rateStretchTimelineClip(project, video.id, TrimEdge::Right, 150,
                                                LinkMode::Linked)
                  .success &&
              duration(project, 0) == 450 && duration(project, 1) == 360 &&
              project.timelineClips[1].speedNum == 2 && project.timelineClips[1].speedDen == 3,
          "確定の結果がpreviewと一致しません");

    // left 端。終端を 600 に揃えた V (300..600) と A (360..600) の V を -150 する。
    // V は 150 から 450f、A は終端を保って 360f になり 240 から始まる (開始のずれ -120)。
    mvm::project::Project left = mvm::project::createDefaultProject();
    auto leftVideo = clip("rate-left-v");
    auto leftAudio = clip("rate-left-a", mvm::project::TimelineClipKind::Audio, kA1);
    leftVideo.timelineStartFrame = 300;
    leftAudio.sourceInFrame = 60;
    leftAudio.timelineStartFrame = 360;
    leftVideo.linkGroupId = leftAudio.linkGroupId = "rate-left-unequal";
    left.timelineClips = {leftVideo, leftAudio};
    const auto leftPreview = mvm::project::previewRateStretch(left, leftVideo.id, TrimEdge::Left,
                                                              -150, LinkMode::Linked);
    bool leftShown = leftPreview.success && leftPreview.clips.size() == 2;
    for (const auto& shown : leftPreview.clips) {
        if (shown.clipId == leftVideo.id)
            leftShown = leftShown && shown.startDelta == -150 && shown.endDelta == 0 &&
                        shown.durationFrames == 450;
        if (shown.clipId == leftAudio.id)
            leftShown = leftShown && shown.startDelta == -120 && shown.endDelta == 0 &&
                        shown.durationFrames == 360;
    }
    check(leftShown, "left端のpreviewでリンク相手が自分の尺で終端を保ちません");
    check(mvm::project::rateStretchTimelineClip(left, leftVideo.id, TrimEdge::Left, -150,
                                                LinkMode::Linked)
                  .success &&
              left.timelineClips[0].timelineStartFrame == 150 &&
              left.timelineClips[1].timelineStartFrame == 240,
          "left端の確定の結果がpreviewと一致しません");

    // 伸縮できない量はpreviewでも動かさない (delta 0、現在の尺のまま)。
    const auto blocked = mvm::project::previewRateStretch(project, video.id, TrimEdge::Right,
                                                          -100000, LinkMode::Linked);
    bool unchanged = blocked.success;
    for (const auto& shown : blocked.clips)
        unchanged = unchanged && shown.startDelta == 0 && shown.endDelta <= 0;
    check(unchanged && blocked.appliedDelta < 0, "縮める方向のpreviewが1000%で止まりません");
}

void testSplitClips() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("split-video");
    video.effects.fadeInFrames = 30;
    video.effects.fadeOutFrames = 40;
    video.linkGroupId = "split-link";
    auto audio = clip("split-audio", mvm::project::TimelineClipKind::Audio, kA1);
    audio.linkGroupId = "split-link";
    project.timelineClips = {video, audio};
    check(mvm::project::validateTimeline(project).success, "分割用のtimelineが不正です");

    const auto beforeReject = project;
    check(!mvm::project::splitTimelineClips(project, {video.id}, 0, sequentialIds(),
                                            mvm::project::LinkMode::Linked)
                  .success &&
              !mvm::project::splitTimelineClips(project, {video.id}, 300, sequentialIds(),
                                                mvm::project::LinkMode::Linked)
                   .success &&
              project == beforeReject,
          "clipの端での分割を拒否しないか、拒否時にProjectが変化しました");

    const auto split = mvm::project::splitTimelineClips(project, {video.id}, 100, sequentialIds(),
                                                        mvm::project::LinkMode::Linked);
    check(split.success && project.timelineClips.size() == 4,
          "linked clipを分割位置で相手ごと分割できません");
    const auto* leftVideo = findClip(project, video.id);
    const auto* leftAudio = findClip(project, audio.id);
    const mvm::project::TimelineClip* rightVideo = nullptr;
    const mvm::project::TimelineClip* rightAudio = nullptr;
    for (const auto& value : project.timelineClips) {
        if (value.id != video.id && value.id != audio.id) {
            if (value.kind == mvm::project::TimelineClipKind::Audio)
                rightAudio = &value;
            else
                rightVideo = &value;
        }
    }
    check(leftVideo && leftAudio && rightVideo && rightAudio, "分割後のclipが見つかりません");
    if (!leftVideo || !leftAudio || !rightVideo || !rightAudio)
        return;
    check(leftVideo->sourceOutFrame == 100 && rightVideo->sourceInFrame == 100 &&
              rightVideo->sourceOutFrame == 300 && rightVideo->timelineStartFrame == 100 &&
              clipEnd(project, *leftVideo) == 100,
          "分割後の素材範囲または配置が違います");
    check(leftVideo->effects.fadeInFrames == 30 && leftVideo->effects.fadeOutFrames == 0 &&
              rightVideo->effects.fadeInFrames == 0 && rightVideo->effects.fadeOutFrames == 40,
          "分割でfade in/outを左右へ振り分けられません");
    check(leftVideo->linkGroupId == "split-link" && leftAudio->linkGroupId == "split-link" &&
              !rightVideo->linkGroupId.empty() &&
              rightVideo->linkGroupId == rightAudio->linkGroupId &&
              rightVideo->linkGroupId != "split-link",
          "分割後の右半分が新しいlink groupで結ばれません");

    // リンク相手が分割位置を含まなければ相手は切らず、右半分は未リンクになる。
    mvm::project::Project partial = mvm::project::createDefaultProject();
    auto shortAudio = audio;
    shortAudio.sourceOutFrame = 50;
    partial.timelineClips = {video, shortAudio};
    check(mvm::project::splitTimelineClips(partial, {video.id}, 100, sequentialIds(),
                                           mvm::project::LinkMode::Linked)
                  .success &&
              partial.timelineClips.size() == 3 &&
              partial.timelineClips.back().linkGroupId.empty() &&
              findClip(partial, shortAudio.id)->sourceOutFrame == 50,
          "分割位置を含まないリンク相手を切ったか、右半分をリンクしたままにしました");

    // 29.97fps 素材を 60fps timeline で切っても左右が重ならない。
    mvm::project::Project fractional = mvm::project::createDefaultProject();
    auto ntsc = clip("ntsc");
    ntsc.sourceFpsNum = 30000;
    ntsc.sourceFpsDen = 1001;
    fractional.timelineClips = {ntsc};
    check(mvm::project::splitTimelineClips(fractional, {ntsc.id}, 101, sequentialIds(),
                                           mvm::project::LinkMode::Linked)
              .success,
          "29.97fps clipを分割できません");
    if (fractional.timelineClips.size() == 2) {
        check(fractional.timelineClips[0].sourceOutFrame ==
                      fractional.timelineClips[1].sourceInFrame &&
                  clipEnd(fractional, fractional.timelineClips[0]) ==
                      fractional.timelineClips[1].timelineStartFrame,
              "29.97fps clipの分割で素材境界とtimeline境界が一致しません");
    }

    auto spanning = threeClips();
    auto upper = clip("upper", mvm::project::TimelineClipKind::Video, kV2);
    upper.timelineStartFrame = 200;
    spanning.timelineClips.push_back(upper);
    const auto ids = mvm::project::clipIdsSpanningFrame(spanning, 350);
    check(ids == std::vector<std::string>({"id-Manim", "id-upper"}),
          "全track分割の対象clipを正しく求められません");
    check(mvm::project::clipIdsSpanningFrame(spanning, 300) ==
              std::vector<std::string>({"id-upper"}),
          "clip境界上のframeを分割対象に含めました");
    // 選択で絞る版: among の順序を保ち、境界ちょうどの clip と存在しない ID を含めない。
    check(mvm::project::clipIdsSpanningFrame(spanning, 350, {"id-upper", "missing", "id-Manim"}) ==
              std::vector<std::string>({"id-upper", "id-Manim"}),
          "選択clipのうち再生ヘッドを含むclipを正しく求められません");
    check(mvm::project::clipIdsSpanningFrame(spanning, 300, {"id-Manim", "id-upper"}) ==
              std::vector<std::string>({"id-upper"}),
          "選択clipの絞り込みでclip境界上のframeを分割対象に含めました");
    check(mvm::project::clipIdsSpanningFrame(spanning, 350, {}).empty(),
          "選択が空なのに分割対象を返しました");
}

// [ / ] の音量。映像を選ぶとリンク相手の audio を変え、映像の音量は 100 のまま
// (映像 clip の音量は validateTimeline が 100 に固定している)。
void testStepClipVolume() {
    using mvm::project::TimelineClipKind;
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("video");
    video.linkGroupId = "volume-link";
    auto audio = clip("audio", TimelineClipKind::Audio, kA1);
    audio.linkGroupId = "volume-link";
    auto keyed = clip("keyed", TimelineClipKind::Audio, kA1);
    keyed.timelineStartFrame = 300;
    keyed.effects.volumeKeys = {{0, 0.0}, {10, 100.0}, {20, 199.0}};
    auto lone = clip("lone", TimelineClipKind::Video, kV2);
    project.timelineClips = {video, audio, keyed, lone};
    check(mvm::project::validateTimeline(project).success, "音量試験用のtimelineが不正です");
    const auto volumeOf = [&](const std::string& id) {
        return findClip(project, id)->effects.volumePercent;
    };

    check(mvm::project::stepClipVolume(project, {video.id}, 1.0).success &&
              std::abs(volumeOf(audio.id) - 112.20184543019634) < 1e-9 &&
              volumeOf(video.id) == 100.0,
          "映像を選んだ+1dBでリンク相手のaudioだけを変えません");

    // key は 0% を保ち、他は同じ規則 (上限 200) で変える。base も変える。
    check(mvm::project::stepClipVolume(project, {keyed.id}, 1.0).success,
          "音量keyのあるaudio clipを+1dBできません");
    const auto& keys = findClip(project, keyed.id)->effects.volumeKeys;
    check(keys.size() == 3 && keys[0].valuePercent == 0.0 &&
              std::abs(keys[1].valuePercent - 112.20184543019634) < 1e-9 &&
              keys[2].valuePercent == 200.0 &&
              std::abs(volumeOf(keyed.id) - 112.20184543019634) < 1e-9,
          "音量keyを0%を保ったまま同じ規則で変えません");

    // 対象外だけ・上限で変化なし・存在しない ID は失敗し、Project を変えない。
    const auto before = project;
    check(!mvm::project::stepClipVolume(project, {lone.id}, 1.0).success && project == before,
          "audioの無い選択で音量の変更を受理しました");
    auto atMaximum = project;
    for (auto& value : atMaximum.timelineClips)
        if (value.id == audio.id)
            value.effects.volumePercent = 200.0;
    const auto maximumBefore = atMaximum;
    check(!mvm::project::stepClipVolume(atMaximum, {audio.id}, 1.0).success &&
              atMaximum == maximumBefore,
          "上限の音量を上げる操作を変更ありとして受理しました");
    check(!mvm::project::stepClipVolume(project, {"missing"}, 1.0).success && project == before,
          "存在しないclipの音量変更を受理しました");

    check(mvm::project::stepClipVolume(project, {audio.id, keyed.id}, -1.0).success &&
              std::abs(volumeOf(audio.id) - 100.0) < 1e-9,
          "複数clipを-1dBで元に戻せません");
}

// トランジションの検証と、編集後の整合 (reconcile)。
// A [0,300) と B [300,600) は同じ 600 frame の素材の前半と後半で、cut の前後に 300 frame ずつ
// 余白がある。期待値は手で数えた値である。
mvm::project::Project transitionProject() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto a = clip("A");
    a.sourceFrameCount = 600;
    a.sourceOutFrame = 300;
    auto b = clip("B");
    b.sourceFrameCount = 600;
    b.sourceInFrame = 300;
    b.sourceOutFrame = 600;
    b.timelineStartFrame = 300;
    project.timelineClips = {a, b};
    project.timelineTransitions = {{"t1", a.id, b.id, 30, 30}};
    return project;
}

bool failsWith(const mvm::project::Project& project, const char* fragment) {
    const auto valid = mvm::project::validateTimeline(project);
    if (valid.success || valid.error.find(fragment) == std::string::npos) {
        std::fprintf(stderr, "  期待: %s / 実際: %s\n", fragment,
                     valid.success ? "(成功)" : valid.error.c_str());
        return false;
    }
    return true;
}

void testTimelineTransitions(const std::filesystem::path& root) {
    using mvm::project::LinkMode;
    using mvm::project::TrimEdge;
    const auto base = transitionProject();
    check(mvm::project::validateTimeline(base).success, "対照: 正しいトランジションを拒否しました");

    // JSON を往復する (enabled=false も一緒に見る)。
    {
        auto saved = base;
        saved.timelineClips[1].enabled = false;
        mvm::test::attachFixtureMedia(saved);
        const auto path = root / "transitions.mvm";
        const auto serialized = mvm::project::serializeProjectJson(saved, path);
        const auto loaded = serialized.success
                                ? mvm::project::parseProjectJsonText(serialized.json, path)
                                : mvm::project::ProjectLoadResult{};
        check(loaded.success && loaded.project.timelineTransitions == saved.timelineTransitions &&
                  !loaded.project.timelineClips[1].enabled &&
                  loaded.project.timelineClips[0].enabled,
              "トランジションと無効clipがJSONを往復しません");
        if (serialized.success) {
            auto missing = serialized.json;
            const auto at = missing.find("\"frames_after_cut\": 30");
            check(at != std::string::npos, "frames_after_cut が出力されません");
            if (at != std::string::npos) {
                missing.erase(at, std::string("\"frames_after_cut\": 30").size());
                // 直前の ", " が残るので JSON としても壊れる。どちらでも読み込みは失敗する。
                check(!mvm::project::parseProjectJsonText(missing, path).success,
                      "frames_after_cut の無いトランジションを受理しました");
            }
        }
    }

    // 検証の負例。いずれも対照群から 1 か所だけ変える。
    auto invalid = base;
    invalid.timelineTransitions.push_back(invalid.timelineTransitions[0]);
    check(failsWith(invalid, "ID が空または重複"), "重複したトランジションIDを受理しました");
    invalid = base;
    invalid.timelineTransitions[0].incomingClipId = "missing";
    check(failsWith(invalid, "clip がありません"), "存在しないclipのトランジションを受理しました");
    invalid = base;
    invalid.timelineClips[1].track = kV2;
    check(failsWith(invalid, "同じ track にありません"), "別trackのトランジションを受理しました");
    invalid = base;
    invalid.timelineClips[1].timelineStartFrame = 301;
    check(failsWith(invalid, "接していません"), "接していないclipのトランジションを受理しました");
    invalid = base;
    invalid.timelineTransitions[0].framesBeforeCut = 0;
    invalid.timelineTransitions[0].framesAfterCut = 0;
    check(failsWith(invalid, "長さが不正"), "0 frameのトランジションを受理しました");
    invalid = base;
    invalid.timelineTransitions[0].framesBeforeCut = -1;
    check(failsWith(invalid, "長さが不正"), "負の長さのトランジションを受理しました");
    invalid = base;
    invalid.timelineTransitions[0].framesBeforeCut = 301;
    check(failsWith(invalid, "余白が足りません"), "頭の余白を超えるトランジションを受理しました");
    invalid = base;
    invalid.timelineTransitions[0].framesAfterCut = 301;
    check(failsWith(invalid, "余白が足りません"), "尻の余白を超えるトランジションを受理しました");
    invalid = base;
    invalid.timelineClips[0].sourceInFrame = 280; // A を [0,20) にする (尻の余白は 300 のまま)
    invalid.timelineClips[1].timelineStartFrame = 20;
    check(failsWith(invalid, "尺を超えています"), "clipの尺を超えるトランジションを受理しました");
    invalid = base;
    invalid.timelineClips[0].effects.fadeOutFrames = 10;
    check(failsWith(invalid, "フェードを設定できません"),
          "フェードとトランジションの併用を受理しました");
    invalid = base;
    invalid.timelineClips[1].effects.fadeInFrames = 10;
    check(failsWith(invalid, "フェードを設定できません"),
          "incoming側のフェードとトランジションの併用を受理しました");
    {
        // B を 60 frame にし、前後のトランジションが B の内側で 40 + 40 = 80 frame 重なる。
        auto overlap = base;
        overlap.timelineClips[1].sourceOutFrame = 360;
        auto c = clip("C");
        c.sourceFrameCount = 600;
        c.sourceInFrame = 300;
        c.sourceOutFrame = 600;
        c.timelineStartFrame = 360;
        overlap.timelineClips.push_back(c);
        overlap.timelineTransitions = {{"t1", "id-A", "id-B", 20, 40},
                                       {"t2", "id-B", c.id, 40, 20}};
        check(failsWith(overlap, "前後のトランジションが重なって"),
              "clipの内側で重なるトランジションを受理しました");
        overlap.timelineTransitions[1].framesBeforeCut = 20;
        check(mvm::project::validateTimeline(overlap).success,
              "対照: clipの内側で重ならない前後のトランジションを拒否しました");
        overlap.timelineTransitions.push_back({"t3", "id-A", c.id, 1, 0});
        check(failsWith(overlap, "接していません"), "接していない組のトランジションを受理しました");
        overlap.timelineTransitions.back() = {"t3", "id-A", "id-B", 1, 1};
        check(failsWith(overlap, "複数のトランジション"),
              "同じclip端の複数トランジションを受理しました");
    }
    {
        // クロスディゾルブは画面全体を覆う不透明な映像どうしでだけ置ける (incoming を不透明度 p で
        // 重ねる作りなので、透過・変形していると区間の終わりで不連続になる)。
        auto scaled = base;
        scaled.timelineClips[1].effects.scaleXPercent = 80;
        check(failsWith(scaled, "画面全体を覆う不透明"),
              "縮小したincomingのディゾルブを受理しました");
        auto moved = base;
        moved.timelineClips[0].effects.positionXPercent = 10;
        check(failsWith(moved, "画面全体を覆う不透明"),
              "移動したoutgoingのディゾルブを受理しました");
        auto cropped = base;
        cropped.timelineClips[1].effects.cropLeftPercent = 5;
        check(failsWith(cropped, "画面全体を覆う不透明"),
              "切り抜いたincomingのディゾルブを受理しました");
        auto translucent = base;
        translucent.timelineClips[1].effects.opacityPercent = 50;
        check(failsWith(translucent, "画面全体を覆う不透明"),
              "半透明のincomingのディゾルブを受理しました");
        // 区間 (outgoing の最後の 30 frame) に掛かる不透明度 key は拒否し、掛からなければ許す。
        auto keyedNear = base;
        keyedNear.timelineClips[0].effects.opacityKeys = {{0, 100.0}, {299, 90.0}};
        check(failsWith(keyedNear, "画面全体を覆う不透明"),
              "区間で不透明度の下がるoutgoingのディゾルブを受理しました");
        auto keyedFar = base;
        keyedFar.timelineClips[0].effects.opacityKeys = {{0, 50.0}, {100, 100.0}};
        check(mvm::project::validateTimeline(keyedFar).success,
              "区間の外の不透明度keyでディゾルブを拒否しました");
        // 音声のクロスフェードは映像の見た目に関係しないので制限しない。
        auto audio = base;
        for (auto& value : audio.timelineClips) {
            value.kind = mvm::project::TimelineClipKind::Audio;
            value.track = kA1;
            value.effects.volumePercent = 50;
        }
        check(mvm::project::validateTimeline(audio).success,
              "音量を変えた音声のクロスフェードを拒否しました");
        // 作るときも同じ理由で断る (余白不足と取り違えない)。
        auto refusedScaled = scaled;
        refusedScaled.timelineTransitions.clear();
        const auto refused = mvm::project::applyDefaultEditTransition(
            refusedScaled, "id-A", "id-B", 60, LinkMode::Linked, sequentialIds());
        check(!refused.success && refused.error.find("画面全体を覆う不透明") != std::string::npos,
              "縮小したclipへのディゾルブの作成を正しい理由で断りません");
    }
    {
        // フレーム保持 clip の端には置けない。
        auto held = base;
        held.timelineTransitions.clear();
        mvm::test::attachFixtureMedia(held);
        check(mvm::project::insertFrameHold(held, "id-A", 100, 30, sequentialIds()).success,
              "前提: フレーム保持を挿入できません");
        const auto* hold = [&]() -> const mvm::project::TimelineClip* {
            for (const auto& value : held.timelineClips)
                if (value.frameHold)
                    return &value;
            return nullptr;
        }();
        check(hold != nullptr, "前提: フレーム保持clipがありません");
        if (hold) {
            held.timelineTransitions = {{"t-hold", "id-A", hold->id, 0, 1}};
            check(failsWith(held, "フレーム保持"),
                  "フレーム保持clipのトランジションを受理しました");
        }
    }

    // 編集後の整合。
    const auto transitionsOf = [](const mvm::project::Project& project) {
        return project.timelineTransitions;
    };
    {
        auto edited = base;
        check(mvm::project::deleteTimelineClip(edited, 1).success &&
                  edited.timelineTransitions.empty(),
              "clipの削除でトランジションを消しません");
    }
    {
        auto edited = base;
        check(mvm::project::moveClip(edited, "id-B", kV1, 400).success &&
                  edited.timelineTransitions.empty(),
              "片側の移動で離れたclipのトランジションを消しません");
    }
    {
        auto edited = base;
        check(mvm::project::moveClips(edited, {"id-A", "id-B"}, "id-A", kV2, 0, LinkMode::Linked)
                      .success &&
                  transitionsOf(edited) == transitionsOf(base),
              "両clipを一緒に動かしたらトランジションが変わりました");
    }
    {
        // roll で cut を +290 動かすと、A の尻の余白が 10 frame になる。
        auto edited = base;
        check(mvm::project::rollTimelineEdit(edited, "id-A", TrimEdge::Right, 290, LinkMode::Linked)
                      .success &&
                  edited.timelineTransitions.size() == 1 &&
                  edited.timelineTransitions[0].framesBeforeCut == 30 &&
                  edited.timelineTransitions[0].framesAfterCut == 10,
              "余白の減ったトランジションを縮めません");
    }
    {
        // A を分割すると、トランジションの outgoing は右半分になる。
        auto edited = base;
        check(mvm::project::splitTimelineClips(edited, {"id-A"}, 100, sequentialIds(),
                                               LinkMode::Linked)
                      .success &&
                  edited.timelineTransitions.size() == 1 &&
                  edited.timelineTransitions[0].outgoingClipId == "new-1" &&
                  edited.timelineTransitions[0].incomingClipId == "id-B" &&
                  edited.timelineTransitions[0].framesBeforeCut == 30,
              "分割した右半分へトランジションを付け替えません");
    }
    {
        // トランジションの区間の中 (cut + 10) で B を切ると、B の左半分は 10 frame になる。
        auto edited = base;
        check(mvm::project::splitTimelineClips(edited, {"id-B"}, 310, sequentialIds(),
                                               LinkMode::Linked)
                      .success &&
                  edited.timelineTransitions.size() == 1 &&
                  edited.timelineTransitions[0].framesAfterCut == 10,
              "区間の中の分割でトランジションを縮めません");
    }
    {
        // 60fps -> 30fps で長さも秒位置で換算する (30 frame = 0.5 秒 -> 15 frame)。
        auto edited = base;
        check(mvm::project::setTimelineFrameRate(edited, 30, 1).success &&
                  edited.timelineTransitions.size() == 1 &&
                  edited.timelineTransitions[0].framesBeforeCut == 15 &&
                  edited.timelineTransitions[0].framesAfterCut == 15,
              "fps変更でトランジションの長さを換算しません");
    }
    {
        // reconcile は変える必要の無いトランジションを変えない。
        auto untouched = base;
        mvm::project::reconcileTimelineTransitions(untouched);
        check(untouched == base, "変更の要らないトランジションをreconcileが変えました");
    }
}

// Shift+E。1 つでも有効なら全部を無効に、全部が無効なら全部を有効にする。リンク相手も揃える。
void testToggleClipsEnabled() {
    using mvm::project::TimelineClipKind;
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("video");
    video.linkGroupId = "enabled-link";
    auto audio = clip("audio", TimelineClipKind::Audio, kA1);
    audio.linkGroupId = "enabled-link";
    auto other = clip("other", TimelineClipKind::Video, kV2);
    other.enabled = false;
    project.timelineClips = {video, audio, other};
    const auto enabledOf = [&](const std::string& id) { return findClip(project, id)->enabled; };

    check(mvm::project::toggleClipsEnabled(project, {video.id, other.id}).success &&
              !enabledOf(video.id) && !enabledOf(audio.id) && !enabledOf(other.id),
          "有効と無効の混ざった選択を全部無効にしません (リンク相手を含む)");
    check(mvm::project::toggleClipsEnabled(project, {video.id, other.id}).success &&
              enabledOf(video.id) && enabledOf(audio.id) && enabledOf(other.id),
          "全部無効の選択を全部有効にしません");
    const auto before = project;
    check(!mvm::project::toggleClipsEnabled(project, {}).success && project == before,
          "空の選択で有効/無効を切り換えました");
    check(!mvm::project::toggleClipsEnabled(project, {video.id, "missing"}).success &&
              project == before,
          "存在しないclipを含む選択で有効/無効を切り換えました");
}

// Shift+D (clip 選択)。期待値は手で数えた値である。
void testApplyDefaultClipFades() {
    using mvm::project::TimelineClipKind;
    check(mvm::project::defaultTransitionFrames(60, 1) == 60 &&
              mvm::project::defaultTransitionFrames(30000, 1001) == 30 &&
              mvm::project::defaultTransitionFrames(24000, 1001) == 24,
          "1秒のtimeline frame数が違います");

    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("video"); // 60fps 素材、300 frame、等速
    video.linkGroupId = "fade-link";
    auto audio = clip("audio", TimelineClipKind::Audio, kA1);
    audio.linkGroupId = "fade-link";
    auto fast = clip("fast", TimelineClipKind::Video, kV2); // 2 倍速: timeline 150 frame
    fast.speedNum = 2;
    auto shortClip = clip("short", TimelineClipKind::Video, kV2); // 50 frame
    shortClip.sourceOutFrame = 50;
    shortClip.timelineStartFrame = 200;
    project.timelineClips = {video, audio, fast, shortClip};
    check(mvm::project::validateTimeline(project).success, "フェード試験用のtimelineが不正です");
    const auto fadesOf = [&](const std::string& id) {
        const auto& effects = findClip(project, id)->effects;
        return std::pair<std::int64_t, std::int64_t>{effects.fadeInFrames, effects.fadeOutFrames};
    };

    check(mvm::project::applyDefaultClipFades(project, {video.id}, 60).success &&
              fadesOf(video.id) == std::pair<std::int64_t, std::int64_t>{60, 60} &&
              fadesOf(audio.id) == std::pair<std::int64_t, std::int64_t>{60, 60},
          "等速clipとリンク相手に60/60のフェードを付けません");
    check(mvm::project::applyDefaultClipFades(project, {fast.id}, 60).success &&
              fadesOf(fast.id) == std::pair<std::int64_t, std::int64_t>{120, 120},
          "2倍速clipのフェードを素材120 frameにしません");
    check(mvm::project::applyDefaultClipFades(project, {shortClip.id}, 60).success &&
              fadesOf(shortClip.id) == std::pair<std::int64_t, std::int64_t>{25, 25},
          "尺の足りないclipで前後を半分ずつにしません");
    const auto before = project;
    check(!mvm::project::applyDefaultClipFades(project, {video.id}, 60).success &&
              project == before,
          "変化の無いフェードの適用を受理しました");

    // 1 frame の clip は先頭だけが取る。
    auto single = mvm::project::createDefaultProject();
    auto one = clip("one");
    one.sourceOutFrame = 1;
    single.timelineClips = {one};
    check(mvm::project::applyDefaultClipFades(single, {one.id}, 60).success &&
              single.timelineClips[0].effects.fadeInFrames == 1 &&
              single.timelineClips[0].effects.fadeOutFrames == 0,
          "1 frameのclipのフェードが検証を通る形になりません");

    // トランジションのある端は変えない。
    auto withTransition = transitionProject();
    check(mvm::project::applyDefaultClipFades(withTransition, {"id-A", "id-B"}, 60).success,
          "トランジションのあるclipにフェードを付けられません");
    const auto& a = *findClip(withTransition, "id-A");
    const auto& b = *findClip(withTransition, "id-B");
    check(a.effects.fadeInFrames == 60 && a.effects.fadeOutFrames == 0 &&
              b.effects.fadeInFrames == 0 && b.effects.fadeOutFrames == 60 &&
              withTransition.timelineTransitions.size() == 1,
          "トランジションのある端のフェードを変えました");
}

// clip の端を指定位置へ動かす。30fps 素材は 60fps timeline で 2 frame 単位にしか動けない。
void testClipWithEdgeAt() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto half = clip("half");
    half.sourceFpsNum = 30;
    half.sourceFrameCount = 100;
    half.sourceOutFrame = 50; // timeline [0, 100)
    std::string error;
    const auto aligned =
        mvm::project::clipWithEdgeAt(project, half, mvm::project::TrimEdge::Right, 102, error);
    check(aligned && aligned->sourceOutFrame == 51 && aligned->sourceInFrame == 0,
          "2 frame単位の位置へ端を動かせません");
    const auto rounded =
        mvm::project::clipWithEdgeAt(project, half, mvm::project::TrimEdge::Right, 101, error);
    check(!rounded && !error.empty(), "素材frameへ一意に換算できない位置を受理しました");
    error.clear();
    const auto beyond =
        mvm::project::clipWithEdgeAt(project, half, mvm::project::TrimEdge::Right, 202, error);
    check(!beyond, "素材の範囲を超える位置を受理しました");
}

// Shift+D (編集点)。transitionProject の A / B は cut の前後に 300 frame ずつ余白がある。
// 期待値は手で数えた値である。
void testApplyDefaultEditTransition() {
    using mvm::project::LinkMode;
    auto base = transitionProject();
    base.timelineTransitions.clear();
    check(mvm::project::touchingClipId(base, "id-A", mvm::project::TrimEdge::Right) == "id-B" &&
              mvm::project::touchingClipId(base, "id-B", mvm::project::TrimEdge::Left) == "id-A" &&
              mvm::project::touchingClipId(base, "id-A", mvm::project::TrimEdge::Left).empty(),
          "接しているclipを求められません");

    auto centered = base;
    centered.timelineClips[0].effects.fadeOutFrames = 10;
    const auto placed = mvm::project::applyDefaultEditTransition(centered, "id-A", "id-B", 60,
                                                                 LinkMode::Linked, sequentialIds());
    check(placed.success && placed.frames == 60 && placed.transitionCount == 1 &&
              centered.timelineTransitions.size() == 1 &&
              centered.timelineTransitions[0].id == placed.transitionId &&
              centered.timelineTransitions[0].framesBeforeCut == 30 &&
              centered.timelineTransitions[0].framesAfterCut == 30 &&
              centered.timelineClips[0].effects.fadeOutFrames == 0,
          "編集点に中央揃えの60 frameのトランジションを置き、端のフェードを消しません");
    const auto replaced = mvm::project::applyDefaultEditTransition(
        centered, "id-A", "id-B", 20, LinkMode::Linked, sequentialIds());
    check(replaced.success && centered.timelineTransitions.size() == 1 &&
              centered.timelineTransitions[0].framesBeforeCut == 10 &&
              centered.timelineTransitions[0].framesAfterCut == 10,
          "同じ編集点の既存のトランジションを置き換えません");

    // outgoing の尻の余白が 10 frame しか無ければ、残りを cut の前へ寄せる。
    auto shortTail = base;
    shortTail.timelineClips[0].sourceFrameCount = 310;
    const auto shifted = mvm::project::applyDefaultEditTransition(
        shortTail, "id-A", "id-B", 60, LinkMode::Linked, sequentialIds());
    check(shifted.success && shifted.frames == 60 &&
              shortTail.timelineTransitions[0].framesBeforeCut == 50 &&
              shortTail.timelineTransitions[0].framesAfterCut == 10,
          "片側の余白が足りないときにもう片側へ寄せません");

    // 両側とも余白が無ければ作らない。
    auto noHandles = base;
    noHandles.timelineClips[0].sourceFrameCount = 300;
    noHandles.timelineClips[1].sourceInFrame = 0;
    noHandles.timelineClips[1].sourceOutFrame = 300;
    const auto noHandlesBefore = noHandles;
    const auto refused = mvm::project::applyDefaultEditTransition(
        noHandles, "id-A", "id-B", 60, LinkMode::Linked, sequentialIds());
    check(!refused.success && refused.error.find("余白が足りない") != std::string::npos &&
              noHandles == noHandlesBefore,
          "余白の無い編集点にトランジションを作りました");
    check(!mvm::project::applyDefaultEditTransition(base, "id-A", "missing", 60, LinkMode::Linked,
                                                    sequentialIds())
               .success,
          "存在しないclipの編集点にトランジションを作りました");

    // 30fps 素材は 60fps timeline で 2 frame 単位にしか延ばせない。61 frame は 60 frame に縮める。
    auto halfRate = base;
    for (auto& value : halfRate.timelineClips) {
        value.sourceFpsNum = 30;
        value.sourceFrameCount = 300;
    }
    halfRate.timelineClips[0].sourceOutFrame = 150;
    halfRate.timelineClips[1].sourceInFrame = 150;
    halfRate.timelineClips[1].sourceOutFrame = 300;
    check(mvm::project::validateTimeline(halfRate).success, "前提: 30fps素材のtimelineが不正です");
    const auto aligned = mvm::project::applyDefaultEditTransition(
        halfRate, "id-A", "id-B", 61, LinkMode::Linked, sequentialIds());
    check(aligned.success && aligned.frames == 60 &&
              halfRate.timelineTransitions[0].framesBeforeCut == 30 &&
              halfRate.timelineTransitions[0].framesAfterCut == 30,
          "素材frameに乗らない長さを縮めません");

    // 置く区間 (30 / 30) だけが不透明なら置ける。outgoing は最後の 30 frame だけが不透明
    // (local 269 以前は 99%)。求めた長さ 60 を事前に丸ごと検査して断らない。
    auto opaqueTail = base;
    opaqueTail.timelineClips[0].effects.opacityKeys = {{269, 99.0}, {270, 100.0}};
    const auto opaquePlaced = mvm::project::applyDefaultEditTransition(
        opaqueTail, "id-A", "id-B", 60, LinkMode::Linked, sequentialIds());
    check(opaquePlaced.success && opaquePlaced.frames == 60 &&
              opaqueTail.timelineTransitions[0].framesBeforeCut == 30 &&
              opaqueTail.timelineTransitions[0].framesAfterCut == 30,
          "置く区間だけが不透明なoutgoingへのディゾルブを断りました");
    // 80 frame なら cut の前は不透明な 30 frame まで、残りを後ろへ寄せる。
    auto opaqueLonger = base;
    opaqueLonger.timelineClips[0].effects.opacityKeys = {{269, 99.0}, {270, 100.0}};
    const auto shiftedByOpacity = mvm::project::applyDefaultEditTransition(
        opaqueLonger, "id-A", "id-B", 80, LinkMode::Linked, sequentialIds());
    check(shiftedByOpacity.success && shiftedByOpacity.frames == 80 &&
              opaqueLonger.timelineTransitions[0].framesBeforeCut == 30 &&
              opaqueLonger.timelineTransitions[0].framesAfterCut == 50,
          "不透明な範囲に合わせてcutの前後を寄せません");

    // 素材 frame に乗る長さは cut の前後で別々に決まる。outgoing が 30fps (尻へは 2 frame 単位)、
    // incoming が 60fps (頭の余白 40) なら、61 frame は 31 / 30 で置ける (60 へ縮めない)。
    auto mixedRate = base;
    mixedRate.timelineClips[0].sourceFpsNum = 30;
    mixedRate.timelineClips[0].sourceFrameCount = 300;
    mixedRate.timelineClips[0].sourceOutFrame = 150;
    mixedRate.timelineClips[1].sourceFrameCount = 600;
    mixedRate.timelineClips[1].sourceInFrame = 40;
    mixedRate.timelineClips[1].sourceOutFrame = 340;
    check(mvm::project::validateTimeline(mixedRate).success, "前提: fpsの違うtimelineが不正です");
    const auto mixedPlaced = mvm::project::applyDefaultEditTransition(
        mixedRate, "id-A", "id-B", 61, LinkMode::Linked, sequentialIds());
    check(mixedPlaced.success && mixedPlaced.frames == 61 &&
              mixedRate.timelineTransitions[0].framesBeforeCut == 31 &&
              mixedRate.timelineTransitions[0].framesAfterCut == 30,
          "素材frameに乗る組があるのに必要以上に縮めました");
    // ID は置いたトランジションの数だけ作る (長さを探す間に使い捨てない)。
    check(mixedPlaced.transitionId == "new-1", "長さの探索でトランジションIDを使い捨てました");

    // リンク相手 (音声) も同じ cut で接していれば一緒に置く。
    auto linked = base;
    auto audioA = linked.timelineClips[0];
    audioA.id = "id-audio-A";
    audioA.kind = mvm::project::TimelineClipKind::Audio;
    audioA.track = kA1;
    auto audioB = linked.timelineClips[1];
    audioB.id = "id-audio-B";
    audioB.kind = mvm::project::TimelineClipKind::Audio;
    audioB.track = kA1;
    linked.timelineClips[0].linkGroupId = audioA.linkGroupId = "link-A";
    linked.timelineClips[1].linkGroupId = audioB.linkGroupId = "link-B";
    linked.timelineClips.push_back(audioA);
    linked.timelineClips.push_back(audioB);
    check(mvm::project::validateTimeline(linked).success, "前提: リンクしたtimelineが不正です");
    const auto both = mvm::project::applyDefaultEditTransition(linked, "id-A", "id-B", 60,
                                                               LinkMode::Linked, sequentialIds());
    check(both.success && both.transitionCount == 2 && linked.timelineTransitions.size() == 2,
          "リンク相手の編集点にもトランジションを置きません");
    // 音声の尻の余白が 20 frame しか無くても、映像と音声は同じ長さ・同じ cut の前後で置く
    // (映像だけ 30 / 30 で音声が 40 / 20 のように食い違わない)。
    auto shortAudio = linked;
    shortAudio.timelineTransitions.clear();
    for (auto& value : shortAudio.timelineClips)
        if (value.id == "id-audio-A")
            value.sourceFrameCount = 320;
    const auto matched = mvm::project::applyDefaultEditTransition(
        shortAudio, "id-A", "id-B", 60, LinkMode::Linked, sequentialIds());
    check(matched.success && matched.frames == 60 && matched.transitionCount == 2 &&
              shortAudio.timelineTransitions.size() == 2 &&
              shortAudio.timelineTransitions[0].framesBeforeCut == 40 &&
              shortAudio.timelineTransitions[0].framesAfterCut == 20 &&
              shortAudio.timelineTransitions[1].framesBeforeCut == 40 &&
              shortAudio.timelineTransitions[1].framesAfterCut == 20,
          "リンク相手の余白に合わせて映像と音声のトランジションを同じ長さにしません");
    auto single = linked;
    single.timelineTransitions.clear();
    check(mvm::project::applyDefaultEditTransition(single, "id-A", "id-B", 60, LinkMode::Single,
                                                   sequentialIds())
                      .transitionCount == 1 &&
              single.timelineTransitions.size() == 1,
          "Singleでリンク相手にもトランジションを置きました");

    check(mvm::project::deleteTimelineTransition(linked, both.transitionId).success &&
              linked.timelineTransitions.size() == 1,
          "トランジションを削除できません");
    const auto beforeMissing = linked;
    check(!mvm::project::deleteTimelineTransition(linked, "missing").success &&
              linked == beforeMissing,
          "存在しないトランジションの削除を受理しました");
}

// 上書き移動。V1 の long [0, 300) の上へ V2 の mover (60 frame) を動かす。期待値は手で数えた値。
void testMoveOverwrite() {
    using mvm::project::LinkMode;
    const auto base = [] {
        auto project = mvm::project::createDefaultProject();
        auto mover = clip("mover", mvm::project::TimelineClipKind::Video, kV2);
        mover.sourceOutFrame = 60;
        project.timelineClips = {clip("long"), mover};
        return project;
    };
    const auto spanOf = [](const mvm::project::Project& project, const std::string& id) {
        const auto* found = findClip(project, id);
        return found ? std::pair<std::int64_t, std::int64_t>{found->timelineStartFrame,
                                                             clipEnd(project, *found)}
                     : std::pair<std::int64_t, std::int64_t>{-1, -1};
    };

    auto inside = base();
    check(mvm::project::moveClips(inside, {"id-mover"}, "id-mover", kV1, 100, LinkMode::Linked,
                                  sequentialIds())
                  .success &&
              spanOf(inside, "id-long") == std::pair<std::int64_t, std::int64_t>{0, 100} &&
              spanOf(inside, "id-mover") == std::pair<std::int64_t, std::int64_t>{100, 160} &&
              spanOf(inside, "new-1") == std::pair<std::int64_t, std::int64_t>{160, 300} &&
              findClip(inside, "new-1")->sourceInFrame == 160,
          "clipの中へ上書き移動して前後に分けません");

    auto tail = base();
    check(mvm::project::moveClips(tail, {"id-mover"}, "id-mover", kV1, 250, LinkMode::Linked,
                                  sequentialIds())
                  .success &&
              spanOf(tail, "id-long") == std::pair<std::int64_t, std::int64_t>{0, 250} &&
              tail.timelineClips.size() == 2,
          "clipの末尾へ上書き移動して末尾を削りません");

    auto head = base();
    check(mvm::project::moveClips(head, {"id-mover"}, "id-mover", kV1, 0, LinkMode::Linked,
                                  sequentialIds())
                  .success &&
              spanOf(head, "id-long") == std::pair<std::int64_t, std::int64_t>{60, 300} &&
              findClip(head, "id-long")->sourceInFrame == 60,
          "clipの先頭へ上書き移動して先頭を削りません");

    // 丸ごと覆った clip は消し、リンク相手は未リンクにする。
    auto covered = base();
    auto tiny = clip("tiny");
    tiny.sourceOutFrame = 30;
    tiny.timelineStartFrame = 400;
    tiny.linkGroupId = "tiny-link";
    auto tinyAudio = clip("tiny-audio", mvm::project::TimelineClipKind::Audio, kA1);
    tinyAudio.sourceOutFrame = 30;
    tinyAudio.timelineStartFrame = 400;
    tinyAudio.linkGroupId = "tiny-link";
    covered.timelineClips.push_back(tiny);
    covered.timelineClips.push_back(tinyAudio);
    check(mvm::project::validateTimeline(covered).success, "前提: 上書き試験のtimelineが不正です");
    check(mvm::project::moveClips(covered, {"id-mover"}, "id-mover", kV1, 390, LinkMode::Linked,
                                  sequentialIds())
                  .success &&
              findClip(covered, "id-tiny") == nullptr &&
              findClip(covered, "id-tiny-audio")->linkGroupId.empty(),
          "丸ごと覆ったclipを消してリンク相手を未リンクにしません");

    // newId を渡さなければ従来どおり重なりを拒否する。
    auto rejected = base();
    const auto before = rejected;
    check(!mvm::project::moveClips(rejected, {"id-mover"}, "id-mover", kV1, 100, LinkMode::Linked)
                  .success &&
              rejected == before,
          "上書きを指定しない移動で重なりを受理しました");
}

void testRippleTrim() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto first = clip("first");
    auto second = clip("second");
    second.timelineStartFrame = 300;
    second.linkGroupId = "ripple-trim-link";
    auto secondAudio = clip("second-audio", mvm::project::TimelineClipKind::Audio, kA1);
    secondAudio.timelineStartFrame = 300;
    secondAudio.linkGroupId = "ripple-trim-link";
    auto third = clip("third");
    third.timelineStartFrame = 700;
    auto other = clip("other", mvm::project::TimelineClipKind::Video, kV2);
    other.timelineStartFrame = 350;
    project.timelineClips = {first, second, secondAudio, third, other};
    check(mvm::project::validateTimeline(project).success, "ripple trim用のtimelineが不正です");

    check(mvm::project::rippleTrimTimelineClip(project, first.id, mvm::project::TrimEdge::Right,
                                               -100, mvm::project::LinkMode::Single)
              .success,
          "right端のripple trimに失敗しました");
    check(clipEnd(project, *findClip(project, first.id)) == 200 &&
              findClip(project, second.id)->timelineStartFrame == 200 &&
              findClip(project, secondAudio.id)->timelineStartFrame == 200 &&
              findClip(project, third.id)->timelineStartFrame == 600 &&
              findClip(project, other.id)->timelineStartFrame == 350,
          "ripple trimで後続clipとリンク相手だけを詰められません");

    check(mvm::project::rippleTrimTimelineClip(project, second.id, mvm::project::TrimEdge::Left, 50,
                                               mvm::project::LinkMode::Single)
              .success,
          "left端のripple trimに失敗しました");
    const auto* trimmed = findClip(project, second.id);
    check(trimmed->timelineStartFrame == 200 && trimmed->sourceInFrame == 50 &&
              findClip(project, third.id)->timelineStartFrame == 550 &&
              findClip(project, secondAudio.id)->timelineStartFrame == 200 &&
              findClip(project, secondAudio.id)->sourceInFrame == 0,
          "left端のripple trimでclip開始位置を保てないか、リンク相手までtrimしました");

    // 素材の範囲を越えるドラッグは失敗させず、素材の端で止める (Premiere と同じ)。
    check(mvm::project::rippleTrimTimelineClip(project, first.id, mvm::project::TrimEdge::Right,
                                               200, mvm::project::LinkMode::Single)
                  .success &&
              findClip(project, first.id)->sourceOutFrame == 300 &&
              findClip(project, second.id)->timelineStartFrame == 300 &&
              findClip(project, third.id)->timelineStartFrame == 650,
          "素材の範囲を越えるripple trimを素材の端で止めません");
    const auto beforeReject = project;
    check(!mvm::project::rippleTrimTimelineClip(project, first.id, mvm::project::TrimEdge::Right, 1,
                                                mvm::project::LinkMode::Single)
                  .success &&
              project == beforeReject,
          "素材の端に達したripple trimを拒否しないか、拒否時にProjectが変化しました");
}

// 素材 600 frame のうち [in, in + 300) を start へ置いた clip。
mvm::project::TimelineClip roomyClip(const char* name, std::int64_t in, std::int64_t start) {
    auto value = clip(name);
    value.sourceFrameCount = 600;
    value.sourceInFrame = in;
    value.sourceOutFrame = in + 300;
    value.timelineStartFrame = start;
    return value;
}

void testRollEdit() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    const auto outgoing = roomyClip("outgoing", 0, 0);
    const auto incoming = roomyClip("incoming", 100, 300);
    project.timelineClips = {outgoing, incoming};
    check(mvm::project::validateTimeline(project).success, "rolling用のtimelineが不正です");

    check(mvm::project::rollTimelineEdit(project, outgoing.id, mvm::project::TrimEdge::Right, 50,
                                         mvm::project::LinkMode::Single)
              .success,
          "right端のrolling編集に失敗しました");
    check(clipEnd(project, project.timelineClips[0]) == 350 &&
              project.timelineClips[1].timelineStartFrame == 350 &&
              project.timelineClips[1].sourceInFrame == 150 &&
              clipEnd(project, project.timelineClips[1]) == 600,
          "rolling編集で境界だけを動かせません");
    check(mvm::project::rollTimelineEdit(project, incoming.id, mvm::project::TrimEdge::Left, -20,
                                         mvm::project::LinkMode::Single)
                  .success &&
              clipEnd(project, project.timelineClips[0]) == 330 &&
              project.timelineClips[1].timelineStartFrame == 330,
          "incoming clipのleft端からrolling編集できません");

    const auto beforeReject = project;
    check(!mvm::project::rollTimelineEdit(project, outgoing.id, mvm::project::TrimEdge::Left, 10,
                                          mvm::project::LinkMode::Single)
                  .success &&
              project == beforeReject,
          "隣接clipが無いrolling編集を拒否しません");
    // incoming を消すほどのドラッグは、incoming を 1 frame 残すところで止める。
    check(mvm::project::rollTimelineEdit(project, outgoing.id, mvm::project::TrimEdge::Right, 400,
                                         mvm::project::LinkMode::Single)
                  .success &&
              clipEnd(project, project.timelineClips[0]) == 599 &&
              project.timelineClips[1].timelineStartFrame == 599 &&
              clipEnd(project, project.timelineClips[1]) == 600,
          "incoming clipを消すrolling編集を1 frame残して止めません");
    const auto beforeMinimum = project;
    check(!mvm::project::rollTimelineEdit(project, outgoing.id, mvm::project::TrimEdge::Right, 1,
                                          mvm::project::LinkMode::Single)
                  .success &&
              project == beforeMinimum,
          "incoming clipが1 frameのrolling編集を拒否しないか、拒否時にProjectが変化しました");

    // 29.97fps の outgoing は 60fps timeline の 602 frame 目に境界を置けない
    // (素材 300 frame = timeline 601、301 frame = 603)。隙間を作らず拒否する。
    mvm::project::Project fractional = mvm::project::createDefaultProject();
    auto ntsc = roomyClip("ntsc-outgoing", 0, 0);
    ntsc.sourceFpsNum = 30000;
    ntsc.sourceFpsDen = 1001;
    fractional.timelineClips = {ntsc, roomyClip("after-ntsc", 100, 601)};
    check(mvm::project::validateTimeline(fractional).success &&
              clipEnd(fractional, fractional.timelineClips[0]) == 601,
          "29.97fps rolling用のtimelineが不正です");
    const auto beforeFractional = fractional;
    check(!mvm::project::rollTimelineEdit(fractional, ntsc.id, mvm::project::TrimEdge::Right, 1,
                                          mvm::project::LinkMode::Single)
                  .success &&
              fractional == beforeFractional,
          "素材frameに揃わないrolling境界で隙間を作りました");
}

// V1 / A1 に [前 | リンク対 | 後] を並べる。リンク対は素材 600 frame の [100, 400) を 300 へ置く。
mvm::project::Project linkedToolFixture() {
    using mvm::project::TimelineClipKind;
    const auto roomy = [](const char* name, TimelineClipKind kind, mvm::project::TrackRef track,
                          std::int64_t in, std::int64_t start) {
        auto value = clip(name, kind, track);
        value.sourceFrameCount = 600;
        value.sourceInFrame = in;
        value.sourceOutFrame = in + 300;
        value.timelineStartFrame = start;
        return value;
    };
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = roomy("pair-video", TimelineClipKind::Video, kV1, 100, 300);
    auto audio = roomy("pair-audio", TimelineClipKind::Audio, kA1, 100, 300);
    video.linkGroupId = "pair";
    audio.linkGroupId = "pair";
    project.timelineClips = {roomy("before-video", TimelineClipKind::Video, kV1, 0, 0),
                             roomy("before-audio", TimelineClipKind::Audio, kA1, 0, 0),
                             video,
                             audio,
                             roomy("after-video", TimelineClipKind::Video, kV1, 100, 600),
                             roomy("after-audio", TimelineClipKind::Audio, kA1, 100, 600)};
    return project;
}

// 既定 (Linked) はリンク相手にも同じ編集を適用し、Single (Alt) は操作した clip だけを編集する。
void testLinkedToolEditing() {
    using mvm::project::LinkMode;
    using mvm::project::TrimEdge;
    const auto base = linkedToolFixture();
    check(mvm::project::validateTimeline(base).success, "リンク編集用のtimelineが不正です");
    const auto at = [](const mvm::project::Project& project, const char* id) {
        return *findClip(project, std::string("id-") + id);
    };

    auto project = base;
    check(mvm::project::trimTimelineClip(project, "id-pair-video", TrimEdge::Right, -50,
                                         LinkMode::Linked)
                  .success &&
              at(project, "pair-audio").sourceOutFrame == 350,
          "Linkedのtrimがリンク相手の端を動かしません");
    project = base;
    check(mvm::project::trimTimelineClip(project, "id-pair-video", TrimEdge::Right, -50,
                                         LinkMode::Single)
                  .success &&
              at(project, "pair-video").sourceOutFrame == 350 &&
              at(project, "pair-audio").sourceOutFrame == 400,
          "Singleのtrimがリンク相手まで動かしました");

    project = base;
    check(mvm::project::rippleTrimTimelineClip(project, "id-pair-video", TrimEdge::Right, -50,
                                               LinkMode::Linked)
                  .success &&
              at(project, "pair-audio").sourceOutFrame == 350 &&
              at(project, "after-video").timelineStartFrame == 550 &&
              at(project, "after-audio").timelineStartFrame == 550,
          "Linkedのripple trimがリンク相手のtrackを詰めません");
    project = base;
    check(mvm::project::rippleTrimTimelineClip(project, "id-pair-video", TrimEdge::Right, -50,
                                               LinkMode::Single)
                  .success &&
              at(project, "pair-audio").sourceOutFrame == 400 &&
              at(project, "after-video").timelineStartFrame == 550 &&
              at(project, "after-audio").timelineStartFrame == 600,
          "Singleのripple trimがリンク相手のtrackまで動かしました");

    project = base;
    check(mvm::project::rollTimelineEdit(project, "id-pair-video", TrimEdge::Right, 20,
                                         LinkMode::Linked)
                  .success &&
              at(project, "pair-audio").sourceOutFrame == 420 &&
              at(project, "after-audio").timelineStartFrame == 620 &&
              at(project, "after-video").timelineStartFrame == 620,
          "Linkedのrolling編集がリンク相手の編集点を動かしません");
    project = base;
    check(mvm::project::rollTimelineEdit(project, "id-pair-video", TrimEdge::Right, 20,
                                         LinkMode::Single)
                  .success &&
              at(project, "pair-audio").sourceOutFrame == 400 &&
              at(project, "after-audio").timelineStartFrame == 600,
          "Singleのrolling編集がリンク相手の編集点まで動かしました");
    // L カット: リンク相手が編集点を持たなければ、相手はそのまま残す。
    project = base;
    std::erase_if(project.timelineClips,
                  [](const auto& value) { return value.id == "id-after-audio"; });
    check(mvm::project::rollTimelineEdit(project, "id-pair-video", TrimEdge::Right, 20,
                                         LinkMode::Linked)
                  .success &&
              at(project, "pair-video").sourceOutFrame == 420 &&
              at(project, "pair-audio").sourceOutFrame == 400,
          "編集点を持たないリンク相手でrolling編集を拒否したか、相手を動かしました");

    project = base;
    check(mvm::project::slipTimelineClip(project, "id-pair-video", 30, LinkMode::Linked).success &&
              at(project, "pair-video").sourceInFrame == 130 &&
              at(project, "pair-audio").sourceInFrame == 130,
          "Linkedのslipがリンク相手をずらしません");
    // 全員がずらせる範囲で止め、リンク相手と同期を崩さない。
    project = base;
    for (auto& value : project.timelineClips) {
        if (value.id == "id-pair-audio")
            value.sourceFrameCount = 420;
    }
    check(mvm::project::slipTimelineClip(project, "id-pair-video", 30, LinkMode::Linked).success &&
              at(project, "pair-video").sourceInFrame == 120 &&
              at(project, "pair-audio").sourceInFrame == 120,
          "Linkedのslipをリンク相手の素材の端で止めません");
    check(mvm::project::slipTimelineClip(project, "id-pair-video", 30, LinkMode::Single).success &&
              at(project, "pair-video").sourceInFrame == 150 &&
              at(project, "pair-audio").sourceInFrame == 120,
          "Singleのslipがリンク相手までずらしました");

    project = base;
    check(mvm::project::slideTimelineClip(project, "id-pair-video", 20, LinkMode::Linked).success &&
              at(project, "pair-audio").timelineStartFrame == 320 &&
              clipEnd(project, at(project, "before-audio")) == 320,
          "Linkedのslideがリンク相手をスライドしません");
    // リンク相手に接している clip が無い (L / J カット) 場合、相手は空白の分だけしか動けない。
    project = base;
    std::erase_if(project.timelineClips, [](const auto& value) {
        return value.id == "id-before-audio" || value.id == "id-after-audio";
    });
    auto blocker = clip("audio-blocker", mvm::project::TimelineClipKind::Audio, kA1);
    blocker.sourceOutFrame = 30;
    blocker.timelineStartFrame = 250; // pair-audio (300 開始) との間に 20 frame の空白
    project.timelineClips.push_back(blocker);
    check(mvm::project::validateTimeline(project).success, "空白slide用のtimelineが不正です");
    const auto gapRange =
        mvm::project::clampSlideEdit(project, "id-pair-video", -1000, LinkMode::Linked);
    check(gapRange.success && gapRange.frame == -20, "リンク相手の前の空白でslideを止めません");

    project = base;
    check(mvm::project::slideTimelineClip(project, "id-pair-video", 20, LinkMode::Single).success &&
              at(project, "pair-video").timelineStartFrame == 320 &&
              at(project, "pair-audio").timelineStartFrame == 300,
          "Singleのslideがリンク相手までスライドしました");

    project = base;
    check(mvm::project::splitTimelineClips(project, {"id-pair-video"}, 400, sequentialIds(),
                                           LinkMode::Single)
                  .success &&
              project.timelineClips.size() == base.timelineClips.size() + 1 &&
              project.timelineClips.back().linkGroupId.empty() &&
              at(project, "pair-audio").sourceOutFrame == 400,
          "Singleの分割がリンク相手まで切ったか、右半分をリンクしたままにしました");

    project = base;
    check(mvm::project::moveClips(project, {"id-pair-video"}, "id-pair-video", kV2, 900,
                                  LinkMode::Single)
                  .success &&
              at(project, "pair-video").timelineStartFrame == 900 &&
              at(project, "pair-audio").timelineStartFrame == 300,
          "Singleの移動がリンク相手まで動かしました");
    project = base;
    check(mvm::project::moveClips(project, {"id-pair-video"}, "id-pair-video", kV2, 900,
                                  LinkMode::Linked)
                  .success &&
              at(project, "pair-audio").timelineStartFrame == 900,
          "Linkedの移動がリンク相手を動かしません");
}

// レーザーで分割し前半を削除した後、右の clip の left 端を大きく引き延ばすと、
// 分割前の clip (素材の先頭、timeline 先頭) まで戻り、それを越える分は止まる。
// 以前は越えた分で trim 全体が失敗し、clip が元の位置へ戻っていた。
void testTrimRestoresSplitClip() {
    using mvm::project::LinkMode;
    using mvm::project::TrimEdge;
    mvm::project::Project project = mvm::project::createDefaultProject();
    // 60fps timeline に 30fps 素材 (音声付き) を置く。
    auto video = clip("restore-video");
    auto audio = clip("restore-audio", mvm::project::TimelineClipKind::Audio, kA1);
    for (auto* value : {&video, &audio}) {
        value->sourceFpsNum = 30;
        value->sourceFrameCount = 300;
        value->sourceOutFrame = 300;
        value->linkGroupId = "restore";
    }
    project.timelineClips = {video, audio};
    check(mvm::project::validateTimeline(project).success, "復元試験のtimelineが不正です");
    check(mvm::project::splitTimelineClips(project, {video.id}, 241, sequentialIds(),
                                           LinkMode::Linked)
                  .success &&
              project.timelineClips.size() == 4,
          "復元試験の分割に失敗しました");
    const auto* left = findClip(project, video.id);
    check(left &&
              mvm::project::deleteTimelineClip(
                  project, static_cast<int>(left - project.timelineClips.data()))
                  .success &&
              project.timelineClips.size() == 2,
          "復元試験で前半を削除できません");
    const std::string rightId =
        project.timelineClips[0].kind == mvm::project::TimelineClipKind::Audio
            ? project.timelineClips[1].id
            : project.timelineClips[0].id;
    check(
        mvm::project::trimTimelineClip(project, rightId, TrimEdge::Left, -100000, LinkMode::Linked)
            .success,
        "素材の先頭を越えるleft trimを素材の先頭で止めません");
    for (const auto& value : project.timelineClips) {
        check(value.sourceInFrame == 0 && value.sourceOutFrame == 300 &&
                  value.timelineStartFrame == 0 && clipEnd(project, value) == 600,
              "分割前のclipまで引き延ばせません");
    }

    // 通常の trim は timeline 先頭より前へ出さない。リップルは開始位置を保つので制約を受けない。
    mvm::project::Project offset = mvm::project::createDefaultProject();
    offset.timelineClips = {roomyClip("offset", 200, 50)};
    const auto trimRange =
        mvm::project::clampEdgeEdit(offset, "id-offset", TrimEdge::Left,
                                    mvm::project::EdgeEditKind::Trim, -1000, LinkMode::Single);
    const auto rippleRange =
        mvm::project::clampEdgeEdit(offset, "id-offset", TrimEdge::Left,
                                    mvm::project::EdgeEditKind::Ripple, -1000, LinkMode::Single);
    check(trimRange.success && trimRange.frame == -50 && rippleRange.success &&
              rippleRange.frame == -200,
          "left端の伸長をtimeline先頭または素材の先頭で止めません");
    const auto shrinkRange =
        mvm::project::clampEdgeEdit(offset, "id-offset", TrimEdge::Right,
                                    mvm::project::EdgeEditKind::Trim, -1000, LinkMode::Single);
    check(shrinkRange.success && shrinkRange.frame == -299,
          "right端の短縮を1 frame残して止めません");
}

void testSlipClip() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {roomyClip("slip", 100, 50)};
    check(mvm::project::slipTimelineClip(project, "id-slip", 30, mvm::project::LinkMode::Single)
                  .success &&
              project.timelineClips[0].sourceInFrame == 130 &&
              project.timelineClips[0].sourceOutFrame == 430 &&
              project.timelineClips[0].timelineStartFrame == 50,
          "slipでin/outだけをずらせません");
    check(mvm::project::slipTimelineClip(project, "id-slip", 1000, mvm::project::LinkMode::Single)
                  .success &&
              project.timelineClips[0].sourceInFrame == 300 &&
              project.timelineClips[0].sourceOutFrame == 600,
          "slipを素材末尾で止められません");
    const auto beforeReject = project;
    check(!mvm::project::slipTimelineClip(project, "id-slip", 5, mvm::project::LinkMode::Single)
                  .success &&
              project == beforeReject,
          "素材末尾でのslipを拒否しません");
    check(mvm::project::slipTimelineClip(project, "id-slip", -1000, mvm::project::LinkMode::Single)
                  .success &&
              project.timelineClips[0].sourceInFrame == 0 &&
              project.timelineClips[0].sourceOutFrame == 300,
          "slipを素材先頭で止められません");
}

void testSlideClip() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips = {roomyClip("before", 0, 0), roomyClip("slide", 100, 300),
                             roomyClip("after", 100, 600)};
    check(mvm::project::validateTimeline(project).success, "slide用のtimelineが不正です");
    check(mvm::project::slideTimelineClip(project, "id-slide", 50, mvm::project::LinkMode::Linked)
              .success,
          "slideに失敗しました");
    check(project.timelineClips[1].timelineStartFrame == 350 &&
              project.timelineClips[1].sourceInFrame == 100 &&
              clipEnd(project, project.timelineClips[0]) == 350 &&
              project.timelineClips[2].timelineStartFrame == 650 &&
              project.timelineClips[2].sourceInFrame == 150 &&
              clipEnd(project, project.timelineClips[2]) == 900,
          "slideで前後clipのout/inを追従させられません");

    // 動かせる範囲 = 前の clip の out と後ろの clip の in を動かせる範囲。+50 のスライド後は
    // before (素材 [0, 350)) の out が [-349, +250]、after (素材 [150, 400)) の in が [-150,
    // +249]。
    auto range =
        mvm::project::clampSlideEdit(project, "id-slide", 1000, mvm::project::LinkMode::Linked);
    check(range.success && range.frame == 249, "slideの右方向の範囲が違います");
    range =
        mvm::project::clampSlideEdit(project, "id-slide", -1000, mvm::project::LinkMode::Linked);
    check(range.success && range.frame == -150, "slideの左方向の範囲が違います");

    // 範囲を越えるドラッグは失敗させず、後ろの clip の素材の先頭で止める。
    check(mvm::project::slideTimelineClip(project, "id-slide", -400, mvm::project::LinkMode::Linked)
                  .success &&
              project.timelineClips[1].timelineStartFrame == 200 &&
              clipEnd(project, project.timelineClips[0]) == 200 &&
              project.timelineClips[2].timelineStartFrame == 500 &&
              project.timelineClips[2].sourceInFrame == 0,
          "範囲を越えるslideを後ろのclipの素材の先頭で止めません");
    const auto beforeReject = project;
    check(!mvm::project::slideTimelineClip(project, "id-slide", -1, mvm::project::LinkMode::Linked)
                  .success &&
              project == beforeReject,
          "後ろのclipの素材の先頭に達したslideを拒否しないか、拒否時にProjectが変化しました");

    // 前の clip が素材の末尾まで使い切っていれば、右へはスライドできない。
    mvm::project::Project exhausted = mvm::project::createDefaultProject();
    exhausted.timelineClips = {roomyClip("before", 300, 0), roomyClip("slide", 100, 300),
                               roomyClip("after", 100, 600)};
    const auto beforeExhausted = exhausted;
    check(
        !mvm::project::slideTimelineClip(exhausted, "id-slide", 50, mvm::project::LinkMode::Linked)
                .success &&
            exhausted == beforeExhausted &&
            mvm::project::clampSlideEdit(exhausted, "id-slide", 50, mvm::project::LinkMode::Linked)
                    .frame == 0,
        "前のclipの素材が足りないslideを拒否しません");
    check(
        mvm::project::slideTimelineClip(exhausted, "id-slide", -50, mvm::project::LinkMode::Linked)
            .success,
        "前のclipの素材が足りなくても左へのslideができません");

    // 前後どちらかに接している clip が無ければ、単なる移動にせず拒否する。
    for (const bool withoutPrevious : {true, false}) {
        mvm::project::Project oneSided = mvm::project::createDefaultProject();
        oneSided.timelineClips = {roomyClip("slide", 100, 300)};
        oneSided.timelineClips.push_back(withoutPrevious ? roomyClip("after", 100, 600)
                                                         : roomyClip("before", 0, 0));
        const auto beforeOneSided = oneSided;
        check(!mvm::project::slideTimelineClip(oneSided, "id-slide", 20,
                                               mvm::project::LinkMode::Linked)
                      .success &&
                  oneSided == beforeOneSided,
              withoutPrevious ? "前のclipが無いslideを受理しました"
                              : "後ろのclipが無いslideを受理しました");
    }
    mvm::project::Project alone = mvm::project::createDefaultProject();
    alone.timelineClips = {roomyClip("slide", 100, 300)};
    check(!mvm::project::slideTimelineClip(alone, "id-slide", 20, mvm::project::LinkMode::Linked)
               .success,
          "前後にclipが無いslideを移動として受理しました");
}

void testTrackSelectFromFrame() {
    auto project = threeClips();
    auto upper = clip("upper", mvm::project::TimelineClipKind::Video, kV2);
    upper.timelineStartFrame = 100;
    project.timelineClips.push_back(upper);
    using mvm::project::SelectDirection;
    check(mvm::project::clipIdsFromFrame(project, 350, SelectDirection::Forward) ==
              std::vector<std::string>({"id-Manim", "id-B", "id-upper"}),
          "前方選択の対象が違います");
    check(mvm::project::clipIdsFromFrame(project, 350, SelectDirection::Backward) ==
              std::vector<std::string>({"id-A", "id-Manim", "id-upper"}),
          "後方選択の対象が違います");
    check(mvm::project::clipIdsFromFrame(project, 350, SelectDirection::Forward, kV1) ==
              std::vector<std::string>({"id-Manim", "id-B"}),
          "track指定の前方選択が他trackのclipを含みました");
    check(mvm::project::clipIdsFromFrame(project, 300, SelectDirection::Forward, kV1) ==
              std::vector<std::string>({"id-Manim", "id-B"}),
          "前方選択がframe直前で終わるclipを含みました");
}

void testValidationFailures() {
    auto duplicate = threeClips();
    duplicate.timelineClips[1].id = duplicate.timelineClips[0].id;
    check(!mvm::project::validateTimeline(duplicate).success, "重複clip IDを拒否しません");

    auto badRate = threeClips();
    badRate.timelineClips[0].sourceFpsNum = 0;
    check(!mvm::project::validateTimeline(badRate).success, "不正source FPSを拒否しません");

    auto emptyRange = threeClips();
    emptyRange.timelineClips[0].sourceOutFrame = emptyRange.timelineClips[0].sourceInFrame;
    check(!mvm::project::validateTimeline(emptyRange).success, "duration 0のclipを拒否しません");

    auto missingTrack = threeClips();
    missingTrack.timelineClips[0].track = mvm::project::TrackRef{mvm::project::TrackKind::Video, 7};
    check(!mvm::project::validateTimeline(missingTrack).success,
          "存在しないtrackを指すclipを拒否しません");

    auto noVideoTrack = threeClips();
    noVideoTrack.videoTracks.clear();
    check(!mvm::project::validateTimeline(noVideoTrack).success,
          "video trackが0本のProjectを拒否しません");

    auto gap = threeClips();
    gap.timelineClips[0].timelineStartFrame = 0;
    gap.timelineClips[1].timelineStartFrame = 400;
    gap.timelineClips[2].timelineStartFrame = 800;
    const auto gapResult = mvm::project::validateTimeline(gap);
    check(gapResult.success && gapResult.totalFrames == 1100, "timeline gapを受理しません");
}

void testDeleteSelection() {
    auto middle = threeClips();
    const auto middleResult = mvm::project::deleteTimelineClip(middle, 1);
    check(middleResult.success && middleResult.selectedIndex == 1,
          "中央削除後に右隣を選択しません");
    check(middle.timelineClips.size() == 2 && middle.timelineClips[1].name == "B",
          "中央 clip を削除できません");

    auto first = threeClips();
    const auto firstResult = mvm::project::deleteTimelineClip(first, 0);
    check(firstResult.success && firstResult.selectedIndex == 0, "先頭削除後に右隣を選択しません");

    auto last = threeClips();
    const auto lastResult = mvm::project::deleteTimelineClip(last, 2);
    check(lastResult.success && lastResult.selectedIndex == 1, "末尾削除後に左隣を選択しません");

    mvm::project::Project only = mvm::project::createDefaultProject();
    only.timelineClips.push_back(clip("only"));
    const auto onlyResult = mvm::project::deleteTimelineClip(only, 0);
    check(onlyResult.success && onlyResult.selectedIndex == -1 && only.timelineClips.empty(),
          "最後の clip 削除後が未選択になりません");

    const auto before = middle.timelineClips;
    check(!mvm::project::deleteTimelineClip(middle, 9).success, "不正 index の削除を拒否しません");
    check(middle.timelineClips == before, "拒否した削除で timeline が変化しました");
}

void testLinkedClipEditing() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    auto video = clip("linked-video");
    auto audio = clip("linked-audio", mvm::project::TimelineClipKind::Audio, kA1);
    video.linkGroupId = "link-1";
    audio.linkGroupId = "link-1";
    project.timelineClips = {video, audio};

    const auto moved = mvm::project::moveClip(project, video.id, kV2, 120);
    check(moved.success && project.timelineClips[0].track == kV2 &&
              project.timelineClips[0].timelineStartFrame == 120 &&
              project.timelineClips[1].track == kA1 &&
              project.timelineClips[1].timelineStartFrame == 120,
          "リンク移動で時間だけを同期できません");

    auto blocked = project;
    auto blocker = clip("audio-blocker", mvm::project::TimelineClipKind::Audio, kA1);
    blocker.timelineStartFrame = 500;
    blocked.timelineClips.push_back(blocker);
    const auto beforeBlocked = blocked.timelineClips;
    check(!mvm::project::moveClip(blocked, video.id, kV1, 300).success &&
              blocked.timelineClips == beforeBlocked,
          "リンク先が衝突する移動を受理しました");

    const auto unlinked = mvm::project::unlinkTimelineClip(project, video.id);
    check(unlinked.success && project.timelineClips[0].linkGroupId.empty() &&
              project.timelineClips[1].linkGroupId.empty(),
          "video/audioリンクを双方から解除できません");
    check(!mvm::project::unlinkTimelineClip(project, video.id).success,
          "未リンクclipのリンク解除を成功にしました");

    video.linkGroupId = "link-2";
    audio.linkGroupId = "link-2";
    project.timelineClips = {video, audio};
    const auto deleted = mvm::project::deleteTimelineClip(project, 0);
    check(deleted.success && project.timelineClips.empty(),
          "リンクclipの削除でvideo/audioの両方を削除しません");

    auto malformed = mvm::project::createDefaultProject();
    auto first = clip("same-kind-1");
    auto second = clip("same-kind-2", mvm::project::TimelineClipKind::Video, kV2);
    first.linkGroupId = "bad-link";
    second.linkGroupId = "bad-link";
    malformed.timelineClips = {first, second};
    check(!mvm::project::validateTimeline(malformed).success,
          "同種clip同士の不正なリンクを受理しました");

    auto orphan = mvm::project::createDefaultProject();
    video.linkGroupId = "orphan-link";
    orphan.timelineClips = {video};
    check(!mvm::project::validateTimeline(orphan).success,
          "counterpartの無いorphan linkを受理しました");

    auto placedPair = mvm::project::createDefaultProject();
    video.linkGroupId = "atomic-link";
    audio.linkGroupId = "atomic-link";
    const auto pairResult =
        mvm::project::placeLinkedAvPairAt(placedPair, video, kV1, audio, kA1, 42);
    check(pairResult.success && pairResult.selectedIndex == 0 &&
              placedPair.timelineClips.size() == 2 &&
              placedPair.timelineClips[0].timelineStartFrame == 42 &&
              placedPair.timelineClips[1].timelineStartFrame == 42,
          "linked A/V pairを単一transactionで配置できません");

    auto blockedPair = placedPair;
    const auto beforePairFailure = blockedPair.timelineClips;
    video.id = "blocked-video";
    audio.id = "blocked-audio";
    video.linkGroupId = "blocked-link";
    audio.linkGroupId = "blocked-link";
    check(!mvm::project::placeLinkedAvPairAt(blockedPair, video, kV1, audio, kA1, 42).success &&
              blockedPair.timelineClips == beforePairFailure,
          "linked pairの衝突失敗で片側だけをcommitしました");
}

void testManimPlacement() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    project.timelineClips.push_back(clip("A"));
    mvm::project::ManimAsset asset;
    asset.sceneName = "Scene";
    asset.generatedVideoPath = "scene.mp4";
    project.manimAssets.push_back(asset);

    const auto end = mvm::project::timelineTrackEndFrame(project, kV1);
    check(end.success && end.frame == 300, "track末尾frameが違います");
    const auto placed = mvm::project::appendManimTimelineClipAt(
        project, project.manimAssets.front(), "id-manim", 60, 1, 300, end.frame, kV1);
    check(placed.success && placed.selectedIndex == 1, "Manim clip を末尾へ配置できません");
    check(project.timelineClips.back().kind == mvm::project::TimelineClipKind::Manim,
          "配置した clip が Manim ではありません");
    check(!mvm::project::appendManimTimelineClipAt(project, project.manimAssets.front(),
                                                   "id-manim-2", 60, 1, 300, 600, kV1)
               .success,
          "同じ Manim asset の重複配置を拒否しません");

    const auto deleted = mvm::project::deleteTimelineClip(project, 1);
    check(deleted.success && project.manimAssets.size() == 1,
          "Manim clip の削除で asset まで削除されました");
}

void testPersistenceTransaction(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "テスト directory を作成できません");

    auto live = threeClips();
    auto candidate = live;
    candidate.timelineClips[1].sourceFpsNum = 30000;
    candidate.timelineClips[1].sourceFpsDen = 1001;
    candidate.timelineClips[1].sourceInFrame = 30;
    candidate.timelineClips[1].sourceOutFrame = 270;
    candidate.timelineClips[1].timelineStartFrame = 300;
    // 速度も round-trip する (29.97fps 素材 240 frame の 80%)。
    candidate.timelineClips[1].speedNum = 4;
    candidate.timelineClips[1].speedDen = 5;
    candidate.timelineClips[2].track = kV2;
    candidate.timelineClips[2].timelineStartFrame = 90;
    candidate.timelineClips[2].effects.scaleXPercent =
        candidate.timelineClips[2].effects.scaleYPercent = 60.0;
    candidate.timelineClips[2].effects.opacityPercent = 55.0;
    candidate.outputWidth = 3840;
    candidate.outputHeight = 2160;
    check(mvm::project::addTrack(candidate, mvm::project::TrackKind::Audio).success,
          "保存用candidateへaudio trackを追加できません");
    candidate.audioTracks[0].muted = true;
    auto audio = clip("voice", mvm::project::TimelineClipKind::Audio, kA1);
    audio.timelineStartFrame = 42;
    // ピッチ保持は等速では持てないので、保存を確かめるために 50% にする。
    audio.speedNum = 1;
    audio.speedDen = 2;
    audio.preservePitch = true;
    audio.linkGroupId = "round-trip-link";
    candidate.timelineClips[2].linkGroupId = "round-trip-link";
    candidate.timelineClips.push_back(audio);
    mvm::test::attachFixtureMedia(candidate);
    check(mvm::project::validateTimeline(candidate).success,
          "複数track配置とeffectsを持つ保存candidateが不正です");

    const auto projectFile = root / "project.mvm";
    const auto saved =
        mvm::project::saveProjectJsonTransaction(live, std::move(candidate), projectFile);
    check(saved.success, "編集済み Project を保存できません");
    const auto loaded = mvm::project::loadProjectJson(projectFile);
    check(loaded.success, "保存した .mvm を読み込めません");
    bool timelineFieldsMatch =
        loaded.success && loaded.project.schemaVersion == mvm::project::kProjectSchemaVersion &&
        loaded.project.timelineFpsNum == 60 && loaded.project.timelineFpsDen == 1 &&
        loaded.project.outputWidth == 3840 && loaded.project.outputHeight == 2160 &&
        loaded.project.videoTracks == live.videoTracks &&
        loaded.project.audioTracks == live.audioTracks &&
        loaded.project.timelineClips.size() == live.timelineClips.size();
    if (timelineFieldsMatch) {
        for (std::size_t index = 0; index < live.timelineClips.size(); ++index) {
            const auto& actual = loaded.project.timelineClips[index];
            const auto& expected = live.timelineClips[index];
            timelineFieldsMatch =
                timelineFieldsMatch && actual.id == expected.id && actual.kind == expected.kind &&
                actual.sourceFpsNum == expected.sourceFpsNum &&
                actual.sourceFpsDen == expected.sourceFpsDen &&
                actual.sourceFrameCount == expected.sourceFrameCount &&
                actual.sourceInFrame == expected.sourceInFrame &&
                actual.sourceOutFrame == expected.sourceOutFrame &&
                actual.timelineStartFrame == expected.timelineStartFrame &&
                actual.track == expected.track && actual.effects == expected.effects &&
                actual.linkGroupId == expected.linkGroupId &&
                actual.speedNum == expected.speedNum && actual.speedDen == expected.speedDen &&
                actual.preservePitch == expected.preservePitch &&
                actual.frameHold == expected.frameHold;
        }
    }
    check(timelineFieldsMatch,
          "現在のschemaのoutput "
          "size・track構成・mute・clip種別・trim・effects・速度がround-tripしません");

    const auto serialized = mvm::project::serializeProjectJson(live, projectFile);
    check(serialized.success, "schema 検査用 JSON を作成できません");
    if (serialized.success) {
        auto withoutField = [&](std::string field) {
            auto text = serialized.json;
            const auto fieldStart = text.find("      \"" + field + "\": ");
            check(fieldStart != std::string::npos, "必須 JSON field が出力されません");
            if (fieldStart != std::string::npos) {
                const auto fieldEnd = text.find('\n', fieldStart);
                text.erase(fieldStart, fieldEnd - fieldStart + 1);
                check(!mvm::project::parseProjectJsonText(text, projectFile).success,
                      "必須 JSON field の欠落を受理しました");
            }
        };
        withoutField("preserve_pitch");
        withoutField("enabled");
        withoutField("frame_hold");
        {
            // トップレベルの timeline_transitions も必須。
            auto text = serialized.json;
            const auto key = text.find("  \"timeline_transitions\": [");
            check(key != std::string::npos, "timeline_transitions が出力されません");
            if (key != std::string::npos) {
                const auto end = text.find("],\n", key);
                text.erase(key, end + 3 - key);
                check(!mvm::project::parseProjectJsonText(text, projectFile).success,
                      "timeline_transitions の欠落を受理しました");
            }
        }
        auto oldSchema = serialized.json;
        const auto schema = oldSchema.find("\"schema_version\": 13");
        check(schema != std::string::npos, "schema 13 が出力されません");
        if (schema != std::string::npos) {
            oldSchema.replace(schema, std::string("\"schema_version\": 13").size(),
                              "\"schema_version\": 12");
            check(!mvm::project::parseProjectJsonText(oldSchema, projectFile).success,
                  "schema 12 を受理しました");
        }
    }

    auto invalidOutput = mvm::project::createDefaultProject();
    invalidOutput.outputWidth = 0;
    check(!mvm::project::validateTimeline(invalidOutput).success,
          "幅0のProject output sizeを受理しました");

    // format marker が無いファイルは .mvm として受理しない。
    const auto strangerPath = root / "stranger.mvm";
    {
        std::ofstream stranger(strangerPath, std::ios::binary);
        stranger
            << R"({"schema_version": 11, "timeline_markers": [], "in_frame": null, "out_frame": null, "timeline_fps_num": 60, "timeline_fps_den": 1,)"
            << R"("video_tracks": [{"name": "V1", "muted": false}], "audio_tracks": [],)"
            << R"("manim_assets": [], "timeline_clips": [],)"
            << R"("media_folders": [], "media_items": []})";
    }
    check(!mvm::project::loadProjectJson(strangerPath).success,
          "format markerが無いファイルを .mvm として受理しました");

    const auto beforeFailure = live.timelineClips;
    auto failedCandidate = live;
    check(mvm::project::deleteTimelineClip(failedCandidate, 0).success,
          "失敗保存用 candidate を編集できません");
    std::ofstream blocker(root / "blocked", std::ios::binary);
    blocker << "file";
    blocker.close();
    const auto failed = mvm::project::saveProjectJsonTransaction(live, std::move(failedCandidate),
                                                                 root / "blocked" / "project.mvm");
    check(!failed.success, "保存不能 path への transaction が成功しました");
    check(live.timelineClips == beforeFailure, "保存失敗時に live Project が変化しました");
}

void testClipKeyEditing() {
    using namespace mvm::project;
    Project project = createDefaultProject();
    auto video = clip("key-video");
    video.sourceFrameCount = 120;
    video.sourceOutFrame = 100;
    project.timelineClips.push_back(video);
    const auto before = project;
    const auto invalid = previewClipKeyEdit(project, video.id, ClipKeyKind::Opacity, 22, 40, 50);
    check(!invalid.success && project == before, "存在しないキーの移動を拒否しProjectを保つ");
    const auto preview = previewClipKeyEdit(project, video.id, ClipKeyKind::Opacity, -1, 20, 80);
    check(preview.success && preview.frame == 20 && preview.effects.opacityKeys.size() == 1 &&
              project == before,
          "キードラッグの候補計算がProjectを変更しない");
    check(editClipKey(project, video.id, ClipKeyKind::Opacity, -1, 20, 80).success &&
              project.timelineClips[0].effects == preview.effects,
          "候補と確定が同じキーフレームになる");
    check(editClipKey(project, video.id, ClipKeyKind::Opacity, -1, 80, 20).success,
          "2個目のキーを追加する");
    const auto& keys = project.timelineClips[0].effects.opacityKeys;
    check(std::abs(evaluateClipKeys(keys, 100, 50) - 50.0) < 1e-9 &&
              evaluateClipKeys(keys, 100, 0) == 80 && evaluateClipKeys(keys, 100, 99) == 20,
          "線形補間と端の保持を評価する");
    const auto clamped = previewClipKeyEdit(project, video.id, ClipKeyKind::Opacity, 20, 99, 150);
    check(clamped.success && clamped.frame == 79 &&
              clamped.effects.opacityKeys[0].valuePercent == 100,
          "キーの位置と値を隣接キー・値域で止める");
    {
        auto removed = project;
        const auto unchanged = removed;
        check(!deleteClipKey(removed, video.id, ClipKeyKind::Opacity, 21).success &&
                  removed == unchanged,
              "存在しないキーの削除を拒否しProjectを保つ");
        check(!deleteClipKey(removed, video.id, ClipKeyKind::Volume, 20).success &&
                  removed == unchanged,
              "種別違いのキー削除を拒否する");
        check(deleteClipKey(removed, video.id, ClipKeyKind::Opacity, 20).success &&
                  removed.timelineClips[0].effects.opacityKeys.size() == 1 &&
                  removed.timelineClips[0].effects.opacityKeys[0].frame == 80,
              "指定したキーだけを削除する");
        check(deleteClipKey(removed, video.id, ClipKeyKind::Opacity, 80).success &&
                  removed.timelineClips[0].effects.opacityKeys.empty(),
              "最後のキーも削除できる");
    }
    const auto original = project;
    check(trimTimelineClip(project, video.id, TrimEdge::Left, 10, LinkMode::Single).success,
          "キー付きclipをtrimする");
    check(project.timelineClips[0].effects.opacityKeys.front().frame == 0 &&
              std::abs(project.timelineClips[0].effects.opacityKeys.front().valuePercent - 80) <
                  1e-9 &&
              project.timelineClips[0].effects.opacityKeys[2].frame == 70 &&
              project.timelineClips[0].effects.opacityKeys.back().frame == 89,
          "trim後の可視カーブと端を保持する");
    auto divided = original;
    int nextId = 0;
    check(splitTimelineClips(
              divided, {video.id}, 50, [&] { return "key-new-" + std::to_string(++nextId); },
              LinkMode::Single)
              .success,
          "キー付きclipを分割する");
    check(divided.timelineClips.size() == 2 &&
              std::abs(evaluateClipKeys(divided.timelineClips[0].effects.opacityKeys, 100, 49) -
                       51.0) < 1e-9 &&
              std::abs(evaluateClipKeys(divided.timelineClips[1].effects.opacityKeys, 100, 0) -
                       50.0) < 1e-9,
          "分割の左右端で元のカーブを保持する");
    const auto rightKeys = divided.timelineClips[1].effects.opacityKeys;
    check(slipTimelineClip(divided, divided.timelineClips[1].id, 10, LinkMode::Single).success &&
              divided.timelineClips[1].effects.opacityKeys == rightKeys,
          "slipはキーのtimeline位置を動かさない");
    auto linked = original;
    auto audio = clip("key-audio", TimelineClipKind::Audio, kA1);
    audio.sourceFrameCount = 120;
    audio.sourceOutFrame = 100;
    audio.linkGroupId = "key-pair";
    linked.timelineClips[0].linkGroupId = "key-pair";
    linked.timelineClips.push_back(audio);
    check(editClipKey(linked, video.id, ClipKeyKind::Opacity, -1, 50, 40).success &&
              linked.timelineClips[1].effects.volumeKeys.empty(),
          "リンク相手にペン操作を複製しない");
    check(trimTimelineClip(linked, video.id, TrimEdge::Left, 10, LinkMode::Linked).success &&
              linked.timelineClips[1].sourceInFrame == 10,
          "リンク相手は自身のtrim規則で編集する");
    auto malformed = original;
    malformed.timelineClips[0].effects.opacityKeys.push_back({20, 30});
    check(!validateTimeline(malformed).success, "重複キーを拒否する");
    malformed = original;
    malformed.timelineClips[0].effects.opacityKeys[0].valuePercent =
        std::numeric_limits<double>::quiet_NaN();
    check(!validateTimeline(malformed).success, "非有限キーを拒否する");
    malformed = original;
    malformed.timelineClips[0].effects.opacityKeys[0].frame = 100;
    check(!validateTimeline(malformed).success, "clip外のキーを拒否する");
    malformed = original;
    malformed.timelineClips[0].effects.volumeKeys = {{5, 50}};
    check(!validateTimeline(malformed).success, "映像clipの音量キーを拒否する");
}

void testTimelineMarks(const std::filesystem::path& root) {
    using namespace mvm::project;
    auto project = createDefaultProject();
    project.timelineMarkers = {5, 15, 40};
    project.inFrame = 5;
    project.outFrame = 40;
    check(validateTimeline(project).success, "正しいマーカーとイン・アウトを受理する");
    const auto projectPath = root / "marks.mvm";
    const auto serialized = serializeProjectJson(project, projectPath);
    check(serialized.success, "マーカー付きProjectを保存形式へ変換する");
    if (serialized.success) {
        const auto loaded = parseProjectJsonText(serialized.json, projectPath);
        check(loaded.success && loaded.project.timelineMarkers == project.timelineMarkers &&
                  loaded.project.inFrame == project.inFrame &&
                  loaded.project.outFrame == project.outFrame,
              "マーカーとイン・アウトを往復する");
        auto missing = serialized.json;
        const auto markerKey = missing.find("\"timeline_markers\":");
        if (markerKey != std::string::npos) {
            const auto end = missing.find("],", markerKey);
            missing.erase(markerKey, end + 2 - markerKey);
            check(!parseProjectJsonText(missing, projectPath).success,
                  "マーカーfield欠損を拒否する");
        }
    }
    auto invalid = project;
    invalid.timelineMarkers = {5, 5};
    check(!validateTimeline(invalid).success, "重複マーカーを拒否する");
    invalid = project;
    invalid.timelineMarkers = {15, 5};
    check(!validateTimeline(invalid).success, "昇順でないマーカーを拒否する");
    invalid = project;
    invalid.inFrame = 40;
    check(!validateTimeline(invalid).success, "空のイン・アウト範囲を拒否する");
    check(setProjectVideoSettings(project, 1920, 1080, 30, 1).success &&
              project.timelineMarkers == std::vector<std::int64_t>({3, 8, 20}) &&
              project.inFrame == 3 && project.outFrame == 20,
          "フレームレート変更でマーカーとイン・アウトを変換する");
    // clip が無くなっても保存位置は保持し、再生尺には加えない。
    const auto timeline = validateTimeline(project);
    check(timeline.success && timeline.totalFrames == 0 && project.timelineMarkers.back() == 20,
          "clip末尾より後のマーカーを保持し、再生尺を延ばさない");
}

} // namespace

// ドロップ位置への配置。行の種別に合う側をその track へ置き、重なりは上書きせずに失敗する。
void testPlaceMediaAtDrop() {
    using mvm::project::TimelineClipKind;
    using mvm::project::TrackKind;
    const auto find = [](const mvm::project::Project& project, const std::string& id) {
        for (const auto& value : project.timelineClips)
            if (value.id == id)
                return value;
        return mvm::project::TimelineClip{};
    };
    const auto pair = [] {
        auto video = clip("drop", TimelineClipKind::Video);
        auto audio = clip("drop-audio", TimelineClipKind::Audio);
        video.linkGroupId = audio.linkGroupId = "drop-link";
        return std::make_pair(video, audio);
    };

    // 映像行 (V2) へのリンク対。音声は空いている A1 へ入る。
    {
        auto project = mvm::project::createDefaultProject();
        auto [video, audio] = pair();
        const auto placed = mvm::project::placeMediaAtDrop(project, video, audio, kV2, 120);
        check(placed.success && find(project, "id-drop").track == kV2 &&
                  find(project, "id-drop").timelineStartFrame == 120 &&
                  find(project, "id-drop-audio").track == kA1 &&
                  find(project, "id-drop-audio").timelineStartFrame == 120,
              "映像行へのドロップでリンク対を置けません");
    }
    // A1 が使用中なら、リンク音声は新しい A2 へ入る。
    {
        auto project = mvm::project::createDefaultProject();
        project.timelineClips = {clip("busy", TimelineClipKind::Audio, kA1)};
        auto [video, audio] = pair();
        const auto placed = mvm::project::placeMediaAtDrop(project, video, audio, kV1, 100);
        check(placed.success && project.audioTracks.size() == 2 &&
                  find(project, "id-drop-audio").track ==
                      mvm::project::TrackRef{TrackKind::Audio, 1},
              "A1使用中のリンク音声を空いたtrackへ置けません");
    }
    // 音声行へのリンク対。音声がその行へ、映像は空いている V1 へ入る。
    {
        auto project = mvm::project::createDefaultProject();
        auto [video, audio] = pair();
        const auto placed = mvm::project::placeMediaAtDrop(project, video, audio, kA1, 30);
        check(placed.success && find(project, "id-drop-audio").track == kA1 &&
                  find(project, "id-drop").track == kV1,
              "音声行へのドロップでリンク対を置けません");
    }
    // 最後の行より外 (index == track 数) は、その種別の track を足して置く。負の位置は 0 へ寄せる。
    {
        auto project = mvm::project::createDefaultProject();
        const auto placed = mvm::project::placeMediaAtDrop(
            project, clip("image", TimelineClipKind::Image), std::nullopt, kV3, -5);
        check(placed.success && project.videoTracks.size() == 3 &&
                  find(project, "id-image").track == kV3 &&
                  find(project, "id-image").timelineStartFrame == 0,
              "新しいtrackへのドロップ、または負の位置の補正ができません");
    }
    {
        auto project = mvm::project::createDefaultProject();
        const auto placed = mvm::project::placeMediaAtDrop(
            project, clip("voice", TimelineClipKind::Audio, kA1), std::nullopt, kA1, 10);
        check(placed.success && find(project, "id-voice").track == kA1, "音声行へ音声を置けません");
    }

    // ドロップした行に置けないときは、上書きせずに置ける track へ回す (時刻はそのまま)。
    auto base = mvm::project::createDefaultProject();
    base.timelineClips = {clip("existing", TimelineClipKind::Video, kV1)};
    const auto before = base;
    {
        auto project = base;
        const auto placed = mvm::project::placeMediaAtDrop(
            project, clip("voice", TimelineClipKind::Audio), std::nullopt, kV2, 40);
        check(placed.success && find(project, "id-voice").track == kA1 &&
                  find(project, "id-voice").timelineStartFrame == 40,
              "映像行へ落とした音声を音声trackへ回せません");
    }
    {
        auto project = base;
        const auto placed = mvm::project::placeMediaAtDrop(
            project, clip("image", TimelineClipKind::Image), std::nullopt, kA1, 40);
        check(placed.success && find(project, "id-image").track == kV2 &&
                  find(project, "id-image").timelineStartFrame == 40,
              "音声行へ落とした画像を空いている映像trackへ回せません");
    }
    {
        // 最下段より下 (新しい audio track の位置) へ画像を落としても、audio track は足さない。
        auto project = base;
        const auto placed =
            mvm::project::placeMediaAtDrop(project, clip("image", TimelineClipKind::Image),
                                           std::nullopt, {TrackKind::Audio, 1}, 40);
        check(placed.success && project.audioTracks.size() == 1 &&
                  find(project, "id-image").track == kV2,
              "音声の新しい行へ落とした画像で余計なaudio trackを足しました");
    }
    {
        // V1 は使用中。V2 が空いていればそこへ、同じ時刻で置く。
        auto project = base;
        const auto placed = mvm::project::placeMediaAtDrop(
            project, clip("over", TimelineClipKind::Video), std::nullopt, kV1, 100);
        check(placed.success && find(project, "id-over").track == kV2 &&
                  find(project, "id-over").timelineStartFrame == 100 &&
                  find(project, "id-existing") == base.timelineClips.front(),
              "使用中の行へ落とした素材を空いているtrackへ回せない、または既存clipを変えました");
    }
    {
        // 空いている映像 track が無ければ新しい track を足す。リンク音声は A1 へ。
        auto project = base;
        project.timelineClips.push_back(clip("upper", TimelineClipKind::Video, kV2));
        auto [video, audio] = pair();
        const auto placed = mvm::project::placeMediaAtDrop(project, video, audio, kV1, 100);
        check(placed.success && project.videoTracks.size() == 3 &&
                  find(project, "id-drop").track == kV3 &&
                  find(project, "id-drop").timelineStartFrame == 100 &&
                  find(project, "id-drop-audio").track == kA1,
              "空きの無い映像trackへ落としたリンク対を新しいtrackへ置けません");
    }

    // 失敗は Project を変えない。
    check(!mvm::project::placeMediaAtDrop(base, clip("far", TimelineClipKind::Video), std::nullopt,
                                          {TrackKind::Video, 3}, 0)
                  .success &&
              !mvm::project::placeMediaAtDrop(base, clip("neg", TimelineClipKind::Video),
                                              std::nullopt, {TrackKind::Video, -1}, 0)
                   .success &&
              base == before,
          "存在しないtrackへ置けてしまいます");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_timeline_edit <work-directory>\n");
        return 2;
    }
    testFrameConversions();
    testSpeedDurationAndFrameHold();
    testTimelineMarks(fromUtf8(argv[1]));
    testClipKeyEditing();
    testAudioSourceSetCompensation();
    testTimelineFrameRates();
    testTimelineFrameRateChange();
    testTrimAndLookup();
    testPlacementHelpers();
    testMultipleClipMove();
    testTrackEditing();
    testAudioClipPlacement();
    testPlaceMediaAtDrop();
    testRippleDelete();
    testRippleDeleteWithLinkedClips();
    testSplitClips();
    testRateStretch();
    testRateStretchLinkedDifferentDurations();
    testStepClipVolume();
    testToggleClipsEnabled();
    testApplyDefaultClipFades();
    testClipWithEdgeAt();
    testApplyDefaultEditTransition();
    testMoveOverwrite();
    testTimelineTransitions(std::filesystem::path(argv[1]));
    testRippleTrim();
    testRollEdit();
    testLinkedToolEditing();
    testTrimRestoresSplitClip();
    testSlipClip();
    testSlideClip();
    testTrackSelectFromFrame();
    testValidationFailures();
    testDeleteSelection();
    testLinkedClipEditing();
    testManimPlacement();
    testPersistenceTransaction(fromUtf8(argv[1]));
    if (failures != 0) {
        std::fprintf(stderr, "timeline edit: %d 件失敗\n", failures);
        return 1;
    }
    std::puts("timeline edit: PASS");
    return 0;
}
