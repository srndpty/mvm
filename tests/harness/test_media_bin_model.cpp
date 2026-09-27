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

void testClassify() {
    auto video = okProbe();
    video.has_video = 1;
    video.has_audio = 1;
    video.width = 1920;
    video.height = 1080;
    video.fps_num = 48000;
    video.fps_den = 2002;
    video.frame_count = 598;
    const auto videoResult = mvm::app::classifyMediaProbe(video, "v.mp4");
    check(videoResult.success && videoResult.item.kind == MediaKind::Video &&
              videoResult.item.fpsNum == 24000 && videoResult.item.fpsDen == 1001 &&
              videoResult.item.frameCount == 598 && videoResult.item.sampleRate == 0,
          "動画のprobeを約分済みfps付きのVideoへ分類できません");

    // MLT は静止画の length を INT_MAX で返す。尺として扱わない。
    auto still = okProbe();
    still.has_video = 1;
    still.width = 800;
    still.height = 600;
    still.fps_num = 25;
    still.fps_den = 1;
    still.frame_count = 0x7FFFFFFF;
    still.is_unbounded_length = 1;
    const auto stillResult = mvm::app::classifyMediaProbe(still, "still.png");
    check(stillResult.success && stillResult.item.kind == MediaKind::Image &&
              stillResult.item.width == 800 && stillResult.item.frameCount == 0 &&
              stillResult.item.fpsNum == 0,
          "無限尺の映像をImageへ分類できません");

    // JPEG は image2 demuxer 経由で 1 frame・有限尺として返る。
    auto jpeg = okProbe();
    jpeg.has_video = 1;
    jpeg.width = 640;
    jpeg.height = 360;
    jpeg.fps_num = 25;
    jpeg.fps_den = 1;
    jpeg.frame_count = 1;
    const auto jpegResult = mvm::app::classifyMediaProbe(jpeg, "still.jpg");
    check(jpegResult.success && jpegResult.item.kind == MediaKind::Image &&
              jpegResult.item.frameCount == 0,
          "音声なし1 frameの映像をImageへ分類できません");
    // 対照: 音声付きなら 1 frame でも静止画扱いしない。
    auto oneFrameWithAudio = jpeg;
    oneFrameWithAudio.has_audio = 1;
    check(mvm::app::classifyMediaProbe(oneFrameWithAudio, "a.mp4").item.kind == MediaKind::Video,
          "音声付き1 frameの素材をImageへ分類しました");

    auto audio = okProbe();
    audio.has_audio = 1;
    audio.sample_rate = 44100;
    audio.duration_sec = 2.5;
    const auto audioResult = mvm::app::classifyMediaProbe(audio, "a.m4a");
    check(audioResult.success && audioResult.item.kind == MediaKind::Audio &&
              audioResult.item.sampleRate == 44100 && audioResult.item.durationSamples == 110250,
          "音声のprobeをsample数付きのAudioへ分類できません");

    // 値が欠けた素材を推測で埋めない。
    // sample 数が int64 に収まらない尺は、llround へ渡す前に拒否する。
    auto huge = audio;
    huge.sample_rate = 48000;
    huge.duration_sec = 1.0e15;
    check(!mvm::app::classifyMediaProbe(huge, "a.m4a").success,
          "int64に収まらない音声尺を受理しました");

    auto noRate = audio;
    noRate.sample_rate = 0;
    check(!mvm::app::classifyMediaProbe(noRate, "a.m4a").success,
          "sample rateの無い音声を受理しました");
    auto unboundedAudio = audio;
    unboundedAudio.is_unbounded_length = 1;
    check(!mvm::app::classifyMediaProbe(unboundedAudio, "a.m4a").success,
          "無限尺の音声を受理しました");
    auto noFps = video;
    noFps.fps_num = 0;
    check(!mvm::app::classifyMediaProbe(noFps, "v.mp4").success, "fpsの無い動画を受理しました");
    auto noSize = still;
    noSize.width = 0;
    check(!mvm::app::classifyMediaProbe(noSize, "still.png").success,
          "解像度の無い静止画を受理しました");
    auto failed = video;
    failed.ok = 0;
    std::strcpy(failed.error, "broken");
    check(!mvm::app::classifyMediaProbe(failed, "v.mp4").success, "probe失敗の結果を受理しました");
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
    testClassify();
    testFormat();
    testModel();
    if (failures == 0)
        std::printf("media bin model: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
