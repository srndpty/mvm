// 数式 clip の書き出し (src/app/timeline_export.cpp)。
//
// - 書き出しは渡された描画済みの PNG (mask) を preview と同じ合成に通し、出力の中央へ
//   clip の色で置く。期待する位置と色は手で計算した (合成の実装を呼ばない)
// - 描画済みの PNG が渡されていない数式 clip は書き出さない (fail-closed)
// - 出力より大きい数式は切らずに失敗する

#include "app/timeline_export.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/project.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

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

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kFrames = 15;

QByteArray decode(const QString& ffmpeg, const std::filesystem::path& path) {
    QProcess process;
    process.start(ffmpeg, {"-v", "error", "-i", QString::fromStdWString(path.wstring()), "-f",
                           "rawvideo", "-pix_fmt", "rgba", "-"});
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
        return {};
    return process.readAllStandardOutput();
}

// 白い不透明な矩形の mask (backend の出力と同じ形: 白の glyph、透明の余白)。
std::filesystem::path writeMask(const QTemporaryDir& directory, const QString& name, int width,
                                int height) {
    QImage mask(width, height, QImage::Format_RGBA8888);
    mask.fill(QColor(255, 255, 255, 255));
    const auto path = directory.filePath(name);
    mask.save(path, "PNG");
    return std::filesystem::path(path.toStdWString());
}

mvm::project::Project projectWithMath() {
    auto project = mvm::project::createDefaultProject();
    project.outputWidth = kWidth;
    project.outputHeight = kHeight;
    mvm::project::TimelineClip clip;
    clip.kind = mvm::project::TimelineClipKind::Math;
    clip.id = "math";
    clip.name = "x^2";
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = clip.sourceOutFrame = kFrames;
    clip.track = {mvm::project::TrackKind::Video, 0};
    clip.math.source = "x^2";
    clip.math.fontSize = 96;
    clip.math.color = "#FFFF0000";
    project.timelineClips.push_back(clip);
    return project;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_math_export <ffmpeg.exe>\n");
        return 2;
    }
    const QString ffmpeg = QString::fromLocal8Bit(argv[1]);
    if (mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) != 0) {
        std::fprintf(stderr, "MLT を初期化できません\n");
        return 1;
    }
    QTemporaryDir temporary;
    check(temporary.isValid(), "作業フォルダー");
    const auto project = projectWithMath();

    mvm::app::TimelineExportRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.videoCrf = 10;
    request.outputPath = std::filesystem::path(temporary.filePath("math.mp4").toStdWString());

    // 描画済みの PNG が無ければ書き出さない。
    auto missing = mvm::app::exportTimeline(project, request);
    check(!missing.success && missing.error.find("描画が完了していません") != std::string::npos,
          "描画済みの PNG が無い数式 clip は書き出さない: " + missing.error);

    // 出力より大きい数式は切らずに失敗する。
    request.mathArtifacts = {{"math", writeMask(temporary, "wide.png", kWidth + 2, 40)}};
    auto oversized = mvm::app::exportTimeline(project, request);
    check(!oversized.success &&
              oversized.error.find("出力サイズを超えています") != std::string::npos,
          "出力より大きい数式は失敗する: " + oversized.error);

    // 80x40 の mask は 320x240 の中央 (左上 120, 100) に、clip の色 (赤) で置かれる。
    request.mathArtifacts = {{"math", writeMask(temporary, "mask.png", 80, 40)}};
    const auto result = mvm::app::exportTimeline(project, request);
    check(result.success && result.frameCount == kFrames,
          "数式 clip を書き出せる: " + result.error);
    const auto frames = decode(ffmpeg, request.outputPath);
    constexpr int frameBytes = kWidth * kHeight * 4;
    check(frames.size() == frameBytes * kFrames, "復号した frame 数を実際に比較する");
    if (frames.size() == frameBytes * kFrames) {
        const auto* frame =
            reinterpret_cast<const unsigned char*>(frames.constData() + 5 * frameBytes);
        const auto pixel = [&](int x, int y) { return frame + (y * kWidth + x) * 4; };
        std::size_t red = 0;
        std::size_t inside = 0;
        // 色差の間引き (4:2:0) で縁がにじむので、矩形の内側 4 px で比べる。
        for (int y = 104; y < 136; ++y)
            for (int x = 124; x < 196; ++x) {
                ++inside;
                const auto* p = pixel(x, y);
                if (p[0] > 200 && p[1] < 60 && p[2] < 60)
                    ++red;
            }
        check(inside > 0 && red == inside, "数式は中央に clip の色で置かれる");
        std::size_t black = 0;
        std::size_t outside = 0;
        for (const auto [x, y] :
             {std::pair{10, 10}, std::pair{300, 10}, std::pair{10, 230}, std::pair{300, 230},
              std::pair{110, 120}, std::pair{210, 120}, std::pair{160, 90}, std::pair{160, 150}}) {
            ++outside;
            const auto* p = pixel(x, y);
            if (p[0] < 20 && p[1] < 20 && p[2] < 20)
                ++black;
        }
        check(black == outside, "数式の外は黒 (mask の外は透明)");
    }
    mvm_mlt_runtime_shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
