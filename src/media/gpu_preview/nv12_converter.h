/*
 * mvm Phase 1 / P1 - NV12 / P010 -> RGB 変換パス (生 D3D11)
 *
 * なぜ QRhi ではなく生の D3D11 なのか:
 *   QRhi には NV12 / P010 のような planar YUV format が無く、
 *   texture array の特定 slice を指す view を作る API も無い。
 *   decode 出力は **NV12 の texture array** なので、QRhi の抽象に載らない。
 *   ここを無理に QRhi へ寄せると、Qt の patch release で壊れる面が増える。
 *
 * Qt には依存しない。呼び出し側 (PreviewRhiRenderer) が
 * QRhiCommandBuffer::beginExternalCommandBuffer() で挟んで使う。
 *
 * **CPU へ画素を戻す経路はここに 1 本だけある** (marker 帯の 1216x64)。
 * full frame の readback はどこにも無い。
 */

#ifndef MVM_GPU_PREVIEW_NV12_CONVERTER_H
#define MVM_GPU_PREVIEW_NV12_CONVERTER_H

#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/gpu_frame.h"
#include "media/gpu_preview/readback_counter.h"

#include <string>
#include <vector>

namespace mvm::gpu {

// 描画先の矩形 (ピクセル)。aspect fit の結果を入れる。
struct FitRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 中身を維持したまま収める矩形を求める。
// 純粋関数なので単体テストする (端数の扱いを間違えると 1px ずれる)。
FitRect aspectFit(int srcWidth, int srcHeight, int dstWidth, int dstHeight);

class Nv12Converter {
public:
    Nv12Converter() = default;
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    bool initialize(SharedD3D11Device& device, ReadbackCounters* counters, std::string& err);
    void release();

    bool ready() const { return ready_; }

    // frame を render target view へ aspect fit で描く。
    // 背景 (fit 矩形の外) は clearColor で塗る。
    bool drawToRenderTarget(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                            int targetWidth, int targetHeight, bool linearFilter,
                            const float clearColor[4], std::string& err);

    // marker 帯 (左上 1216x64) だけを 1:1 で RGBA へ変換し、CPU へ読む。
    //
    // **これは full-frame readback ではない。**
    // 1080p の 3.7% / 4K の 0.9% しか読まない。
    // カウンタも full-frame とは別に持つ (readback_counter.h を参照)。
    //
    // rgbaOut は bandWidth*bandHeight*4 バイトになる。
    bool readMarkerBand(const DecodedGpuFrame& frame, int bandWidth, int bandHeight,
                        std::vector<unsigned char>& rgbaOut, std::string& err);

private:
    struct SrvPair {
        ID3D11ShaderResourceView* luma = nullptr;
        ID3D11ShaderResourceView* chroma = nullptr;
    };

    // (texture, arrayIndex) ごとの SRV。毎フレーム作り直すと
    // 60fps で 120 回/秒の resource 生成になり、計測値を汚す。
    // decode pool は固定サイズなので、この cache は有界である。
    struct SrvCacheEntry {
        ID3D11Texture2D* texture = nullptr;
        unsigned int arrayIndex = 0;
        SrvPair srv;
    };

    std::vector<SrvCacheEntry> srvCache_;

    bool ensureShaders(std::string& err);
    bool acquireSrvs(const DecodedGpuFrame& frame, SrvPair& out, std::string& err);
    bool drawInternal(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                      const FitRect& viewport, const float uvRect[4], bool linearFilter,
                      std::string& err);

    SharedD3D11Device* shared_ = nullptr;
    ReadbackCounters* counters_ = nullptr;
    bool ready_ = false;

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11Buffer* cbuffer_ = nullptr;
    ID3D11SamplerState* samplerPoint_ = nullptr;
    ID3D11SamplerState* samplerLinear_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    ID3D11DepthStencilState* depth_ = nullptr;

    // marker probe 用の小さな中間 RT と staging。1216x64 固定。
    ID3D11Texture2D* bandTexture_ = nullptr;
    ID3D11RenderTargetView* bandRtv_ = nullptr;
    ID3D11Texture2D* bandStaging_ = nullptr;
    int bandWidth_ = 0;
    int bandHeight_ = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_NV12_CONVERTER_H
