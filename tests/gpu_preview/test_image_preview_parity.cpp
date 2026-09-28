// 画像 clip の preview と書き出しで、effect (位置・拡大・回転・crop・不透明度) の効き方が
// 一致することの画素比較。
//
// preview 側は製品と同じ mapTimelinePreviewFrame / applyPreviewLayerEffects で layer を作り、
// 静止画 decoder の raster (decode -> 向き -> 出力解像度へ配置) を RGBA8 の静止画 layer として
// GPU compositor で合成する。video layer の無い composition である (画像だけの区間)。
// 書き出し側は exportTimeline の出力を ffmpeg で 1 frame 復号する。
//
// 色は encoder / 色変換で揺れるので値の一致は見ない。画素ごとに赤・青・黒 (その他) へ
// 分類し、境界の画素を除いて分類が一致することを確かめる。
// 分類が配置の違いを判別できていることを、preview だけ位置をずらした対照で確かめる。
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "media/still_image/still_image_decoder.h"
#include "project/timeline_edit.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

namespace {

using namespace mvm;
namespace fs = std::filesystem;

constexpr int kW = 320;
constexpr int kH = 180;

[[noreturn]] void failNow(const std::string& text) {
    std::fprintf(stderr, "FAIL: %s\n", text.c_str());
    std::exit(3);
}

void require(bool condition, const std::string& text) {
    if (!condition)
        failNow(text);
}

class OwnedDevice {
public:
    ~OwnedDevice() {
        shared.release();
        if (context)
            context->Release();
        if (device)
            device->Release();
    }

    bool create(std::string& err) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        const HRESULT rc =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device, &selected, &context);
        if (FAILED(rc)) {
            err = "D3D11 hardware deviceを生成できません";
            return false;
        }
        return shared.adopt(device, context, err);
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    gpu::SharedD3D11Device shared;
};

// 64x32、左上 32x16 が赤で残りが青。縦横比が出力 (16:9) と違うので上下に余白ができる。
project::Project makeProject(const fs::path& image) {
    auto project = project::createDefaultProject();
    project.outputWidth = kW;
    project.outputHeight = kH;
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Image;
    clip.mediaPath = image;
    clip.name = "画像";
    clip.id = "image";
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 30;
    clip.sourceOutFrame = 30;
    clip.track = {project::TrackKind::Video, 0};
    clip.effects.positionXPercent = 12.0;
    clip.effects.positionYPercent = -8.0;
    clip.effects.scalePercent = 70.0;
    clip.effects.rotationDegrees = 15.0;
    clip.effects.cropRightPercent = 10.0;
    clip.effects.opacityPercent = 100.0;
    project.timelineClips.push_back(clip);
    return project;
}

enum class Kind { Red, Blue, Black, Other };

Kind classify(int r, int g, int b) {
    if (r > 150 && g < 90 && b < 90)
        return Kind::Red;
    if (b > 150 && r < 90 && g < 90)
        return Kind::Blue;
    if (r < 40 && g < 40 && b < 40)
        return Kind::Black;
    return Kind::Other;
}

struct Agreement {
    int compared = 0;
    int agreed = 0;
    int coloured = 0; // 書き出しで赤か青だった画素 (画像が見えている範囲)
};

