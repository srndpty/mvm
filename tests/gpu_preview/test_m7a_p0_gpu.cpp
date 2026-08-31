// M7a-P0: 既存GPU compositor primitiveだけでcrop/配置/opacityを実画素検証する。

#include "core/clip_fade.h"
#include "media/gpu_preview/gpu_compositor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace mvm::gpu;

struct OwnedDevice {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    SharedD3D11Device shared;

    ~OwnedDevice() {
        shared.release();
        if (context)
            context->Release();
        if (device)
            device->Release();
    }

    bool create(std::string& error) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        const HRESULT result =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                              D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              levels, 2, D3D11_SDK_VERSION, &device, &selected, &context);
        if (FAILED(result)) {
            error = "D3D11 hardware deviceを作れません";
            return false;
        }
        return shared.adopt(device, context, error);
    }
};

struct OwnedTexture {
    ID3D11Texture2D* texture = nullptr;

    ~OwnedTexture() {
        if (texture)
            texture->Release();
    }
};

struct Metrics {
    int foregroundPixels = 0;
    int minX = 64;
    int minY = 64;
    int maxX = -1;
    int maxY = -1;
    double meanRgb = 0.0;
};

bool makeFixture(ID3D11Device* device, OwnedTexture& output, std::string& error) {
    constexpr int width = 16;
    constexpr int height = 16;
    std::vector<unsigned char> data(width * height * 3 / 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            data[static_cast<std::size_t>(y * width + x)] = x < width / 2 ? 81 : 41;
    }
    unsigned char* uv = data.data() + width * height;
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width; x += 2) {
            const bool left = x < width / 2;
            uv[y * width + x] = left ? 90 : 240;
            uv[y * width + x + 1] = left ? 240 : 110;
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data.data(), width, static_cast<UINT>(data.size())};
    if (FAILED(device->CreateTexture2D(&description, &initial, &output.texture))) {
        error = "M7a-P0 NV12 fixtureを作れません";
        return false;
    }
    return true;
}

bool makeBlackFixture(ID3D11Device* device, OwnedTexture& output, std::string& error) {
    constexpr int width = 16;
    constexpr int height = 16;
    std::vector<unsigned char> data(width * height * 3 / 2, 128);
    std::fill_n(data.begin(), width * height, static_cast<unsigned char>(16));
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{data.data(), width, static_cast<UINT>(data.size())};
    if (FAILED(device->CreateTexture2D(&description, &initial, &output.texture))) {
        error = "M7a-P0黒背景textureを作れません";
        return false;
    }
    return true;
}

DecodedGpuFrame frameFor(OwnedTexture& texture, long long sourceFrame,
                         unsigned long long sourceId) {
    DecodedGpuFrame frame;
    frame.frameNumber = sourceFrame;
    frame.pts = sourceFrame;
    frame.timeBase = {1, 60};
    frame.width = 16;
    frame.height = 16;
    frame.pixelFormat = GpuPixelFormat::NV12;
    frame.texture = texture.texture;
    frame.colorSpace = ColorSpace::BT709;
    frame.colorRange = ColorRange::Limited;
    frame.sourceId = {sourceId};
    frame.sourceGeneration = {1};
    frame.resourceEpoch = {1};
    frame.lifetime = std::make_shared<int>(1);
    return frame;
}

Metrics measure(GpuCompositor& compositor, std::string& error) {
    std::vector<unsigned char> pixels;
    Metrics result;
    if (!compositor.readOutputProbe(0, 0, 64, 64, pixels, error))
        return result;
    double sum = 0.0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)) * 4;
            const int red = pixels[offset];
            const int green = pixels[offset + 1];
            const int blue = pixels[offset + 2];
            sum += red + green + blue;
            if (red + green + blue > 30) {
                ++result.foregroundPixels;
                result.minX = std::min(result.minX, x);
                result.minY = std::min(result.minY, y);
                result.maxX = std::max(result.maxX, x);
                result.maxY = std::max(result.maxY, y);
            }
        }
    }
    result.meanRgb = sum / (64.0 * 64.0 * 3.0);
    return result;
}

bool samplesPresent(const std::vector<Metrics>& samples) {
    return !samples.empty() &&
           std::all_of(samples.begin(), samples.end(),
                       [](const Metrics& value) { return value.foregroundPixels > 0; });
}

