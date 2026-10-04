// 数式 clip の書き出し (src/app/timeline_export.cpp)。
//
// - 書き出しは渡された描画済みの PNG (mask) を preview と同じ合成に通し、出力の中央へ
//   clip の色で置く。期待する位置と色は手で計算した (合成の実装を呼ばない)
// - 描画済みの PNG が渡されていない数式 clip は書き出さない (fail-closed)
// - 出力より大きい数式は切らずに失敗する
// - Write は先頭の区間を連番で書き出し、その後は静止の描画。frame ごとの期待矩形は手で決め、
//   ずらした期待値・別の frame の preview を対照にする

#include "app/math_clip_render.h"
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media/still_image/static_image.h"
#include "project/project.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

template<class T>
struct ComRelease {
    void operator()(T* pointer) const {
        if (pointer)
            pointer->Release();
    }
};

// 描画済みの mask を製品の静止画合成へ通す。試験専用の小領域 readback で実 GPU 画素を比較する。
std::vector<unsigned char> previewRgba(const mvm::project::Project& project,
                                       const std::filesystem::path& mask, double shiftX = 0) {
    const auto raster =
        mvm::app::composeMathClipFromPng(mask, project.timelineClips[0].math, kWidth, kHeight);
    check(raster.success, "preview 用の数式 raster を作る: " + raster.error);
    if (!raster.success)
        return {};
    ID3D11Device* rawDevice = nullptr;
    ID3D11DeviceContext* rawContext = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const auto created =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &rawDevice, &selected, &rawContext);
    std::unique_ptr<ID3D11Device, ComRelease<ID3D11Device>> device(rawDevice);
    std::unique_ptr<ID3D11DeviceContext, ComRelease<ID3D11DeviceContext>> context(rawContext);
    check(SUCCEEDED(created), "hardware D3D11 device を作る");
    if (FAILED(created))
        return {};
    mvm::gpu::SharedD3D11Device shared;
    mvm::gpu::ReadbackCounters readbacks;
    mvm::gpu::GpuCompositor compositor;
    mvm::gpu::DecodedGpuFrame still;
    std::string error;
    const bool initialized =
        shared.adopt(device.get(), context.get(), error) &&
        compositor.initialize(shared, readbacks, kWidth, kHeight, error) &&
        mvm::gpu::makeStillImageFrame(shared, kWidth, kHeight, raster.rgba.data(),
                                      raster.rgba.size(), {}, still, error);
    check(initialized, "GPU の数式 layer を作る: " + error);
    if (!initialized)
        return {};
    const auto mapping = mvm::app::mapTimelinePreviewFrame(project, 5);
    check(mapping.success && mapping.stillLayers.size() == 1, "preview の数式 layer を実際に使う");
    if (!mapping.success || mapping.stillLayers.size() != 1)
        return {};
    auto effects = mvm::project::evaluateClipEffects(project.timelineClips[0].effects, 5);
    effects.positionXPercent += shiftX;
    mvm::preview::PreviewCompositionLayer mapped;
    mvm::app::applyPreviewLayerEffects(mapped, effects, mapping.stillLayers[0].opacity, 0, kFrames);
    mvm::gpu::CompositionLayerFrame layer;
    layer.frame = still;
    layer.destination = {mapped.destination.x, mapped.destination.y, mapped.destination.width,
                         mapped.destination.height};
    layer.sourceUv = {mapped.sourceRect.x, mapped.sourceRect.y, mapped.sourceRect.width,
                      mapped.sourceRect.height};
    layer.effectsEnabled = mapped.effectsEnabled;
    layer.opacity = mapped.opacity;
    layer.rotationDegrees = mapped.rotationDegrees;
    mvm::gpu::ComposedFrame frame;
    frame.compositionEpoch = {1};
    frame.layers.push_back(layer);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rawTexture = nullptr;
    ID3D11RenderTargetView* rawTarget = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&description, nullptr, &rawTexture)))
        device->CreateRenderTargetView(rawTexture, nullptr, &rawTarget);
    std::unique_ptr<ID3D11Texture2D, ComRelease<ID3D11Texture2D>> texture(rawTexture);
    std::unique_ptr<ID3D11RenderTargetView, ComRelease<ID3D11RenderTargetView>> target(rawTarget);
    check(texture && target, "GPU 比較の描画先を作る");
    if (!texture || !target)
        return {};
    {
        std::lock_guard<mvm::gpu::D3D11Lock> lock(shared.lock());
        const float black[] = {0, 0, 0, 1};
        context->ClearRenderTargetView(target.get(), black);
    }
    bool success =
        compositor.composeLayersToTarget(frame, {target.get(), kWidth, kHeight}, 1, error);
    std::vector<unsigned char> result;
    for (int y = 0; success && y < kHeight; y += 60) {
        std::vector<unsigned char> band;
        success = compositor.readExternalOutputProbe(texture.get(), 0, y, kWidth, 60, band, error);
        result.insert(result.end(), band.begin(), band.end());
    }
    check(success, "数式の実 GPU 画素を読む: " + error);
    check(compositor.shutdown(5000, error), "比較用 GPU compositor を終了する: " + error);
    return success ? result : std::vector<unsigned char>{};
}

