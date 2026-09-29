import QtQuick
import QtTest
import "../../apps/mvm/PreviewTransform.js" as Transform

// プレビュー上で素材を動かす・拡縮するときの計算 (apps/mvm/PreviewTransform.js)。
// 期待値は手で計算した値で、実装の式を呼んで作らない。
TestCase {
    name: "PreviewTransform"

    // 1920x1080 の出力に、(100, 100, 400, 200) の素材と、別の素材 (1000, 600, 300, 300)。
    readonly property var canvasLines: Transform.snapLines(1920, 1080, [])
    readonly property var otherLines: Transform.snapLines(1920, 1080,
                                                          [{ "x": 1000, "y": 600, "width": 300, "height": 300 }])
    readonly property var rect: ({ "x": 100, "y": 100, "width": 400, "height": 200 })

    function compareRect(actual, x, y, width, height, message) {
        fuzzyCompare(actual.x, x, 1e-9, message + " x");
        fuzzyCompare(actual.y, y, 1e-9, message + " y");
        fuzzyCompare(actual.width, width, 1e-9, message + " width");
        fuzzyCompare(actual.height, height, 1e-9, message + " height");
    }

    function test_snapLinesIncludeCanvasAndOtherClips() {
        compare(canvasLines.xs, [0, 960, 1920]);
        compare(canvasLines.ys, [0, 540, 1080]);
        compare(otherLines.xs, [0, 960, 1920, 1000, 1150, 1300]);
        compare(otherLines.ys, [0, 540, 1080, 600, 750, 900]);
    }

    // 中央が画面中央 (960) の 4px 手前に来る移動は、画面中央へ吸着する。
    function test_moveSnapsCenterToCanvasCenter() {
        const snapped = Transform.snapMove(rect, 656, 0, canvasLines, 8, true);
        compare(snapped.dx, 660);
        compare(snapped.guidesX, [960]);
    }

    // 左端が画面の左端 (0) へ、下端が他の素材の上端 (600) へ吸着する。
    function test_moveSnapsEdgesToCanvasAndOtherClip() {
        const snapped = Transform.snapMove(rect, -95, 297, otherLines, 8, true);
        compare(snapped.dx, -100);
        compare(snapped.dy, 300);
        compare(snapped.guidesX, [0]);
        compare(snapped.guidesY, [600]);
    }

    // 負例: 閾値の外は吸着しない。Ctrl (enabled = false) なら閾値内でも吸着しない。
    function test_moveDoesNotSnapOutsideThresholdOrWithCtrl() {
        const far = Transform.snapMove(rect, 640, 0, canvasLines, 8, true);
        compare(far.dx, 640);
        compare(far.guidesX, []);
        const free = Transform.snapMove(rect, 656, 0, canvasLines, 8, false);
        compare(free.dx, 656);
        compare(free.guidesX, []);
        const invalid = Transform.snapMove(rect, NaN, 5, canvasLines, 8, true);
        compare(invalid.dx, 0);
        compare(invalid.dy, 0);
    }

    // 既定は反対側の角・辺を固定する。8 つのハンドルすべて。
    function test_resizeKeepsOppositeSideFixed() {
        const plain = { "keepAspect": false, "fromCenter": false };
        compareRect(Transform.resizeRect(rect, "br", { "x": 600, "y": 400 }, plain, 1),
                    100, 100, 500, 300, "br");
        compareRect(Transform.resizeRect(rect, "tl", { "x": 50, "y": 20 }, plain, 1),
                    50, 20, 450, 280, "tl");
        compareRect(Transform.resizeRect(rect, "tr", { "x": 700, "y": 50 }, plain, 1),
                    100, 50, 600, 250, "tr");
        compareRect(Transform.resizeRect(rect, "bl", { "x": 0, "y": 500 }, plain, 1),
                    0, 100, 500, 400, "bl");
        // 辺は掴んだ軸だけが変わる。
        compareRect(Transform.resizeRect(rect, "r", { "x": 800, "y": 999 }, plain, 1),
                    100, 100, 700, 200, "r");
        compareRect(Transform.resizeRect(rect, "l", { "x": 200, "y": -50 }, plain, 1),
                    200, 100, 300, 200, "l");
        compareRect(Transform.resizeRect(rect, "t", { "x": 0, "y": 0 }, plain, 1),
                    100, 0, 400, 300, "t");
        compareRect(Transform.resizeRect(rect, "b", { "x": 0, "y": 250 }, plain, 1),
                    100, 100, 400, 150, "b");
    }

    // Alt: 中心 (300, 200) を固定して対称に伸縮する。
    function test_resizeFromCenterIsSymmetric() {
        const centered = { "keepAspect": false, "fromCenter": true };
        compareRect(Transform.resizeRect(rect, "r", { "x": 600, "y": 0 }, centered, 1),
                    0, 100, 600, 200, "r from center");
        compareRect(Transform.resizeRect(rect, "br", { "x": 350, "y": 260 }, centered, 1),
                    250, 140, 100, 120, "br from center");
    }

    // Ctrl: 縦横比 (2:1) を保つ。角は大きく変わった軸に合わせ、辺はもう一方を中心で伸縮する。
    function test_resizeKeepsAspect() {
        const aspect = { "keepAspect": true, "fromCenter": false };
        compareRect(Transform.resizeRect(rect, "br", { "x": 900, "y": 350 }, aspect, 1),
                    100, 100, 800, 400, "br aspect");
        compareRect(Transform.resizeRect(rect, "tl", { "x": 300, "y": 0 }, aspect, 1),
                    -100, 0, 600, 300, "tl aspect");
        compareRect(Transform.resizeRect(rect, "r", { "x": 300, "y": 0 }, aspect, 1),
                    100, 150, 200, 100, "r aspect");
        const both = { "keepAspect": true, "fromCenter": true };
        compareRect(Transform.resizeRect(rect, "r", { "x": 700, "y": 0 }, both, 1),
                    -100, 0, 800, 400, "r aspect from center");
    }

    // 負例: 反対側を越えて裏返さず、最小の大きさで止まる。pointer が NaN なら変えない。
    function test_resizeDoesNotFlip() {
        const plain = { "keepAspect": false, "fromCenter": false };
        compareRect(Transform.resizeRect(rect, "r", { "x": -500, "y": 0 }, plain, 4),
                    100, 100, 4, 200, "r past left");
        compareRect(Transform.resizeRect(rect, "t", { "x": 0, "y": 900 }, plain, 4),
                    100, 296, 400, 4, "t past bottom");
        compareRect(Transform.resizeRect(rect, "l", { "x": 5000, "y": 0 },
                                         { "keepAspect": false, "fromCenter": true }, 4),
                    298, 100, 4, 200, "l past center");
        compareRect(Transform.resizeRect(rect, "br", { "x": NaN, "y": 5 }, plain, 4),
                    100, 100, 400, 200, "NaN pointer");
    }

    // 拡縮の吸着は掴んだ辺だけを動かす。右端 954 は画面中央 960 へ吸着し、左端は動かない。
    function test_resizeSnapsOnlyDraggedEdge() {
        const plain = { "keepAspect": false, "fromCenter": false };
        const resized = Transform.resizeRect(rect, "r", { "x": 954, "y": 0 }, plain, 1);
        const snapped = Transform.snapResize(rect, resized, "r", canvasLines, 8, plain, 1);
        compareRect(snapped.rect, 100, 100, 860, 200, "r snapped");
        compare(snapped.guidesX, [960]);
        compare(snapped.guidesY, []);
        // Alt なら反対側も同じだけ動く (中心 300 を保つ)。
        const centered = { "keepAspect": false, "fromCenter": true };
        const wide = Transform.resizeRect(rect, "r", { "x": 954, "y": 0 }, centered, 1);
        const centeredSnap = Transform.snapResize(rect, wide, "r", canvasLines, 8, centered, 1);
        compareRect(centeredSnap.rect, -360, 100, 1320, 200, "r snapped from center");
    }

    // Ctrl で比率を保つときは、近い方の軸だけを吸着させてもう一方は比率で決める。
    function test_resizeSnapKeepsAspect() {
        const aspect = { "keepAspect": true, "fromCenter": false };
        const resized = Transform.resizeRect(rect, "br", { "x": 1916, "y": 0 }, aspect, 1);
        const snapped = Transform.snapResize(rect, resized, "br", canvasLines, 8, aspect, 1);
        compareRect(snapped.rect, 100, 100, 1820, 910, "br aspect snapped");
        compare(snapped.guidesX, [1920]);
    }

    // 負例: 閾値の外なら拡縮は吸着しない。辺が無い軸 (右辺の y) は吸着しない。
    function test_resizeSnapOutsideThreshold() {
        const plain = { "keepAspect": false, "fromCenter": false };
        const resized = Transform.resizeRect(rect, "r", { "x": 940, "y": 0 }, plain, 1);
        const snapped = Transform.snapResize(rect, resized, "r", canvasLines, 8, plain, 1);
        compareRect(snapped.rect, 100, 100, 840, 200, "r not snapped");
        compare(snapped.guidesX, []);
        const nearY = { "x": 100, "y": 536, "width": 400, "height": 200 };
        const edgeOnly = Transform.snapResize(nearY, nearY, "r", canvasLines, 8, plain, 1);
        compare(edgeOnly.guidesY, []);
    }

    // 回転した矩形の外接矩形 (吸着の相手・自分に使う)。90° なら縦横が入れ替わる。
    function test_rotatedBounds() {
        compareRect(Transform.rotatedBounds(rect, 300, 200, 90), 200, 0, 200, 400, "90 deg");
        compareRect(Transform.rotatedBounds(rect, 300, 200, 0), 100, 100, 400, 200, "0 deg");
    }

    // 90° 回した (100, 100, 400, 200) (中心 (300, 200)) の右辺を、画面上で (300, 500) まで引く。
    // 素材の座標では右辺が 600 になり、幅 500。固定する左辺の中点は画面上の (300, 0) のまま。
    function test_resizeRotatedKeepsAnchorOnScreen() {
        const plain = { "keepAspect": false, "fromCenter": false };
        const resized = Transform.resizeRotatedRect(rect, 300, 200, 90, "r",
                                                    { "x": 300, "y": 500 }, plain, 1);
        compareRect(resized, 50, 150, 500, 200, "r rotated 90");
        // 画面上の位置を独立に確かめる: 新しい中心 (矩形の中央) の周りに 90° 回す。
        const pivotX = resized.x + resized.width / 2;
        const pivotY = resized.y + resized.height / 2;
        const anchor = Transform.rotatePoint(resized.x, pivotY, pivotX, pivotY, 90);
        fuzzyCompare(anchor.x, 300, 1e-9, "anchor x");
        fuzzyCompare(anchor.y, 0, 1e-9, "anchor y");
        const dragged = Transform.rotatePoint(resized.x + resized.width, pivotY, pivotX, pivotY, 90);
        fuzzyCompare(dragged.x, 300, 1e-9, "dragged x");
        fuzzyCompare(dragged.y, 500, 1e-9, "dragged y");
    }

    // 回転 0 なら通常の拡縮と同じ。pointer が NaN なら変えない。
    function test_resizeRotatedWithoutRotation() {
        const plain = { "keepAspect": false, "fromCenter": false };
        compareRect(Transform.resizeRotatedRect(rect, 300, 200, 0, "br", { "x": 600, "y": 400 },
                                                plain, 1),
                    100, 100, 500, 300, "br 0 deg");
        compareRect(Transform.resizeRotatedRect(rect, 300, 200, 30, "br", { "x": NaN, "y": 0 },
                                                plain, 1),
                    100, 100, 400, 200, "NaN pointer");
    }

    // 非対称な crop では回転の中心 (200, 150) が見えている矩形の中心 (300, 200) と違う。
    // Alt はその中心を固定する: 右辺を 800 へ引くと倍率 2 (中心から右辺まで 300 -> 600)。
    function test_resizeFromCenterKeepsOffCenterPivot() {
        const centered = { "keepAspect": false, "fromCenter": true, "pivot": { "x": 200, "y": 150 } };
        compareRect(Transform.resizeRect(rect, "r", { "x": 800, "y": 0 }, centered, 1),
                    0, 100, 800, 200, "r about pivot");
        // 吸着でも固定点との比を保つ: 右辺 1916 -> 1920 (中心から 1720 = 倍率 1720/300)。
        const wide = Transform.resizeRect(rect, "r", { "x": 1916, "y": 0 }, centered, 1);
        const snapped = Transform.snapResize(rect, wide, "r", canvasLines, 8, centered, 1);
        const scale = 1720 / 300;
        compareRect(snapped.rect, 200 - 100 * scale, 100, 1720 + 100 * scale, 200,
                    "r snapped about pivot");
    }

    // 回転 30° + 回転の中心が矩形の中心から外れている + Alt。回転の中心は画面上で動かない。
    function test_resizeRotatedFromCenterKeepsPivotOnScreen() {
        const pivotX = 200, pivotY = 150, degrees = 30;
        const pointer = Transform.rotatePoint(800, 150, pivotX, pivotY, degrees);
        const alt = { "keepAspect": false, "fromCenter": true };
        const resized = Transform.resizeRotatedRect(rect, pivotX, pivotY, degrees, "r", pointer,
                                                    alt, 1);
        compareRect(resized, 0, 100, 800, 200, "r rotated about pivot");
        // 新しい回転の中心 (矩形の中の同じ割合 1/4, 1/4) が元の中心と同じ点。
        fuzzyCompare(resized.x + resized.width / 4, pivotX, 1e-9, "pivot x");
        fuzzyCompare(resized.y + resized.height / 4, pivotY, 1e-9, "pivot y");
        // Ctrl+Alt: 縦横比を保ち、回転の中心を固定する。
        const both = { "keepAspect": true, "fromCenter": true };
        const scaled = Transform.resizeRotatedRect(rect, pivotX, pivotY, degrees, "r", pointer,
                                                   both, 1);
        compareRect(scaled, 0, 50, 800, 400, "r rotated keep aspect about pivot");
        fuzzyCompare(scaled.x + scaled.width / 4, pivotX, 1e-9, "aspect pivot x");
        fuzzyCompare(scaled.y + scaled.height / 4, pivotY, 1e-9, "aspect pivot y");
    }

    // Ctrl だけ (Alt なし) では回転の中心を固定しない。幅 1.5 倍に合わせた高さは、見えている
    // 矩形の中心 (y = 200) を保って伸びる。回転の中心 (y = 150) を保つと 25px 動いてしまう。
    function test_keepAspectWithoutAltIgnoresPivot() {
        const ctrl = { "keepAspect": true, "fromCenter": false, "pivot": { "x": 200, "y": 150 } };
        compareRect(Transform.resizeRect(rect, "r", { "x": 700, "y": 0 }, ctrl, 1),
                    100, 50, 600, 300, "r ctrl only");
        // 吸着でも同じ (右辺 1916 -> 1920)。
        const wide = Transform.resizeRect(rect, "r", { "x": 1916, "y": 0 }, ctrl, 1);
        const snapped = Transform.snapResize(rect, wide, "r", canvasLines, 8, ctrl, 1);
        compareRect(snapped.rect, 100, 200 - 910 / 2, 1820, 910, "r ctrl only snapped");
    }

    // 回転の中心が見えている矩形の外 (左側の余白を含む crop) にあっても、Alt の拡縮は潰れない。
    // 中心 x = 50 から左辺 (100) までの距離 50 が 100 になるように左辺を 150 へ引くと倍率 2。
    function test_resizeFromCenterWithPivotOutsideRect() {
        const alt = { "keepAspect": false, "fromCenter": true, "pivot": { "x": 50, "y": 150 } };
        compareRect(Transform.resizeRect(rect, "l", { "x": 150, "y": 0 }, alt, 1),
                    150, 100, 800, 200, "l with pivot outside");
        compareRect(Transform.resizeRect(rect, "r", { "x": 950, "y": 0 }, alt, 1),
                    150, 100, 800, 200, "r with pivot outside");
    }

    function test_thresholdScalesToOutputPixels() {
        compare(Transform.thresholdOutputPx(8, 1920, 960), 16);
        compare(Transform.thresholdOutputPx(8, 1920, 0), 8);
    }
}