void writeMetrics(std::ostream& output, const char* name, const Metrics& metrics) {
    output << "    {\"name\":\"" << name << "\",\"foreground_pixels\":" << metrics.foregroundPixels
           << ",\"bbox\":[" << metrics.minX << ',' << metrics.minY << ',' << metrics.maxX << ','
           << metrics.maxY << "],\"mean_rgb\":" << metrics.meanRgb << '}';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--negative-empty")
        return samplesPresent({}) ? 1 : 7;
    if (argc != 2) {
        std::fprintf(stderr, "使い方: mvm_test_m7a_p0_gpu <raw.json>\n");
        return 2;
    }

    std::string error;
    OwnedDevice device;
    OwnedTexture texture;
    OwnedTexture blackTexture;
    if (!device.create(error) || !makeFixture(device.device, texture, error) ||
        !makeBlackFixture(device.device, blackTexture, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    ReadbackCounters readbacks;
    GpuCompositor compositor;
    if (!compositor.initialize(device.shared, readbacks, 64, 64, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }

    const long long decodedSourceFrame = 110;
    const long long sourceInFrame = 100;
    const long long previewOutputOrdinal = 2;
    const long long localFrame = decodedSourceFrame - sourceInFrame;
    const double fade = mvm::core::clipFadeFactor(localFrame, 20, 12, 0);
    const double wrongFade = mvm::core::clipFadeFactor(previewOutputOrdinal, 20, 12, 0);
    if (std::abs(fade - wrongFade) < 1e-12) {
        std::fprintf(stderr, "FAIL: clip-local source authority testがordinalと区別できません\n");
        return 1;
    }

    const DecodedGpuFrame source = frameFor(texture, decodedSourceFrame, 71);
    const DecodedGpuFrame black = frameFor(blackTexture, decodedSourceFrame, 70);
    auto compose = [&](RectF destination, RectF sourceUv, float opacity, Metrics& metrics) {
        ComposedFrame frame{decodedSourceFrame,
                            {1},
                            {{black, {0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F, 0},
                             {source, destination, sourceUv, opacity, 1}},
                            {1}};
        if (!compositor.compose(frame, error))
            return false;
        metrics = measure(compositor, error);
        return error.empty();
    };

    Metrics baseline;
    Metrics croppedAndPlaced;
    Metrics faded;
    if (!compose({0, 0, 1, 1}, {0, 0, 1, 1}, 1.0F, baseline) ||
        !compose({0.25F, 0.25F, 0.5F, 0.5F}, {0.25F, 0.0F, 0.5F, 1.0F}, 1.0F, croppedAndPlaced) ||
        !compose({0.25F, 0.25F, 0.5F, 0.5F}, {0.25F, 0.0F, 0.5F, 1.0F},
                 static_cast<float>(0.8 * fade), faded)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    const std::vector<Metrics> samples{baseline, croppedAndPlaced, faded};
    if (!samplesPresent(samples) || croppedAndPlaced.minX < 15 || croppedAndPlaced.maxX > 48 ||
        croppedAndPlaced.minY < 15 || croppedAndPlaced.maxY > 48 ||
        faded.meanRgb >= croppedAndPlaced.meanRgb) {
        std::fprintf(stderr, "FAIL: GPU geometryまたはopacityの実画素結果が期待範囲外です\n");
        return 1;
    }

    std::ofstream json(argv[1], std::ios::binary | std::ios::trunc);
    json << "{\n  \"schema\":\"m7a-p0-v1\",\n"
         << "  \"fade_authority\":{\"decoded_source_frame\":" << decodedSourceFrame
         << ",\"source_in_frame\":" << sourceInFrame << ",\"local_frame\":" << localFrame
         << ",\"unrelated_output_ordinal\":" << previewOutputOrdinal << ",\"factor\":" << fade
         << "},\n"
         << "  \"rotation\":{\"supported_by_current_primitive\":false,"
            "\"required_local_extension\":\"centered textured quad vertex transform\","
            "\"additional_render_pass_required\":false,"
            "\"cpu_readback_required\":false},\n"
         << "  \"samples\":[\n";
    writeMetrics(json, "baseline", baseline);
    json << ",\n";
    writeMetrics(json, "asymmetric_source_uv_scaled_positioned", croppedAndPlaced);
    json << ",\n";
    writeMetrics(json, "clip_local_fade_opacity", faded);
    json << "\n  ]\n}\n";
    if (!json.good()) {
        std::fprintf(stderr, "FAIL: GPU生JSONを書き込めません\n");
        return 1;
    }

    if (!compositor.shutdown(10000, error)) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    std::puts("M7a-P0 GPU primitive: PASS");
    return 0;
}
