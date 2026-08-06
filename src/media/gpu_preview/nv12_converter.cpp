#include "media/gpu_preview/nv12_converter.h"

#include "media/gpu_preview/color_metadata.h"

#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>

namespace mvm::gpu {
namespace {

std::string hr(const char* what, HRESULT code) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s に失敗しました (HRESULT=0x%08lX)", what,
                  static_cast<unsigned long>(code));
    return buf;
}

template<class T>
void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// --------------------------------------------------------------------------
// shader
// --------------------------------------------------------------------------
// decode texture は **必ず Texture2DArray として** 束ねる。
// FFmpeg の decode pool は array texture であり、
// 非 array の場合も ArraySize=1 の view として扱えば分岐が消える。
const char kShaderSource[] = R"HLSL(
cbuffer Params : register(b0)
{
    float4 uvRect;   // xy = offset, zw = scale
    float4 lum;      // x = yScale, y = yOffset, z = uvScale, w = sampleScale
    float4 mat;      // x = vr, y = ug, z = vg, w = ub
};

Texture2DArray<float4> texLuma   : register(t0);
Texture2DArray<float4> texChroma : register(t1);
SamplerState samp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);   // (0,0) (2,0) (0,2)
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float4 ps_main(VSOut i) : SV_Target
{
    float2 uv = uvRect.xy + i.uv * uvRect.zw;

    // sampleScale は P010 (10bit を 16bit の上位へ詰める) の補正。
    // NV12 では 1.0。
    float  y  = texLuma.Sample(samp, float3(uv, 0)).r * lum.w;
    float2 c  = texChroma.Sample(samp, float3(uv, 0)).rg * lum.w;

    y = (y - lum.y) * lum.x;
    c = (c - 0.5) * lum.z;

    float3 rgb;
    rgb.r = y + mat.x * c.y;
    rgb.g = y - mat.y * c.x - mat.z * c.y;
    rgb.b = y + mat.w * c.x;
    return float4(saturate(rgb), 1.0);
}
)HLSL";

struct ShaderParams {
    float uvRect[4];
    float lum[4];
    float mat[4];
};

bool compile(const char* entry, const char* target, ID3DBlob** out, std::string& err) {
    ID3DBlob* errors = nullptr;
    const HRESULT rc =
        D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "nv12_converter.hlsl", nullptr,
                   nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(rc)) {
        err = hr("shader のコンパイル", rc);
        if (errors) {
            err += ": ";
            err.append(static_cast<const char*>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
            errors->Release();
        }
        return false;
    }
    if (errors)
        errors->Release();
    return true;
}

// NV12 / P010 の各平面に対応する SRV format。
// **平面の選択は format で行う** (D3D11 の NV12 は R8_UNORM が Y、
// R8G8_UNORM が UV を指す)。
bool planeFormats(GpuPixelFormat f, DXGI_FORMAT& luma, DXGI_FORMAT& chroma, float& sampleScale) {
    switch (f) {
    case GpuPixelFormat::NV12:
        luma = DXGI_FORMAT_R8_UNORM;
        chroma = DXGI_FORMAT_R8G8_UNORM;
        sampleScale = 1.0f;
        return true;
    case GpuPixelFormat::P010:
        luma = DXGI_FORMAT_R16_UNORM;
        chroma = DXGI_FORMAT_R16G16_UNORM;
        // P010 は 10bit を 16bit の上位ビットへ詰める (下位 6bit は 0)。
        // R16_UNORM は 65535 で割るので、1023 で割った値へ直す。
        sampleScale = 65535.0f / 65472.0f;
        return true;
    case GpuPixelFormat::Unknown:
        break;
    }
    return false;
}

} // namespace

