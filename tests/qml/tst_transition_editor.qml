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

    // A / B はリップルトリム、cut 線はローリング。B の先頭を詰めると B の終端が手前へ来る。
    function test_edgeEditFor() {
        compare(SpanMath.edgeEditFor("rippleA"), { "side": "outgoing", "edge": "right", "tool": "ripple",
                                                   "cutShift": 1, "incomingEndShift": 1 });
        compare(SpanMath.edgeEditFor("rippleB"), { "side": "incoming", "edge": "left", "tool": "ripple",
                                                   "cutShift": 0, "incomingEndShift": -1 });
        compare(SpanMath.edgeEditFor("roll"), { "side": "outgoing", "edge": "right", "tool": "rolling",
                                                "cutShift": 1, "incomingEndShift": 0 });
        compare(SpanMath.edgeEditFor(""), null);
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
