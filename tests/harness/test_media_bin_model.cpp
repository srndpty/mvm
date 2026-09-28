// プロジェクトパネルの app 層: probe 結果の分類、表示用の整形、平坦化した行 model。
// 期待値は実装の式を呼ばずに手で書く。

#include "media_bin_model.h"
#include "media_import.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <QCoreApplication>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

using mvm::app::MediaBinModel;
using mvm::project::MediaKind;

MvmMltProbeResult okProbe() {
    MvmMltProbeResult probe{};
    probe.ok = 1;
    return probe;
}

// FFmpeg で調べた stream の事実。classify の前提として手で組む。
mvm::media::MediaStreamFacts factsWith(int video, int attached, int audio) {
    mvm::media::MediaStreamFacts facts;
    facts.ok = true;
    facts.formatName = "mov,mp4,m4a,3gp,3g2,mj2";
    facts.videoStreamCount = video;
    facts.attachedPictureCount = attached;
    facts.audioStreamCount = audio;
    facts.videoCodecName = video > 0 ? "h264" : "";
    return facts;
}

mvm::media::MediaStreamFacts stillFacts(int packets) {
    mvm::media::MediaStreamFacts facts;
    facts.ok = true;
    facts.formatName = "png_pipe";
    facts.videoStreamCount = 1;
    facts.videoCodecName = "png";
    facts.stillImageCodec = true;
    facts.imagePacketCountUpTo2 = packets;
    return facts;
}

void testRoute() {
    using mvm::app::MediaRoute;
    using mvm::app::routeMedia;

    check(routeMedia(stillFacts(1)).route == MediaRoute::StillImage,
          "1 packet の静止画 codec をStillImageへ振り分けられません");
    const auto animated = routeMedia(stillFacts(2));
    check(animated.route == MediaRoute::Rejected &&
              animated.error.find("アニメーション") != std::string::npos,
          "2 packet の画像をアニメーションとして拒否しません");
    check(routeMedia(stillFacts(0)).route == MediaRoute::Rejected,
          "packet の無い画像を受理しました");

    // FFmpeg が開けないアニメーション WebP でも、header の flag で理由を添えて拒否する。
    mvm::media::MediaStreamFacts webp;
    webp.ok = false;
    webp.error = "stream 情報を取得できません";
    webp.webpAnimationFlag = true;
    const auto webpDecision = routeMedia(webp);
    check(webpDecision.route == MediaRoute::Rejected &&
              webpDecision.error.find("アニメーション") != std::string::npos,
          "開けないアニメーション WebP をアニメーションとして拒否しません");
    // 対照: flag が無ければ「解析できません」で拒否する。
    webp.webpAnimationFlag = false;
    check(routeMedia(webp).error.find("解析できません") != std::string::npos,
          "開けない素材の拒否理由が解析失敗になっていません");

    auto exr = stillFacts(1);
    exr.stillImageCodec = false;
    exr.hdrImageCodec = true;
    exr.videoCodecName = "exr";
    check(routeMedia(exr).route == MediaRoute::Rejected, "HDR 画像を受理しました");

    // 対照: Motion JPEG の動画は静止画 codec 扱いにならず、時間を持つ素材へ回る。
    auto motionJpeg = factsWith(1, 0, 1);
    motionJpeg.videoCodecName = "mjpeg";
    check(routeMedia(motionJpeg).route == MediaRoute::TimeBased,
          "Motion JPEG の動画を時間を持つ素材へ振り分けられません");
    check(routeMedia(factsWith(0, 1, 1)).route == MediaRoute::TimeBased,
          "カバーアート付きの音声を時間を持つ素材へ振り分けられません");
    check(routeMedia(factsWith(0, 0, 0)).route == MediaRoute::Rejected,
          "映像も音声も無い素材を受理しました");
}

