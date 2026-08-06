/*
 * mvm Phase 1 / P1 - FFmpeg (D3D11VA) による hardware decoder
 *
 * AV_HWDEVICE_TYPE_D3D11VA を使い、AV_PIX_FMT_D3D11 の frame だけを受け取る。
 * **software decode fallback は実装しない。**
 * hardware decode できない素材は fail-closed で報告して止まる。
 *
 * このヘッダに FFmpeg の型は出さない (公開部は int と std::string だけ)。
 */

#ifndef MVM_GPU_PREVIEW_FFMPEG_D3D11_DECODER_H
#define MVM_GPU_PREVIEW_FFMPEG_D3D11_DECODER_H

#include "media/gpu_preview/d3d11_shared_device.h"
#include "media/gpu_preview/readback_counter.h"
#include "media/gpu_preview/video_decoder.h"

#include <memory>

namespace mvm::gpu {

// open が失敗した理由。呼び出し側 (テスト・CLI) が終了コードを分けるために使う。
// 「開けなかった」を 1 つにまとめると、素材が壊れているのか
// hardware decoder が無いのかを区別できない。
enum class OpenFailure {
    None = 0,
    CannotOpenFile,        // ファイルが開けない / container が壊れている
    NoVideoStream,         // 映像 stream が無い
    NoHardwareDecoder,     // その codec に D3D11VA の hwaccel が無い
    HardwareContextFailed, // AVHWDeviceContext を作れなかった
    CodecOpenFailed,       // avcodec_open2 に失敗
    DeviceMismatch,        // decode texture が別の ID3D11Device に属する
};

const char* toString(OpenFailure f);

// --------------------------------------------------------------------------
// 単体テストできる純粋な判定
// --------------------------------------------------------------------------
// get_format callback の中身。FFmpeg が提示した候補から
// AV_PIX_FMT_D3D11 だけを選ぶ。無ければ AV_PIX_FMT_NONE を返す
// (software 形式へ落ちない = fail-closed)。
//
// 引数・戻り値は AVPixelFormat の生値 (int)。
// FFmpeg のヘッダ抜きでテストできるようにするためである。
int chooseHwPixelFormat(const int* candidates, int count);

// decode 結果の format が受け入れ可能か。AV_PIX_FMT_D3D11 のみ真。
bool isExpectedHwFormat(int avPixelFormat);

// AV_PIX_FMT_D3D11 の生値 (FFmpeg のヘッダに依存せずテストへ渡すため)。
int expectedHwPixelFormatValue();

// --------------------------------------------------------------------------

class FFmpegD3D11Decoder final : public IVideoDecoder {
public:
    // shared device は decoder より長生きすること。
    // counters は null 可 (その場合 globalReadbackCounters を使う)。
    explicit FFmpegD3D11Decoder(SharedD3D11Device& device, ReadbackCounters* counters = nullptr);
    ~FFmpegD3D11Decoder() override;

    bool open(const std::string& utf8Path, std::string& err) override;
    DecodeStatus requestFrame(DecodedGpuFrame& out, std::string& err) override;
    bool seek(long long frameNumber, std::string& err) override;
    void flush() override;
    void close() override;
    const VideoStreamInfo& info() const override;
    unsigned long long generation() const override;
    unsigned long long resourceEpoch() const override;

    OpenFailure openFailure() const;

    // decode 出力 texture から GetDevice で遡って調べた adapter。
    // open 後に最初の 1 枚を decode するまでは valid=false。
    const AdapterInfo& decodeAdapter() const;

    // 同じく、decode texture が実際に属していた ID3D11Device のポインタ。
    // **adapter が同じでも device が別**ということはありうる。
    // 「同一 device (= zero-copy)」と「同一 adapter (= GPU copy で足りる)」を
    // 区別するために両方を出す。未 decode なら 0。
    unsigned long long decodeDevicePointer() const;

    // 統計 (JSON へ出す)。
    long long decodedFrameCount() const;
    long long softwareFrameRejectCount() const;
    long long decodeErrorCount() const;
    // seek が 1 回目で行き過ぎ、手前へ戻して測り直した回数。
    // 0 でないこと自体は不具合ではないが、**隠さずに出す**。
    long long seekBackoffCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_FFMPEG_D3D11_DECODER_H
