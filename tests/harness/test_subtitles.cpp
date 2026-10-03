#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "project/project_json.h"
#include "project/subtitles.h"
#include "project/timeline_edit.h"
#include "test_media_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <QGuiApplication>
using namespace mvm::project;

void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    auto p = createDefaultProject();
    std::string error;
    std::vector<SubtitleCue> cues;
    int serial = 0;
    auto id = [&] { return "s" + std::to_string(++serial); };
    require(
        parseSrt("\xef\xbb\xbf"
                 "7\r\n00:00:00,000 --> 00:00:01,000\r\n日本語\r\n次の行\r\n\r\n9\r\n00:00:01,000 "
                 "--> 00:00:02,000\r\n終わり\r\n",
                 60, 1, id, cues, error),
        "日本語SRTの読込");
    require(cues.size() == 2 && cues[0].endFrame == 60 && cues[0].content == "日本語\n次の行",
            "SRTの本文・時刻を比較");
    require(applySubtitleCues(p, cues, true, error), "字幕の適用");
    require(!activeSubtitleAt(p, -1) && activeSubtitleAt(p, 59)->id == cues[0].id &&
                activeSubtitleAt(p, 60)->id == cues[1].id && !activeSubtitleAt(p, 120),
            "字幕の半開区間");
    require(timelineEndFrame(p).frame == 120, "字幕だけのタイムライン尺");
    auto json = serializeProjectJson(p, "subtitles.mvm");
    require(json.success, "字幕JSON保存");
    auto loaded = parseProjectJsonText(json.json, "subtitles.mvm");
    require(loaded.success && loaded.project == p, "字幕JSONの往復比較");
    auto old = json.json;
    old.replace(old.find("schema_version\": 16"), 19, "schema_version\": 15");
    require(!parseProjectJsonText(old, "subtitles.mvm").success, "旧schemaを拒否");
    std::string srt;
    require(writeSrt(p, srt, error), "SRT書出し");
    std::vector<SubtitleCue> again;
    require(parseSrt(srt, 60, 1, id, again, error) && again.size() == 2 && again[1].endFrame == 120,
            "SRT往復");
    for (const auto& text :
         {"1\n00:00:01,000 --> 00:00:00,000\n逆\n", "1\n00:00:00,000 --> 00:00:00,001\n短い\n",
          "1\n00:00:00,000 --> 00:00:01,000\n\n", "1\n00:00:00,000 --> 00:00:01,000\n<b>装飾</b>\n",
          "1\n00:00:00,000 --> 00:00:01,000\n前\n\n2\n00:00:00,500 --> 00:00:02,000\n重複\n"}) {
        auto before = cues;
        require(!parseSrt(text, 60, 1, id, cues, error) && cues == before &&
                    error.find("行目") != std::string::npos,
                "不正SRTは部分適用しない");
    }
    // 本文中の < > { } は装飾ではない。実際の装飾記法だけを拒否する。
    for (const char* plain : {"a < b", "std::vector<int>", "f(x) = { x > 0 のとき }", "<>"}) {
        std::vector<SubtitleCue> read;
        const std::string srtText =
            std::string("1\n00:00:00,000 --> 00:00:01,000\n") + plain + "\n";
        require(parseSrt(srtText, 60, 1, id, read, error) && read.size() == 1 &&
                    read[0].content == plain,
                "記号を含む普通の本文は読み込む");
    }
    for (const char* markup :
         {"<i>斜体</i>", "<B>太字</B>", "<font color=\"red\">赤</font>", "{\\an8}上に表示"}) {
        std::vector<SubtitleCue> read;
        const std::string srtText =
            std::string("1\n00:00:00,000 --> 00:00:01,000\n") + markup + "\n";
        require(!parseSrt(srtText, 60, 1, id, read, error) &&
                    error.find("装飾記法") != std::string::npos,
                "実際の装飾記法は拒否する");
    }
    {
        // 文字起こしで作った (clip にリンクした) 字幕も SRT へ出せる。リンクは SRT に含めない。
        auto linkedSrt = createDefaultProject();
        TimelineClip speech;
        speech.kind = TimelineClipKind::Text;
        speech.id = "speech";
        speech.name = "発話";
        speech.text.content = "発話";
        speech.sourceFpsNum = 60;
        speech.sourceFrameCount = speech.sourceOutFrame = 120;
        linkedSrt.timelineClips = {speech};
        linkedSrt.subtitles.emplace();
        linkedSrt.subtitles->cues = {{"a", 0, 60, "std::vector<int> の説明", "speech"},
                                     {"b", 60, 120, "後半", "speech"}};
        std::string linkedText;
        std::vector<SubtitleCue> reread;
        require(writeSrt(linkedSrt, linkedText, error) &&
                    parseSrt(linkedText, 60, 1, id, reread, error) && reread.size() == 2 &&
                    reread[0].content == "std::vector<int> の説明" && reread[0].endFrame == 60 &&
                    reread[1].startFrame == 60 && reread[1].linkClipId.empty(),
                "clip にリンクした字幕を SRT へ書き出し、区間と本文を保つ");
    }
    std::int64_t frame = -1;
    require(subtitleTimeToFrame(25, 60, 1, frame, error) && frame == 2, "同率は後方へ丸める");
    require(subtitleTimeToFrame(1001, 30000, 1001, frame, error) && frame == 30, "非整数fps換算");
    std::vector<SubtitleCue> timed;
    require(
        subtitleCuesFromTimedText({{1000, 2000, "速い字幕"}}, 60, 1, 2, 1, 120, id, timed, error) &&
            timed[0].startFrame == 150 && timed[0].endFrame == 180,
        "認識結果は速度の逆数と配置を反映");
    require(!subtitleCuesFromTimedText({}, 60, 1, 1, 1, 0, id, timed, error), "認識結果0件を拒否");
    auto unsupported = p;
    unsupported.subtitles->cues[0].content = "<b>本文</b>";
    const auto previousSrt = srt;
    require(!writeSrt(unsupported, srt, error) && srt == previousSrt,
            "未対応記法のSRT出力は既存内容を保持");
    auto before = p;
    auto blank = p.subtitles->cues[0];
    blank.content = "　\n\t";
    require(!editSubtitle(p, blank, error) && p == before, "全角空白だけの字幕本文を拒否");
    auto bad = p.subtitles->cues[0];
    bad.endFrame = 100;
    require(!editSubtitle(p, bad, error) && p == before, "重複編集は変更しない");
    require(splitSubtitle(p, cues[0].id, 30, id(), error) && p.subtitles->cues.size() == 3,
            "字幕分割");
    require(mergeSubtitle(p, cues[0].id, error) && p.subtitles->cues.size() == 2, "字幕結合");
    // 分割は本文も分ける。区間の中の比率に最も近い句読点・改行の直後で分ける。
    require(splitSubtitle(p, cues[0].id, 30, id(), error) &&
                p.subtitles->cues[0].content == "日本語" &&
                p.subtitles->cues[1].content == "次の行",
            "分割で改行の位置から本文を前後に分ける");
    require(mergeSubtitle(p, cues[0].id, error) && p.subtitles->cues[0].content == "日本語\n次の行",
            "分割した本文の結合で元へ戻る");
    const auto parts = [](const char* text, double ratio, std::optional<std::size_t> at) {
        return splitSubtitleText(text, ratio, at);
    };
    require(parts("はい、さっさと人間関係の今日ですからね", 0.3, std::nullopt) ==
                std::pair<std::string, std::string>{"はい、", "さっさと人間関係の今日ですからね"},
            "比率に最も近い読点の直後で分ける");
    require(parts("はい、さっさと人間関係の今日ですからね", 0.3, 7) ==
                std::pair<std::string, std::string>{"はい、さっさと", "人間関係の今日ですからね"},
            "カーソルの位置 (文字数) を指定したらそこで分ける");
    require(parts("あいうえおかきくけこ", 0.5, std::nullopt) ==
                std::pair<std::string, std::string>{"あいうえお", "かきくけこ"},
            "句読点が無ければ比率の位置で分ける");
    require(parts("あいうえおかきくけこ", 0.5, 0) ==
                    std::pair<std::string, std::string>{"あいうえお", "かきくけこ"} &&
                parts("あいうえおかきくけこ", 0.5, 10) ==
                    std::pair<std::string, std::string>{"あいうえお", "かきくけこ"},
            "端のカーソル位置は無視して比率で分ける");
    require(parts("あ", 0.5, std::nullopt) == std::pair<std::string, std::string>{"あ", "あ"},
            "1 文字の本文は両方に残す");
    TimelineClip clip;
    clip.kind = TimelineClipKind::Text;
    clip.id = "text";
    clip.name = "文字";
    clip.text.content = "別の文字";
    clip.sourceFpsNum = 60;
    clip.sourceFrameCount = clip.sourceOutFrame = 180;
    p.timelineClips.push_back(clip);
    clip.id = "text2";
    clip.track.index = 1;
    clip.timelineStartFrame = 120;
    p.timelineClips.push_back(clip);
    require(validateTimeline(p).success, "リップル対照群");
    require(editTimelineTime(p, 30, 30, 0).success, "全トラック時間削除");
    require(p.timelineClips.size() == 3 && p.timelineClips[1].sourceOutFrame == 120 &&
                p.timelineClips[1].timelineStartFrame == 30 &&
                p.timelineClips[2].timelineStartFrame == 90 &&
                p.subtitles->cues.back().startFrame == 30,
            "全トラックと字幕を実際に比較");
    before = p;
    require(!editTimelineTime(p, -1, 0, 20).success && p == before, "不正時間編集は不変");
    require(editTimelineTime(p, 45, 0, 15).success && p.timelineClips.size() == 4,
            "挿入境界で分割");
    before = p;
    require(editTimelineTime(p, 50, 0, 0).success && p == before,
            "0フレームの時間編集は分割しない");
    auto linked = createDefaultProject();
    TimelineClip video;
    video.id = "video";
    video.name = "映像";
    video.mediaPath = "素材.mp4";
    video.sourceFpsNum = 60;
    video.sourceFrameCount = video.sourceOutFrame = 180;
    video.linkGroupId = "pair";
    video.effects.opacityKeys = {{0, 0}, {179, 100}};
    auto audio = video;
    audio.id = "audio";
    audio.kind = TimelineClipKind::Audio;
    audio.track.kind = TrackKind::Audio;
    audio.effects.opacityKeys.clear();
    linked.timelineClips = {video, audio};
    linked.videoTracks[0].muted = true;
    linked.audioTracks[0].muted = true;
    mvm::test::attachFixtureMedia(linked);
    require(validateTimeline(linked).success, "非表示・ミュートのリンク対照群");
    require(editTimelineTime(linked, 60, 30, 0).success && linked.timelineClips.size() == 4,
            "リンク素材の全トラック削除");
    require(linked.timelineClips[0].linkGroupId == linked.timelineClips[2].linkGroupId &&
                linked.timelineClips[1].linkGroupId == linked.timelineClips[3].linkGroupId &&
                linked.timelineClips[0].linkGroupId != linked.timelineClips[1].linkGroupId &&
                linked.timelineClips[1].sourceInFrame == 90 &&
                linked.timelineClips[3].timelineStartFrame == 60,
            "分割リンク・素材位置・時刻を保持");
    require(std::abs(evaluateClipEffects(linked.timelineClips[1].effects, 0).opacityPercent -
                     90.0 * 100 / 179) < 1e-8,
            "後半のキーフレーム曲線を保持");
    auto unrepresentable = createDefaultProject();
    video.linkGroupId.clear();
    video.effects.opacityKeys.clear();
    video.sourceFpsNum = 30;
    unrepresentable.timelineClips = {video};
    mvm::test::attachFixtureMedia(unrepresentable);
    require(validateTimeline(unrepresentable).success, "異なる素材fpsの対照群");
    before = unrepresentable;
    require(!editTimelineTime(unrepresentable, 1, 0, 1).success && unrepresentable == before,
            "素材フレームで表せない境界は操作全体を拒否");
    // 文字起こし元の clip への字幕リンク。
    auto source = createDefaultProject();
    TimelineClip speech;
    speech.kind = TimelineClipKind::Text;
    speech.id = "speech";
    speech.name = "発話";
    speech.text.content = "発話";
    speech.sourceFpsNum = 60;
    speech.sourceFrameCount = speech.sourceOutFrame = 120;
    speech.timelineStartFrame = 60;
    source.timelineClips = {speech};
    source.subtitles.emplace();
    source.subtitles->cues = {{"a", 50, 90, "前半", "speech"},
                              {"b", 100, 170, "後半", "speech"},
                              {"free", 300, 330, "手入力", {}}};
    require(validateTimeline(source).success, "リンク字幕の対照群");
    const auto cueStart = [](const Project& project, const char* cueId) {
        for (const auto& cue : project.subtitles->cues)
            if (cue.id == cueId)
                return cue.startFrame;
        return std::int64_t{-1};
    };
    auto moved = source;
    require(moveClip(moved, "speech", {TrackKind::Video, 0}, 90).success &&
                cueStart(moved, "a") == 80 && cueStart(moved, "b") == 130 &&
                cueStart(moved, "free") == 300,
            "clip の移動にリンク字幕だけが追従");
    moved = source;
    require(moveClips(moved, {"speech"}, "speech", {TrackKind::Video, 0}, 90, LinkMode::Single)
                    .success &&
                moved.timelineClips[0].timelineStartFrame == 90 && cueStart(moved, "a") == 50,
            "Alt の移動では字幕を追従させない");
    moved = source;
    require(moveClip(moved, "speech", {TrackKind::Video, 0}, 0).success &&
                moved.timelineClips[0].timelineStartFrame == 10 && cueStart(moved, "a") == 0,
            "リンク字幕が 0 より前へ出ない位置で止める");
    moved = source;
    require(!moveClip(moved, "speech", {TrackKind::Video, 0}, 210).success && moved == source,
            "他の字幕と重なる移動は全体を拒否");
    moved = source;
    require(deleteTimelineClip(moved, 0).success && moved.subtitles->cues.size() == 1 &&
                moved.subtitles->cues[0].id == "free",
            "clip の削除でリンク字幕も消す");
    moved = source;
    require(splitTimelineClips(
                moved, {"speech"}, 100, [] { return std::string("right"); }, LinkMode::Linked)
                    .success &&
                moved.subtitles->cues[0].linkClipId == "speech" &&
                moved.subtitles->cues[1].linkClipId == "right",
            "分割位置以降の字幕は右半分へリンクを付け替える");
    moved = source;
    int splitIds = 0;
    require(
        splitTimelineClips(
            moved, {"speech"}, 80, [&] { return "split-" + std::to_string(++splitIds); },
            LinkMode::Linked)
                .success &&
            moved.subtitles->cues.size() == 4 && moved.subtitles->cues[0].id == "a" &&
            moved.subtitles->cues[0].endFrame == 80 &&
            moved.subtitles->cues[0].linkClipId == "speech" &&
            moved.subtitles->cues[0].content == "前" && moved.subtitles->cues[1].startFrame == 80 &&
            moved.subtitles->cues[1].endFrame == 90 && moved.subtitles->cues[1].content == "半" &&
            moved.subtitles->cues[1].linkClipId != "speech" &&
            moved.subtitles->cues[1].linkClipId == moved.subtitles->cues[2].linkClipId,
        "分割位置を跨ぐ字幕は分割位置で 2 つに分け、後半を右半分へリンクする");
    {
        const auto rightClip = moved.subtitles->cues[1].linkClipId;
        const auto rightIndex =
            static_cast<int>(std::find_if(moved.timelineClips.begin(), moved.timelineClips.end(),
                                          [&](const auto& clip) { return clip.id == rightClip; }) -
                             moved.timelineClips.begin());
        require(deleteTimelineClip(moved, rightIndex).success &&
                    moved.subtitles->cues.size() == 2 && moved.subtitles->cues[0].id == "a" &&
                    moved.subtitles->cues[0].endFrame == 80,
                "右半分の clip を消すと、その上にあった字幕の部分だけが消える");
    }
    moved = source;
    require(unlinkTimelineClip(moved, "speech").success &&
                std::all_of(moved.subtitles->cues.begin(), moved.subtitles->cues.end(),
                            [](const auto& cue) { return cue.linkClipId.empty(); }),
            "リンク解除で字幕のリンクも外す");
    require(!unlinkTimelineClip(moved, "speech").success, "リンクの無い clip の解除は拒否");
    auto linkedJson = serializeProjectJson(source, "linked.mvm");
    require(linkedJson.success &&
                linkedJson.json.find("\"link_clip_id\": \"speech\"") != std::string::npos,
            "字幕リンクを保存");
    auto linkedLoaded = parseProjectJsonText(linkedJson.json, "linked.mvm");
    require(linkedLoaded.success && linkedLoaded.project == source, "字幕リンクのJSON往復");
    auto dangling = source;
    dangling.subtitles->cues[0].linkClipId = "missing";
    require(!validateTimeline(dangling).success, "存在しない clip へのリンクを拒否");
    auto danglingJson = linkedJson.json;
    danglingJson.replace(danglingJson.find("\"link_clip_id\": \"speech\""), 24,
                         "\"link_clip_id\": \"missing\"");
    require(!parseProjectJsonText(danglingJson, "linked.mvm").success,
            "存在しないリンク先のJSONを拒否");
    auto orphaned = source;
    orphaned.timelineClips.clear();
    require(finalizeTimelineCandidate(orphaned).success && orphaned.subtitles->cues.size() == 3 &&
                orphaned.subtitles->cues[0].linkClipId.empty(),
            "素材の削除などで消えた clip へのリンクだけを外す");
    QString rasterError;
    auto raster = mvm::app::renderSubtitleRaster(p.subtitles->cues[0], p.subtitles->style, 1920,
                                                 1080, rasterError);
    require(!raster.isNull(), "字幕ラスタ描画");
    bool opaque = false;
    for (int y = 0; y < raster.height(); ++y)
        for (int x = 0; x < raster.width(); ++x)
            opaque = opaque || qAlpha(raster.pixel(x, y)) != 0;
    require(opaque, "空画像で通らない");
    // 描かずに収まるかだけを見る検査は、描画と同じ結果になる (書式変更の検査に使う)。
    {
        const auto& cue = p.subtitles->cues[0];
        auto tall = p.subtitles->style;
        tall.fontSize = 1000;
        auto missing = p.subtitles->style;
        missing.fontFamily = "mvm-存在しない書体";
        auto narrow = p.subtitles->style;
        narrow.sideMargin = 0.4999;
        narrow.outlineWidth = 32;
        QString checkError, renderError;
        require(mvm::app::checkSubtitleLayout(cue, p.subtitles->style, 1920, 1080, checkError),
                "収まる字幕の検査");
        for (const auto& style : {tall, missing, narrow}) {
            const bool fits = mvm::app::checkSubtitleLayout(cue, style, 1920, 1080, checkError);
            const bool rendered =
                !mvm::app::renderSubtitleRaster(cue, style, 1920, 1080, renderError).isNull();
            require(!fits && !rendered && checkError == renderError,
                    "画面を超える・フォントが無い・余白が広すぎる字幕は検査と描画の両方で拒否する");
        }
    }
    mvm::app::TimelineExportRequest request;
    auto plan = mvm::app::mapTimelineExportPlan(p, request);
    require(plan.success && plan.clips.back().subtitle.has_value(), "焼き込み計画に字幕を含める");
    request.burnSubtitles = false;
    auto noSub = mvm::app::mapTimelineExportPlan(p, request);
    require(noSub.success && noSub.clips.size() + p.subtitles->cues.size() == plan.clips.size(),
            "焼き込みオフ");
    std::puts("字幕・SRT・全トラック時間編集の検査に合格しました");
}