// ---- Write ----
//
// 30 frame の clip の先頭 20 frame が Write。Write の frame i の mask (80x40) は左から 4i 列が
// 不透明な白 (frame 0 は空)。静止の mask は 80x40 全面。横位置は frame 0 から 20 にかけて
// 0% から 10% (32 px) へ動き、その後は 10% のまま (Write と静止の区間の境で途切れないこと)。
constexpr int kWriteClipFrames = 30;
constexpr int kWriteFrames = 20;

// 期待値 (実装を呼ばずに手で決める): 出力 frame t の赤い矩形の左上と幅。
int expectedShift(int t) {
    return static_cast<int>(320.0 * 0.10 * std::min(t, kWriteFrames) / kWriteFrames + 0.5);
}

int expectedColumns(int t) {
    return t < kWriteFrames ? 4 * t : 80;
}

mvm::project::Project projectWithWrite() {
    auto project = projectWithMath();
    auto& clip = project.timelineClips[0];
    clip.sourceFrameCount = clip.sourceOutFrame = kWriteClipFrames;
    clip.mathAnimation = {mvm::project::MathIntroKind::Write, kWriteFrames};
    clip.effects.positionXKeys = {{0, 0}, {kWriteFrames, 10}};
    return project;
}

std::vector<std::filesystem::path> writeSequence(const QTemporaryDir& directory, int count) {
    std::vector<std::filesystem::path> frames;
    for (int index = 0; index < count; ++index) {
        QImage mask(80, 40, QImage::Format_RGBA8888);
        mask.fill(Qt::transparent);
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 4 * index; ++x)
                mask.setPixelColor(x, y, QColor(255, 255, 255, 255));
        const auto path = directory.filePath(QStringLiteral("write-%1.png").arg(index));
        mask.save(path, "PNG");
        frames.emplace_back(path.toStdWString());
    }
    return frames;
}

bool isRed(const unsigned char* p) {
    return p[0] > 200 && p[1] < 60 && p[2] < 60;
}

bool isBlack(const unsigned char* p) {
    return p[0] < 30 && p[1] < 30 && p[2] < 30;
}

// frame の画素を「t の期待矩形の中は赤、外は黒」と比べる。縁の 3 px (4:2:0 のにじみ) は除く。
// compared は比べた画素数。
std::size_t matchingPixels(const unsigned char* frame, int t, std::size_t& compared) {
    const int left = 120 + expectedShift(t);
    const int right = left + expectedColumns(t);
    std::size_t matched = 0;
    compared = 0;
    for (int y = 104; y < 136; ++y)
        for (int x = 100; x < 250; ++x) {
            const bool inside = x >= left && x < right;
            if (std::abs(x - left) < 3 || std::abs(x - right) < 3)
                continue;
            ++compared;
            const auto* p = frame + (y * kWidth + x) * 4;
            matched += inside ? isRed(p) : isBlack(p);
        }
    return matched;
}

