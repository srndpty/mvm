#ifndef MVM_MEDIA_MATH_MATH_RENDER_H
#define MVM_MEDIA_MATH_MATH_RENDER_H

// 数式 renderer の backend 中立な契約。
//
// ここには Manim を含め特定の backend の型・設定を出さない。backend は app 層が組み立てて
// MathRenderBackend (fingerprint と render 関数) として渡す。Project の型にも依存しない。

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

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

// Project には入らない値。fingerprint が同じ backend は同じ入力から同じ画素を出す。
struct MathRenderBackend {
    MathToolchainFingerprint fingerprint;
    MathRenderFunction render;
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

} // namespace mvm::math

#endif // MVM_MEDIA_MATH_MATH_RENDER_H
