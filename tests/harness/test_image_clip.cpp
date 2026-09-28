// 画像 clip (TimelineClipKind::Image) の契約。
//   model: schema 8 の往復、検証の負例 (それぞれ正しい版を対照にする)、分割・trim・既定の尺
//   preview: decode source ではなく静止画 layer に入ること
//   書き出し: 静止画 decoder と同じ画素 (縦横比を保って中央に置き、EXIF の向きを反映) が出ること。
//             静止画でなくなった素材 (差し替え) は取り込みと同じ判定で拒否すること
// 素材は scripts/make-testmedia.ps1 -Mode Smoke が _import/ へ作る。期待値は直書きする。
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project_json.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

namespace {

namespace fs = std::filesystem;
using mvm::project::LinkMode;
using mvm::project::TimelineClipKind;
using mvm::project::TrackKind;
using mvm::project::TrimEdge;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
}

fs::path importDir;

mvm::project::TimelineClip imageClip(const std::string& id, const char* file, int track,
                                     std::int64_t start, std::int64_t frames) {
    mvm::project::TimelineClip clip;
    clip.kind = TimelineClipKind::Image;
    clip.id = id;
    clip.name = id;
    clip.mediaPath = importDir / file;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = frames;
    clip.sourceOutFrame = frames;
    clip.timelineStartFrame = start;
    clip.track = {TrackKind::Video, track};
    return clip;
}

mvm::project::Project projectWith(mvm::project::TimelineClip clip) {
    auto project = mvm::project::createDefaultProject();
    project.outputWidth = 320;
    project.outputHeight = 240;
    project.timelineClips.push_back(std::move(clip));
    return project;
}

bool valid(const mvm::project::Project& project) {
    return mvm::project::validateTimeline(project).success;
}

