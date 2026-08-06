/*
 * mvm Phase 1 / P1 - GPU preview の境界となるフレーム記述
 *
 * ここは decoder と surface の間の**唯一の受け渡し形式**である。
 * FFmpeg の型 (AVFrame / AVPixelFormat / AVRational) も
 * Qt の型 (QRhi / QQuickItem) も、この構造体には出さない。
 * 出すと、片方を差し替えるたびにもう片方が壊れる。
 *
 * 依存は d3d11.h と標準ライブラリだけ。
 */

#ifndef MVM_GPU_PREVIEW_GPU_FRAME_H
#define MVM_GPU_PREVIEW_GPU_FRAME_H

#include <cstdint>
#include <d3d11.h>
#include <memory>

namespace mvm::gpu {

// PTS の time base や fps を有理数のまま持つ。
// double へ落とすと 60000/1001 で長尺の後半に 1 frame ずれる。
struct Rational {
    long long num = 0;
    long long den = 1;

    constexpr bool valid() const { return num > 0 && den > 0; }
};

// decode 出力の画素形式。P1 が扱うのはこの 2 つだけ。
// software 形式は**意図的に持たない**。持つと fallback を書きたくなる。
enum class GpuPixelFormat {
    Unknown = 0,
    NV12, // 8bit  4:2:0 (DXGI_FORMAT_NV12)
    P010, // 10bit 4:2:0 (DXGI_FORMAT_P010)
};

enum class ColorSpace {
    Unknown = 0,
    BT601,
    BT709,
    BT2020NCL, // non-constant luminance
};

enum class ColorRange {
    Unknown = 0,
    Limited, // 16..235 (Y) / 16..240 (C)
    Full,    // 0..255
};

// 「PTS が無い」を 0 で表さない。0 は正当な PTS である。
inline constexpr long long kNoPts = INT64_MIN;

// --------------------------------------------------------------------------
// 世代を表す 3 つの概念 (P1.2 §2)
// --------------------------------------------------------------------------
// P1.1 では compositionEpoch と sourceGeneration を 1 つの GenerationId に
// まとめ、decoder が **両方**を発行していた。これは誤りだった。
//
//   - decoder は「自分の pool を作り直した」ことしか知らない
//   - 「合成の構成が変わった」ことを知っているのは compositor だけ
//
// 1 本の decoder しかないうちは同じ値で困らないが、P2 で source が 2 本になると
// **source A の open が source B のフレームを future/stale にしてしまう。**
// 先に分けておく。
//
// 3 つの所有者:
//   ResourceEpoch     : decoder が open ごとに発行する (decode pool / texture / SRV の世代)
//   SourceGeneration  : decoder が seek / flush ごとに発行する (source 単位)
//   CompositionEpoch  : **compositor が発行する** (timeline / 合成構成の世代)
//
// **decoder は CompositionEpoch を発行してはいけない。**
// P1.2 では compositor がまだ無いので、preview 層が暫定的に所有する。
//
// 素の unsigned long long を使わず別型にしているのは、取り違えを型で止めるため。
// 実際 P1.1 では resourceEpoch を compositionEpoch として渡していた。

// どの source から来たフレームか。P1.2 では 1 本だが、値は決め打ちにしない。
struct SourceId {
    unsigned long long value = 0;

    friend bool operator==(SourceId a, SourceId b) { return a.value == b.value; }

    friend bool operator!=(SourceId a, SourceId b) { return !(a == b); }

    friend bool operator<(SourceId a, SourceId b) { return a.value < b.value; }
};

// decode pool / texture / SRV の世代。decoder が open ごとに発行する。
// SRV cache の key に使う (texture のアドレスは pool 解放後に再利用されるため)。
struct ResourceEpoch {
    unsigned long long value = 0;

    friend bool operator==(ResourceEpoch a, ResourceEpoch b) { return a.value == b.value; }

    friend bool operator!=(ResourceEpoch a, ResourceEpoch b) { return !(a == b); }

    friend bool operator<(ResourceEpoch a, ResourceEpoch b) { return a.value < b.value; }
};

// source 単位の seek / flush 世代。stale / future の判定に使う。
struct SourceGeneration {
    unsigned long long value = 0;

    friend bool operator==(SourceGeneration a, SourceGeneration b) { return a.value == b.value; }

    friend bool operator!=(SourceGeneration a, SourceGeneration b) { return !(a == b); }

    friend bool operator<(SourceGeneration a, SourceGeneration b) { return a.value < b.value; }

    friend bool operator>(SourceGeneration a, SourceGeneration b) { return b < a; }
};

// 合成構成の世代。**compositor が発行する。** decoder は発行しない。
struct CompositionEpoch {
    unsigned long long value = 0;

    friend bool operator==(CompositionEpoch a, CompositionEpoch b) { return a.value == b.value; }

    friend bool operator!=(CompositionEpoch a, CompositionEpoch b) { return !(a == b); }

    friend bool operator<(CompositionEpoch a, CompositionEpoch b) { return a.value < b.value; }
};

// --------------------------------------------------------------------------
// lifetime token
// --------------------------------------------------------------------------
// decode texture は FFmpeg の pool 内の array texture の 1 slice である。
// AVFrame の参照を落とした瞬間、**同じ slice へ次のフレームが decode されうる**。
// したがって「表示に使い終わるまで AVFrame を持ち続ける」ことが
// 正しさの条件であり、最適化ではない。
//
// 具体的な解放処理は decoder 側が deleter として仕込む。
// この層は「生きているかどうか」しか知らない。
using FrameLifetimeToken = std::shared_ptr<void>;

// --------------------------------------------------------------------------
// DecodedGpuFrame
// --------------------------------------------------------------------------
struct DecodedGpuFrame {
    long long frameNumber = -1;
    long long pts = kNoPts;
    Rational timeBase{0, 1};

    int width = 0;
    int height = 0;
    GpuPixelFormat pixelFormat = GpuPixelFormat::Unknown;

    // decode 結果そのもの。CPU へ落とさない。
    ID3D11Texture2D* texture = nullptr;
    // texture array のどの要素か。D3D11 の subresource index と同じ意味で使う
    // (mip level は常に 1 なので subresource == arrayIndex)。
    unsigned int arrayIndex = 0;

    ColorSpace colorSpace = ColorSpace::Unknown;
    ColorRange colorRange = ColorRange::Unknown;
    // metadata が未指定で解像度から推定したか。推定を黙って確定値にしない。
    bool colorSpaceInferred = false;
    bool colorRangeInferred = false;

    FrameLifetimeToken lifetime;

    // どの source の、どの世代の、どの resource 世代のフレームか。
    //
    // **compositionEpoch は持たない。** 合成構成の世代を知っているのは
    // compositor であり、decoder ではない (P1.2 §2)。
    // P2 で compositor がフレームを受け取った時点で付与する。
    SourceId sourceId{};
    SourceGeneration sourceGeneration{};
    ResourceEpoch resourceEpoch{};

    bool valid() const {
        return texture != nullptr && width > 0 && height > 0 &&
               pixelFormat != GpuPixelFormat::Unknown && frameNumber >= 0 &&
               static_cast<bool>(lifetime);
    }
};

const char* toString(GpuPixelFormat f);
const char* toString(ColorSpace c);
const char* toString(ColorRange r);

} // namespace mvm::gpu

#endif // MVM_GPU_PREVIEW_GPU_FRAME_H
