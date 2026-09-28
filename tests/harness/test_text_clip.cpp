#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "失敗: %s\n", message);
        std::exit(1);
    }
}

mvm::project::Project makeProject() {
    auto project = mvm::project::createDefaultProject();
    project.outputWidth = 320;
    project.outputHeight = 240;
    project.videoTracks.push_back({"V3", false});
    mvm::project::TimelineClip clip;
    clip.kind = mvm::project::TimelineClipKind::Text;
    clip.name = "文字";
    clip.id = "text-1";
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 30;
    clip.sourceOutFrame = 30;
    clip.track = {mvm::project::TrackKind::Video, 2};
    clip.text.content = "日本語 A±\n次の行";
    clip.text.fontSize = 32;
    clip.text.x = 12;
    clip.text.y = 18;
    clip.text.bold = true;
    clip.text.alignment = "center";
    clip.text.outlineWidth = 2;
    clip.text.backgroundColor = "#80000000";
    project.timelineClips.push_back(clip);
    return project;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    auto project = makeProject();
    require(mvm::project::validateTimeline(project).success, "文字 clip を検証できません");
    const auto serialized = mvm::project::serializeProjectJson(project, "text-test.mvm");
    require(serialized.success, "schema 7 を保存できません");
    const auto parsed = mvm::project::parseProjectJsonText(serialized.json, "text-test.mvm");
    require(parsed.success && parsed.project == project, "文字データが round-trip しません");

    auto placement = mvm::project::createDefaultProject();
    placement.outputWidth = 320;
    placement.outputHeight = 240;
    for (int track = 0; track < 2; ++track) {
        mvm::project::TimelineClip video;
        video.kind = mvm::project::TimelineClipKind::Video;
        video.mediaPath = "video.mp4";
        video.name = "映像";
        video.id = "underlay-" + std::to_string(track);
        video.sourceFpsNum = 60;
        video.sourceFrameCount = 30;
        video.sourceOutFrame = 30;
        video.track = {mvm::project::TrackKind::Video, track};
        placement.timelineClips.push_back(video);
    }
    auto textToPlace = project.timelineClips[0];
    const auto placed = mvm::project::placeTextClipAt(placement, textToPlace, 0);
    require(
        placed.success && placement.videoTracks.size() == 3 &&
            placement.timelineClips[static_cast<std::size_t>(placed.selectedIndex)].track.index ==
                2,
        "V1/V2 の上に V3 文字 clip を配置できません");
    const auto beforeFull = placement;
    textToPlace.id = "text-2";
    require(!mvm::project::placeTextClipAt(placement, textToPlace, 0).success &&
                placement == beforeFull,
            "V1～V3 が埋まったときに Project を変更しました");

    auto laterOverlap = mvm::project::createDefaultProject();
    laterOverlap.timelineClips.push_back(beforeFull.timelineClips[0]);
    auto laterVideo = beforeFull.timelineClips[1];
    laterVideo.timelineStartFrame = 10;
    laterOverlap.timelineClips.push_back(laterVideo);
    textToPlace.id = "text-3";
    const auto placedAboveOverlap = mvm::project::placeTextClipAt(laterOverlap, textToPlace, 0);
    require(
        placedAboveOverlap.success && laterOverlap.videoTracks.size() == 3 &&
            laterOverlap.timelineClips[static_cast<std::size_t>(placedAboveOverlap.selectedIndex)]
                    .track.index == 2,
        "文字 clip の途中で重なる V2 を避けて V3 に配置できません");

    std::string missing = serialized.json;
    const std::string needle = "\"font_family\": \"Meiryo\",\n";
    const auto at = missing.find(needle);
    require(at != std::string::npos, "必須 field の対照群がありません");
    missing.erase(at, needle.size());
    require(!mvm::project::parseProjectJsonText(missing, "text-test.mvm").success,
            "font_family 欠落を受理しました");
    auto invalid = project;
    invalid.timelineClips[0].text.color = "bad";
    require(!mvm::project::validateTimeline(invalid).success, "不正な色を受理しました");
    invalid = project;
    invalid.timelineClips[0].mediaPath = "unexpected.png";
    require(!mvm::project::validateTimeline(invalid).success,
            "文字 clip の素材 path を受理しました");

    QString error;
    const QImage image = mvm::app::renderTextRaster(project.timelineClips[0].text, 320, 240, error);
    if (image.isNull())
        std::fprintf(stderr, "文字ラスタ: %s\n", error.toUtf8().constData());
    require(!image.isNull(), "透過文字画像を作成できません");
    require(qAlpha(image.pixel(0, 0)) == 0, "画像外側が透過ではありません");
    bool drawn = false;
    for (int y = 18; y < 120; ++y)
        for (int x = 12; x < 300; ++x)
            drawn = drawn || qAlpha(image.pixel(x, y)) > 0;
    require(drawn, "文字が空振りで描画されていません");
    auto badFont = project.timelineClips[0].text;
    badFont.fontFamily = "mvm-missing-font-xyz";
    require(mvm::app::renderTextRaster(badFont, 320, 240, error).isNull(),
            "不在フォントを代替表示しました");

    mvm::app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;
    const auto plan = mvm::app::mapTimelineExportPlan(project, request);
    require(plan.success && plan.backend == mvm::app::TimelineExportResult::Backend::Tractor &&
                plan.clips.size() == 1 && plan.clips[0].text && plan.clips[0].videoTrackIndex == 2,
            "V3 文字 clip を tractor に写せません");
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
            "MLT runtime を初期化できません");
    QTemporaryDir exported;
    require(exported.isValid(), "書き出しテストの一時 directory を作れません");
    request.outputPath =
        std::filesystem::path(exported.filePath(QStringLiteral("text.mp4")).toStdWString());
    request.timeoutMs = 120000;
    const auto rendered = mvm::app::exportTimeline(project, request);
    if (!rendered.success)
        std::fprintf(stderr, "書き出し: %s\n", rendered.error.c_str());
    require(rendered.success && rendered.frameCount == 30,
            "文字だけの V3 書き出しを検証できません");
    const QString exportedFrame = exported.filePath(QStringLiteral("frame.png"));
    QProcess decoder;
    decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"),
                   QString::fromStdWString(request.outputPath.wstring()),
                   QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-y"),
                   exportedFrame});
    require(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
            "書き出し frame を復号できません");
    const QImage decoded(exportedFrame);
    require(!decoded.isNull() && decoded.size() == image.size(), "書き出し frame の寸法が違います");
    require(decoded.pixelColor(0, 0).red() < 20 && decoded.pixelColor(0, 0).green() < 20 &&
                decoded.pixelColor(0, 0).blue() < 20,
            "文字外側が黒ではありません");
    int compared = 0;
    double difference = 0.0;
    for (int y = 18; y < 120; ++y) {
        for (int x = 12; x < 300; ++x) {
            const QColor expected = image.pixelColor(x, y);
            if (expected.alpha() < 128)
                continue;
            const QColor actual = decoded.pixelColor(x, y);
            difference += std::abs(expected.red() - actual.red()) +
                          std::abs(expected.green() - actual.green()) +
                          std::abs(expected.blue() - actual.blue());
            ++compared;
        }
    }
    require(compared > 100 && difference / (compared * 3) < 25.0,
            "文字ラスタと書き出しの画素が一致しません");

    auto stacked = project;
    for (int track = 0; track < 2; ++track) {
        mvm::project::TimelineClip video;
        video.kind = mvm::project::TimelineClipKind::Video;
        video.mediaPath = MVM_TEXT_TEST_VIDEO;
        video.name = "背景映像";
        video.id = "video-" + std::to_string(track);
        video.sourceFpsNum = 60;
        video.sourceFpsDen = 1;
        video.sourceFrameCount = 300;
        video.sourceOutFrame = 30;
        video.track = {mvm::project::TrackKind::Video, track};
        stacked.timelineClips.push_back(video);
    }
    require(mvm::project::validateTimeline(stacked).success,
            "V1/V2 映像と V3 文字の構成を検証できません");
    const auto preview = mvm::app::mapTimelinePreviewFrame(stacked, 0);
    require(preview.success && preview.layers.size() == 2,
            "preview の動画 source に文字 clip が混入しました");
    request.outputPath =
        std::filesystem::path(exported.filePath(QStringLiteral("stacked.mp4")).toStdWString());
    const auto stackedResult = mvm::app::exportTimeline(stacked, request);
    if (!stackedResult.success)
        std::fprintf(stderr, "3トラック書き出し: %s\n", stackedResult.error.c_str());
    require(stackedResult.success && stackedResult.frameCount == 30,
            "V1/V2 映像と V3 文字を書き出せません");
    const QString stackedFrame = exported.filePath(QStringLiteral("stacked-frame.png"));
    decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"),
                   QString::fromStdWString(request.outputPath.wstring()),
                   QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-y"),
                   stackedFrame});
    require(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
            "3トラックの書き出し frame を復号できません");
    const QImage stackedDecoded(stackedFrame);
    int whiteTextPixels = 0;
    for (int y = 18; y < 120; ++y)
        for (int x = 12; x < 300; ++x) {
            const QColor expected = image.pixelColor(x, y);
            const QColor actual = stackedDecoded.pixelColor(x, y);
            if (expected.alpha() == 255 && expected.red() > 245 && actual.red() > 215 &&
                actual.green() > 215 && actual.blue() > 215)
                ++whiteTextPixels;
        }
    require(whiteTextPixels > 100, "V3 の文字が映像の上に描画されていません");
    mvm_mlt_runtime_shutdown();

    auto edited = project;
    const auto trim = mvm::project::trimTimelineClip(
        edited, "text-1", mvm::project::TrimEdge::Right, 10, mvm::project::LinkMode::Single);
    require(trim.success &&
                mvm::project::timelineClipDuration(edited, edited.timelineClips[0]).frame == 40,
            "文字 clip を右へ延長できません");
    require(!mvm::project::slipTimelineClip(edited, "text-1", 1, mvm::project::LinkMode::Single)
                 .success,
            "文字 clip の素材位置変更を受理しました");
    // テロップの定位置。期待値は文字の矩形の大きさから独立に計算する。
    {
        auto telop = project.timelineClips[0].text;
        const QSizeF block = mvm::app::textBlockSize(telop, error);
        require(block.width() > 0 && block.height() > 0, "文字の矩形の大きさを求められません");
        const int expectedY = static_cast<int>(std::lround(240 * 0.85 - block.height()));
        const auto left = mvm::app::textPresetPlacement(telop, 320, 240, "left");
        const auto center = mvm::app::textPresetPlacement(telop, 320, 240, "center");
        const auto right = mvm::app::textPresetPlacement(telop, 320, 240, "right");
        require(left.success && center.success && right.success, "定位置を求められません");
        require(left.x == 16 && left.y == expectedY, "左の定位置が下 15% / 左 5% ではありません");
        require(center.x == static_cast<int>(std::lround((320 - block.width()) / 2)) &&
                    center.y == expectedY,
                "中央の定位置が下 15% の中央ではありません");
        require(right.x == static_cast<int>(std::lround(320 * 0.95 - block.width())) &&
                    right.y == expectedY,
                "右の定位置が下 15% / 右 5% ではありません");
        // 描いた画像の矩形も、求めた位置に来ること (描画と配置が同じ寸法を使っている)。
        telop.x = center.x;
        telop.y = center.y;
        telop.backgroundColor = "#FF000000";
        const QImage placedImage = mvm::app::renderTextRaster(telop, 320, 240, error);
        int bottom = -1;
        for (int y = 0; y < placedImage.height(); ++y)
            for (int x = 0; x < placedImage.width(); ++x)
                if (qAlpha(placedImage.pixel(x, y)) > 0)
                    bottom = y;
        require(std::abs((bottom + 1) - static_cast<int>(std::lround(240 * 0.85))) <= 1,
                "定位置に置いた文字の下端が画面の下から 15% にありません");
        require(!mvm::app::textPresetPlacement(telop, 320, 240, "top").success,
                "未知の揃えを受理しました");
    }
    std::puts("文字 clip の保存・描画・編集・定位置を確認しました");
    return 0;
}
