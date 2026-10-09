// 実 Manim + LaTeX で数式 backend "manim-mathtex" を確かめる手動の smoke。
//
// 通常の CTest には入れない (Manim と MiKTeX の導入を build・試験の必須条件にしない。
// mvm_manim_smoke と同じ扱い)。latex と dvisvgm は、この process の PATH から探す。
//
//   mvm_math_manim_smoke <manim.exe> <作業 directory>
//
// 二次方程式の解の公式の導出 (docs/math-clips.md の受け入れ scenario) の式をすべて描き、
// mvm の静止画 decoder で読み直して、大きさ・透過の余白・白い glyph を確かめる。
// 不正な式が InvalidSource になり、TeX の error 行が message になることも確かめる。
// P1: 解の公式を Write で書く連番を描き、Project の保存・再読込と映像だけの書き出しへ通す。

#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/manim/manim_math_tex.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media/still_image/static_image.h"
#include "project/project_json.h"
#include "util/mvm_win_utf8.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QImage>
#include <QProcess>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::filesystem::path fromUtf8(const char* text) {
    wchar_t* wide = mvm_utf8_to_wide(text ? text : "");
    const std::filesystem::path result = wide ? wide : L"";
    mvm_str_free(wide);
    return result;
}

void inspectPng(const mvm::math::MathStaticRenderResult& rendered, const std::string& label) {
    const auto decoded = mvm::media::loadStaticImage(rendered.png);
    check(decoded.success, label + ": PNG を mvm の静止画 decoder で読める: " + decoded.error);
    if (!decoded.success)
        return;
    const auto& image = decoded.image;
    check(image.width == rendered.width && image.height == rendered.height,
          label + ": PNG の大きさが backend の報告と一致する");
    bool cornersClear = true;
    for (const auto [x, y] :
         {std::pair{0, 0}, std::pair{image.width - 1, 0}, std::pair{0, image.height - 1},
          std::pair{image.width - 1, image.height - 1}}) {
        const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x)) *
                        4U;
        cornersClear = cornersClear && image.rgba[at + 3] == 0;
    }
    check(cornersClear, label + ": 四隅 (padding) は透明");
    std::size_t opaque = 0;
    bool opaqueWhite = true;
    for (std::size_t at = 0; at + 3 < image.rgba.size(); at += 4) {
        if (image.rgba[at + 3] != 255)
            continue;
        ++opaque;
        opaqueWhite = opaqueWhite && image.rgba[at] == 255 && image.rgba[at + 1] == 255 &&
                      image.rgba[at + 2] == 255;
    }
    check(opaque > 0, label + ": 不透明な glyph の画素がある");
    check(opaqueWhite, label + ": 不透明な画素は白 (色は mvm 側で付ける)");
}

// backend の単体検査だけで終えず、実際の式を既存の Project と書き出しへ通す。
// 音声はナレーションの代わりの試験音。人が制作する手順の検証とは区別する。
bool independentAudioFailure(const mvm::app::TimelineExportResult& math,
                             const mvm::app::TimelineExportResult& text,
                             const mvm::app::TimelineExportResult& image) {
    return !math.success && !text.success && !image.success && !math.error.empty() &&
           text.error == math.error && image.error == math.error;
}

