// lint の negative test の対照群。**ビルド対象ではない。**
//
// 「印がある file の、1 箇所だけの小領域 readback」は許される。
// これが落ちるなら、禁止検査が広すぎて正当な実装まで塞いでいる。
//
// MVM_ALLOW_SMALL_REGION_READBACK
//
// 印があり、readback を構成する token がそれぞれ 1 箇所しかない。
// 2 箇所目を足すと lint は落ちる (それが狙いである)。

#include <d3d11.h>

void mvm_lint_fixture_small_readback(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // marker 帯相当の 1216x64 だけを読む。full frame ではない。
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1216;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)))
        return;

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        ctx->Unmap(staging, 0);
    staging->Release();
}