// 境界 (近傍 2 px に別の分類がある画素) は拡縮・回転の sampling で揺れるので除く。
Agreement compare(const std::vector<Kind>& preview, const std::vector<Kind>& exported) {
    Agreement result;
    const auto at = [](const std::vector<Kind>& kinds, int x, int y) {
        return kinds[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
    };
    for (int y = 2; y < kH - 2; ++y) {
        for (int x = 2; x < kW - 2; ++x) {
            const Kind center = at(exported, x, y);
            bool interior = center != Kind::Other;
            for (int dy = -2; interior && dy <= 2; ++dy)
                for (int dx = -2; interior && dx <= 2; ++dx)
                    interior = at(exported, x + dx, y + dy) == center;
            if (!interior)
                continue;
            ++result.compared;
            result.agreed += at(preview, x, y) == center ? 1 : 0;
            result.coloured += center == Kind::Red || center == Kind::Blue ? 1 : 0;
        }
    }
    return result;
}

std::string describe(const Agreement& a) {
    return "比較 " + std::to_string(a.compared) + " / 一致 " + std::to_string(a.agreed) +
           " / 画像の画素 " + std::to_string(a.coloured);
}

// 製品と同じ経路で layer を作り、GPU compositor で合成して分類する。
// shiftX は対照用に preview だけ位置をずらす量 (%)。
std::vector<Kind> composePreview(OwnedDevice& device, gpu::GpuCompositor& compositor,
                                 const project::Project& project, const gpu::DecodedGpuFrame& still,
                                 double shiftX) {
    const auto mapped = app::mapTimelinePreviewFrame(project, 0);
    require(mapped.success && mapped.layers.empty() && mapped.stillLayers.size() == 1 &&
                mapped.stillLayers[0].kind == project::TimelineClipKind::Image,
            "preview の対応づけが画像 1 枚になりません");
    auto effects = project.timelineClips[0].effects;
    effects.positionXPercent += shiftX;
    preview::PreviewCompositionLayer layer;
    app::applyPreviewLayerEffects(layer, effects, mapped.stillLayers[0].opacity, 0, 30);

    gpu::CompositionLayerFrame frameLayer;
    frameLayer.frame = still;
    frameLayer.destination = {layer.destination.x, layer.destination.y, layer.destination.width,
                              layer.destination.height};
    frameLayer.sourceUv = {layer.sourceRect.x, layer.sourceRect.y, layer.sourceRect.width,
                           layer.sourceRect.height};
    frameLayer.opacity = layer.opacity;
    frameLayer.effectsEnabled = layer.effectsEnabled;
    frameLayer.rotationDegrees = layer.rotationDegrees;
    gpu::ComposedFrame frame;
    frame.compositionEpoch = {1};
    frame.layers.push_back(frameLayer);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    require(SUCCEEDED(device.device->CreateTexture2D(&td, nullptr, &texture)) &&
                SUCCEEDED(device.device->CreateRenderTargetView(texture, nullptr, &rtv)),
            "合成先を作れません");
    {
        std::lock_guard<gpu::D3D11Lock> guard(device.shared.lock());
        const float black[4] = {0, 0, 0, 1};
        device.context->ClearRenderTargetView(rtv, black);
    }
    std::string err;
    const bool composed = compositor.composeLayersToTarget(frame, {rtv, kW, kH}, 1, err);
    std::vector<unsigned char> rgba;
    bool read = composed;
    for (int y = 0; read && y < kH; y += 60) {
        std::vector<unsigned char> band;
        read = compositor.readExternalOutputProbe(texture, 0, y, kW, 60, band, err);
        rgba.insert(rgba.end(), band.begin(), band.end());
    }
    rtv->Release();
    texture->Release();
    require(composed && read && rgba.size() == static_cast<std::size_t>(kW) * kH * 4,
            "preview を合成できません: " + err);
    // MVM_PARITY_DUMP に directory を渡すと、比較した画像を保存する (食い違いの調査用)。
    if (const char* dump = std::getenv("MVM_PARITY_DUMP")) {
        QImage(rgba.data(), kW, kH, kW * 4, QImage::Format_RGBA8888)
            .save(QString::fromUtf8(dump) + QStringLiteral("/preview-%1.png").arg(shiftX));
    }
    std::vector<Kind> kinds;
    for (std::size_t i = 0; i < rgba.size(); i += 4)
        kinds.push_back(classify(rgba[i], rgba[i + 1], rgba[i + 2]));
    return kinds;
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_image_preview_parity <画像>\n");
        return 2;
    }
    const fs::path image = fs::absolute(fs::path(reinterpret_cast<const char8_t*>(argv[1])));
    const auto project = makeProject(image);
    require(project::validateTimeline(project).success, "前提: 画像 clip の Project が不正です");

    // 書き出し。
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
            "MLT runtime を初期化できません");
    QTemporaryDir directory;
    require(directory.isValid(), "一時 directory を作れません");
    app::TimelineExportRequest request;
    request.width = kW;
    request.height = kH;
    request.timeoutMs = 120000;
    request.outputPath = fs::path(directory.filePath(QStringLiteral("image.mp4")).toStdWString());
    const auto rendered = app::exportTimeline(project, request);
    require(rendered.success, "画像 clip を書き出せません: " + rendered.error);
    const QString framePath = directory.filePath(QStringLiteral("frame.png"));
    QProcess decoder;
    decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"),
                   QString::fromStdWString(request.outputPath.wstring()),
                   QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-y"),
                   framePath});
    require(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
            "書き出し frame を復号できません");
    const QImage exportedImage(framePath);
    if (const char* dump = std::getenv("MVM_PARITY_DUMP"))
        exportedImage.save(QString::fromUtf8(dump) + QStringLiteral("/exported.png"));
    require(exportedImage.width() == kW && exportedImage.height() == kH,
            "書き出し frame の寸法が違います");
    std::vector<Kind> exported;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const QColor c = exportedImage.pixelColor(x, y);
            exported.push_back(classify(c.red(), c.green(), c.blue()));
        }

    // preview。書き出しと同じ decoder と配置で raster を作る。
    const auto decoded = media::decodeStillImage(image);
    require(decoded.success, "画像を decode できません: " + decoded.error);
    const auto fitted = media::fitStillImageToRaster(decoded.image, kW, kH);
    require(fitted.success, "画像を配置できません: " + fitted.error);
    OwnedDevice device;
    std::string err;
    require(device.create(err), err);
    gpu::ReadbackCounters readbacks;
    gpu::GpuCompositor compositor;
    require(compositor.initialize(device.shared, readbacks, kW, kH, err), err);
    gpu::DecodedGpuFrame still;
    require(gpu::makeStillImageFrame(device.shared, kW, kH, fitted.raster.rgba.data(),
                                     fitted.raster.rgba.size(), {200}, still, err),
            err);

    const Agreement same =
        compare(composePreview(device, compositor, project, still, 0.0), exported);
    const Agreement shifted =
        compare(composePreview(device, compositor, project, still, 25.0), exported);
    std::printf("同じ effect: %s\n位置をずらした対照: %s\n", describe(same).c_str(),
                describe(shifted).c_str());
    // 空振りしていないこと: 画像の画素を十分に比べている。
    require(same.coloured > 2000, "画像の画素がほとんど比較されていません: " + describe(same));
    require(same.agreed * 100 >= same.compared * 97,
            "preview と書き出しで画像の配置が一致しません: " + describe(same));
    require(shifted.agreed * 100 <= shifted.compared * 85,
            "対照: 位置をずらしても一致したままです (比較が配置を判別していません): " +
                describe(shifted));
    compositor.retireLayerTexture(still.texture);
    still = {};
    require(compositor.shutdown(5000, err), err);
    // MLT の shutdown は最後にする。shutdown の後に FFmpeg を使うと avformat の中で落ちた
    // (MLT が登録した log callback が unload 後も残るため、と推測している)。
    mvm_mlt_runtime_shutdown();
    std::puts("画像 clip の effect (位置・拡大・回転・crop) が preview と書き出しで一致しました");
    return 0;
}