void testClassify() {
    const auto avFacts = factsWith(1, 0, 1);
    const auto audioFacts = factsWith(0, 0, 1);

    auto video = okProbe();
    video.has_video = 1;
    video.has_audio = 1;
    video.width = 1920;
    video.height = 1080;
    video.fps_num = 48000;
    video.fps_den = 2002;
    video.frame_count = 598;
    video.sar_num = 64;
    video.sar_den = 48;
    const auto videoResult = mvm::app::classifyMediaProbe(video, avFacts, "v.mp4");
    check(videoResult.success && videoResult.item.kind == MediaKind::Video &&
              videoResult.item.fpsNum == 24000 && videoResult.item.fpsDen == 1001 &&
              videoResult.item.frameCount == 598 && videoResult.item.sampleRate == 0 &&
              videoResult.hasAudio && videoResult.sarNum == 4 && videoResult.sarDen == 3,
          "動画のprobeを約分済みfps付きのVideoへ分類できません");

    // 静止画は MLT を通さない。映像 stream があるのに無限尺・1 frame なら動画として扱えない。
    auto unbounded = video;
    unbounded.is_unbounded_length = 1;
    unbounded.frame_count = 0x7FFFFFFF;
    check(!mvm::app::classifyMediaProbe(unbounded, avFacts, "v.mp4").success,
          "無限尺の映像を受理しました");
    auto oneFrame = video;
    oneFrame.frame_count = 1;
    const auto oneFrameResult = mvm::app::classifyMediaProbe(oneFrame, avFacts, "v.mp4");
    check(!oneFrameResult.success && oneFrameResult.error.find("1 frame") != std::string::npos,
          "1 frame だけの映像を拒否しません");

    // カバーアート付きの音声: MLT は映像ありと返すが (実測)、本物の映像が無いので Audio。
    auto audio = okProbe();
    audio.has_audio = 1;
    audio.sample_rate = 44100;
    audio.duration_sec = 2.5;
    auto cover = audio;
    cover.has_video = 1;
    cover.width = 600;
    cover.height = 600;
    cover.fps_num = 60;
    cover.fps_den = 1;
    cover.frame_count = 150;
    const auto coverResult = mvm::app::classifyMediaProbe(cover, factsWith(0, 1, 1), "a.mp3");
    check(coverResult.success && coverResult.item.kind == MediaKind::Audio &&
              coverResult.item.width == 0 && coverResult.item.durationSamples == 110250,
          "カバーアート付きの音声をAudioへ分類できません");
    // 対照: 本物の映像 stream があれば、同じ probe でも Video。
    check(mvm::app::classifyMediaProbe(cover, factsWith(1, 1, 1), "v.mp4").item.kind ==
              MediaKind::Video,
          "映像 stream のある素材をVideoへ分類できません");

    const auto audioResult = mvm::app::classifyMediaProbe(audio, audioFacts, "a.m4a");
    check(audioResult.success && audioResult.item.kind == MediaKind::Audio &&
              audioResult.item.sampleRate == 44100 && audioResult.item.durationSamples == 110250 &&
              audioResult.durationSec == 2.5,
          "音声のprobeをsample数付きのAudioへ分類できません");

    // 値が欠けた素材を推測で埋めない。
    // sample 数が int64 に収まらない尺は、llround へ渡す前に拒否する。
    auto huge = audio;
    huge.sample_rate = 48000;
    huge.duration_sec = 1.0e15;
    check(!mvm::app::classifyMediaProbe(huge, audioFacts, "a.m4a").success,
          "int64に収まらない音声尺を受理しました");

    auto noRate = audio;
    noRate.sample_rate = 0;
    check(!mvm::app::classifyMediaProbe(noRate, audioFacts, "a.m4a").success,
          "sample rateの無い音声を受理しました");
    auto unboundedAudio = audio;
    unboundedAudio.is_unbounded_length = 1;
    check(!mvm::app::classifyMediaProbe(unboundedAudio, audioFacts, "a.m4a").success,
          "無限尺の音声を受理しました");
    // facts は映像ありなのに MLT が映像を開けていない。どちらかを選ばずに失敗させる。
    check(!mvm::app::classifyMediaProbe(audio, avFacts, "v.mp4").success,
          "MLTが映像を開けていない動画を受理しました");
    auto noFps = video;
    noFps.fps_num = 0;
    check(!mvm::app::classifyMediaProbe(noFps, avFacts, "v.mp4").success,
          "fpsの無い動画を受理しました");
    auto noSize = video;
    noSize.width = 0;
    check(!mvm::app::classifyMediaProbe(noSize, avFacts, "v.mp4").success,
          "解像度の無い動画を受理しました");
    auto failed = video;
    failed.ok = 0;
    std::strcpy(failed.error, "broken");
    check(!mvm::app::classifyMediaProbe(failed, avFacts, "v.mp4").success,
          "probe失敗の結果を受理しました");
}

