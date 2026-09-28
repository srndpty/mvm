// 文字 clip の preview と書き出しで、映像との重なり順が一致することの画素比較。
//
// preview 側は製品と同じ mapTimelinePreviewFrame / previewLayerStack で合成順を決め、
// 文字は renderTextRaster の画像を RGBA8 の静止画 layer として GPU compositor で合成する。
// 書き出し側は exportTimeline の出力を ffmpeg で 1 frame 復号する。
//
// 色は decoder / encoder / 色変換で揺れるので値の一致は見ない。文字の画素ごとに
// 「白く見えるか」を分類し、両者の分類が一致することを確かめる。
// 分類が z 順の違いを判別できていることを、順序を逆にした preview (対照) で確かめる。
#include "app/text_raster.h"
#include "app/timeline_export.h"
#include "app/timeline_preview_mapping.h"
#include "media/gpu_preview/ffmpeg_d3d11_decoder.h"
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "media/mlt/mvm_mlt_runtime.h"
#include "project/timeline_edit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

namespace {

using namespace mvm;

// 16:9 にして letterbox を作らない。帯の扱いの違いを重なり順の違いと混ぜない。
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

project::TimelineClip videoClip(const std::string& id, int track) {
    project::TimelineClip clip;
    clip.kind = project::TimelineClipKind::Video;
    clip.mediaPath = MVM_TEXT_TEST_VIDEO;
    clip.name = id;
    clip.id = id;
    clip.sourceFpsNum = 60;
    clip.sourceFpsDen = 1;
    clip.sourceFrameCount = 300;
    clip.sourceOutFrame = 30;
    clip.track = {project::TrackKind::Video, track};
    return clip;
}

project::Project makeProject(int textTrack) {
    auto project = project::createDefaultProject();
    project.outputWidth = kW;
    project.outputHeight = kH;
    project.videoTracks.push_back({"V3", false});
    project::TimelineClip text;
    text.kind = project::TimelineClipKind::Text;
    text.name = "文字";
    text.id = "text";
    text.sourceFpsNum = 60;
    text.sourceFpsDen = 1;
    text.sourceFrameCount = 30;
    text.sourceOutFrame = 30;
    text.track = {project::TrackKind::Video, textTrack};
    text.text.content = "重なり順 ABC";
    text.text.fontSize = 40;
    text.text.x = 10;
    text.text.y = 40;
    text.text.bold = true;
    project.timelineClips.push_back(text);
    int track = 0;
    for (const char* id : {"video-a", "video-b"}) {
        if (track == textTrack)
            ++track;
        project.timelineClips.push_back(videoClip(id, track++));
    }
    return project;
}

bool isWhite(int r, int g, int b) {
    return r > 215 && g > 215 && b > 215;
}

// 文字画像のうち不透明な白の画素で、上下左右 1 画素も不透明な白であるもの。
// 書き出しは 4:2:0 に圧縮されるので、文字の縁は色がにじむ。縁を比較から外し、
// 重なり順だけが見え方を決める画素に絞る。
std::vector<std::pair<int, int>> glyphPixels(const QImage& raster) {
    const auto solid = [&raster](int x, int y) {
        if (x < 0 || y < 0 || x >= raster.width() || y >= raster.height())
            return false;
        const QColor c = raster.pixelColor(x, y);
        return c.alpha() == 255 && isWhite(c.red(), c.green(), c.blue());
    };
    std::vector<std::pair<int, int>> pixels;
    for (int y = 0; y < raster.height(); ++y)
        for (int x = 0; x < raster.width(); ++x)
            if (solid(x, y) && solid(x - 1, y) && solid(x + 1, y) && solid(x, y - 1) &&
                solid(x, y + 1))
                pixels.push_back({x, y});
    return pixels;
}

std::filesystem::path exportTo(const project::Project& project, const QTemporaryDir& directory,
                               const QString& name) {
    app::TimelineExportRequest request;
    request.width = kW;
    request.height = kH;
    request.timeoutMs = 120000;
    request.outputPath =
        std::filesystem::path(directory.filePath(name + QStringLiteral(".mp4")).toStdWString());
    const auto rendered = app::exportTimeline(project, request);
    require(rendered.success && rendered.frameCount == 30,
            "書き出しに失敗しました: " + rendered.error);
    return request.outputPath;
}

// 書き出した動画の frameIndex 番目 (0 始まり) を画像にする。
QImage extractFrame(const std::filesystem::path& video, const QTemporaryDir& directory,
                    const QString& name, int frameIndex) {
    const QString png = directory.filePath(name + QStringLiteral("-%1.png").arg(frameIndex));
    QProcess decoder;
    decoder.start(QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
                  {QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"),
                   QString::fromStdWString(video.wstring()), QStringLiteral("-vf"),
                   QStringLiteral("select=eq(n\\,%1)").arg(frameIndex), QStringLiteral("-frames:v"),
                   QStringLiteral("1"), QStringLiteral("-y"), png});
    require(decoder.waitForFinished(30000) && decoder.exitCode() == 0,
            "書き出し frame を復号できません");
    QImage frame(png);
    require(!frame.isNull() && frame.width() == kW && frame.height() == kH,
            "書き出し frame の寸法が違います");
    return frame.convertToFormat(QImage::Format_RGBA8888);
}

QImage exportFrame(const project::Project& project, const QTemporaryDir& directory,
                   const QString& name) {
    return extractFrame(exportTo(project, directory, name), directory, name, 0);
}

struct Preview {
    OwnedDevice device;
    gpu::ReadbackCounters readbacks;
    gpu::GpuCompositor compositor;
    std::unique_ptr<gpu::FFmpegD3D11Decoder> decoder;
    gpu::DecodedGpuFrame video;
};

// 素材の frameIndex 番目を GPU へ復号する。書き出しと同じ frame の映像で比べるため。
void decodeVideo(Preview& preview, long long frameIndex) {
    std::string err;
    preview.video = {};
    preview.decoder =
        std::make_unique<gpu::FFmpegD3D11Decoder>(preview.device.shared, gpu::SourceId{1});
    require(preview.decoder->open(MVM_TEXT_TEST_VIDEO, err), "素材を開けません: " + err);
    for (int attempt = 0; attempt < 600; ++attempt) {
        const auto status = preview.decoder->requestFrame(preview.video, err);
        if (status == gpu::DecodeStatus::Ok && preview.video.frameNumber == frameIndex)
            return;
        require(status == gpu::DecodeStatus::Ok || status == gpu::DecodeStatus::Again,
                "素材の frame を復号できません: " + err);
    }
    failNow("素材の frame " + std::to_string(frameIndex) + " が出てきません");
}

void openPreview(Preview& preview) {
    std::string err;
    require(preview.device.create(err), err);
    require(preview.compositor.initialize(preview.device.shared, preview.readbacks, kW, kH, err),
            err);
    decodeVideo(preview, 0);
}

// 製品の合成順どおりに 1 frame を合成し、kW x kH の RGBA を返す。
// flipText が true なら文字を反対の端 (最前面なら最背面、それ以外なら最前面) へ動かす (対照)。
// ignoreOpacity が true なら文字の不透明度を 1 にする (対照)。
std::vector<unsigned char> composePreview(Preview& preview, const project::Project& project,
                                          const gpu::DecodedGpuFrame& still, bool flipText,
                                          std::int64_t outputFrame = 0,
                                          bool ignoreOpacity = false) {
    const auto mapped = app::mapTimelinePreviewFrame(project, outputFrame);
    require(mapped.success && mapped.layers.size() == 2 && mapped.textLayers.size() == 1,
            "preview の対応づけが映像 2 本と文字 1 枚になりません");
    auto stack = app::previewLayerStack(mapped);
    if (flipText) {
        const auto text =
            std::find_if(stack.begin(), stack.end(), [](const auto& entry) { return entry.text; });
        const auto moved = *text;
        const bool wasTop = text + 1 == stack.end();
        stack.erase(text);
        stack.insert(wasTop ? stack.begin() : stack.end(), moved);
    }
    gpu::ComposedFrame frame;
    frame.outputFrameNumber = outputFrame;
    frame.compositionEpoch = {1};
    int z = 0;
    for (const auto& entry : stack) {
        gpu::CompositionLayerFrame layer;
        // 同じ素材を 2 layer に使う。z 順以外の差を作らない。
        layer.frame = entry.text ? still : preview.video;
        // 文字の不透明度は製品と同じく対応づけの評価値 (値・key・fade) を使う。
        if (entry.text && !ignoreOpacity)
            layer.opacity = static_cast<float>(mapped.textLayers[entry.index].opacity);
        layer.zOrder = z++;
        frame.layers.push_back(layer);
    }
    std::string err;
    // compose() は 2 layer 専用なので、3 layer は専用の target へ描く。
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    require(SUCCEEDED(preview.device.device->CreateTexture2D(&td, nullptr, &texture)) &&
                SUCCEEDED(preview.device.device->CreateRenderTargetView(texture, nullptr, &rtv)),
            "合成先を作れません");
    {
        std::lock_guard<gpu::D3D11Lock> guard(preview.device.shared.lock());
        const float black[4] = {0, 0, 0, 1};
        preview.device.context->ClearRenderTargetView(rtv, black);
    }
    const bool composed = preview.compositor.composeLayersToTarget(frame, {rtv, kW, kH}, 3, err);
    std::vector<unsigned char> rgba;
    // 小領域 readback の上限に収まるよう、行の帯に分けて読む。
    bool read = composed;
    for (int y = 0; read && y < kH; y += 60) {
        std::vector<unsigned char> band;
        read = preview.compositor.readExternalOutputProbe(texture, 0, y, kW, 60, band, err);
        rgba.insert(rgba.end(), band.begin(), band.end());
    }
    rtv->Release();
    texture->Release();
    require(composed && read && rgba.size() == static_cast<std::size_t>(kW) * kH * 4,
            "preview を合成できません: " + err);
    return rgba;
}

struct Agreement {
    int compared = 0;
    int agreed = 0;
    int previewWhite = 0;
    int exportWhite = 0;
};

Agreement compare(const std::vector<std::pair<int, int>>& glyphs,
                  const std::vector<unsigned char>& preview, const QImage& exported) {
    Agreement result;
    for (const auto& [x, y] : glyphs) {
        const unsigned char* p =
            &preview[(static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4];
        const QColor e = exported.pixelColor(x, y);
        const bool previewIsWhite = isWhite(p[0], p[1], p[2]);
        const bool exportIsWhite = isWhite(e.red(), e.green(), e.blue());
        ++result.compared;
        result.agreed += previewIsWhite == exportIsWhite ? 1 : 0;
        result.previewWhite += previewIsWhite ? 1 : 0;
        result.exportWhite += exportIsWhite ? 1 : 0;
    }
    return result;
}

// 文字の画素の平均輝度 (preview, 書き出し)。半透明の frame で濃さを比べる。
std::pair<double, double> meanLuma(const std::vector<std::pair<int, int>>& glyphs,
                                   const std::vector<unsigned char>& preview,
                                   const QImage& exported) {
    double previewSum = 0.0;
    double exportSum = 0.0;
    for (const auto& [x, y] : glyphs) {
        const unsigned char* p =
            &preview[(static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)) * 4];
        const QColor e = exported.pixelColor(x, y);
        previewSum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        exportSum += 0.2126 * e.red() + 0.7152 * e.green() + 0.0722 * e.blue();
    }
    const double count = static_cast<double>(glyphs.size());
    return {previewSum / count, exportSum / count};
}

std::string describe(const Agreement& a) {
    return "比較 " + std::to_string(a.compared) + " / 一致 " + std::to_string(a.agreed) +
           " / preview 白 " + std::to_string(a.previewWhite) + " / 書き出し白 " +
           std::to_string(a.exportWhite);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    require(mvm_mlt_runtime_init(MVM_MLT_MODULE_DIR, MVM_MLT_DATA_DIR) == 0,
            "MLT runtime を初期化できません");
    QTemporaryDir directory;
    require(directory.isValid(), "一時 directory を作れません");

    Preview preview;
    openPreview(preview);

    // case 0: V1 映像 / V2 文字 / V3 映像。V3 の映像が文字を隠す。
    // case 1: V1 映像 / V2 映像 / V3 文字。文字が最前面に出る。
    for (int textTrack : {1, 2}) {
        const auto project = makeProject(textTrack);
        require(project::validateTimeline(project).success, "検査用 Project が不正です");
        QString rasterError;
        const QImage raster =
            app::renderTextRaster(project.timelineClips[0].text, kW, kH, rasterError)
                .convertToFormat(QImage::Format_RGBA8888);
        require(!raster.isNull(), "文字画像を作れません: " + rasterError.toStdString());
        const auto glyphs = glyphPixels(raster);
        require(glyphs.size() > 200, "比較できる文字の画素が足りません");

        std::vector<unsigned char> pixels(static_cast<std::size_t>(kW) * kH * 4);
        for (int y = 0; y < kH; ++y)
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * kW * 4,
                        raster.constScanLine(y), static_cast<std::size_t>(kW) * 4);
        gpu::DecodedGpuFrame still;
        std::string err;
        require(gpu::makeStillImageFrame(preview.device.shared, kW, kH, pixels.data(),
                                         pixels.size(), gpu::SourceId{}, still, err),
                err);

        const QImage exported =
            exportFrame(project, directory, QStringLiteral("case%1").arg(textTrack));
        const Agreement ordered =
            compare(glyphs, composePreview(preview, project, still, false), exported);
        const Agreement flipped =
            compare(glyphs, composePreview(preview, project, still, true), exported);
        std::printf("V%d 文字: 製品順 %s\n", textTrack + 1, describe(ordered).c_str());
        std::printf("V%d 文字: 文字を反対端(対照) %s\n", textTrack + 1, describe(flipped).c_str());

        const bool textOnTop = textTrack == 2;
        // 書き出しが期待どおりの重なり順であること。ここが崩れると比較の前提が無い。
        require(textOnTop ? ordered.exportWhite * 10 >= ordered.compared * 9
                          : ordered.exportWhite * 10 <= ordered.compared,
                "書き出しの文字の見え方が track 順と合いません: " + describe(ordered));
        require(ordered.agreed * 100 >= ordered.compared * 95,
                "preview と書き出しで文字の見え方が一致しません: " + describe(ordered));
        // 対照: 順序を逆にすると不一致になること。比較が z 順を判別している証拠。
        require(flipped.agreed * 2 <= flipped.compared,
                "対照: 文字を反対の端へ動かしても一致しました。比較が重なり順を判別していません: " +
                    describe(flipped));
        preview.compositor.retireLayerTexture(still.texture);
    }
    // case 2: V3 文字の不透明度を key で 100% (frame 0) -> 0% (frame 15) にする (Pen の
    // automation)。
    //         preview と書き出しで、各 frame の文字の見え方と濃さが一致すること。
    {
        auto project = makeProject(2);
        project.timelineClips[0].effects.opacityKeys = {{0, 100.0}, {15, 0.0}};
        require(project::validateTimeline(project).success, "opacity 付きの Project が不正です");
        QString rasterError;
        const QImage raster =
            app::renderTextRaster(project.timelineClips[0].text, kW, kH, rasterError)
                .convertToFormat(QImage::Format_RGBA8888);
        require(!raster.isNull(), "文字画像を作れません: " + rasterError.toStdString());
        const auto glyphs = glyphPixels(raster);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(kW) * kH * 4);
        for (int y = 0; y < kH; ++y)
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * kW * 4,
                        raster.constScanLine(y), static_cast<std::size_t>(kW) * 4);
        gpu::DecodedGpuFrame still;
        std::string err;
        require(gpu::makeStillImageFrame(preview.device.shared, kW, kH, pixels.data(),
                                         pixels.size(), gpu::SourceId{}, still, err),
                err);
        const auto video = exportTo(project, directory, QStringLiteral("opacity"));
        for (const int frameIndex : {0, 8, 15}) {
            decodeVideo(preview, frameIndex);
            const QImage exported =
                extractFrame(video, directory, QStringLiteral("opacity"), frameIndex);
            const auto rendered = composePreview(preview, project, still, false, frameIndex);
            const Agreement agreement = compare(glyphs, rendered, exported);
            const auto [previewLuma, exportLuma] = meanLuma(glyphs, rendered, exported);
            std::printf("opacity frame %d: %s / 平均輝度 preview %.1f 書き出し %.1f\n", frameIndex,
                        describe(agreement).c_str(), previewLuma, exportLuma);
            require(agreement.agreed * 100 >= agreement.compared * 95,
                    "opacity key 付き文字の見え方が preview と書き出しで一致しません: frame " +
                        std::to_string(frameIndex));
            require(std::abs(previewLuma - exportLuma) <= 12.0,
                    "opacity key 付き文字の濃さが preview と書き出しで一致しません: frame " +
                        std::to_string(frameIndex));
            if (frameIndex == 0)
                require(agreement.exportWhite * 10 >= agreement.compared * 9,
                        "前提: frame 0 の書き出しで文字が見えません");
            if (frameIndex == 15) {
                require(agreement.exportWhite * 10 <= agreement.compared,
                        "前提: frame 15 の書き出しで文字が消えていません");
                // 対照: 不透明度を無視した preview は書き出しと食い違う。
                const Agreement ignored = compare(
                    glyphs, composePreview(preview, project, still, false, frameIndex, true),
                    exported);
                std::printf("opacity frame 15 (不透明度を無視した対照): %s\n",
                            describe(ignored).c_str());
                require(ignored.agreed * 2 <= ignored.compared,
                        "対照: 不透明度を無視しても一致しました。比較が不透明度を判別していません");
            }
        }
        preview.compositor.retireLayerTexture(still.texture);
    }

    preview.decoder->close();
    preview.video = {};
    std::string err;
    require(preview.compositor.shutdown(5000, err), err);
    mvm_mlt_runtime_shutdown();
    std::puts("文字と映像の重なり順が preview と書き出しで一致することを確認しました");
    return 0;
}
