// lint の negative test 用フィクスチャ。**ビルド対象ではない。**
//
// gpu_preview 層では、decode 結果を CPU へ戻す経路を作ってはいけない
// (P1.1 §9)。例外は marker 帯 / color patch の小領域 readback だけで、
// 実装は nv12_converter.cpp の 1 箇所に限る。
//
// ここには「動いてしまうが zero-copy を壊す」書き方を意図的に置いている。
// これらが lint で落ちなければ、規約は強制されていない。
//
// scripts/lint.ps1 -Path <このディレクトリ> -AsLayer gpu_preview
// が exit 1 になることを CTest が検査する。

#include <d3d11.h>

extern "C" {
struct AVFrame;
int av_hwframe_transfer_data(AVFrame* dst, const AVFrame* src, int flags);
}

// 1) GPU frame を毎回 CPU へ落とす
void mvm_lint_fixture_transfer(AVFrame* dst, const AVFrame* src) {
    av_hwframe_transfer_data(dst, src, 0);
}

// 2) swscale で CPU 変換する
void mvm_lint_fixture_swscale(void);
int sws_scale(void*, const unsigned char* const*, const int*, int, int, unsigned char* const*,
              const int*);

// 3) full-frame の staging readback を作る
void mvm_lint_fixture_staging(ID3D11Device* dev) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920;
    td.Height = 1080;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&td, nullptr, &staging);
}
