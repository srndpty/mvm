#ifndef MVM_CORE_LAYER_PLACEMENT_H
#define MVM_CORE_LAYER_PLACEMENT_H

namespace mvm::core {

// 出力 canvas を 0..1 に正規化した矩形。
struct LayerRect {
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
};

// 素材 1 枚を canvas へ置いた結果 (いずれも正規化座標)。
//   destination : 描く矩形
//   sourceUv    : 素材 frame の中で描く範囲 (frame を 0..1 とした座標)
//   pivotX/Y    : 回転の中心 (crop 範囲を置いた矩形の中心)
//   empty       : crop が素材の外だけを指していて、何も描かない
struct LayerPlacement {
    LayerRect destination;
    LayerRect sourceUv;
    double pivotX = 0.5;
    double pivotY = 0.5;
    bool empty = true;
};

// preview と書き出しで同じ置き方にするための幾何。書き出し (MLT) と同じ順で決める。
//   1. 素材 frame を比率を保って canvas の中央へ収める (letterbox)
//   2. canvas 上の crop 範囲で切り抜く (letterbox の余白は透明のまま)
//   3. crop 範囲を destination へ写す。縦横の倍率は別々でよい (引き伸ばす)
// frame / canvas の寸法が正でない、または crop / destination の幅・高さが正でなければ empty。
LayerPlacement placeLayer(int frameWidth, int frameHeight, int canvasWidth, int canvasHeight,
                          const LayerRect& crop, const LayerRect& destination);

} // namespace mvm::core

#endif // MVM_CORE_LAYER_PLACEMENT_H
