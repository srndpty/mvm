// 最新の解の公式 Project を、製品の MvmController が組む要求のまま
// exportTimeline（MLT / H.264）へ渡す。
//
// 画素の authority は encoder の YUV 変換前（encoderFrameValidator の RGBA）。
// 復号した H.264 の RGB は寸法と frame 数の照合に使い、画素の一致判定には使わない。
// 代表 frame の pre-YUV は PNG として残し、全出力 frame の到達を欠かさず数える。

#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "mvm_controller.h"
#include "project/project_json.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUrl>

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// 分子の移動、分数線の伸縮、打ち消し、境界での出現・消滅、最後の中央寄せ。
constexpr std::int64_t kRepresentative[] = {599,  600,  630,  660,  1260, 1278, 1296,
                                            5340, 5370, 5400, 6600, 6630, 6660, 6720};

struct Observation {
    std::mutex mutex;
    std::map<std::int64_t, int> calls;
    std::map<std::int64_t, std::vector<std::uint8_t>> samples;
    bool rejected = false;
    std::string error;
    int width = 0;
    int height = 0;
};

bool observeFrame(Observation& observation, std::int64_t frame, const std::uint8_t* rgba, int width,
                  int height) {
    std::lock_guard lock(observation.mutex);
    if (width != kWidth || height != kHeight || rgba == nullptr) {
        observation.rejected = true;
        observation.error = "pre-YUV の寸法が Project と違います";
        return false;
    }
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    ++observation.calls[frame];
    observation.width = width;
    observation.height = height;
    for (const auto wanted : kRepresentative) {
        if (wanted == frame && observation.samples.find(frame) == observation.samples.end()) {
            observation.samples.emplace(frame, std::vector<std::uint8_t>(rgba, rgba + bytes));
        }
    }
    return true;
}

