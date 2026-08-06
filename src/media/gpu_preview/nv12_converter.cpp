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
    float4 misc;     // x = chroma neutral (8bit: 128/255, 10bit: 512/1023)
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
    // **chroma の中立点は 0.5 ではない。**
    // 8bit は 128/255 = 0.50196、10bit は 512/1023 = 0.50049 である。
    // 0.5 を使うと全画素に約 1 LSB の色かぶりが出る (color patch 検査で検出した)。
    c = (c - misc.x) * lum.z;

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
    float misc[4];
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
bool planeFormats(GpuPixelFormat f, DXGI_FORMAT& luma, DXGI_FORMAT& chroma, float& sampleScale,
                  float& chromaNeutral) {
    switch (f) {
    case GpuPixelFormat::NV12:
        luma = DXGI_FORMAT_R8_UNORM;
        chroma = DXGI_FORMAT_R8G8_UNORM;
        sampleScale = 1.0f;
        // **8bit の chroma 中立点は 0.5 ではなく 128/255 である。**
        chromaNeutral = 128.0f / 255.0f;
        return true;
    case GpuPixelFormat::P010:
        luma = DXGI_FORMAT_R16_UNORM;
        chroma = DXGI_FORMAT_R16G16_UNORM;
        // P010 は 10bit を 16bit の上位ビットへ詰める (下位 6bit は 0)。
        // R16_UNORM は 65535 で割るので、1023 で割った値へ直す。
        sampleScale = 65535.0f / 65472.0f;
        // 10bit の中立点は 512/1023。
        chromaNeutral = 512.0f / 1023.0f;
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
    pendingStamp_.clear();

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
    // key は (resource_epoch, texture, arrayIndex, pixelFormat)。
    // **texture ポインタだけでは足りない。** decoder を開き直すと
    // 古い pool が解放され、同じアドレスに新しい pool が来ることがある。
    // その状態で古い SRV を再利用すると、別のフレームの画素を描く。
    const unsigned long long epoch = frame.generation.compositionEpoch;
    for (size_t i = 0; i < srvCache_.size(); i++) {
        const auto& e = srvCache_[i];
        if (e.epoch == epoch && e.texture == frame.texture && e.arrayIndex == frame.arrayIndex &&
            e.pixelFormat == frame.pixelFormat) {
            out = e.srv;
            pendingStamp_.push_back(i);
            return true;
        }
    }

    DXGI_FORMAT lumaFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT chromaFmt = DXGI_FORMAT_UNKNOWN;
    float scale = 1.0f;
    float neutral = 0.5f;
    if (!planeFormats(frame.pixelFormat, lumaFmt, chromaFmt, scale, neutral)) {
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

    SrvCacheEntry entry;
    entry.epoch = epoch;
    entry.texture = frame.texture;
    entry.arrayIndex = frame.arrayIndex;
    entry.pixelFormat = frame.pixelFormat;
    entry.srv = pair;
    srvCache_.push_back(entry);
    if (srvCache_.size() > srvCachePeak_)
        srvCachePeak_ = srvCache_.size();
    pendingStamp_.push_back(srvCache_.size() - 1);
    out = pair;
    return true;
}

void Nv12Converter::stampSubmissionSerial(unsigned long long serial) {
    for (size_t i : pendingStamp_) {
        if (i < srvCache_.size())
            srvCache_[i].lastUsedSerial = serial;
    }
    pendingStamp_.clear();
}

size_t Nv12Converter::activeDecoderPools() const {
    // (epoch, texture) の異なる組み合わせを数える。
    // decode pool 1 つにつき array texture 1 枚なので、これが pool 数になる。
    std::vector<std::pair<unsigned long long, ID3D11Texture2D*>> seen;
    for (const auto& e : srvCache_) {
        const std::pair<unsigned long long, ID3D11Texture2D*> k{e.epoch, e.texture};
        bool found = false;
        for (const auto& s2 : seen)
            if (s2 == k) {
                found = true;
                break;
            }
        if (!found)
            seen.push_back(k);
    }
    return seen.size();
}

void Nv12Converter::retireEntriesNotInEpoch(unsigned long long epoch, GpuRetirementQueue& queue) {
    // 旧 epoch の SRV を **即 Release しない。**
    // GPU がまだそのフレームを読んでいる可能性がある。
    // 最後に使った submission serial とともに retirement queue へ渡し、
    // その serial が完了してから解放させる。
    std::vector<SrvCacheEntry> keep;
    keep.reserve(srvCache_.size());
    for (auto& e : srvCache_) {
        if (e.epoch == epoch) {
            keep.push_back(e);
            continue;
        }
        auto* holder = new SrvPair{e.srv.luma, e.srv.chroma};
        queue.retire(e.lastUsedSerial, std::shared_ptr<void>(holder, [](void* p) {
                         auto* pair = static_cast<SrvPair*>(p);
                         if (pair->luma)
                             pair->luma->Release();
                         if (pair->chroma)
                             pair->chroma->Release();
                         delete pair;
                     }));
        retiredSrvEntries_++;
    }
    // 添字が変わるので、まだ刻んでいない参照は捨てる
    // (次の draw で刻み直される。刻み損ねた entry は serial 0 のまま
    //  retire されるが、それは「まだ一度も描いていない」= 解放して安全)。
    pendingStamp_.clear();
    srvCache_.swap(keep);
}

bool Nv12Converter::drawInternal(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                                 const FitRect& viewport, const float uvRect[4], bool linearFilter,
                                 std::string& err) {
    SrvPair srv;
    if (!acquireSrvs(frame, srv, err))
        return false;

    DXGI_FORMAT lumaFmt, chromaFmt;
    float sampleScale = 1.0f;
    float chromaNeutral = 0.5f;
    planeFormats(frame.pixelFormat, lumaFmt, chromaFmt, sampleScale, chromaNeutral);

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
    params.misc[0] = chromaNeutral;

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

// MVM_ALLOW_SMALL_REGION_READBACK
//
// **このファイルは CPU readback を書いてよい唯一の場所である。**
// scripts/lint.ps1 が gpu_preview 層で staging / CPU_ACCESS_READ / MAP_READ を
// 禁止し、この印がある file だけを、しかも **各 1 箇所まで**許可する。
// 2 本目の readback 経路を足すと lint が落ちる。
//
// marker 帯と color patch の共通実装。
// **CPU へ画素を戻す経路はこの 1 本だけ。** 呼び出し元は 2 つ
// (readMarkerBand / readColorPatches) で、どちらも左上の小領域しか読まない。
// full frame を読む経路をここへ足さないこと (lint が場所を限定して検査する)。
bool Nv12Converter::readSmallRegionTopLeft(const DecodedGpuFrame& frame, int bandWidth,
                                           int bandHeight, std::vector<unsigned char>& rgbaOut,
                                           std::string& err) {
    if (!ready_) {
        err = "Nv12Converter が初期化されていません";
        return false;
    }
    if (!frame.valid()) {
        err = "frame が不正です";
        return false;
    }
    if (bandWidth <= 0 || bandHeight <= 0 || bandWidth > frame.width || bandHeight > frame.height) {
        err = "読み取る小領域の大きさが素材に収まりません";
        return false;
    }
    // full frame を読ませない。ここが唯一の readback 経路なので、
    // 「うっかり全画素」を機械的に塞いでおく。
    if (static_cast<long long>(bandWidth) * bandHeight * 4 > kMaxSmallRegionBytes) {
        err = "小領域 readback の上限を超えています (full-frame readback は禁止)";
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
    return true;
}

bool Nv12Converter::readMarkerBand(const DecodedGpuFrame& frame, int bandWidth, int bandHeight,
                                   std::vector<unsigned char>& rgbaOut, std::string& err) {
    if (!readSmallRegionTopLeft(frame, bandWidth, bandHeight, rgbaOut, err))
        return false;
    // **full frame ではなく帯だけを読んだ**ことを、専用のカウンタで記録する。
    counters_->noteMarkerBandReadback();
    return true;
}

bool Nv12Converter::readColorPatches(const DecodedGpuFrame& frame, int patchW, int patchH,
                                     std::vector<unsigned char>& rgbaOut, std::string& err) {
    if (!readSmallRegionTopLeft(frame, patchW, patchH, rgbaOut, err))
        return false;
    counters_->noteColorPatchReadback();
    return true;
}

} // namespace mvm::gpu