void acceptScenario(const std::filesystem::path& root, const std::vector<std::string>& sources,
                    const std::vector<std::filesystem::path>& masks) {
    namespace project = mvm::project;
    check(masks.size() == 7, "受け入れに使う実数式の描画が揃う");
    if (masks.size() != 7)
        return;
    const QString ffmpeg = QStringLiteral(MVM_MATH_FFMPEG);
    const auto audioPath = root / "acceptance.wav";
    QProcess audio;
    audio.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
                         "sine=frequency=440:sample_rate=48000:duration=3", "-ac", "2", "-c:a",
                         "pcm_s16le", QString::fromStdWString(audioPath.wstring())});
    const bool audioGenerated = audio.waitForFinished(30000) &&
                                audio.exitStatus() == QProcess::NormalExit && audio.exitCode() == 0;
    check(audioGenerated, "A1 の試験音を UCRT64 FFmpeg で生成する");
    if (!audioGenerated)
        return;
    auto value = project::createDefaultProject();
    value.videoTracks.resize(2);
    value.videoTracks[0].name = "V1";
    value.videoTracks[1].name = "V2";
    project::TimelineClip title;
    title.kind = project::TimelineClipKind::Text;
    title.id = "title";
    title.name = title.text.content = "二次方程式の解の公式";
    title.sourceFpsNum = 60;
    title.sourceFrameCount = title.sourceOutFrame = 180;
    title.track = {project::TrackKind::Video, 0};
    title.effects.positionYPercent = -35;
    value.timelineClips.push_back(title);
    mvm::app::TimelineExportRequest request;
    request.outputPath = root / "acceptance.mp4";
    for (int i = 0; i < 6; ++i) {
        project::TimelineClip clip;
        clip.kind = project::TimelineClipKind::Math;
        clip.id = "math-" + std::to_string(i);
        clip.name = "式 " + std::to_string(i + 1);
        clip.math.source = sources[static_cast<std::size_t>(i)];
        clip.sourceFpsNum = 60;
        clip.sourceFrameCount = clip.sourceOutFrame = 30;
        clip.timelineStartFrame = i * 30;
        clip.track = {project::TrackKind::Video, 1};
        clip.effects.fadeInFrames = clip.effects.fadeOutFrames = 6;
        if (i == 5) {
            clip.effects.fadeOutFrames = 6;
            clip.effects.scaleXKeys = clip.effects.scaleYKeys = {{0, 100}, {29, 120}};
            clip.effects.positionYKeys = {{0, -10}, {29, 0}};
            clip.effects.opacityKeys = {{0, 80}, {29, 100}};
        }
        request.mathArtifacts.emplace(clip.id, masks[static_cast<std::size_t>(i)]);
        value.timelineClips.push_back(clip);
        if (i > 0)
            value.timelineTransitions.push_back(
                {"dissolve-" + std::to_string(i), "math-" + std::to_string(i - 1), clip.id, 3, 3});
    }
    project::MediaItem item;
    item.id = "audio-source";
    item.name = "ナレーション経路の試験音";
    item.kind = project::MediaKind::Audio;
    item.mediaPath = audioPath;
    item.sampleRate = 48000;
    item.durationSamples = 144000;
    value.mediaItems.push_back(item);
    project::TimelineClip sound;
    sound.id = "audio";
    sound.name = item.name;
    sound.kind = project::TimelineClipKind::Audio;
    sound.mediaItemId = item.id;
    sound.mediaPath = audioPath;
    sound.sourceFpsNum = 60;
    sound.sourceFrameCount = sound.sourceOutFrame = 180;
    sound.track = {project::TrackKind::Audio, 0};
    value.timelineClips.push_back(sound);
    const auto projectPath = root / "acceptance.mvm";
    auto dissolve = value;
    for (auto& clip : dissolve.timelineClips)
        clip.effects = {};
    const auto unsupported = project::saveProjectJson(dissolve, root / "unsupported-dissolve.mvm");
    check(!unsupported.success && unsupported.error.find("クロスディゾルブ") != std::string::npos,
          "既存契約は数式間の dissolve を拒否する: " + unsupported.error);
    value.timelineTransitions.clear();
    const auto saved = project::saveProjectJson(value, projectPath);
    check(saved.success, "6 式・文字・音声・fade・keyframe の Project を保存する: " + saved.error);
    const auto loaded = project::loadProjectJson(projectPath);
    check(loaded.success && loaded.project == value,
          "受け入れ Project を変更なく開き直す: " + loaded.error);
    if (!loaded.success)
        return;
    for (int i = 0; i < 6; ++i) {
        const auto mapping = mvm::app::mapTimelinePreviewFrame(loaded.project, i * 30 + 15);
        std::size_t mathLayers = 0;
        for (const auto& layer : mapping.stillLayers)
            mathLayers += layer.kind == project::TimelineClipKind::Math;
        check(mapping.success && mathLayers == 1, "各区間の preview が実際に 1 式を使う");
    }
    const bool initialized = mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0;
    check(initialized, "受け入れ書き出しの MLT を初期化する");
    if (!initialized)
        return;
    auto missing = request;
    missing.mathArtifacts.erase("math-5");
    missing.outputPath = root / "missing.mp4";
    const auto rejected = mvm::app::exportTimeline(loaded.project, missing);
    check(!rejected.success && rejected.error.find("描画が完了していません") != std::string::npos,
          "6 式のうち最後の artifact が無くても全体の書き出しを拒否する");
    // 同じ WAV・尺・FPS・設定を保ち、数式の種別だけを通常の静止 layer に置き換える。
    const auto imagePath = root / "control.png";
    QImage image(1920, 1080, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    check(image.save(QString::fromStdWString(imagePath.wstring())), "画像対照の素材を作る");
    const auto control = [&](bool useImage) {
        auto project = loaded.project;
        if (useImage) {
            project::MediaItem media;
            media.id = "image-source";
            media.kind = project::MediaKind::Image;
            media.mediaPath = imagePath;
            media.name = "対照画像";
            media.width = 1920;
            media.height = 1080;
            project.mediaItems.push_back(media);
        }
        for (auto& clip : project.timelineClips) {
            if (clip.kind != project::TimelineClipKind::Math)
                continue;
            clip.math = {};
            clip.kind =
                useImage ? project::TimelineClipKind::Image : project::TimelineClipKind::Text;
            if (useImage) {
                clip.mediaPath = imagePath;
                clip.mediaItemId = "image-source";
            } else {
                clip.text.content = "通常の文字による対照";
            }
        }
        const std::string name = useImage ? "audio-image" : "audio-text";
        int mathCount = 0;
        int audioCount = 0;
        for (const auto& clip : project.timelineClips) {
            mathCount += clip.kind == project::TimelineClipKind::Math;
            audioCount += clip.kind == project::TimelineClipKind::Audio;
        }
        check(mathCount == 0 && audioCount == 1 && project.timelineClips.back() == sound,
              name + ": Math は 0 本で、同じ WAV・尺の A1 が残る");
        check(project::saveProjectJson(project, root / (name + ".mvm")).success,
              name + ": Math の無い対照を保存する");
        auto settings = request;
        settings.mathArtifacts.clear();
        settings.outputPath = root / (name + ".mp4");
        std::fprintf(stderr, "対照開始: %s\n", name.c_str());
        const auto result = mvm::app::exportTimeline(project, settings);
        std::fprintf(stderr, "対照終了: %s success=%d frame=%lld error=%s\n", name.c_str(),
                     result.success, result.frameCount, result.error.c_str());
        return result;
    };
    const auto textControl = control(false);
    const auto imageControl = control(true);
    std::fprintf(stderr, "対照開始: audio-math\n");
    const auto exported = mvm::app::exportTimeline(loaded.project, request);
    std::fprintf(stderr, "対照終了: audio-math success=%d frame=%lld error=%s\n", exported.success,
                 exported.frameCount, exported.error.c_str());
    const bool independent = independentAudioFailure(exported, textControl, imageControl);
    // 対照が成功した場合や別の失敗の場合を「独立」として閉じない。
    mvm::app::TimelineExportResult failedControl;
    failedControl.error = "対照の失敗";
    auto successfulControl = failedControl;
    successfulControl.success = true;
    auto otherFailure = failedControl;
    otherFailure.error = "別の失敗";
    check(!independentAudioFailure(failedControl, successfulControl, successfulControl),
          "Math だけが失敗する負例は独立と分類しない");
    check(!independentAudioFailure(failedControl, failedControl, successfulControl),
          "片方の対照だけが失敗する負例は独立と分類しない");
    check(!independentAudioFailure(failedControl, failedControl, otherFailure),
          "対照の失敗が異なる負例は独立と分類しない");
    check(!independentAudioFailure({}, {}, {}), "失敗の根拠が空の負例は独立と分類しない");
    check((exported.success && exported.frameCount == 180) || independent,
          "音声付き数式は成立するか、同条件の Text・画像でも独立な失敗が再現される");
    std::fprintf(stderr, "音声の帰属: %s\n",
                 independent ? "Math 非依存の既存書き出し不成立"
                             : (exported.success ? "成立" : "Math P0 の受け入れを保留"));
    // 音声付きの失敗を成功に変えない。映像だけの対照も独立に検査し、原因の範囲を残す。
    auto videoOnly = loaded.project;
    videoOnly.timelineClips.pop_back();
    auto videoRequest = request;
    videoRequest.outputPath = root / "acceptance-video-only.mp4";
    check(project::saveProjectJson(videoOnly, root / "acceptance-video-only.mvm").success,
          "映像だけの対照 Project を保存する");
    const auto videoExported = mvm::app::exportTimeline(videoOnly, videoRequest);
    check(videoExported.success && videoExported.frameCount == 180,
          "映像だけの対照は 180 frame を書き出す: " + videoExported.error);
    mvm_mlt_runtime_shutdown();
    if (!videoExported.success)
        return;
    QProcess video;
    video.start(ffmpeg,
                {"-v", "error", "-i", QString::fromStdWString(videoRequest.outputPath.wstring()),
                 "-an", "-vf", "scale=320:180", "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
    const bool decoded = video.waitForFinished(30000) &&
                         video.exitStatus() == QProcess::NormalExit && video.exitCode() == 0;
    const auto pixels = video.readAllStandardOutput();
    constexpr int frameBytes = 320 * 180 * 4;
    check(decoded && pixels.size() == frameBytes * 180, "MP4 の実 frame 数を復号して比較する");
    if (pixels.size() == frameBytes * 180) {
        for (int i = 0; i < 6; ++i) {
            std::size_t glyphs = 0;
            const auto* frame = reinterpret_cast<const unsigned char*>(pixels.constData()) +
                                (i * 30 + 15) * frameBytes;
            for (int y = 55; y < 130; ++y)
                for (int x = 10; x < 310; ++x) {
                    const auto* p = frame + (y * 320 + x) * 4;
                    glyphs += p[0] > 100 && p[1] > 100 && p[2] > 100;
                }
            check(glyphs > 30, "各区間に数式の glyph があり、文字 title だけで通っていない");
        }
    }
    if (!exported.success) {
        std::fprintf(stderr,
                     "音声付き出力が不成立のため、音声の復号・sample 比較は実施できません\n");
        return;
    }
    QProcess pcm;
    pcm.start(ffmpeg, {"-v", "error", "-i", QString::fromStdWString(request.outputPath.wstring()),
                       "-vn", "-ac", "1", "-ar", "48000", "-f", "f32le", "-"});
    const bool audioDecoded = pcm.waitForFinished(30000) &&
                              pcm.exitStatus() == QProcess::NormalExit && pcm.exitCode() == 0;
    const auto samples = pcm.readAllStandardOutput();
    double energy = 0;
    for (qsizetype at = 0; at + 4 <= samples.size(); at += 4) {
        float sample = 0;
        std::memcpy(&sample, samples.constData() + at, 4);
        const double amplitude = static_cast<double>(sample);
        energy += amplitude * amplitude;
    }
    check(audioDecoded && samples.size() >= 144000 * 4 && samples.size() < 146000 * 4 &&
              std::isfinite(energy) && energy > 100,
          "A1 が MP4 に残り、3 秒の実音声 sample と非無音を比較する");
}

// alpha の合計 (glyph の被覆)。
std::uint64_t alphaSum(const mvm::media::StillImage& image) {
    std::uint64_t sum = 0;
    for (std::size_t at = 3; at < image.rgba.size(); at += 4)
        sum += image.rgba[at];
    return sum;
}

// P1 の Write: 二次方程式の解の公式を 1.5 秒 (60 fps で 90 frame) で書く。
// 実 Manim の連番を mvm の decoder で読み、Project の保存・再読込と映像だけの書き出しへ通す。
// 式どうしの変形はしない (P2)。
void writeScenario(const std::filesystem::path& root, const mvm::math::MathRenderBackend& backend,
                   const std::string& source, const std::filesystem::path& staticMask) {
    namespace project = mvm::project;
    namespace math = mvm::math;
    constexpr int kWrite = 90;
    constexpr int kClip = 300;
    const auto staticImage = mvm::media::loadStaticImage(staticMask);
    check(staticImage.success, "Write: 静止の mask を読める");
    check(static_cast<bool>(backend.renderSequence) && backend.sequenceTemplate == "manim-write/1",
          "Write: backend が連番の描画と script の識別を持つ");
    if (!staticImage.success || !backend.renderSequence)
        return;
    math::MathSequenceRenderRequest request;
    request.spec = {{"latex", source, 96}, math::MathAnimationKind::Write, kWrite};
    request.jobDirectory = root / L"job write";
    const auto started = std::chrono::steady_clock::now();
    const auto rendered = backend.renderSequence(request, nullptr);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    std::printf("Write: %s %zu 枚 %dx%d %lld ms\n", math::mathRenderStatusName(rendered.status),
                rendered.frames.size(), rendered.width, rendered.height,
                static_cast<long long>(elapsed));
    check(rendered.status == math::MathRenderStatus::Ok && rendered.frames.size() == kWrite,
          "Write: 90 枚の連番を描ける: " + rendered.message + "\n" + rendered.log);
    if (rendered.status != math::MathRenderStatus::Ok || rendered.frames.size() != kWrite)
        return;
    check(rendered.width == staticImage.image.width && rendered.height == staticImage.image.height,
          "Write: 連番の大きさは静止の mask と同じ");
    const auto staticSum = alphaSum(staticImage.image);
    bool allDecoded = true;
    bool sameSize = true;
    bool edgesClear = true;
    bool opaqueWhite = true;
    std::vector<std::uint64_t> sums;
    for (const auto& frame : rendered.frames) {
        const auto decoded = mvm::media::loadStaticImage(frame);
        allDecoded = allDecoded && decoded.success;
        if (!decoded.success)
            break;
        const auto& image = decoded.image;
        sameSize = sameSize && image.width == staticImage.image.width &&
                   image.height == staticImage.image.height;
        const auto alphaAt = [&](int x, int y) {
            return image.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                               static_cast<std::size_t>(x)) *
                                  4 +
                              3];
        };
        for (int x = 0; x < image.width; ++x)
            for (const int y : {0, image.height - 1})
                edgesClear = edgesClear && alphaAt(x, y) == 0;
        for (int y = 0; y < image.height; ++y)
            for (const int x : {0, image.width - 1})
                edgesClear = edgesClear && alphaAt(x, y) == 0;
        for (std::size_t at = 0; at + 3 < image.rgba.size(); at += 4)
            if (image.rgba[at + 3] == 255)
                opaqueWhite = opaqueWhite && image.rgba[at] == 255 && image.rgba[at + 1] == 255 &&
                              image.rgba[at + 2] == 255;
        sums.push_back(alphaSum(image));
    }
    check(allDecoded && sameSize, "Write: 全 frame を mvm の decoder で読め、大きさが揃う");
    check(edgesClear, "Write: Write の線も外周 1 px に掛からない (余白に収まる)");
    check(opaqueWhite, "Write: 不透明な画素は白");
    if (sums.size() == kWrite) {
        std::printf("Write の被覆 (静止比): frame 0 %.4f / 22 %.3f / 45 %.3f / 67 %.3f / 89 %.3f\n",
                    static_cast<double>(sums[0]) / static_cast<double>(staticSum),
                    static_cast<double>(sums[22]) / static_cast<double>(staticSum),
                    static_cast<double>(sums[45]) / static_cast<double>(staticSum),
                    static_cast<double>(sums[67]) / static_cast<double>(staticSum),
                    static_cast<double>(sums[89]) / static_cast<double>(staticSum));
        check(sums[0] == 0, "Write: frame 0 は空 (進み具合 0)");
        check(sums[45] > 0 && sums[45] < staticSum, "Write: 途中の frame は書きかけ");
        check(static_cast<double>(sums[89]) >= 0.9 * static_cast<double>(staticSum),
              "Write: 最後の frame (89/90) は静止の 90% 以上を覆う");
    }

    // Project: 5 秒の数式 clip の先頭 1.5 秒を Write、fade in 6 frame。映像だけを書き出す。
    auto value = project::createDefaultProject();
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Math;
    clip.id = "math-write";
    clip.name = "解の公式 (Write)";
    clip.math.source = source;
    clip.sourceFpsNum = 60;
    clip.sourceFrameCount = clip.sourceOutFrame = kClip;
    clip.track = {project::TrackKind::Video, 0};
    clip.effects.fadeInFrames = 6;
    clip.mathAnimation = {project::MathIntroKind::Write, kWrite};
    value.timelineClips.push_back(clip);
    const auto projectPath = root / "write.mvm";
    const auto saved = project::saveProjectJson(value, projectPath);
    check(saved.success, "Write: Project を保存する: " + saved.error);
    const auto loaded = project::loadProjectJson(projectPath);
    check(loaded.success && loaded.project == value &&
              loaded.project.timelineClips[0].mathAnimation ==
                  project::MathClipAnimation{project::MathIntroKind::Write, kWrite},
          "Write: Project を変更なく開き直す: " + loaded.error);
    if (!loaded.success)
        return;
    const bool initialized = mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0;
    check(initialized, "Write: 書き出しの MLT を初期化する");
    if (!initialized)
        return;
    mvm::app::TimelineExportRequest export_;
    export_.outputPath = root / "write.mp4";
    export_.mathArtifacts = {{clip.id, staticMask}};
    auto missing = export_;
    missing.outputPath = root / "write-missing.mp4";
    const auto rejected = mvm::app::exportTimeline(loaded.project, missing);
    check(!rejected.success &&
              rejected.error.find("Write の描画が完了していません") != std::string::npos,
          "Write: 連番が無ければ書き出さない: " + rejected.error);
    export_.mathWriteFrames = {{clip.id, rendered.frames}};
    // MLT は閉じない (後の P0 の受け入れが同じ初期化を使い、最後に閉じる)。
    const auto exported = mvm::app::exportTimeline(loaded.project, export_);
    check(exported.success && exported.frameCount == kClip,
          "Write: 300 frame を書き出す: " + exported.error);
    if (!exported.success)
        return;
    QProcess video;
    video.start(QStringLiteral(MVM_MATH_FFMPEG),
                {"-v", "error", "-i", QString::fromStdWString(export_.outputPath.wstring()), "-an",
                 "-vf", "scale=320:180", "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
    const bool decoded = video.waitForFinished(30000) &&
                         video.exitStatus() == QProcess::NormalExit && video.exitCode() == 0;
    const auto pixels = video.readAllStandardOutput();
    constexpr int frameBytes = 320 * 180 * 4;
    check(decoded && pixels.size() == frameBytes * kClip, "Write: MP4 の実 frame 数を復号する");
    if (pixels.size() != frameBytes * kClip)
        return;
    const auto glyphs = [&](int t) {
        std::size_t count = 0;
        const auto* frame = reinterpret_cast<const unsigned char*>(pixels.constData()) +
                            static_cast<std::size_t>(t) * frameBytes;
        for (int y = 40; y < 140; ++y)
            for (int x = 40; x < 280; ++x) {
                const auto* p = frame + (y * 320 + x) * 4;
                count += p[0] > 100 && p[1] > 100 && p[2] > 100;
            }
        return count;
    };
    const auto at10 = glyphs(10);
    const auto at45 = glyphs(45);
    const auto at89 = glyphs(89);
    const auto at150 = glyphs(150);
    const auto at299 = glyphs(299);
    std::printf(
        "Write の書き出しの glyph 画素: frame 10 %zu / 45 %zu / 89 %zu / 150 %zu / 299 %zu\n", at10,
        at45, at89, at150, at299);
    // 同じ画素の frame でも H.264 の圧縮で閾値付近の数画素が揺れる (実測 440 と 442)。
    check(at150 > 30 && (at150 > at299 ? at150 - at299 : at299 - at150) * 50 <= at150,
          "Write: Write の後は静止の式 (画素数の差は 2% 以内)");
    check(at10 < at45 && at45 < at150, "Write: 書き出しでも式が順に書かれていく");
    check(static_cast<double>(at89) >= 0.8 * static_cast<double>(at150),
          "Write: Write の最後の frame は書き終えた式に近い");
}

} // namespace

int main() {
    mvm_enable_utf8_console();
    int argc = 0;
    char** argv = mvm_win_get_utf8_args(&argc);
    if (!argv || argc != 3) {
        std::fprintf(stderr, "使い方: mvm_math_manim_smoke <manim.exe> <作業 directory>\n");
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }
    const auto manimExe = std::filesystem::absolute(fromUtf8(argv[1]));
    const auto root = std::filesystem::absolute(fromUtf8(argv[2]));
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    std::error_code error;
    if (!std::filesystem::create_directories(root, error)) {
        std::fprintf(stderr, "新しい作業 directory を指定してください: %s\n",
                     error.message().c_str());
        mvm_win_free_utf8_args(argv, argc);
        return 2;
    }

    namespace math = mvm::math;
    const auto preflight =
        mvm::manim::preflightManimMathTex({manimExe, root / L"preflight"}, nullptr);
    if (preflight.status != math::MathPreflightStatus::Available) {
        std::fprintf(stderr, "UNAVAILABLE: %s\n", preflight.message.c_str());
        mvm_win_free_utf8_args(argv, argc);
        return 3;
    }
    std::printf("fingerprint:\n%s", preflight.backend.fingerprint.canonical.c_str());

    const std::vector<std::string> sources = {
        "ax^2 + bx + c = 0",
        "x^2 + \\frac{b}{a}x = -\\frac{c}{a}",
        "x^2 + \\frac{b}{a}x + \\left(\\frac{b}{2a}\\right)^2 = "
        "\\left(\\frac{b}{2a}\\right)^2 - \\frac{c}{a}",
        "\\left(x + \\frac{b}{2a}\\right)^2 = \\frac{b^2 - 4ac}{4a^2}",
        "x = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}",
        "x = \\frac{-b \\pm \\sqrt{\\boxed{b^2 - 4ac}}}{2a}",
        // 引用符と改行を含む式が JSON 経由でそのまま Python へ届くこと。
        "\\text{\"quoted\"}\n+ 1",
    };
    int index = 0;
    std::vector<std::filesystem::path> masks;
    for (const auto& source : sources) {
        math::MathStaticRenderRequest request;
        request.spec = {"latex", source, 96};
        request.jobDirectory = root / (L"job " + std::to_wstring(index));
        const auto started = std::chrono::steady_clock::now();
        const auto rendered = preflight.backend.render(request, nullptr);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const std::string label = "式 " + std::to_string(index);
        std::printf("%s: %s %dx%d %lld ms\n", label.c_str(),
                    math::mathRenderStatusName(rendered.status), rendered.width, rendered.height,
                    static_cast<long long>(elapsed));
        check(rendered.status == math::MathRenderStatus::Ok,
              label + " を描ける: " + rendered.message + "\n" + rendered.log);
        if (rendered.status == math::MathRenderStatus::Ok) {
            inspectPng(rendered, label);
            masks.push_back(rendered.png);
        }
        ++index;
    }

    math::MathStaticRenderRequest bad;
    bad.spec = {"latex", "\\fracc{a}{b}", 96};
    bad.jobDirectory = root / L"job invalid";
    const auto invalid = preflight.backend.render(bad, nullptr);
    std::printf("不正な式: %s \"%s\"\n", math::mathRenderStatusName(invalid.status),
                invalid.message.c_str());
    check(invalid.status == math::MathRenderStatus::InvalidSource, "不正な式は InvalidSource");
    check(invalid.message.find("Undefined control sequence") != std::string::npos,
          "message は TeX の error 行");

    if (failures == 0) {
        // 解の公式 (sources[4]) の Write を別の Project で確かめてから、P0 の受け入れを行う
        // (MLT の初期化を共有し、P0 の受け入れの最後に閉じる)。
        writeScenario(root, preflight.backend, sources[4], masks[4]);
        acceptScenario(root, sources, masks);
    } else {
        std::fprintf(stderr, "backend の検査が不成立のため、統合受け入れは実施できません\n");
    }

    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    mvm_win_free_utf8_args(argv, argc);
    return failures == 0 && checks > 0 ? 0 : 1;
}
