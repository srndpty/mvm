/*
 * mvm - フレーム固有マーカーの読み取り
 *
 * 仕様: docs/research/test-media-format.md
 *   セル幅 64px、帯 (0,0)-(1216,64)。
 *   cell0=白(同期) cell1=黒(同期) cell2..17=16bit LSB first cell18=白(同期)
 * OCR は使わない。セル中心の輝度を閾値判定するだけ。
 *
 * Phase 0 の検証ハーネス (tests/harness/bench_common.h) と
 * Phase 1 の GPU preview スパイク (src/media/gpu_preview) の両方が使う。
 * 「要求したフレームが本当に取り出せたか」の判定はこの 1 実装だけに置く。
 * 2 箇所に書くと、実装が食い違ったときに「片方だけ通る」形で表面化する。
 *
 * 依存は標準ライブラリだけ。MLT にも Qt にも FFmpeg にも libpng にも依存しない。
 */

#ifndef MVM_MARKER_H
#define MVM_MARKER_H

#include <algorithm>
#include <vector>

namespace mvm::marker {

constexpr int kCellSize = 64;
constexpr int kCellCount = 19;
constexpr int kDataCells = 16;

struct MarkerRead {
    bool syncOk = false;
    long long value = -1;
    int lumaMin = 255, lumaMax = 0;
};

// セル幅を明示して読む版。
//
// マーカーは解像度に関係なく **64px セルの固定ピクセル**で焼き込まれている
// (docs/research/test-media-format.md)。したがって縮小した proxy では
// セル幅も縮む。960x540 の proxy は 3840x2160 の 1/4 なのでセル幅 16px になる。
//
// 既定の 64px 決め打ちで proxy を読むと `w < 1216` で必ず読めず、
// 「proxy のマーカーが壊れている」と誤って結論することになる。
inline MarkerRead readMarkerWithCellSize(const unsigned char* rgba, int w, int h, int cellSize) {
    MarkerRead r;
    if (!rgba || cellSize < 4 || w < cellSize * kCellCount || h < cellSize)
        return r;

    // 標本はセル幅に比例させる。セル中心の半分の幅を平均する。
    const int radius = std::max(2, cellSize / 4);

    auto cellLuma = [&](int cell) -> int {
        const int cx = cell * cellSize + cellSize / 2;
        const int cy = cellSize / 2;
        long sum = 0;
        int n = 0;
        for (int y = cy - radius; y < cy + radius; y++) {
            for (int x = cx - radius; x < cx + radius; x++) {
                const size_t offset =
                    ((size_t)(unsigned)y * (size_t)(unsigned)w + (size_t)(unsigned)x) * 4u;
                const unsigned char* px = rgba + offset;
                // BT.601 luma
                sum += (long)(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
                n++;
            }
        }
        return n ? (int)(sum / n) : 0;
    };

    std::vector<int> luma((size_t)kCellCount);
    for (int c = 0; c < kCellCount; c++) {
        luma[(size_t)c] = cellLuma(c);
        r.lumaMin = std::min(r.lumaMin, luma[(size_t)c]);
        r.lumaMax = std::max(r.lumaMax, luma[(size_t)c]);
    }

    // 同期セルで閾値の妥当性を確認する。
    // ここが崩れていれば読み取り位置がずれているか、素材が別物である。
    const int threshold = 128;
    bool sync = (luma[0] > threshold) && (luma[1] < threshold) &&
                (luma[(size_t)kCellCount - 1] > threshold);

    // さらに「19 セルすべてが白か黒に振り切れていること」を要求する。
    //
    // なぜ必要か:
    //   セル幅を間違えて読むと、マーカー帯の外側 (映像本体) を
    //   セルとして読んでしまう。同期 3 セルだけの検査では、
    //   たまたま明暗が合致したときに **偽の同期**が成立する。
    //   実際に 4K を 1080p profile へ入れた合成フレームで、
    //   64px 決め打ちの読み取りが偽同期し、frame 0 を 64512 と読んだ。
    //
    //   マーカーは純白と純黒で焼かれているので、正しいセル幅なら
    //   すべてのセルが振り切れる。映像本体は中間調が混ざるので落ちる。
    const int kWhite = 200;
    const int kBlack = 55;
    bool saturated = true;
    for (int c = 0; c < kCellCount; c++) {
        if (luma[(size_t)c] < kWhite && luma[(size_t)c] > kBlack) {
            saturated = false;
            break;
        }
    }
    sync = sync && saturated;
    r.syncOk = sync;
    if (!sync)
        return r;

    long long value = 0;
    for (int b = 0; b < kDataCells; b++) {
        if (luma[(size_t)(2 + b)] > threshold)
            value |= (1LL << b);
    }
    r.value = value;
    return r;
}

// 元素材と同じ 64px セルで読む。等倍の素材はこちらを使う。
inline MarkerRead readMarker(const unsigned char* rgba, int w, int h) {
    return readMarkerWithCellSize(rgba, w, h, kCellSize);
}

// 縮小された素材を読む。元の幅からセル幅を割り出す。
// 例: 3840 -> 960 なら 64 * 960 / 3840 = 16px セル。
inline MarkerRead readMarkerScaled(const unsigned char* rgba, int w, int h, int originalWidth) {
    if (originalWidth <= 0)
        return MarkerRead{};
    int cell = (int)((long long)kCellSize * w / originalWidth);
    return readMarkerWithCellSize(rgba, w, h, cell);
}

// セル幅を自動判定して読む。
//
// なぜ必要か:
//   合成後のフレームでは、素材の解像度と profile の解像度が違うと
//   マーカーも一緒に拡縮される。3840x2160 の素材を 1920x1080 の profile へ
//   入れると 0.5 倍になり、64px セルは 32px になる。
//   960x540 の proxy を 1920x1080 へ入れると 2 倍になり、やはり 32px である。
//
//   64px 決め打ちで読むと、この場合 **全フレームが不一致**になる。
//   実際に 4K scenario の seek 計測で 314/314 が不一致になり、
//   「seek 精度が壊れた」ように見えた。原因は計測側だった。
//
// 同期セル (白・黒・…・白) が一致した候補だけを採用するので、
// 当てずっぽうで値を読むことにはならない。どれも一致しなければ syncOk=false。
inline MarkerRead readMarkerAuto(const unsigned char* rgba, int w, int h) {
    // 大きい方から試す。小さいセル幅は誤検出しやすいので後回しにする。
    static const int kCandidates[] = {64, 48, 32, 24, 16, 12, 8};
    for (int cell : kCandidates) {
        if (w < cell * kCellCount || h < cell)
            continue;
        MarkerRead r = readMarkerWithCellSize(rgba, w, h, cell);
        if (r.syncOk)
            return r;
    }
    return MarkerRead{};
}

} // namespace mvm::marker

#endif // MVM_MARKER_H