void testModel() {
    const auto base = projectWith(imageClip("img", "jpg_quadrant.jpg", 0, 0, 300));
    check(valid(base), "画像 clip の Project を検証できません");

    // schema 8 の往復。kind は "image" で保存される。
    const auto serialized = mvm::project::serializeProjectJson(base, importDir / "image.mvm");
    check(serialized.success && serialized.json.find("\"kind\": \"image\"") != std::string::npos,
          "画像 clip を kind image として保存できません");
    const auto parsed =
        mvm::project::parseProjectJsonText(serialized.json, importDir / "image.mvm");
    // 素材 path は project directory からの相対で保存され、読み込みで解決されるので表記が変わる。
    // 同じファイルを指すことだけを見て、他の field は完全一致を要求する。
    check(parsed.success && parsed.project.timelineClips.size() == 1,
          "画像 clip を読み戻せません: " + parsed.error);
    if (parsed.success && parsed.project.timelineClips.size() == 1) {
        auto roundTripped = parsed.project.timelineClips[0];
        check(fs::equivalent(roundTripped.mediaPath, base.timelineClips[0].mediaPath),
              "画像 clip の素材 path が round-trip しません: " + roundTripped.mediaPath.string());
        roundTripped.mediaPath = base.timelineClips[0].mediaPath;
        check(roundTripped == base.timelineClips[0] && roundTripped.kind == TimelineClipKind::Image,
              "画像 clip の field が round-trip しません");
    }

    // 検証の負例。どれも base から 1 か所だけ変える (base 自体は上で受理している)。
    const auto rejects = [&](const char* what, const auto& mutate) {
        auto broken = base;
        mutate(broken.timelineClips[0]);
        check(!valid(broken), std::string("画像 clip の不正を受理しました: ") + what);
    };
    rejects("media path が空", [](auto& clip) { clip.mediaPath.clear(); });
    rejects("速度 50%", [](auto& clip) { clip.speedDen = 2; });
    rejects("素材の in が 0 でない", [](auto& clip) { clip.sourceInFrame = 1; });
    rejects("素材の out が尺と違う", [](auto& clip) { clip.sourceOutFrame = 200; });
    rejects("音量", [](auto& clip) { clip.effects.volumePercent = 50.0; });
    rejects("audio track", [](auto& clip) { clip.track = {TrackKind::Audio, 0}; });
    rejects("文字データ", [](auto& clip) { clip.text.content = "x"; });
    {
        // リンクは video / audio の 1 組だけ。画像は速度もリンクも持たない。
        auto linked = base;
        linked.timelineClips[0].linkGroupId = "g";
        mvm::project::TimelineClip audio;
        audio.kind = TimelineClipKind::Audio;
        audio.id = "a";
        audio.name = "a";
        audio.mediaPath = importDir / "opus_48k.opus";
        audio.sourceFpsNum = 60;
        audio.sourceFpsDen = 1;
        audio.sourceFrameCount = 60;
        audio.sourceOutFrame = 60;
        audio.track = {TrackKind::Audio, 0};
        audio.linkGroupId = "g";
        linked.timelineClips.push_back(audio);
        check(!valid(linked), "リンク付きの画像 clip を受理しました");
        // 対照: リンクを外せば受理する。
        linked.timelineClips[0].linkGroupId.clear();
        linked.timelineClips[1].linkGroupId.clear();
        check(valid(linked), "対照: リンクの無い画像 + 音声を受理しません");
    }
    // 位置・拡大・回転・crop・不透明度の key は video と同じく持てる。
    {
        auto effects = base;
        auto& clip = effects.timelineClips[0];
        clip.effects.positionXPercent = 10.0;
        clip.effects.scalePercent = 50.0;
        clip.effects.rotationDegrees = 15.0;
        clip.effects.cropLeftPercent = 10.0;
        clip.effects.opacityKeys = {{0, 0.0}, {60, 100.0}};
        check(valid(effects),
              "位置・拡大・回転・crop・不透明度 key 付きの画像 clip を受理しません");
    }

    // 既定の尺は 5 秒。
    check(mvm::project::defaultStillClipFrames(60, 1) == 300 &&
              mvm::project::defaultStillClipFrames(30000, 1001) == 150 &&
              mvm::project::defaultStillClipFrames(24, 1) == 120,
          "既定の尺が 5 秒ではありません");

    // trim で伸ばせる (素材の尺に縛られない)。スリップとレート調整は持たない。
    {
        auto edited = base;
        check(mvm::project::trimTimelineClip(edited, "img", TrimEdge::Right, 600, LinkMode::Single)
                      .success &&
                  mvm::project::timelineClipDuration(edited, edited.timelineClips[0]).frame == 900,
              "画像 clip を右へ伸ばせません");
        check(!mvm::project::slipTimelineClip(edited, "img", 1, LinkMode::Single).success,
              "画像 clip のスリップを受理しました");
        check(!mvm::project::rateStretchTimelineClip(edited, "img", TrimEdge::Right, 10,
                                                     LinkMode::Single)
                   .success,
              "画像 clip のレート調整を受理しました");
    }

    // 分割。文字 clip も同じ経路を通る (以前は常に「分割位置を素材 frame へ一意に換算できません」
    // で失敗していた)。
    for (const auto kind : {TimelineClipKind::Image, TimelineClipKind::Text}) {
        auto split = base;
        auto& clip = split.timelineClips[0];
        clip.kind = kind;
        if (kind == TimelineClipKind::Text) {
            clip.mediaPath.clear();
            clip.text.content = "abc";
        }
        int next = 0;
        const auto result = mvm::project::splitTimelineClips(
            split, {"img"}, 100, [&] { return "right-" + std::to_string(++next); },
            LinkMode::Linked);
        const char* name = kind == TimelineClipKind::Image ? "画像" : "文字";
        check(result.success && split.timelineClips.size() == 2,
              std::string(name) + " clip を分割できません: " + result.error);
        if (!result.success || split.timelineClips.size() != 2)
            continue;
        const auto& left = split.timelineClips[0];
        const auto& right = split.timelineClips[1];
        check(left.timelineStartFrame == 0 && left.sourceOutFrame == 100 &&
                  right.timelineStartFrame == 100 && right.sourceInFrame == 0 &&
                  right.sourceOutFrame == 200 && valid(split),
              std::string(name) + " clip の分割結果が 0-100 / 100-300 になりません");
    }

    // 置き場所: 再生ヘッドで最上位の clip より上の映像 track。
    {
        auto placement = base;
        const auto placed = mvm::project::placeStillClipAt(
            placement, imageClip("img2", "png_rgb24.png", 0, 0, 300), 10);
        check(placed.success &&
                  placement.timelineClips[static_cast<std::size_t>(placed.selectedIndex)].track ==
                      mvm::project::TrackRef{TrackKind::Video, 1},
              "画像 clip を V1 の画像の上 (V2) へ置けません");
    }

    // preview: decode source ではなく静止画 layer に入る。
    const auto preview = mvm::app::mapTimelinePreviewFrame(base, 10);
    check(preview.success && preview.layers.empty() && preview.stillLayers.size() == 1 &&
              preview.stillLayers[0].kind == TimelineClipKind::Image,
          "画像 clip が静止画 layer に入りません");
}

// 書き出した mp4 の先頭 frame を PNG へ復号する。
QImage decodeFirstFrame(const fs::path& video, const QString& png) {
    QProcess decoder;
    decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"),
                   QString::fromStdWString(video.wstring()), QStringLiteral("-frames:v"),
                   QStringLiteral("1"), QStringLiteral("-y"), png});
    if (!decoder.waitForFinished(30000) || decoder.exitCode() != 0)
        return {};
    return QImage(png);
}

bool isRed(const QColor& c) {
    return c.red() > 200 && c.green() < 70 && c.blue() < 70;
}

bool isBlue(const QColor& c) {
    return c.blue() > 200 && c.red() < 70 && c.green() < 70;
}

bool isBlack(const QColor& c) {
    return c.red() < 25 && c.green() < 25 && c.blue() < 25;
}

