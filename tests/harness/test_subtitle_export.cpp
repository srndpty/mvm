#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/subtitles.h"

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

QByteArray decode(const std::filesystem::path& path) {
    QProcess process;
    process.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {"-v", "error", "-i", QString::fromStdWString(path.wstring()), "-f", "rawvideo",
                   "-pix_fmt", "rgba", "-"});
    require(process.waitForFinished(30000) && process.exitCode() == 0, "動画の復号");
    return process.readAllStandardOutput();
}

std::vector<unsigned char> previewPixels(const QImage& raster) {
    struct Device {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        mvm::gpu::SharedD3D11Device shared;

        ~Device() {
            shared.release();
            if (context)
                context->Release();
            if (device)
                device->Release();
        }
    } device;

    D3D_FEATURE_LEVEL level{};
    require(SUCCEEDED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &device.device, &level, &device.context)),
            "プレビュー用D3D11生成");
    std::string error;
    require(device.shared.adopt(device.device, device.context, error), "共有デバイス生成");
    mvm::gpu::ReadbackCounters counters;
    mvm::gpu::GpuCompositor compositor;
    require(compositor.initialize(device.shared, counters, 320, 240, error),
            "製品GPU compositor初期化");
    const auto straight = raster.convertToFormat(QImage::Format_RGBA8888);
    mvm::gpu::DecodedGpuFrame still;
    require(mvm::gpu::makeStillImageFrame(device.shared, 320, 240, straight.constBits(),
                                          static_cast<std::size_t>(straight.sizeInBytes()), {1},
                                          still, error),
            "製品静止画レイヤー生成");
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 320;
    description.Height = 240;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    require(SUCCEEDED(device.device->CreateTexture2D(&description, nullptr, &texture)) &&
                SUCCEEDED(device.device->CreateRenderTargetView(texture, nullptr, &target)),
            "プレビュー合成先");
    mvm::gpu::ComposedFrame frame;
    frame.outputFrameNumber = 5;
    frame.compositionEpoch = {1};
    mvm::gpu::CompositionLayerFrame layer;
    layer.frame = still;
    layer.zOrder = 0;
    frame.layers.push_back(layer);
    require(compositor.composeLayersToTarget(frame, {target, 320, 240}, 1, error), "字幕GPU合成");
    std::vector<unsigned char> pixels;
    for (int y = 0; y < 240; y += 60) {
        std::vector<unsigned char> band;
        require(compositor.readExternalOutputProbe(texture, 0, y, 320, 60, band, error),
                "プレビュー画素の取得");
        pixels.insert(pixels.end(), band.begin(), band.end());
    }
    compositor.retireLayerTexture(still.texture);
    require(compositor.shutdown(10000, error), "GPU完了待ち");
    target->Release();
    texture->Release();
    return pixels;
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0, "MLT初期化");
    QTemporaryDir temporary;
    require(temporary.isValid(), "作業フォルダー");
    auto project = mvm::project::createDefaultProject();
    project.outputWidth = 320;
    project.outputHeight = 240;
    project.subtitles.emplace();
    project.subtitles->style.fontSize = 28;
    project.subtitles->cues = {{"cue", 5, 15, "日本語\n字幕", {}}};
    mvm::app::TimelineExportRequest request;
    request.width = 320;
    request.height = 240;
    request.videoCrf = 10;
    request.outputPath = std::filesystem::path(temporary.filePath("subtitles.mp4").toStdWString());
    auto result = mvm::app::exportTimeline(project, request);
    if (!result.success)
        std::fprintf(stderr, "%s\n", result.error.c_str());
    require(result.success && result.frameCount == 15, "字幕だけの動画書き出し");
    const auto frames = decode(request.outputPath);
    constexpr int frameBytes = 320 * 240 * 4;
    require(frames.size() == frameBytes * 15, "復号フレーム数を実際に比較");
    QString error;
    const auto raster = mvm::app::renderSubtitleRaster(project.subtitles->cues.front(),
                                                       project.subtitles->style, 320, 240, error);
    require(!raster.isNull(), "共通ラスタの描画");
    const auto preview = previewPixels(raster);
    require(preview.size() == static_cast<std::size_t>(frameBytes), "実プレビューの比較画素数");
    std::size_t white = 0, agreed = 0;
    for (int i = 0; i < frameBytes; i += 4)
        if (preview[static_cast<std::size_t>(i)] > 220) {
            ++white;
            if (static_cast<unsigned char>(frames[5 * frameBytes + i]) > 180)
                ++agreed;
        }
    require(white > 100 && agreed * 100 >= white * 98, "実GPUプレビューと焼き込みの字幕画素を比較");
    for (int frame : {4, 5, 14}) {
        std::size_t compared = 0, bright = 0;
        double difference = 0;
        const auto* actual =
            reinterpret_cast<const unsigned char*>(frames.constData() + frame * frameBytes);
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x) {
                const int i = (y * 320 + x) * 4;
                if (actual[i] > 80 || actual[i + 1] > 80 || actual[i + 2] > 80)
                    ++bright;
                const auto expected = raster.pixelColor(x, y);
                if (expected.alpha() < 240 || expected.red() < 80)
                    continue;
                difference += std::abs(expected.red() - actual[i]) +
                              std::abs(expected.green() - actual[i + 1]) +
                              std::abs(expected.blue() - actual[i + 2]);
                ++compared;
            }
        if (frame < 5)
            require(bright == 0, "開始直前には字幕を表示しない");
        else
            require(bright > 100 && compared > 100 &&
                        difference / (3.0 * static_cast<double>(compared)) < 25,
                    "表示区間内のラスタと書き出し画素を比較");
    }
    // 終端後の黒を観測するため、透明文字でタイムラインの尺だけを延ばす。
    mvm::project::TimelineClip tail;
    tail.id = "tail";
    tail.name = "透明";
    tail.kind = mvm::project::TimelineClipKind::Text;
    tail.sourceFpsNum = 60;
    tail.sourceFrameCount = tail.sourceOutFrame = 20;
    tail.text.content = "透明";
    tail.text.color = "#00000000";
    project.timelineClips.push_back(tail);
    request.outputPath = std::filesystem::path(temporary.filePath("boundary.mp4").toStdWString());
    result = mvm::app::exportTimeline(project, request);
    if (!result.success)
        std::fprintf(stderr, "%s\n", result.error.c_str());
    require(result.success && result.frameCount == 20, "終了境界の動画");
    const auto boundary = decode(request.outputPath);
    require(boundary.size() == frameBytes * 20, "終了境界の復号件数");
    for (int frame : {15, 16})
        for (int i = 0; i < frameBytes; i += 4)
            require(static_cast<unsigned char>(boundary[frame * frameBytes + i]) < 20,
                    "終了以降には字幕を表示しない");
    project.timelineClips.clear();
    request.burnSubtitles = false;
    request.outputPath =
        std::filesystem::path(temporary.filePath("no-subtitles.mp4").toStdWString());
    result = mvm::app::exportTimeline(project, request);
    require(result.success && result.frameCount == 15, "焼き込みオフでも字幕の尺を保持");
    const auto black = decode(request.outputPath);
    require(black.size() == frameBytes * 15, "焼き込みオフの復号件数");
    for (int i = 0; i < black.size(); i += 4)
        require(static_cast<unsigned char>(black[i]) < 20, "焼き込みオフは黒背景");
    mvm_mlt_runtime_shutdown();
    std::puts("字幕の開始・終了境界と焼き込み画素の検査に合格しました");
}