void testClassifyStillImage() {
    mvm::media::StillImageDecodeResult decoded;
    decoded.success = true;
    decoded.image.width = 600;
    decoded.image.height = 800;
    decoded.image.rgba.assign(600U * 800U * 4U, 255);
    const auto still = mvm::app::classifyStillImage(decoded, "p.jpg");
    check(still.success && still.item.kind == MediaKind::Image && still.item.width == 600 &&
              still.item.height == 800 && still.item.fpsNum == 0 && still.item.frameCount == 0,
          "decode した静止画をImageへ分類できません");
    auto failed = decoded;
    failed.success = false;
    failed.error = "壊れています";
    const auto failedResult = mvm::app::classifyStillImage(failed, "p.jpg");
    check(!failedResult.success && failedResult.error == "壊れています",
          "decode に失敗した静止画を受理しました");
}

mvm::project::MediaItem video(const char* id, std::int64_t fpsNum, std::int64_t fpsDen,
                              std::int64_t frames, const char* folderId = "") {
    mvm::project::MediaItem item;
    item.id = id;
    item.name = id;
    item.kind = MediaKind::Video;
    item.mediaPath = std::string("C:/media/") + id + ".mp4";
    item.folderId = folderId;
    item.fpsNum = fpsNum;
    item.fpsDen = fpsDen;
    item.frameCount = frames;
    item.width = 1920;
    item.height = 1080;
    return item;
}

mvm::project::MediaItem audio(const char* id, int rate, std::int64_t samples,
                              const char* folderId = "") {
    mvm::project::MediaItem item;
    item.id = id;
    item.name = id;
    item.kind = MediaKind::Audio;
    item.mediaPath = std::string("C:/media/") + id + ".wav";
    item.folderId = folderId;
    item.sampleRate = rate;
    item.durationSamples = samples;
    return item;
}

void testFormat() {
    check(mvm::app::formatMediaRate(video("a", 24000, 1001, 1)) == QStringLiteral("23.976 fps"),
          "23.976fpsの表示が違います");
    check(mvm::app::formatMediaRate(video("a", 30000, 1001, 1)) == QStringLiteral("29.97 fps"),
          "29.97fpsの表示が違います");
    check(mvm::app::formatMediaRate(video("a", 60, 1, 1)) == QStringLiteral("60.00 fps"),
          "60fpsの表示が違います");
    check(mvm::app::formatMediaRate(audio("a", 48000, 1)) == QStringLiteral("48000 Hz"),
          "音声のsample rate表示が違います");

    // 23.976 の frame 桁は公称 24 で数える。598 frame = 24 秒 22 frame。
    check(mvm::app::formatMediaDuration(video("a", 24000, 1001, 598)) ==
              QStringLiteral("00:00:24:22"),
          "23.976fps素材の尺表示が違います");
    check(mvm::app::formatMediaDuration(video("a", 60, 1, 60 * 3661 + 5)) ==
              QStringLiteral("01:01:01:05"),
          "時・分を持つ尺の表示が違います");
    check(mvm::app::formatMediaDuration(audio("a", 48000, 48000 * 269 + 24000)) ==
              QStringLiteral("00:04:29.500"),
          "音声の尺表示が違います");

    // 手編集された巨大な値でも overflow しない。期待値は Python で別に計算した。
    //   INT64_MAX / 48000 = 192153584100724 秒、端数 → 162 ms
    check(mvm::app::formatMediaDuration(audio("a", 48000, INT64_MAX)) ==
              QStringLiteral("53375995583:39:01.162"),
          "巨大なsample数の尺表示がoverflowしました");
    //   公称 fps = ceil(INT64_MAX / 2) = 2^62。num + den - 1 で切り上げると overflow する。
    check(mvm::app::formatMediaDuration(video("a", INT64_MAX, 2, 10)) ==
              QStringLiteral("00:00:00:10"),
          "巨大なfpsの尺表示がoverflowしました");

    check(mvm::app::formatMediaSize(video("a", 60, 1, 1)) == QStringLiteral("1920 × 1080"),
          "解像度の表示が違います");
    check(mvm::app::formatMediaSize(audio("a", 48000, 1)).isEmpty(),
          "音声素材に解像度を表示しました");
}

