import QtQuick
import QtTest
import "../../apps/mvm/TrackEyePaint.js" as EyePaint

// video track の目玉のドラッグ塗り (Photoshop のレイヤーの目玉) の判定の検査。
// 期待値は手で数えた値である (実装の式を呼ばない)。
TestCase {
    name: "TrackEyePaint"

    // 押した track を反転した値で塗る。
    function test_paintMutedFor() {
        compare(EyePaint.paintMutedFor(false), true);
        compare(EyePaint.paintMutedFor(true), false);
    }

    // tracksTop 54、track の高さ 54、video 3 本。上の行ほど index が大きい (V3 が最上段)。
    function test_videoIndexAtY() {
        compare(EyePaint.videoIndexAtY(54, 54, 54, 3), 2);
        compare(EyePaint.videoIndexAtY(107, 54, 54, 3), 2);
        compare(EyePaint.videoIndexAtY(108, 54, 54, 3), 1);
        compare(EyePaint.videoIndexAtY(215, 54, 54, 3), 0);
        // ruler の上・audio の行へはみ出しても端の video track に寄せる。
        compare(EyePaint.videoIndexAtY(0, 54, 54, 3), 2);
        compare(EyePaint.videoIndexAtY(-40, 54, 54, 3), 2);
        compare(EyePaint.videoIndexAtY(216, 54, 54, 3), 0);
        compare(EyePaint.videoIndexAtY(900, 54, 54, 3), 0);
        // video track が無ければ塗れない。
        compare(EyePaint.videoIndexAtY(60, 54, 54, 0), -1);
    }

    function test_addPassedFillsSkippedTracks() {
        // 速く動かして 1 から 4 へ飛んでも、間の 2 / 3 を通ったものとして塗る。
        compare(EyePaint.addPassed([1], 1, 4), [1, 2, 3, 4]);
        // 下向きも同じ。
        compare(EyePaint.addPassed([4], 4, 1), [4, 3, 2, 1]);
        // 戻っても一度通った track は外さず、重複もしない。
        compare(EyePaint.addPassed([1, 2, 3], 3, 1), [1, 2, 3]);
        compare(EyePaint.addPassed([2], 2, 2), [2]);
        // 範囲外 (-1) は何も足さない。元の配列は変えない。
        const painted = [0];
        compare(EyePaint.addPassed(painted, 0, -1), [0]);
        compare(EyePaint.addPassed(painted, 0, 1), [0, 1]);
        compare(painted, [0]);
    }

    function test_displayedMuted() {
        // 塗っている最中は、通った track にだけ塗る値を見せる。
        compare(EyePaint.displayedMuted(1, false, true, [1, 2], true), true);
        compare(EyePaint.displayedMuted(3, false, true, [1, 2], true), false);
        // 表示へ戻す塗りでは、非表示だった track が表示に見える。
        compare(EyePaint.displayedMuted(2, true, true, [2], false), false);
        // 塗っていなければ実際の値。
        compare(EyePaint.displayedMuted(1, false, false, [1], true), false);
        compare(EyePaint.displayedMuted(1, true, false, [], false), true);
    }
}