template<typename Done>
bool waitUntil(Done done, int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < milliseconds) {
        QGuiApplication::processEvents();
        QThread::msleep(20);
    }
    return done();
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLTを初期化できません\n");
        return 2;
    }

    struct RuntimeGuard {
        ~RuntimeGuard() { mvm_mlt_runtime_shutdown(); }
    } runtimeGuard;

    if (argc != 5) {
        std::fprintf(stderr, "使い方: mvm_math_score_export_parity <project.mvm> <manim.exe> "
                             "<出力directory> <ffmpeg.exe>\n");
        return 2;
    }
    const std::filesystem::path projectPath(argv[1]);
    const std::filesystem::path manimPath(argv[2]);
    const std::filesystem::path outputDirectory(argv[3]);
    const std::filesystem::path ffmpegPath(argv[4]);
    std::error_code makeDirectoryError;
    std::filesystem::create_directories(outputDirectory, makeDirectoryError);
    if (makeDirectoryError) {
        std::fprintf(stderr, "出力directoryを作れません\n");
        return 2;
    }

    const auto loaded = mvm::project::loadProjectJson(projectPath);
    if (!loaded.success) {
        std::fprintf(stderr, "Projectを開けません: %s\n", loaded.error.c_str());
        return 3;
    }
    if (loaded.project.outputWidth != kWidth || loaded.project.outputHeight != kHeight ||
        loaded.project.timelineFpsNum != 60 || loaded.project.timelineFpsDen != 1) {
        std::fprintf(stderr, "想定した画面サイズまたはfpsではありません\n");
        return 3;
    }

    Observation observation;
    mvm::app::TimelineExportResult exported;
    mvm::app::MvmController::ExportRunner runner =
        [&](const mvm::project::Project& project, const mvm::app::TimelineExportRequest& request) {
            auto input = request;
            input.timeoutMs = 4 * 60 * 60 * 1000;
            input.encoderFrameValidator = [&](std::int64_t frame, const std::uint8_t* rgba,
                                              int width, int height) {
                return observeFrame(observation, frame, rgba, width, height);
            };
            exported = mvm::app::exportTimeline(project, input);
            return exported;
        };
    auto controller = std::make_unique<mvm::app::MvmController>(
        projectPath, manimPath, loaded.project, nullptr, std::move(runner),
        mvm::app::MvmController::ExportThreadFactory{},
        mvm::app::MvmController::FileRevealer{
            [](const std::filesystem::path&, QString&) { return true; }});
    if (!controller->holdsProjectLock()) {
        std::fprintf(stderr, "%s\n", controller->statusText().toUtf8().constData());
        return 4;
    }

    const bool prepared = waitUntil(
        [&] {
            for (const auto& clip : controller->projectForTest().timelineClips) {
                if (clip.kind != mvm::project::TimelineClipKind::Math || !clip.enabled)
                    continue;
                const auto state = controller->mathClipData(QString::fromStdString(clip.id))
                                       .value(QStringLiteral("state"))
                                       .toString();
                if (state == QStringLiteral("error") || state == QStringLiteral("unavailable"))
                    return true;
                if (state != QStringLiteral("ready"))
                    return false;
            }
            return true;
        },
        30 * 60 * 1000);
    if (!prepared) {
        std::fprintf(stderr, "数式の準備が時間内に終わりませんでした\n");
        return 5;
    }
    for (const auto& clip : controller->projectForTest().timelineClips) {
        if (clip.kind != mvm::project::TimelineClipKind::Math || !clip.enabled)
            continue;
        const auto data = controller->mathClipData(QString::fromStdString(clip.id));
        const auto state = data.value(QStringLiteral("state")).toString();
        if (state != QStringLiteral("ready")) {
            std::fprintf(stderr, "数式を書き出せません: %s\n",
                         data.value(QStringLiteral("message")).toString().toUtf8().constData());
            return 5;
        }
    }

    const auto outputPath = outputDirectory / "quadratic-export.mp4";
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    if (!controller->exportTimeline(
            QUrl::fromLocalFile(QString::fromStdString(outputPath.string())))) {
        std::fprintf(stderr, "書き出しを開始できません: %s\n",
                     controller->statusText().toUtf8().constData());
        return 6;
    }
    const bool finished = waitUntil([&] { return !controller->exporting(); }, 4 * 60 * 60 * 1000);
    if (!finished || !exported.success) {
        std::fprintf(stderr, "書き出しが完了しませんでした: %s\n", exported.error.c_str());
        return 6;
    }

    QJsonArray missing;
    QJsonArray repeated;
    for (std::int64_t frame = 0; frame < exported.frameCount; ++frame) {
        const auto found = observation.calls.find(frame);
        if (found == observation.calls.end())
            missing.append(static_cast<qint64>(frame));
        else if (found->second > 1)
            repeated.append(QJsonObject{{QStringLiteral("frame"), static_cast<qint64>(frame)},
                                        {QStringLiteral("calls"), found->second}});
    }
    bool outside = false;
    for (const auto& entry : observation.calls) {
        if (entry.first < 0 || entry.first >= exported.frameCount)
            outside = true;
    }
    QJsonArray samples;
    int alphaFailures = 0;
    for (const auto frame : kRepresentative) {
        const auto found = observation.samples.find(frame);
        QJsonObject sample;
        sample.insert(QStringLiteral("frame"), static_cast<qint64>(frame));
        sample.insert(QStringLiteral("saved"), found != observation.samples.end());
        if (found == observation.samples.end()) {
            ++alphaFailures;
            samples.append(sample);
            continue;
        }
        const auto& rgba = found->second;
        int minimum = 255;
        int maximum = 0;
        for (std::size_t index = 3; index < rgba.size(); index += 4) {
            minimum = std::min(minimum, static_cast<int>(rgba[index]));
            maximum = std::max(maximum, static_cast<int>(rgba[index]));
        }
        sample.insert(QStringLiteral("alpha_min"), minimum);
        sample.insert(QStringLiteral("alpha_max"), maximum);
        if (minimum != 255 || maximum != 255)
            ++alphaFailures;
        QImage image(rgba.data(), kWidth, kHeight, kWidth * 4, QImage::Format_RGBA8888);
        const auto path = outputDirectory / ("pre-yuv-" + std::to_string(frame) + ".png");
        sample.insert(QStringLiteral("png"), QString::fromStdString(path.string()));
        if (!image.copy().save(QString::fromStdString(path.string()), "PNG"))
            ++alphaFailures;
        samples.append(sample);
    }

    QJsonObject report;
    report.insert(QStringLiteral("authority"), QStringLiteral("pre-yuv-rgba"));
    report.insert(QStringLiteral("encoder"), QStringLiteral("h264"));
    report.insert(QStringLiteral("decoded_rgb_is_pixel_authority"), false);
    report.insert(QStringLiteral("frame_count"), static_cast<qint64>(exported.frameCount));
    report.insert(QStringLiteral("duration_sec"), exported.durationSec);
    report.insert(QStringLiteral("width"), observation.width);
    report.insert(QStringLiteral("height"), observation.height);
    report.insert(QStringLiteral("observed_frames"), static_cast<qint64>(observation.calls.size()));
    report.insert(QStringLiteral("missing_frames"), missing);
    report.insert(QStringLiteral("repeated_fetches"), repeated);
    report.insert(QStringLiteral("samples"), samples);
    report.insert(QStringLiteral("ffmpeg"), QString::fromStdString(ffmpegPath.string()));
    report.insert(QStringLiteral("output"), QString::fromStdString(outputPath.string()));
    const auto reportPath = outputDirectory / "pre-yuv-coverage.json";
    QFile file(QString::fromStdString(reportPath.string()));
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "照合結果を書けません\n");
        return 7;
    }
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    file.close();

    const bool covered =
        missing.isEmpty() && !outside &&
        observation.calls.size() == static_cast<std::size_t>(exported.frameCount) &&
        exported.frameCount == 6840 && alphaFailures == 0 && observation.width == kWidth &&
        observation.height == kHeight;
    if (!covered) {
        std::fprintf(stderr, "pre-YUV の frame 被覆または alpha が要求を満たしません\n");
        return 8;
    }
    std::fprintf(stderr, "pre-YUV %lld frame を被覆しました\n",
                 static_cast<long long>(exported.frameCount));
    return 0;
}
