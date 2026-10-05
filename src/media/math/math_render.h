#ifndef MVM_MEDIA_MATH_MATH_RENDER_H
#define MVM_MEDIA_MATH_MATH_RENDER_H

// 数式 renderer の backend 中立な契約。
//
// ここには Manim を含め特定の backend の型・設定を出さない。backend は app 層が組み立てて
// MathRenderBackend (fingerprint と render 関数) として渡す。Project の型にも依存しない。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mvm::math {

// 描画結果を変える入力だけ。色・背景・配置は mvm 側で合成するので含めない。
struct MathRenderSpec {
    std::string syntax; // "latex"
    std::string source;
    int fontSize = 0; // 1 em の px
    bool operator==(const MathRenderSpec&) const = default;
};

// backend の toolchain の識別。canonical は "key=value\n" の行の並びで、cache key の材料になる。
struct MathToolchainFingerprint {
    std::string backendId;
    std::string canonical;
    bool operator==(const MathToolchainFingerprint&) const = default;
};

struct MathStaticRenderRequest {
    MathRenderSpec spec;
    // backend が自由に使ってよい作業 directory。呼び出し側が作り、後で消す。
    std::filesystem::path jobDirectory;
    std::chrono::milliseconds timeout{60000};
};

enum class MathRenderStatus {
    Ok,
    InvalidSource,      // 式そのものの誤り (TeX の error など)
    BackendUnavailable, // toolchain が無い・起動できない
    Failed,             // それ以外の失敗
    TimedOut,
    Cancelled,
};

// 白一色の glyph を透過背景に描いた PNG。glyph の tight bbox に padding を足した大きさ。
struct MathStaticRenderResult {
    MathRenderStatus status = MathRenderStatus::Failed;
    std::filesystem::path png;
    int width = 0;
    int height = 0;
    // 利用者に見せる短い説明 (InvalidSource なら TeX の error 行)。
    std::string message;
    // 調査用の詳細 (backend の標準出力・標準エラー)。
    std::string log;
};

using MathRenderFunction = std::function<MathStaticRenderResult(
    const MathStaticRenderRequest& request, const std::atomic<bool>* cancel)>;

// 時間に沿って式を出す animation の種類。
enum class MathAnimationKind { Write };

// 連番の描画結果を変える入力。frames 枚を描き、frame i は進み具合 i / frames
// (0 は何も無い状態、最後の frame は (frames - 1) / frames)。進み具合 1 は静止の描画と同じなので
// 連番には含めない。各 frame の大きさは同じ spec の静止の描画と同じ。
struct MathSequenceSpec {
    MathRenderSpec still;
    MathAnimationKind animation = MathAnimationKind::Write;
    std::int64_t frames = 0;
    bool operator==(const MathSequenceSpec&) const = default;
};

struct MathSequenceRenderRequest {
    MathSequenceSpec spec;
    std::filesystem::path jobDirectory;
    std::chrono::milliseconds timeout{60000};
};

// 白一色の glyph を透過背景に描いた PNG の連番 (frame 0 から順)。
struct MathSequenceRenderResult {
    MathRenderStatus status = MathRenderStatus::Failed;
    std::vector<std::filesystem::path> frames;
    int width = 0;
    int height = 0;
    std::string message;
    std::string log;
};

using MathSequenceRenderFunction = std::function<MathSequenceRenderResult(
    const MathSequenceRenderRequest& request, const std::atomic<bool>* cancel)>;

// Project には入らない値。fingerprint が同じ backend は同じ入力から同じ画素を出す。
struct MathRenderBackend {
    MathToolchainFingerprint fingerprint;
    MathRenderFunction render;
    // 連番の描画 script の識別 (例 "manim-write/1")。静止の key に入れないよう fingerprint と
    // 分けて持つ (連番の描き方を変えても静止の cache を無効にしない)。
    std::string sequenceTemplate;
    // 連番を描けない backend では空。
    MathSequenceRenderFunction renderSequence;
    // renderSequence が描ける最大の枚数 (backend の能力)。超える要求は描かずに未対応として
    // 失敗させる。Project の値の正しさとは別 (Project は時間の意味だけで検証する)。
    std::int64_t maximumSequenceFrames = 0;
};

enum class MathPreflightStatus { Available, Unavailable, Cancelled };

struct MathPreflightResult {
    MathPreflightStatus status = MathPreflightStatus::Unavailable;
    MathRenderBackend backend; // Available のときだけ有効
    std::string message;       // Unavailable の理由 (導入の案内を含む)
};

const char* mathRenderStatusName(MathRenderStatus status);

// cache key (小文字 16 進 64 桁)。次の正準形の SHA-256。失敗 (CNG の失敗) なら空文字列。
//
//   mvm-math-static/1\n
//   syntax=<byte 数>:<syntax>\n
//   source=<byte 数>:<source>\n
//   font_size=<10 進>\n
//   backend=<byte 数>:<backendId>\n
//   toolchain=<byte 数>:<canonical>\n
//
// 値に改行や "=" を含んでも別の入力と同じ正準形にならないよう、文字列は byte 数を前置する。
// 形を変えたら先頭の版を上げる (古い cache を引かないため)。
std::string mathRenderKey(const MathRenderSpec& spec, const MathToolchainFingerprint& toolchain);

const char* mathAnimationKindName(MathAnimationKind kind);

// 連番の cache key (小文字 16 進 64 桁)。静止の key と別の名前空間で、次の正準形の SHA-256。
//
//   mvm-math-sequence/1\n
//   animation=<byte 数>:<write など>\n
//   frames=<10 進>\n
//   syntax=<byte 数>:<syntax>\n
//   source=<byte 数>:<source>\n
//   font_size=<10 進>\n
//   backend=<byte 数>:<backendId>\n
//   toolchain=<byte 数>:<canonical>\n
//   sequence_template=<byte 数>:<sequenceTemplate>\n
//
// 色・背景・ClipEffects・clip の尺・timeline の fps は含めない (静止の key と同じ方針)。
std::string mathSequenceKey(const MathSequenceSpec& spec, const MathToolchainFingerprint& toolchain,
                            const std::string& sequenceTemplate);

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_RENDER_H