void testExport() {
    QTemporaryDir exported;
    check(exported.isValid(), "書き出しの一時 directory を作れません");
    mvm::app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;
    request.timeoutMs = 120000;

    // 64x32 (左上 32x16 が赤、他は青) を 320x240 へ: 320x160 で上下に 40 px の黒。
    {
        const auto project = projectWith(imageClip("img", "jpg_quadrant.jpg", 0, 0, 30));
        const auto plan = mvm::app::mapTimelineExportPlan(project, request);
        check(plan.success && plan.clips.size() == 1 && plan.clips[0].still &&
                  plan.backend == mvm::app::TimelineExportResult::Backend::Tractor,
              "画像 clip を静止画として tractor に写せません");
        request.outputPath =
            fs::path(exported.filePath(QStringLiteral("quadrant.mp4")).toStdWString());
        const auto rendered = mvm::app::exportTimeline(project, request);
        check(rendered.success && rendered.frameCount == 30,
              "画像 clip を書き出せません: " + rendered.error);
        const QImage frame =
            decodeFirstFrame(request.outputPath, exported.filePath(QStringLiteral("quadrant.png")));
        check(frame.width() == 320 && frame.height() == 240, "書き出し frame を復号できません");
        if (frame.width() == 320) {
            check(isRed(frame.pixelColor(80, 80)), "画像の左上 (赤) が書き出されていません");
            check(isBlue(frame.pixelColor(240, 160)), "画像の右下 (青) が書き出されていません");
            check(isBlack(frame.pixelColor(160, 15)) && isBlack(frame.pixelColor(160, 225)),
                  "縦横比を保った余白 (上下 40 px) が黒ではありません");
        }
    }

    // EXIF orientation 6: 32x64 になり、赤は右上。320x240 では 120x240 で x = 100..219。
    {
        const auto project = projectWith(imageClip("rot", "jpg_exif_orient6.jpg", 0, 0, 30));
        request.outputPath =
            fs::path(exported.filePath(QStringLiteral("orient6.mp4")).toStdWString());
        const auto rendered = mvm::app::exportTimeline(project, request);
        check(rendered.success, "向き付きの画像 clip を書き出せません: " + rendered.error);
        const QImage frame =
            decodeFirstFrame(request.outputPath, exported.filePath(QStringLiteral("orient6.png")));
        check(frame.width() == 320, "向き付きの書き出し frame を復号できません");
        if (frame.width() == 320) {
            check(isRed(frame.pixelColor(190, 60)), "向きを反映した画像の右上が赤ではありません");
            check(isBlue(frame.pixelColor(130, 60)), "向きを反映した画像の左上が青ではありません");
            check(isBlack(frame.pixelColor(40, 120)) && isBlack(frame.pixelColor(280, 120)),
                  "縦長の画像の左右の余白が黒ではありません");
        }
    }

    // 取り込んだ後に同じ path が動画・アニメーション画像・HDR 画像へ差し替えられた画像 clip。
    // 取り込みと同じ判定で拒否し、先頭 frame を静止画として書き出さない。
    // (正しい静止画を書き出せることは上の 2 件が対照になる)
    struct Swap {
        const char* file;
        const char* reason;
    };

    const Swap swaps[] = {
        {"gif_animated.gif", "アニメーション"},
        {"apng_animated.png", "アニメーション"},
        {"webp_animated.webp", "アニメーション"},
        {"mp4_h264_with_cover.mp4", "静止画ではありません"},
        {"exr_float.exr", "HDR"},
    };
    for (const auto& swap : swaps) {
        const auto project = projectWith(imageClip("swapped", swap.file, 0, 0, 30));
        check(valid(project), std::string("前提: 画像 clip の Project が不正です: ") + swap.file);
        request.outputPath =
            fs::path(exported.filePath(QStringLiteral("swapped.mp4")).toStdWString());
        const auto rendered = mvm::app::exportTimeline(project, request);
        check(!rendered.success && rendered.error.find(swap.reason) != std::string::npos,
              std::string("静止画ではなくなった素材を画像 clip として書き出しました: ") +
                  swap.file + " (" + rendered.error + ")");
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_image_clip <_import directory>\n");
        return 2;
    }
    // 素材 path は project directory からの相対で保存されるので、基準を絶対 path にしておく。
    importDir = fs::absolute(fs::path(reinterpret_cast<const char8_t*>(argv[1])));
    if (!fs::exists(importDir / "jpg_exif_orient6.jpg")) {
        std::fprintf(stderr, "素材がありません。先に pwsh scripts/make-testmedia.ps1 -Mode Smoke を"
                             "実行してください\n");
        return 3;
    }
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT runtime を初期化できません\n");
        return 4;
    }
    testModel();
    testExport();
    mvm_mlt_runtime_shutdown();
    if (failures == 0)
        std::puts("画像 clip の model・preview mapping・書き出しを確認しました");
    return failures == 0 ? 0 : 1;
}