// preview: 静止の画素を texture にし、Write の frame の patch を mask の矩形へ書いてから、
// preview と同じ effect の幾何で GPU 合成する (engine の静止画 animation と同じ経路の部品)。
std::vector<unsigned char> previewWriteRgba(const mvm::project::Project& project,
                                            const std::filesystem::path& staticMask,
                                            const std::filesystem::path& writeMask,
                                            std::int64_t frameNumber) {
    const auto& clip = project.timelineClips[0];
    const auto raster = mvm::app::composeMathClipFromPng(staticMask, clip.math, kWidth, kHeight);
    const auto decoded = mvm::media::loadStaticImage(writeMask);
    mvm::math::MathComposeStyle style;
    int left = 0;
    int top = 0;
    const bool ready = raster.success && decoded.success &&
                       mvm::app::mathComposeStyleFor(clip.math, style) &&
                       mvm::math::mathRasterPlacement(decoded.image.width, decoded.image.height,
                                                      kWidth, kHeight, left, top);
    check(ready, "preview 用の Write の画素を作る");
    if (!ready)
        return {};
    std::vector<std::uint8_t> coverage;
    for (std::size_t index = 3; index < decoded.image.rgba.size(); index += 4)
        coverage.push_back(decoded.image.rgba[index]);
    std::vector<std::uint8_t> patch(coverage.size() * 4U);
    mvm::math::composeMathPatch(coverage.data(), decoded.image.width, decoded.image.height, style,
                                patch.data());

    ID3D11Device* rawDevice = nullptr;
    ID3D11DeviceContext* rawContext = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    const auto created =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          levels, 2, D3D11_SDK_VERSION, &rawDevice, &selected, &rawContext);
    std::unique_ptr<ID3D11Device, ComRelease<ID3D11Device>> device(rawDevice);
    std::unique_ptr<ID3D11DeviceContext, ComRelease<ID3D11DeviceContext>> context(rawContext);
    check(SUCCEEDED(created), "Write: hardware D3D11 device を作る");
    if (FAILED(created))
        return {};
    mvm::gpu::SharedD3D11Device shared;
    mvm::gpu::ReadbackCounters readbacks;
    mvm::gpu::GpuCompositor compositor;
    mvm::gpu::DecodedGpuFrame still;
    std::string error;
    const bool initialized =
        shared.adopt(device.get(), context.get(), error) &&
        compositor.initialize(shared, readbacks, kWidth, kHeight, error) &&
        mvm::gpu::makeUpdatableStillImageFrame(shared, kWidth, kHeight, raster.rgba.data(),
                                               raster.rgba.size(), {}, still, error) &&
        mvm::gpu::updateStillImageRegion(shared, still, left, top, decoded.image.width,
                                         decoded.image.height, patch.data(), patch.size(), error);
    check(initialized, "Write: GPU の数式 layer を作る: " + error);
    if (!initialized)
        return {};
    const auto effects = mvm::project::evaluateClipEffects(clip.effects, frameNumber);
    mvm::preview::PreviewCompositionLayer mapped;
    mvm::app::applyPreviewLayerEffects(mapped, effects, 1.0, 0, kWriteClipFrames);
    mvm::gpu::CompositionLayerFrame layer;
    layer.frame = still;
    layer.destination = {mapped.destination.x, mapped.destination.y, mapped.destination.width,
                         mapped.destination.height};
    layer.sourceUv = {mapped.sourceRect.x, mapped.sourceRect.y, mapped.sourceRect.width,
                      mapped.sourceRect.height};
    layer.effectsEnabled = mapped.effectsEnabled;
    layer.opacity = mapped.opacity;
    mvm::gpu::ComposedFrame frame;
    frame.compositionEpoch = {1};
    frame.layers.push_back(layer);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rawTexture = nullptr;
    ID3D11RenderTargetView* rawTarget = nullptr;
    if (SUCCEEDED(device->CreateTexture2D(&description, nullptr, &rawTexture)))
        device->CreateRenderTargetView(rawTexture, nullptr, &rawTarget);
    std::unique_ptr<ID3D11Texture2D, ComRelease<ID3D11Texture2D>> texture(rawTexture);
    std::unique_ptr<ID3D11RenderTargetView, ComRelease<ID3D11RenderTargetView>> target(rawTarget);
    check(texture && target, "Write: GPU 比較の描画先を作る");
    if (!texture || !target)
        return {};
    {
        std::lock_guard<mvm::gpu::D3D11Lock> lock(shared.lock());
        const float black[] = {0, 0, 0, 1};
        context->ClearRenderTargetView(target.get(), black);
    }
    bool success =
        compositor.composeLayersToTarget(frame, {target.get(), kWidth, kHeight}, 1, error);
    std::vector<unsigned char> result;
    for (int y = 0; success && y < kHeight; y += 60) {
        std::vector<unsigned char> band;
        success = compositor.readExternalOutputProbe(texture.get(), 0, y, kWidth, 60, band, error);
        result.insert(result.end(), band.begin(), band.end());
    }
    check(success, "Write: 実 GPU 画素を読む: " + error);
    check(compositor.shutdown(5000, error), "Write: 比較用 GPU compositor を終了する: " + error);
    return success ? result : std::vector<unsigned char>{};
}

