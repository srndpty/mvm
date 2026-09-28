// 静止画 layer (RGBA8 straight alpha) を GPU compositor が track 順に合成することの検査。
//
// 文字 clip は preview でこの経路を通る。video layer との前後関係が z 順だけで
// 決まること、alpha と opacity が効くこと、texture を手放すと SRV cache が参照を
// 残さないことを、合成結果の画素で確かめる。
#include "media/gpu_preview/gpu_compositor.h"
#include "media/gpu_preview/still_image_frame.h"
#include "media/gpu_preview/validation_reference.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mvm::gpu;

namespace {
constexpr int kW = 64;
constexpr int kH = 32;

int fail(const std::string& text) {
    std::fprintf(stderr, "FAIL: %s\n", text.c_str());
    return 3;
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
    SharedD3D11Device shared;
};

struct OwnedTexture {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    ~OwnedTexture() {
        if (rtv)
            rtv->Release();
        if (texture)
            texture->Release();
    }
};

// 一様な NV12 (BT.709 limited)。期待色は compositor の source probe から取るので
// ここで YUV -> RGB を計算しない。
bool makeNv12(ID3D11Device* device, unsigned char y, unsigned char u, unsigned char v,
              OwnedTexture& out, std::string& err) {
    std::vector<unsigned char> bytes(static_cast<size_t>(kW) * kH * 3 / 2, y);
    for (size_t i = static_cast<size_t>(kW) * kH; i + 1 < bytes.size(); i += 2) {
        bytes[i] = u;
        bytes[i + 1] = v;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{bytes.data(), kW, static_cast<UINT>(bytes.size())};
    if (FAILED(device->CreateTexture2D(&td, &init, &out.texture))) {
        err = "NV12 textureを生成できません";
        return false;
    }
    return true;
}

DecodedGpuFrame videoFrame(OwnedTexture& texture, SourceId source) {
    DecodedGpuFrame f;
    f.frameNumber = 0;
    f.pts = 0;
    f.timeBase = {1, 60};
    f.width = kW;
    f.height = kH;
    f.pixelFormat = GpuPixelFormat::NV12;
    f.texture = texture.texture;
    f.colorSpace = ColorSpace::BT709;
    f.colorRange = ColorRange::Limited;
    f.sourceId = source;
    f.sourceGeneration = {1};
    f.resourceEpoch = {source.value};
    f.lifetime = std::make_shared<int>(1);
    return f;
}

// 左 1/4 が不透明の白、次の 1/4 が alpha 128 の白、右半分が透明。
std::vector<unsigned char> stillPixels() {
    std::vector<unsigned char> rgba(static_cast<size_t>(kW) * kH * 4, 0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW / 2; ++x) {
            unsigned char* p = &rgba[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = x < kW / 4 ? 255 : 128;
        }
    return rgba;
}

Rgba8 readAt(GpuCompositor& c, ID3D11Texture2D* texture, int x, int y, std::string& err) {
    std::vector<unsigned char> rgba;
    const bool ok = texture ? c.readExternalOutputProbe(texture, x, y, 1, 1, rgba, err)
                            : c.readOutputProbe(x, y, 1, 1, rgba, err);
    if (!ok || rgba.size() < 4)
        return {0, 0, 0, 0};
    return {rgba[0], rgba[1], rgba[2], rgba[3]};
}

std::string text(Rgba8 v) {
    return std::to_string(v.r) + "," + std::to_string(v.g) + "," + std::to_string(v.b);
}

bool closeTo(Rgba8 actual, Rgba8 expected, int tolerance) {
    return std::abs(actual.r - expected.r) <= tolerance &&
           std::abs(actual.g - expected.g) <= tolerance &&
           std::abs(actual.b - expected.b) <= tolerance;
}

Rgba8 mix(Rgba8 top, Rgba8 bottom, double alpha) {
    const auto m = [alpha](int a, int b) {
        return static_cast<int>(std::lround(a * alpha + b * (1.0 - alpha)));
    };
    return {m(top.r, bottom.r), m(top.g, bottom.g), m(top.b, bottom.b), 255};
}

CompositionLayerFrame layer(const DecodedGpuFrame& frame, int z, RectF destination = {0, 0, 1, 1},
                            float opacity = 1.0f) {
    return {frame, destination, {0, 0, 1, 1}, opacity, z};
}

// 2 地点の画素を期待値と比べる。どちらも満たさないと失敗にする。
bool expectAt(GpuCompositor& c, ID3D11Texture2D* texture, int x, int y, Rgba8 expected,
              int tolerance, const char* what, std::string& err) {
    const Rgba8 actual = readAt(c, texture, x, y, err);
    if (!err.empty())
        return false;
    if (!closeTo(actual, expected, tolerance)) {
        err = std::string(what) + " (位置 " + std::to_string(x) + "," + std::to_string(y) +
              " 実測 " + text(actual) + " 期待 " + text(expected) + ")";
        return false;
    }
    return true;
}

} // namespace

int main() {
    std::string err;
    OwnedDevice device;
    if (!device.create(err))
        return fail(err);
    ReadbackCounters readbacks;
    GpuCompositor compositor;
    if (!compositor.initialize(device.shared, readbacks, kW, kH, err))
        return fail(err);

    OwnedTexture red;
    OwnedTexture blue;
    if (!makeNv12(device.device, 63, 102, 240, red, err) ||
        !makeNv12(device.device, 32, 240, 118, blue, err))
        return fail(err);
    const DecodedGpuFrame bottom = videoFrame(red, {1});
    const DecodedGpuFrame top = videoFrame(blue, {2});
    std::vector<unsigned char> probe;
    if (!compositor.readSourceProbe(bottom, 0.5f, 0.5f, probe, err))
        return fail(err);
    const Rgba8 redColor{probe[0], probe[1], probe[2], 255};
    if (!compositor.readSourceProbe(top, 0.5f, 0.5f, probe, err))
        return fail(err);
    const Rgba8 blueColor{probe[0], probe[1], probe[2], 255};
    const Rgba8 white{255, 255, 255, 255};
    if (closeTo(redColor, white, 40) || closeTo(blueColor, white, 40) ||
        closeTo(redColor, blueColor, 40))
        return fail("期待色どうしが近すぎて前後関係を判別できません");

    // 不正な画素数は texture を作らずに拒否する。
    const std::vector<unsigned char> pixels = stillPixels();
    DecodedGpuFrame still;
    if (makeStillImageFrame(device.shared, kW, kH, pixels.data(), pixels.size() - 4, {100}, still,
                            err))
        return fail("画素数の足りない静止画を受理しました");
    err.clear();
    if (!makeStillImageFrame(device.shared, kW, kH, pixels.data(), pixels.size(), {100}, still,
                             err))
        return fail(err);
    if (!still.valid() || still.pixelFormat != GpuPixelFormat::RGBA8)
        return fail("静止画 frame が不正です");

    // 1. 静止画が上: 不透明部は白、半透明部は白と赤の中間、透明部は赤。
    if (!compositor.compose({0, {1}, {layer(bottom, 0), layer(still, 1)}, {}}, err))
        return fail(err);
    if (!expectAt(compositor, nullptr, 4, 16, white, 3, "静止画の不透明部が白ではありません",
                  err) ||
        !expectAt(compositor, nullptr, 24, 16, mix(white, redColor, 128.0 / 255.0), 6,
                  "静止画の alpha が合成に効いていません", err) ||
        !expectAt(compositor, nullptr, 48, 16, redColor, 3, "静止画の透明部が映像を隠しました",
                  err))
        return fail(err);

    // 2. 映像が上: z 順だけを入れ替えると静止画は完全に隠れる。
    //    1 と同じ位置の白が消えることが、前後関係を z で決めている証拠になる。
    if (!compositor.compose({0, {1}, {layer(still, 0), layer(bottom, 1)}, {}}, err))
        return fail(err);
    if (!expectAt(compositor, nullptr, 4, 16, redColor, 3, "下の静止画が上の映像より前に出ました",
                  err))
        return fail(err);

    // 3. layer opacity は静止画の alpha に掛かる。
    if (!compositor.compose({0, {1}, {layer(bottom, 0), layer(still, 1, {0, 0, 1, 1}, 0.5f)}, {}},
                            err))
        return fail(err);
    if (!expectAt(compositor, nullptr, 4, 16, mix(white, redColor, 0.5), 6,
                  "layer opacity が静止画に効いていません", err))
        return fail(err);

    // 4. V1 映像 / V2 静止画 / V3 映像 (右半分) の 3 layer。
    //    V3 が覆う範囲だけ静止画が隠れ、覆わない範囲では見える。
    OwnedTexture target;
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW;
        td.Height = kH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device.device->CreateTexture2D(&td, nullptr, &target.texture)) ||
            FAILED(device.device->CreateRenderTargetView(target.texture, nullptr, &target.rtv)))
            return fail("3 layer 用の target を生成できません");
    }
    {
        // destination は aspect fit されるため、左上 8x8 の枠に 2:1 の素材が 8x4 で入る。
        // 5 で texture の解放を見るので、静止画 frame の複製をこの block の外へ残さない。
        const ComposedFrame stacked{
            0,
            {1},
            {layer(bottom, 0), layer(still, 1), layer(top, 2, {0.0f, 0.0f, 0.125f, 0.25f})},
            {}};
        {
            std::lock_guard<D3D11Lock> guard(device.shared.lock());
            const float black[4] = {0, 0, 0, 1};
            device.context->ClearRenderTargetView(target.rtv, black);
        }
        if (!compositor.composeLayersToTarget(stacked, {target.rtv, kW, kH}, 3, err))
            return fail(err);
    }
    if (!expectAt(compositor, target.texture, 3, 3, blueColor, 3,
                  "V3 の映像が V2 の静止画より前に出ていません", err) ||
        !expectAt(compositor, target.texture, 4, 20, white, 3,
                  "V3 が覆わない範囲の静止画が見えません", err) ||
        !expectAt(compositor, target.texture, 48, 20, redColor, 3,
                  "V1 の映像が透明部から見えません", err))
        return fail(err);

    // 5. texture を手放すと SRV cache から外れる。D3D11 の view は resource の
    //    外部参照数に現れないので、cache の entry 数で確かめる。
    //    無関係な texture の retire では減らないことを対照に取る。
    const size_t before = compositor.srvCacheEntries();
    if (before != 3)
        return fail("SRV cache が映像 2 枚と静止画 1 枚の 3 entry になっていません: " +
                    std::to_string(before));
    compositor.retireLayerTexture(target.texture);
    if (compositor.srvCacheEntries() != before)
        return fail("対照: 無関係な texture の retire で cache が減りました");
    ID3D11Texture2D* stillTexture = still.texture;
    compositor.retireLayerTexture(stillTexture);
    still = {};
    if (compositor.srvCacheEntries() != before - 1)
        return fail("静止画 texture を retire しても SRV cache に残っています");
    // 既存の video entry は残り、引き続き描ける。
    if (!compositor.compose({0, {1}, {layer(bottom, 0), layer(top, 1)}, {}}, err))
        return fail(err);

    if (!compositor.shutdown(5000, err))
        return fail(err);
    std::puts("静止画 layer の z 順・alpha・opacity・3 layer 合成・texture 解放を確認しました");
    return 0;
}