QStringList visibleIds(const MediaBinModel& model) {
    QStringList ids;
    for (int row = 0; row < model.rowCount(); ++row)
        ids << model.data(model.index(row, 0), MediaBinModel::EntryIdRole).toString();
    return ids;
}

void testModel() {
    mvm::project::Project project = mvm::project::createDefaultProject();
    // vector 順は表示順にしない。folder が先、同種内は名前順。
    project.mediaFolders = {{"f-z", "Zoo", ""}, {"f-a", "Assets", ""}, {"f-sub", "Sub", "f-a"}};
    project.mediaItems = {video("b-clip", 60, 1, 10), audio("a-voice", 48000, 10),
                          video("inner", 60, 1, 10, "f-a"), audio("deep", 48000, 10, "f-sub")};
    mvm::project::TimelineClip clip;
    clip.mediaPath = "C:/media/b-clip.mp4";
    project.timelineClips.push_back(clip);

    MediaBinModel model;
    int countChanges = 0;
    int inserts = 0;
    int resets = 0;
    QObject::connect(&model, &MediaBinModel::entryCountChanged, [&] { ++countChanges; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&] { ++inserts; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    model.setProject(project);
    check(model.entryCount() == 7 && countChanges == 1, "entry総数が違います");
    check(visibleIds(model) == QStringList({"f-a", "f-z", "a-voice", "b-clip"}),
          "折りたたみ状態のroot行の並びが違います");

    const int clipRow = model.rowOfEntry(QStringLiteral("b-clip"));
    check(model.data(model.index(clipRow, 0), MediaBinModel::InUseRole).toBool() &&
              !model.data(model.index(model.rowOfEntry("a-voice"), 0), MediaBinModel::InUseRole)
                   .toBool(),
          "timeline使用中の印が違います");
    check(model.data(model.index(0, 0), MediaBinModel::HasChildrenRole).toBool() &&
              !model.data(model.index(1, 0), MediaBinModel::HasChildrenRole).toBool(),
          "folderの子の有無が違います");

    inserts = 0;
    resets = 0;
    model.setExpanded(QStringLiteral("f-a"), true);
    check(visibleIds(model) == QStringList({"f-a", "f-sub", "inner", "f-z", "a-voice", "b-clip"}),
          "展開したfolderの子が直後に並びません");
    // reset するとスクロール位置が失われるため、行の挿入で通知する。
    check(inserts == 1 && resets == 0, "展開をresetで通知しました");
    check(model.data(model.index(model.rowOfEntry("f-sub"), 0), MediaBinModel::DepthRole).toInt() ==
              1,
          "子の深さが違います");

    model.setExpanded(QStringLiteral("f-sub"), true);
    check(model.rowOfEntry("deep") == 2, "孫の素材が展開されません");

    // 親を閉じると孫ごと隠れる。再度開くと孫の展開状態は保たれる。
    model.toggleExpanded(QStringLiteral("f-a"));
    check(visibleIds(model) == QStringList({"f-a", "f-z", "a-voice", "b-clip"}),
          "folderを閉じても子孫が残ります");
    model.toggleExpanded(QStringLiteral("f-a"));
    check(model.rowOfEntry("deep") == 2, "孫folderの展開状態が失われました");

    check(model.containingFolderOf("f-sub") == "f-sub" &&
              model.containingFolderOf("inner") == "f-a" &&
              model.containingFolderOf("a-voice").isEmpty(),
          "読み込み先folderの解決が違います");
    check(model.parentFolderOf("f-sub") == "f-a" && model.parentFolderOf("f-a").isEmpty(),
          "親folderの解決が違います");

    // 編集で消えた folder の展開状態は持ち越さない。
    project.mediaFolders.erase(project.mediaFolders.begin() + 2);
    project.mediaItems.pop_back();
    model.setProject(project);
    check(model.entryCount() == 5 && countChanges == 2, "削除後のentry総数が違います");
    check(visibleIds(model) == QStringList({"f-a", "inner", "f-z", "a-voice", "b-clip"}),
          "再設定後に展開状態が保たれません");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testRoute();
    testClassify();
    testClassifyStillImage();
    testFormat();
    testModel();
    if (failures == 0)
        std::printf("media bin model: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