void testWriteExport(const QString& ffmpeg, const QTemporaryDir& temporary) {
    const auto project = projectWithWrite();
    const auto staticMask = writeMask(temporary, "write-static.png", 80, 40);
    const auto sequence = writeSequence(temporary, kWriteFrames);
    mvm::app::TimelineExportRequest request;
    request.width = kWidth;
    request.height = kHeight;
    request.videoCrf = 10;
    request.mathArtifacts = {{"math", staticMask}};

    request.outputPath = std::filesystem::path(temporary.filePath("no-write.mp4").toStdWString());
    const auto missing = mvm::app::exportTimeline(project, request);
    check(!missing.success &&
              missing.error.find("Write の描画が完了していません") != std::string::npos,
          "Write の連番が無い数式 clip は書き出さない: " + missing.error);
    request.mathWriteFrames = {
        {"math", std::vector<std::filesystem::path>(sequence.begin(), sequence.begin() + 10)}};
    const auto partial = mvm::app::exportTimeline(project, request);
    check(!partial.success && partial.error.find("連番が足りません") != std::string::npos,
          "Write の連番が尺より少なければ書き出さない: " + partial.error);

    const auto plan = mvm::app::mapTimelineExportPlan(project, request);
    check(plan.success && plan.clips.size() == 2 && plan.clips[0].mathWrite &&
              plan.clips[0].timelineDurationFrames == kWriteFrames && !plan.clips[1].mathWrite &&
              plan.clips[1].timelineStartFrame == kWriteFrames &&
              plan.clips[1].timelineDurationFrames == kWriteClipFrames - kWriteFrames,
          "Write の区間 (20 frame) と静止の区間 (10 frame) に分ける");

    request.mathWriteFrames = {{"math", sequence}};
    request.outputPath = std::filesystem::path(temporary.filePath("write.mp4").toStdWString());
    const auto result = mvm::app::exportTimeline(project, request);
    check(result.success && result.frameCount == kWriteClipFrames,
          "Write 付きの数式 clip を書き出せる: " + result.error);
    const auto frames = decode(ffmpeg, request.outputPath);
    constexpr int frameBytes = kWidth * kHeight * 4;
    check(frames.size() == frameBytes * kWriteClipFrames,
          "Write: 復号した frame 数を実際に比較する");
    if (frames.size() != frameBytes * kWriteClipFrames)
        return;
    const auto frameAt = [&](int t) {
        return reinterpret_cast<const unsigned char*>(frames.constData() + t * frameBytes);
    };
    for (const int t : {0, 5, 10, 15, 19, 20, 25, 29}) {
        std::size_t compared = 0;
        const auto matched = matchingPixels(frameAt(t), t, compared);
        check(compared > 3000 && matched == compared,
              "frame " + std::to_string(t) + " は独立な期待矩形 (左 " +
                  std::to_string(120 + expectedShift(t)) + "、幅 " +
                  std::to_string(expectedColumns(t)) + ") と一致する (" + std::to_string(matched) +
                  "/" + std::to_string(compared) + ")");
    }
    // 対照: 3 frame ずれた期待値とは一致しない (比較が空振りでない)。
    std::size_t compared = 0;
    const auto shifted = matchingPixels(frameAt(10), 13, compared);
    check(compared > 3000 && compared - shifted > 300,
          "3 frame ずれた期待値とは不一致になる (" + std::to_string(compared - shifted) + " 画素)");

    // preview (GPU) と書き出しの画素分類を比べる。frame 10 は Write の frame 10。
    const auto preview = previewWriteRgba(project, staticMask, sequence[10], 10);
    const auto wrong = previewWriteRgba(project, staticMask, sequence[13], 10);
    check(preview.size() == frameBytes && wrong.size() == frameBytes,
          "Write の preview と対照の実 GPU 画素を取得した");
    if (preview.size() == frameBytes && wrong.size() == frameBytes) {
        std::size_t comparedGpu = 0;
        std::size_t matchedGpu = 0;
        std::size_t mismatchedControl = 0;
        const auto* exported = frameAt(10);
        const int left = 120 + expectedShift(10);
        for (int y = 104; y < 136; ++y)
            for (int x = 100; x < 250; ++x) {
                if (std::abs(x - left) < 3 || std::abs(x - (left + 40)) < 3 ||
                    std::abs(x - (left + 52)) < 3)
                    continue;
                const auto at = (y * kWidth + x) * 4;
                ++comparedGpu;
                matchedGpu += isRed(preview.data() + at) == isRed(exported + at);
                mismatchedControl += isRed(wrong.data() + at) != isRed(exported + at);
            }
        check(comparedGpu > 3000 && matchedGpu == comparedGpu,
              "Write の preview と書き出しの画素分類が一致する (" + std::to_string(matchedGpu) +
                  "/" + std::to_string(comparedGpu) + ")");
        // frame 13 は 52 列、frame 10 は 40 列。縁の 3 px を除いた差は 6 列 x 32 行 = 192 画素。
        check(mismatchedControl >= 150, "別の Write の frame の対照は不一致となる (" +
                                            std::to_string(mismatchedControl) + " 画素)");
    }
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
    // 拡大 150%、横へ +10%、縦へ -10%。mask の中心は (192,96)、大きさは 120x60。
    // 期待値は ClipEffects の実装を呼ばずに固定する。位置・拡大の keyframe も共通経路で評価する。
    auto moved = project;
    auto& effects = moved.timelineClips[0].effects;
    effects.scaleXPercent = effects.scaleYPercent = 150;
    effects.positionXKeys = {{0, 0}, {10, 20}};
    effects.positionYPercent = -10;
    request.outputPath =
        std::filesystem::path(temporary.filePath("math-effects.mp4").toStdWString());
    const auto transformed = mvm::app::exportTimeline(moved, request);
    check(transformed.success && transformed.frameCount == kFrames,
          "位置・拡大・keyframe を付けた数式を書き出せる: " + transformed.error);
    const auto movedFrames = decode(ffmpeg, request.outputPath);
    check(movedFrames.size() == frameBytes * kFrames, "変形した出力の frame 数を実際に比較する");
    const auto mapping = mvm::app::mapTimelinePreviewFrame(moved, 5);
    check(mapping.success && mapping.stillLayers.size() == 1 &&
              mapping.stillLayers[0].kind == mvm::project::TimelineClipKind::Math,
          "同じ数式を preview でも静止画 layer として扱う");
    if (movedFrames.size() == frameBytes * kFrames) {
        const auto* frame =
            reinterpret_cast<const unsigned char*>(movedFrames.constData() + 5 * frameBytes);
        std::size_t compared = 0;
        std::size_t red = 0;
        for (int y = 70; y < 122; ++y)
            for (int x = 136; x < 248; ++x) {
                ++compared;
                const auto* p = frame + (y * kWidth + x) * 4;
                red += p[0] > 200 && p[1] < 60 && p[2] < 60;
            }
        check(compared == 5824 && red == compared,
              "数式の変形と keyframe が独立な期待矩形の全画素と一致する");
        std::size_t black = 0;
        for (const auto [x, y] :
             {std::pair{120, 96}, std::pair{260, 96}, std::pair{192, 54}, std::pair{192, 138}}) {
            const auto* p = frame + (y * kWidth + x) * 4;
            black += p[0] < 20 && p[1] < 20 && p[2] < 20;
        }
        check(black == 4, "変形後の矩形の外は透明で、背景が見える");
        const auto preview = previewRgba(moved, request.mathArtifacts.at("math"));
        const auto shifted = previewRgba(moved, request.mathArtifacts.at("math"), -20);
        check(preview.size() == frameBytes && shifted.size() == frameBytes,
              "preview と位置を変えた対照の実 GPU 画素を取得した");
        if (preview.size() == frameBytes && shifted.size() == frameBytes) {
            std::size_t comparedGpu = 0;
            std::size_t matchedGpu = 0;
            std::size_t mismatchedControl = 0;
            for (int y = 70; y < 122; ++y)
                for (int x = 136; x < 248; ++x) {
                    const auto at = (y * kWidth + x) * 4;
                    const auto isRed = [](const unsigned char* p) {
                        return p[0] > 200 && p[1] < 60 && p[2] < 60;
                    };
                    const bool exportedRed = isRed(frame + at);
                    ++comparedGpu;
                    matchedGpu += isRed(preview.data() + at) == exportedRed;
                    mismatchedControl += isRed(shifted.data() + at) != exportedRed;
                }
            check(comparedGpu == 5824 && matchedGpu == comparedGpu,
                  "ClipEffects・keyframe 付き数式の preview と書き出しの画素分類が一致する");
            check(mismatchedControl > 2000, "位置を変えた対照は不一致となり、比較が空振りでない");
        }
    }
    testWriteExport(ffmpeg, temporary);
    mvm_mlt_runtime_shutdown();
    std::fprintf(stderr, "%d 検査中 %d 件失敗\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
