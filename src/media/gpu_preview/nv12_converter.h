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
#include "media/gpu_preview/gpu_completion.h"
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

    // clear を行わず1 layerだけ描く。compositorが全layer検証と1回のclearを
    // 済ませた後に呼ぶ。shaderはstraight alphaとしてopacityを出力する。
    bool drawLayer(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                   const FitRect& destination, const float sourceUv[4], float opacity,
                   bool linearFilter, std::string& err);

    // composition の issue 前準備。SRV 生成を含む失敗しうる resource 準備を
    // clear/draw より前に完了させる。drawLayer は同じ cache entry を再利用する。
    bool prepareLayer(const DecodedGpuFrame& frame, std::string& err);

    // RGBA render target の指定小領域だけを読む。既存の単一 staging 経路を再利用する。
    bool readOutputProbe(ID3D11Texture2D* texture, int x, int y, int width, int height,
                         std::vector<unsigned char>& rgbaOut, std::string& err);

    // marker 帯 (左上 1216x64) だけを 1:1 で RGBA へ変換し、CPU へ読む。
    //
    // **これは full-frame readback ではない。**
    // 1080p の 3.7% / 4K の 0.9% しか読まない。
    // カウンタも full-frame とは別に持つ (readback_counter.h を参照)。
    //
    // rgbaOut は bandWidth*bandHeight*4 バイトになる。
    //
    // MVM_ALLOW_SMALL_REGION_READBACK: この関数と readColorPatches だけが
    // CPU へ画素を戻してよい (scripts/lint.ps1 が場所を限定して検査する)。
    bool readMarkerBand(const DecodedGpuFrame& frame, int bandWidth, int bandHeight,
                        std::vector<unsigned char>& rgbaOut, std::string& err);

    // color patch の検査用に、左上の patchW x patchH だけを 1:1 で読む (§6)。
    // marker 帯と同じ経路・同じ shader を通る。full-frame readback は増やさない。
    //
    // MVM_ALLOW_SMALL_REGION_READBACK
    bool readColorPatches(const DecodedGpuFrame& frame, int patchW, int patchH,
                          std::vector<unsigned char>& rgbaOut, std::string& err);

    // actual-target correctness probeの期待値用。sourceの正規化座標を1 pixelだけ読む。
    bool readSourceProbe(const DecodedGpuFrame& frame, float u, float v,
                         std::vector<unsigned char>& rgbaOut, std::string& err);

    // --- SRV cache の世代管理 (§4) ------------------------------------------
    // decoder を開き直すと decode pool の texture がまるごと入れ替わる。
    // ポインタは再利用されうるので、**texture の同一性だけでは足りない**。
    // resource_epoch を key に含め、epoch が変わったら旧 entry を retire する。
    //
    // 旧 entry は即 Release しない。GPU がまだ読んでいる可能性があるので、
    // 最後に使った submission serial とともに retirement queue へ渡す。
    void retireEntriesNotInEpoch(ResourceEpoch epoch, GpuRetirementQueue& queue);

    // draw で使った entry に、この submission serial を刻む。
    // **signalSubmission() の直後に呼ぶ。**
    void stampSubmissionSerial(unsigned long long serial);

    size_t srvCacheEntries() const { return srvCache_.size(); }

    size_t srvCacheEntriesPeak() const { return srvCachePeak_; }

    long long retiredSrvEntries() const { return retiredSrvEntries_; }

    // cache が抱えている (resource_epoch, texture) の **異なる組み合わせ数**。
    //
    // 「今 open している decoder の数」ではない (§6)。
    // 1 decoder = 1 array texture なので値としては一致しがちだが、
    // 意味が違うものを同じ名前で呼ぶと、増えた理由を取り違える。
    size_t srvCacheTextureGroups() const;

private:
    struct SrvPair {
        ID3D11ShaderResourceView* luma = nullptr;
        ID3D11ShaderResourceView* chroma = nullptr;
    };

    // cache key は (resource_epoch, texture identity, array index, pixel format)。
    // 毎フレーム作り直すと 60fps で 120 回/秒の resource 生成になり、
    // 計測値を汚す。decode pool は固定サイズなので、epoch ごとには有界である。
    struct SrvCacheEntry {
        ResourceEpoch epoch{};
        ID3D11Texture2D* texture = nullptr;
        unsigned int arrayIndex = 0;
        GpuPixelFormat pixelFormat = GpuPixelFormat::Unknown;
        SrvPair srv;
        // この entry を使った最後の submission serial。
        // retire するときの解放条件になる。
        unsigned long long lastUsedSerial = 0;
    };

    std::vector<SrvCacheEntry> srvCache_;
    // stampSubmissionSerial がまだ刻んでいない entry の添字。
    std::vector<size_t> pendingStamp_;
    size_t srvCachePeak_ = 0;
    long long retiredSrvEntries_ = 0;

    // 小領域 readback の上限。これを超える要求は拒否する
    // (full-frame readback をうっかり書けないようにするため)。
    // 1216x64 の marker 帯 = 311,296 byte。余裕を見て 1 MiB。
    static constexpr long long kMaxSmallRegionBytes = 1024 * 1024;

    bool readSmallRegionTopLeft(const DecodedGpuFrame& frame, int bandWidth, int bandHeight,
                                std::vector<unsigned char>& rgbaOut, std::string& err);
    bool ensureReadbackSurfaces(int width, int height, std::string& err);
    bool mapReadbackSurface(int width, int height, std::vector<unsigned char>& rgbaOut,
                            std::string& err);
    bool ensureShaders(std::string& err);
    bool acquireSrvs(const DecodedGpuFrame& frame, SrvPair& out, std::string& err);
    bool drawInternal(const DecodedGpuFrame& frame, ID3D11RenderTargetView* rtv,
                      const FitRect& viewport, const float uvRect[4], bool linearFilter,
                      float opacity, std::string& err);

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
