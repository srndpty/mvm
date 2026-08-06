/*
 * mvm Phase 1 / P1 - decoder の境界
 *
 * 巨大な IMediaEngine は作らない。P1 の判定に要るのは
 * 「開く / 次のフレームを取る / seek / flush / 閉じる」だけである。
 */

#ifndef MVM_GPU_PREVIEW_VIDEO_DECODER_H
#define MVM_GPU_PREVIEW_VIDEO_DECODER_H

#include "media/gpu_preview/gpu_frame.h"

#include <string>

namespace mvm::gpu {

// 「取れなかった」を 1 つにまとめない。
// EOF と decode error を同じ扱いにすると、壊れた素材が
// 「最後まで再生できた」ことになる (Phase 0 で最も避けたい形の事故)。
enum class DecodeStatus {
    Ok = 0,
    Eof,   // 正常に末尾へ到達した
    Again, // まだ出力が無い。packet を送り続ければよい
    Error, // decode に失敗した。継続してはいけない
};

const char* toString(DecodeStatus s);

// 素材の静的情報。open 後に確定する。
struct VideoStreamInfo {
    int width = 0;
    int height = 0;
    Rational frameRate{0, 1}; // r_frame_rate 相当
    Rational timeBase{0, 1};  // stream の time base
    long long startPts = kNoPts;
    long long frameCount = -1; // 不明なら -1。**0 にしない**
    std::string codecName;
    std::string hwaccelName;
    GpuPixelFormat pixelFormat = GpuPixelFormat::Unknown;
    ColorSpace colorSpace = ColorSpace::Unknown;
    ColorRange colorRange = ColorRange::Unknown;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // UTF-8 の path を受ける。Windows の日本語パスを含む。
    virtual bool open(const std::string& utf8Path, std::string& err) = 0;

    // 次のフレームを 1 枚取り出す。
    // Ok のときだけ out が有効。err は Error のときだけ意味を持つ。
    virtual DecodeStatus requestFrame(DecodedGpuFrame& out, std::string& err) = 0;

    // 指定 frame number 以下の最も近いキーフレームへ飛び、
    // 目標フレームまで decode を進める。
    // 成功後、次の requestFrame は目標フレームを返す。
    virtual bool seek(long long frameNumber, std::string& err) = 0;

    // decoder 内部の状態を捨てる。generation が進む。
    virtual void flush() = 0;

    virtual void close() = 0;

    virtual const VideoStreamInfo& info() const = 0;

    // seek / flush のたびに増える。表示側の stale rejection に使う。
    virtual unsigned long long generation() const = 0;
};

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_VIDEO_DECODER_H