FitRect aspectFit(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
    FitRect r;
    if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0)
        return r;

    // 整数演算で比較する。double だと 16:9 を 16:9 の枠へ入れたときに
    // 1px の隙間が出たり出なかったりする。
    const long long a = static_cast<long long>(srcWidth) * dstHeight;
    const long long b = static_cast<long long>(dstWidth) * srcHeight;

    if (a > b) {
        // 元の方が横長 -> 幅を合わせる
        r.width = dstWidth;
        r.height = static_cast<int>((static_cast<long long>(dstWidth) * srcHeight + srcWidth / 2) /
                                    srcWidth);
    } else {
        r.height = dstHeight;
        r.width = static_cast<int>((static_cast<long long>(dstHeight) * srcWidth + srcHeight / 2) /
                                   srcHeight);
    }
    if (r.width < 1)
        r.width = 1;
    if (r.height < 1)
        r.height = 1;
    if (r.width > dstWidth)
        r.width = dstWidth;
    if (r.height > dstHeight)
        r.height = dstHeight;

    r.x = (dstWidth - r.width) / 2;
    r.y = (dstHeight - r.height) / 2;
    return r;
}

// --------------------------------------------------------------------------

Nv12Converter::~Nv12Converter() {
    release();
}

bool Nv12Converter::initialize(SharedD3D11Device& device, ReadbackCounters* counters,
                               std::string& err) {
    release();
    if (!device.valid()) {
        err = "共有 D3D11 device が未初期化です";
        return false;
    }
    shared_ = &device;
    counters_ = counters ? counters : &globalReadbackCounters();

    if (!ensureShaders(err)) {
        release();
        return false;
    }
    ready_ = true;
    return true;
}

