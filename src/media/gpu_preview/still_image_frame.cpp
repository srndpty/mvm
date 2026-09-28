#include "media/gpu_preview/still_image_frame.h"

#include <cstdio>
#include <limits>
#include <memory>

namespace mvm::gpu {

bool makeStillImageFrame(SharedD3D11Device& device, int width, int height,
                         const unsigned char* rgba, std::size_t byteCount, SourceId sourceId,
                         DecodedGpuFrame& out, std::string& err) {
    out = {};
    if (!device.valid() || width <= 0 || height <= 0 || !rgba ||
        static_cast<unsigned long long>(width) * 4ULL > std::numeric_limits<unsigned int>::max() ||
        byteCount != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U) {
        err = "静止画 layer の寸法または画素数が不正です";
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{rgba, static_cast<UINT>(width) * 4U, 0};
    ID3D11Texture2D* texture = nullptr;
    const HRESULT rc = device.device()->CreateTexture2D(&td, &init, &texture);
    if (FAILED(rc) || !texture) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "静止画 layer の texture 生成に失敗しました (HRESULT=0x%08lX)",
                      static_cast<unsigned long>(rc));
        err = text;
        return false;
    }
    out.frameNumber = 0;
    out.width = width;
    out.height = height;
    out.pixelFormat = GpuPixelFormat::RGBA8;
    out.texture = texture;
    out.arrayIndex = 0;
    out.colorSpace = ColorSpace::BT709;
    out.colorRange = ColorRange::Full;
    out.sourceId = sourceId;
    out.lifetime = std::shared_ptr<void>(
        texture, [](void* p) { static_cast<ID3D11Texture2D*>(p)->Release(); });
    return true;
}

} // namespace mvm::gpu
