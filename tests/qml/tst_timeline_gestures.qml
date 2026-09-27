import QtQuick
import QtTest
import "../../apps/mvm/TimelineGestures.js" as Gestures

// タイムラインの clip に対するマウス操作が、どの編集として確定するかの検査。
// delegate は Gestures の戻り値どおりに controller を呼ぶだけなので、ここで
// press / release の値の取り違え (例: レーザーが clip 先頭で切る) を捕まえる。
TestCase {
    name: "TimelineGestures"

    // レーザーは押した位置で切る。release までにマウスが動いても、離した位置や
    // clip の先頭へずれない。
    function test_razorSplitsAtPressFrame() {
        const state = Gestures.bodyPress("razor", Qt.NoModifier, 150);
        const action = Gestures.bodyRelease(state, false, 100, 153, 0);
        compare(action.action, "split");
        compare(action.frame, 150);
        compare(action.allTracks, false);
        compare(action.linked, true);
    }

    function test_razorModifiersAreFixedAtPress() {
        const allTracks = Gestures.bodyPress("razor", Qt.ShiftModifier, 40);
        compare(Gestures.bodyRelease(allTracks, false, 0, 40, 0).allTracks, true);
        const single = Gestures.bodyPress("razor", Qt.AltModifier, 40);
        compare(Gestures.bodyRelease(single, false, 0, 40, 0).linked, false);
    }

    // 選択ツールは既定でリンク相手ごと、Alt なら片方だけを動かす。
    function test_moveUsesDragTargetAndLinkMode() {
        const linked = Gestures.bodyPress("select", Qt.NoModifier, 10);
        const moved = Gestures.bodyRelease(linked, true, 240, 12, 0);
        compare(moved.action, "move");
        compare(moved.frame, 240);
        compare(moved.linked, true);
        const single = Gestures.bodyPress("select", Qt.AltModifier, 10);
        compare(Gestures.bodyRelease(single, true, 240, 12, 0).linked, false);
    }

    function test_clickSelectsAtReleaseFrame() {
        const state = Gestures.bodyPress("ripple", Qt.NoModifier, 10);
        const action = Gestures.bodyRelease(state, false, 0, 12, 0);
        compare(action.action, "select");
        compare(action.frame, 12);
        const additive = Gestures.bodyPress("select", Qt.ShiftModifier, 10);
        compare(Gestures.bodyRelease(additive, false, 0, 12, 0).action, "toggle");
    }

    // トラックの選択ツールは press で選択済み。離しただけで選択を 1 つに戻さない。
    function test_trackSelectReleaseKeepsSelection() {
        const state = Gestures.bodyPress("trackForward", Qt.ShiftModifier, 10);
        compare(state.gesture, "trackSelect");
        compare(state.additive, false);
        compare(Gestures.bodyRelease(state, false, 0, 12, 0).action, "none");
        compare(Gestures.bodyRelease(state, true, 80, 12, 0).action, "move");
    }

    function test_slipAndSlideUseDragFrames() {
        const slip = Gestures.bodyPress("slip", Qt.AltModifier, 10);
        const slipped = Gestures.bodyRelease(slip, false, 0, 10, -7);
        compare(slipped.action, "slip");
        compare(slipped.delta, -7);
        compare(slipped.linked, false);
        const slide = Gestures.bodyPress("slide", Qt.NoModifier, 10);
        compare(Gestures.bodyRelease(slide, false, 0, 10, 0).action, "none");
        compare(Gestures.bodyRelease(slide, false, 0, 10, 5).action, "slide");
    }

    function test_edgeReleaseFollowsTool() {
        compare(Gestures.edgeRelease("select", "left", 3, true).action, "trim");
        compare(Gestures.edgeRelease("ripple", "right", -4, true).action, "rippleTrim");
        const roll = Gestures.edgeRelease("rolling", "right", 2, false);
        compare(roll.action, "roll");
        compare(roll.edge, "right");
        compare(roll.delta, 2);
        compare(roll.linked, false);
        compare(Gestures.edgeRelease("ripple", "left", 0, true).action, "none");
    }

    // Main.qml の clip と同じ座標系。video は最大 100%、audio は最大 200%。
    function penGeometry(maximum, pixelsPerFrame) {
        return { "pixelsPerFrame": pixelsPerFrame, "maximum": maximum, "width": 800,
                 "height": 54, "inset": 4, "keyPixels": 8, "linePixels": 12 };
    }

    function test_penValueAndYRoundTrip() {
        const video = penGeometry(100, 4);
        compare(Gestures.penY(video, 100), 4);
        compare(Gestures.penY(video, 0), 50);
        for (const value of [0, 25, 50, 93, 100])
            compare(Gestures.penValueAt(video, Gestures.penY(video, value)), value);
        // キーの四角は整数 px に置く。端と中央はその位置からも同じ値へ戻る。
        for (const value of [0, 50, 100])
            compare(Gestures.penValueAt(video, Math.round(Gestures.penY(video, value))), value);
        const audio = penGeometry(200, 4);
        for (const value of [0, 100, 200])
            compare(Gestures.penValueAt(audio, Gestures.penY(audio, value)), value);
        // inset の外は端の値に留める。
        compare(Gestures.penValueAt(video, 0), 100);
        compare(Gestures.penValueAt(video, 54), 0);
        compare(Gestures.penValueAt(audio, -10), 200);
        const flat = { "pixelsPerFrame": 4, "maximum": 100, "width": 800, "height": 4,
                       "inset": 4, "keyPixels": 8, "linePixels": 12 };
        verify(Number.isFinite(Gestures.penValueAt(flat, 2)));
    }

    function test_penLineClickAddsOrSelects() {
        const video = penGeometry(100, 4);
        // 線は 100% (y = 4)。12px 下までは追加、それより下は clip の選択。
        const added = Gestures.penPress([], 100, 20, 80, 10, video);
        compare(added.gesture, "pen");
        compare(added.originalFrame, -1);
        compare(added.frame, 20);
        compare(added.value, Gestures.penValueAt(video, 10));
        compare(added.grabOffsetY, 0);
        compare(Gestures.penPress([], 100, 20, 80, 16, video).gesture, "pen");
        compare(Gestures.penPress([], 100, 20, 80, 17, video).gesture, "select");
    }

    function test_penLinePointsMatchDrawing() {
        const video = penGeometry(100, 4);
        const flat = Gestures.penLinePoints([], 50, video);
        compare(flat.length, 2);
        compare(flat[0].x, 0);
        compare(flat[1].x, 800);
        compare(flat[0].y, Gestures.penY(video, 50));
        // キーの外側は端の値を保つ。
        const points = Gestures.penLinePoints([{ frame: 10, value: 0 }, { frame: 20, value: 100 }],
                                              50, video);
        compare(points.length, 4);
        compare(points[0].y, Gestures.penY(video, 0));
        compare(points[1].x, 40);
        compare(points[3].x, 800);
        compare(points[3].y, Gestures.penY(video, 100));
        compare(Gestures.penLineYAt(points, 20), Gestures.penY(video, 0));
        compare(Gestures.penLineYAt(points, 60), Gestures.penY(video, 50));
        compare(Gestures.penLineYAt(points, 900), Gestures.penY(video, 100));
    }

    function test_penLineHitFollowsSlopedLineBetweenFrames() {
        // frame 10 = 0%、frame 11 = 100%、1 frame = 32px。frame 10.5 (x = 336) の線は 50%。
        // frame へ丸めて線の値を評価すると 100% になり、見えている線を押しても選択になる。
        const zoomed = penGeometry(100, 32);
        const keys = [{ frame: 10, value: 0 }, { frame: 11, value: 100 }];
        const y = Gestures.penY(zoomed, 50);
        compare(Gestures.penLineYAt(Gestures.penLinePoints(keys, 100, zoomed), 336), y);
        const added = Gestures.penPress(keys, 100, 11, 336, y, zoomed);
        compare(added.gesture, "pen");
        compare(added.originalFrame, -1);
        // キーを置く frame だけは frame へ丸めた値。
        compare(added.frame, 11);
        compare(added.value, 50);
        compare(Gestures.penPress(keys, 100, 11, 336, y + 13, zoomed).gesture, "select");
    }

    function test_penDragsPressedKeyWithoutJump() {
        const video = penGeometry(100, 4);
        const keys = [{ frame: 10, value: 80 }, { frame: 30, value: 20 }];
        // キー (x = 40, y = 13.2) の右下を掴む。
        const pressed = Gestures.penPress(keys, 80, 11, 43, 15, video);
        compare(pressed.gesture, "pen");
        compare(pressed.originalFrame, 10);
        compare(pressed.frame, 10);
        compare(pressed.value, 80);
        compare(pressed.grabOffsetX, -3);
        fuzzyCompare(pressed.grabOffsetY, Gestures.penY(video, 80) - 15, 1e-9);
        // 真横へ動かしても値は変わらない。
        compare(Gestures.penValueAt(video, 15 + pressed.grabOffsetY), 80);
        const moved = Gestures.penRelease(pressed, 19, 40);
        compare(moved.action, "editKey");
        compare(moved.originalFrame, 10);
        compare(moved.frame, 19);
        compare(moved.value, 40);
    }

    function test_penPicksNearestKey() {
        const video = penGeometry(100, 4);
        const keys = [{ frame: 10, value: 50 }, { frame: 12, value: 50 }];
        const y = Gestures.penY(video, 50);
        compare(Gestures.penNearestKey(keys, video, 45, 50).frame, 12);
        compare(Gestures.penNearestKey(keys, video, 43, 50).frame, 10);
        compare(Gestures.penPress(keys, 50, 11, 45, y, video).originalFrame, 12);
        // 上下 12px を超えるとキーではない。
        compare(Gestures.penNearestKey(keys, video, 40, Gestures.penValueAt(video, y + 13)), null);
    }

    function test_penKeyHitStaysWithinPixelsAtHighZoom() {
        // 1 frame = 32px。frame へ丸めてから比べると、隣の frame (32px 先) まで拾ってしまう。
        const zoomed = penGeometry(100, 32);
        const keys = [{ frame: 10, value: 50 }];
        compare(Gestures.penNearestKey(keys, zoomed, 326, 50).frame, 10);
        compare(Gestures.penNearestKey(keys, zoomed, 329, 50), null);
        compare(Gestures.penNearestKey(keys, zoomed, 352, 50), null);
        // 誤って隣の frame のキーを消さない。
        const y = Gestures.penY(zoomed, 50);
        compare(Gestures.penPress(keys, 50, 11, 352, y, zoomed, Qt.AltModifier).gesture, "none");
        compare(Gestures.penPress(keys, 50, 10, 324, y, zoomed, Qt.AltModifier).gesture,
                "deleteKey");
    }

    function test_penAltDeletesKeyAndShiftSnaps() {
        const video = penGeometry(100, 4);
        const keys = [{ frame: 10, value: 80 }];
        const keyY = Gestures.penY(video, 80);
        const removed = Gestures.penPress(keys, 80, 11, 42, keyY + 2, video, Qt.AltModifier);
        compare(removed.gesture, "deleteKey");
        compare(removed.frame, 10);
        // キーから外れた Alt+クリックは、線の上でもキーを増やさず、clip の選択にもしない。
        compare(Gestures.penPress(keys, 80, 30, 120, keyY, video, Qt.AltModifier).gesture, "none");
        compare(Gestures.penPress(keys, 80, 30, 120, 50, video, Qt.AltModifier).gesture, "none");
        compare(Gestures.penPress(keys, 80, 11, 42, keyY, video, Qt.NoModifier).gesture, "pen");
        compare(Gestures.penSnapValue(143, Qt.ShiftModifier), 100);
        compare(Gestures.penSnapValue(143, Qt.ShiftModifier | Qt.AltModifier), 100);
        compare(Gestures.penSnapValue(143, Qt.NoModifier), 143);
        compare(Gestures.penSnapValue(143, undefined), 143);
    }
}