bool Nv12Converter::ensureShaders(std::string& err) {
    ID3D11Device* dev = shared_->device();

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!compile("vs_main", "vs_5_0", &vsBlob, err))
        return false;
    if (!compile("ps_main", "ps_5_0", &psBlob, err)) {
        vsBlob->Release();
        return false;
    }

    HRESULT rc =
        dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (SUCCEEDED(rc))
        rc = dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                    &ps_);
    vsBlob->Release();
    psBlob->Release();
    if (FAILED(rc)) {
        err = hr("shader オブジェクトの生成", rc);
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    rc = dev->CreateBuffer(&bd, nullptr, &cbuffer_);
    if (FAILED(rc)) {
        err = hr("constant buffer の生成", rc);
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    rc = dev->CreateSamplerState(&sd, &samplerPoint_);
    if (SUCCEEDED(rc)) {
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        rc = dev->CreateSamplerState(&sd, &samplerLinear_);
    }
    if (FAILED(rc)) {
        err = hr("sampler state の生成", rc);
        return false;
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rc = dev->CreateRasterizerState(&rd, &raster_);
    if (FAILED(rc)) {
        err = hr("rasterizer state の生成", rc);
        return false;
    }

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    rc = dev->CreateBlendState(&bl, &blend_);
    if (FAILED(rc)) {
        err = hr("blend state の生成", rc);
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    rc = dev->CreateDepthStencilState(&ds, &depth_);
    if (FAILED(rc)) {
        err = hr("depth stencil state の生成", rc);
        return false;
    }
    return true;
}

void Nv12Converter::release() {
    for (auto& e : srvCache_) {
        safeRelease(e.srv.luma);
        safeRelease(e.srv.chroma);
    }
    srvCache_.clear();

    safeRelease(bandRtv_);
    safeRelease(bandTexture_);
    safeRelease(bandStaging_);
    bandWidth_ = bandHeight_ = 0;

    safeRelease(depth_);
    safeRelease(blend_);
    safeRelease(raster_);
    safeRelease(samplerLinear_);
    safeRelease(samplerPoint_);
    safeRelease(cbuffer_);
    safeRelease(ps_);
    safeRelease(vs_);

    shared_ = nullptr;
    counters_ = nullptr;
    ready_ = false;
}

bool Nv12Converter::acquireSrvs(const DecodedGpuFrame& frame, SrvPair& out, std::string& err) {
    for (const auto& e : srvCache_) {
        if (e.texture == frame.texture && e.arrayIndex == frame.arrayIndex) {
            out = e.srv;
            return true;
        }
    }

    DXGI_FORMAT lumaFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT chromaFmt = DXGI_FORMAT_UNKNOWN;
    float scale = 1.0f;
    if (!planeFormats(frame.pixelFormat, lumaFmt, chromaFmt, scale)) {
        err = "対応していない画素形式です";
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    frame.texture->GetDesc(&td);
    if ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        // ここへ来たら device 側の BindFlags 設定が効いていない。
        // CPU readback へ逃げず、失敗として報告する。
        err = "decode texture に D3D11_BIND_SHADER_RESOURCE がありません。"
              "AVD3D11VADeviceContext::BindFlags の設定が効いていません";
        return false;
    }
    if (frame.arrayIndex >= td.ArraySize) {
        err = "array index が texture の範囲外です";
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MostDetailedMip = 0;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.FirstArraySlice = frame.arrayIndex;
    sd.Texture2DArray.ArraySize = 1;

    SrvPair pair;
    sd.Format = lumaFmt;
    HRESULT rc = shared_->device()->CreateShaderResourceView(frame.texture, &sd, &pair.luma);
    if (SUCCEEDED(rc)) {
        sd.Format = chromaFmt;
        rc = shared_->device()->CreateShaderResourceView(frame.texture, &sd, &pair.chroma);
    }
    if (FAILED(rc)) {
        safeRelease(pair.luma);
        err = hr("decode texture の shader resource view 生成", rc);
        return false;
    }

    srvCache_.push_back(SrvCacheEntry{frame.texture, frame.arrayIndex, pair});
    out = pair;
    return true;
}

bool Nv12Converter::drawInternal(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                                 const FitRect& viewport, const float uvRect[4], bool linearFilter,
                                 std::string& err) {
    SrvPair srv;
    if (!acquireSrvs(frame, srv, err))
        return false;

    DXGI_FORMAT lumaFmt, chromaFmt;
    float sampleScale = 1.0f;
    planeFormats(frame.pixelFormat, lumaFmt, chromaFmt, sampleScale);

    const YuvToRgbCoefficients k = coefficientsFor(frame.colorSpace, frame.colorRange);

    ShaderParams params{};
    std::memcpy(params.uvRect, uvRect, sizeof params.uvRect);
    params.lum[0] = k.yScale;
    params.lum[1] = k.yOffset;
    params.lum[2] = k.uvScale;
    params.lum[3] = sampleScale;
    params.mat[0] = k.vr;
    params.mat[1] = k.ug;
    params.mat[2] = k.vg;
    params.mat[3] = k.ub;

    ID3D11DeviceContext* ctx = shared_->context();

    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT rc = ctx->Map(cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    if (FAILED(rc)) {
        err = hr("constant buffer の Map", rc);
        return false;
    }
    std::memcpy(m.pData, &params, sizeof params);
    ctx->Unmap(cbuffer_, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(viewport.x);
    vp.TopLeftY = static_cast<float>(viewport.y);
    vp.Width = static_cast<float>(viewport.width);
    vp.Height = static_cast<float>(viewport.height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[2] = {srv.luma, srv.chroma};
    ID3D11SamplerState* samp = linearFilter ? samplerLinear_ : samplerPoint_;
    const float blendFactor[4] = {0, 0, 0, 0};

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend_, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(depth_, 0);
    ctx->RSSetState(raster_);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_, nullptr, 0);
    ctx->PSSetShader(ps_, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cbuffer_);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->Draw(3, 0);

    // SRV を外す。次に同じ texture が render target になる可能性は無いが、
    // 束ねたまま返すと呼び出し側 (QRhi) の状態追跡と衝突しうる。
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);

    counters_->noteGpuCopy();
    return true;
}

bool Nv12Converter::drawToRenderTarget(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                                       int targetWidth, int targetHeight, bool linearFilter,
                                       const float clearColor[4], std::string& err) {
    if (!ready_) {
        err = "Nv12Converter が初期化されていません";
        return false;
    }
    if (!rtv || targetWidth <= 0 || targetHeight <= 0) {
        err = "描画先が不正です";
        return false;
    }
    if (!frame.valid()) {
        err = "frame が不正です";
        return false;
    }

    std::lock_guard<D3D11Lock> guard(shared_->lock());
    ID3D11DeviceContext* ctx = shared_->context();

    // 背景を先に塗る。aspect fit の外側が前フレームのまま残らないようにする。
    ctx->ClearRenderTargetView(rtv, clearColor);

    const FitRect fit = aspectFit(frame.width, frame.height, targetWidth, targetHeight);
    const float uvRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    return drawInternal(frame, rtv, fit, uvRect, linearFilter, err);
}

bool Nv12Converter::readMarkerBand(const DecodedGpuFrame& frame, int bandWidth, int bandHeight,
                                   std::vector<unsigned char>& rgbaOut, std::string& err) {
    if (!ready_) {
        err = "Nv12Converter が初期化されていません";
        return false;
    }
    if (!frame.valid()) {
        err = "frame が不正です";
        return false;
    }
    if (bandWidth <= 0 || bandHeight <= 0 || bandWidth > frame.width || bandHeight > frame.height) {
        err = "marker 帯の大きさが素材に収まりません";
        return false;
    }

    std::lock_guard<D3D11Lock> guard(shared_->lock());
    ID3D11Device* dev = shared_->device();
    ID3D11DeviceContext* ctx = shared_->context();

    if (bandWidth_ != bandWidth || bandHeight_ != bandHeight) {
        safeRelease(bandRtv_);
        safeRelease(bandTexture_);
        safeRelease(bandStaging_);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(bandWidth);
        td.Height = static_cast<UINT>(bandHeight);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT rc = dev->CreateTexture2D(&td, nullptr, &bandTexture_);
        if (SUCCEEDED(rc))
            rc = dev->CreateRenderTargetView(bandTexture_, nullptr, &bandRtv_);
        if (SUCCEEDED(rc)) {
            td.BindFlags = 0;
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            rc = dev->CreateTexture2D(&td, nullptr, &bandStaging_);
        }
        if (FAILED(rc)) {
            safeRelease(bandRtv_);
            safeRelease(bandTexture_);
            safeRelease(bandStaging_);
            err = hr("marker 帯用 texture の生成", rc);
            return false;
        }
        bandWidth_ = bandWidth;
        bandHeight_ = bandHeight;
    }

    // 表示と同じ shader で 1:1 に描く。
    // 別経路で読むと「表示は壊れているが marker は合う」状態を作ってしまう。
    const FitRect vp{0, 0, bandWidth, bandHeight};
    const float uvRect[4] = {0.0f, 0.0f,
                             static_cast<float>(bandWidth) / static_cast<float>(frame.width),
                             static_cast<float>(bandHeight) / static_cast<float>(frame.height)};
    if (!drawInternal(frame, bandRtv_, vp, uvRect, /*linearFilter=*/false, err))
        return false;

    ctx->CopyResource(bandStaging_, bandTexture_);

    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT rc = ctx->Map(bandStaging_, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(rc)) {
        err = hr("marker 帯 staging の Map", rc);
        return false;
    }

    const size_t bw = static_cast<size_t>(bandWidth);
    const size_t bh = static_cast<size_t>(bandHeight);
    rgbaOut.resize(bw * bh * 4);
    const auto* src = static_cast<const unsigned char*>(m.pData);
    for (int y = 0; y < bandHeight; y++) {
        std::memcpy(rgbaOut.data() + static_cast<size_t>(y) * bw * 4,
                    src + static_cast<size_t>(y) * m.RowPitch, bw * 4);
    }
    ctx->Unmap(bandStaging_, 0);

    // **full frame ではなく帯だけを読んだ**ことを、専用のカウンタで記録する。
    counters_->noteMarkerBandReadback();
    return true;
}

} // namespace mvm::gpu
