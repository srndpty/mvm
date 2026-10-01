import QtQuick
import QtTest
import "../../apps/mvm/TransitionEditorMath.js" as SpanMath

// エフェクトコントロールでトランジションの長さ・配置・ドラッグから cut 前後の frame 数を
// 求める計算の検査。期待値は手で数えた値である (実装の式を呼ばない)。
TestCase {
    name: "TransitionEditorMath"

    function test_alignmentOf() {
        compare(SpanMath.alignmentOf(0, 40), "start");
        compare(SpanMath.alignmentOf(40, 0), "end");
        compare(SpanMath.alignmentOf(30, 30), "center");
        // 奇数の長さの中央は 1 frame ずれる。
        compare(SpanMath.alignmentOf(30, 31), "center");
        compare(SpanMath.alignmentOf(31, 30), "center");
        compare(SpanMath.alignmentOf(10, 50), "custom");
    }

    // 余白は前 300 / 後 300。
    function test_spanForDurationKeepsAlignment() {
        compare(SpanMath.spanForDuration(40, "center", 30, 30, 300, 300), { "before": 20, "after": 20 });
        compare(SpanMath.spanForDuration(41, "center", 30, 30, 300, 300), { "before": 20, "after": 21 });
        compare(SpanMath.spanForDuration(40, "start", 0, 60, 300, 300), { "before": 0, "after": 40 });
        compare(SpanMath.spanForDuration(40, "end", 60, 0, 300, 300), { "before": 40, "after": 0 });
        // カスタムは cut の前の割合 (10 : 50 = 1/6) を保つ。120 の 1/6 は 20。
        compare(SpanMath.spanForDuration(120, "custom", 10, 50, 300, 300), { "before": 20, "after": 100 });
        // 小数の入力は frame に丸め、0 以下は 1 frame にする。
        compare(SpanMath.spanForDuration(40.4, "start", 0, 60, 300, 300), { "before": 0, "after": 40 });
        compare(SpanMath.spanForDuration(0, "start", 0, 60, 300, 300), { "before": 0, "after": 1 });
    }

    // 片側の余白が足りなければもう片側へ寄せ、合計は両側の上限の和で止める。
    function test_spanForDurationShiftsToOtherSide() {
        // 後ろの余白 10: 中央 60 は 30/30 にならず 50/10。
        compare(SpanMath.spanForDuration(60, "center", 30, 30, 300, 10), { "before": 50, "after": 10 });
        // cut で開始でも後ろが 10 しか無ければ前へ溢れる。
        compare(SpanMath.spanForDuration(60, "start", 0, 10, 300, 10), { "before": 50, "after": 10 });
        // 合計は 20 + 10 = 30 まで。
        compare(SpanMath.spanForDuration(100, "center", 5, 5, 20, 10), { "before": 20, "after": 10 });
    }

    function test_spanForAlignmentKeepsDuration() {
        compare(SpanMath.spanForAlignment("start", 30, 30, 300, 300), { "before": 0, "after": 60 });
        compare(SpanMath.spanForAlignment("end", 30, 30, 300, 300), { "before": 60, "after": 0 });
        compare(SpanMath.spanForAlignment("center", 0, 60, 300, 300), { "before": 30, "after": 30 });
    }

    // 前 30 / 後 30、余白は前 40 / 後 35。
    function test_spanForDragEdges() {
        // 左端を左へ 5: 開始が早まり前が 35。
        compare(SpanMath.spanForDrag("left", -5, 30, 30, 40, 35), { "before": 35, "after": 30 });
        // 左へ 50 でも前の余白 40 で止まる。
        compare(SpanMath.spanForDrag("left", -50, 30, 30, 40, 35), { "before": 40, "after": 30 });
        // 左端を cut の先まで右へ寄せても前は 0 (後ろがあるので合計 1 以上)。
        compare(SpanMath.spanForDrag("left", 100, 30, 30, 40, 35), { "before": 0, "after": 30 });
        compare(SpanMath.spanForDrag("right", 3, 30, 30, 40, 35), { "before": 30, "after": 33 });
        compare(SpanMath.spanForDrag("right", 50, 30, 30, 40, 35), { "before": 30, "after": 35 });
        // 前が 0 なら右端は 1 frame 残す。
        compare(SpanMath.spanForDrag("right", -100, 0, 30, 40, 35), { "before": 0, "after": 1 });
        // 小数の移動は frame に丸める。
        compare(SpanMath.spanForDrag("right", 2.6, 30, 30, 40, 35), { "before": 30, "after": 33 });
    }

    // A / B はリップルトリム、cut 線はローリング。ドラッグ中は掴んだ端がマウスに付いてくる。
    function test_edgeEditFor() {
        compare(SpanMath.edgeEditFor("rippleA"),
                { "side": "outgoing", "edge": "right", "tool": "ripple",
                  "outgoingEndShift": 1, "incomingStartShift": 1, "incomingEndShift": 1 });
        compare(SpanMath.edgeEditFor("rippleB"),
                { "side": "incoming", "edge": "left", "tool": "ripple",
                  "outgoingEndShift": 0, "incomingStartShift": 1, "incomingEndShift": 0 });
        compare(SpanMath.edgeEditFor("roll"),
                { "side": "outgoing", "edge": "right", "tool": "rolling",
                  "outgoingEndShift": 1, "incomingStartShift": 1, "incomingEndShift": 0 });
        compare(SpanMath.edgeEditFor(""), null);
    }

    // 60fps では 0.5 秒 = 30 frame。23.976fps (24000/1001) の 1 秒は 23.976 frame で 24 に丸める。
    function test_secondsAndFrames() {
        compare(SpanMath.secondsToFrames(0.5, 60, 1), 30);
        compare(SpanMath.secondsToFrames(1, 24000, 1001), 24);
        compare(SpanMath.framesToSeconds(30, 60, 1), 0.5);
        fuzzyCompare(SpanMath.framesToSeconds(24, 24000, 1001), 1.001, 1e-9);
        compare(SpanMath.secondsToFrames(1, 0, 1), 0);
    }

    // 60fps、frame [0, 120] を 240 px (2 px / frame)。細かい目盛りは 5 frame (10 px)、文字は 80 px 以上の
    // 30 frame (60 px) では足りず 60 frame (120 px)。
    function test_rulerTicks() {
        const ticks = SpanMath.rulerTicks(0, 120, 240, 60, 80);
        compare(ticks.length, 25);
        compare(ticks[0], { "frame": 0, "major": true });
        compare(ticks[1], { "frame": 5, "major": false });
        compare(ticks[12], { "frame": 60, "major": true });
        compare(ticks[24], { "frame": 120, "major": true });
        // 負の frame は描かず、細かい目盛りの倍数から始める。
        const shifted = SpanMath.rulerTicks(-7, 13, 200, 60, 80);
        compare(shifted[0], { "frame": 0, "major": true });
        compare(shifted[1], { "frame": 1, "major": false });
        // 1 frame あたり 10 px なら細かい目盛りは毎 frame、文字は 10 frame (100 px)。
        compare(shifted[10], { "frame": 10, "major": true });
    }

    function test_spanForDragBody() {
        // 右へ 4: 長さ 60 のまま後ろへずれる。
        compare(SpanMath.spanForDrag("body", 4, 30, 30, 40, 35), { "before": 26, "after": 34 });
        // 右へは後ろの余白 35 まで (5 frame)。
        compare(SpanMath.spanForDrag("body", 20, 30, 30, 40, 35), { "before": 25, "after": 35 });
        // 左へは前の余白 40 まで (10 frame)。
        compare(SpanMath.spanForDrag("body", -20, 30, 30, 40, 35), { "before": 40, "after": 20 });
        // 長さが cut の片側に収まっていれば、cut を越えるまでずらせる。
        compare(SpanMath.spanForDrag("body", 100, 10, 0, 40, 35), { "before": 0, "after": 10 });
    }
}
