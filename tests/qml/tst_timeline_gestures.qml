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

    function test_penUsesPressedKeyAndDragValue() {
        const keys = [{ frame: 10, value: 80 }, { frame: 30, value: 20 }];
        const existing = Gestures.penPress(keys, 100, 11, 82, 2, 8);
        compare(existing.originalFrame, 10);
        const moved = Gestures.penRelease(existing, 19, 40);
        compare(moved.action, "editKey");
        compare(moved.originalFrame, 10);
        compare(moved.frame, 19);
        compare(moved.value, 40);
        const inserted = Gestures.penPress(keys, 50, 20, 50, 2, 8);
        compare(inserted.originalFrame, -1);
        compare(Gestures.penRelease(inserted, 21, 48).frame, 21);
        compare(Gestures.penPress(keys, 50, 20, 90, 2, 8).gesture, "select");
    }

    function test_penAltDeletesKeyAndShiftSnaps() {
        const keys = [{ frame: 10, value: 80 }];
        const removed = Gestures.penPress(keys, 80, 11, 82, 2, 8, Qt.AltModifier);
        compare(removed.gesture, "deleteKey");
        compare(removed.frame, 10);
        // キーから外れた Alt+クリックはキーを増やさず、clip の選択にもしない。
        compare(Gestures.penPress(keys, 80, 30, 80, 2, 8, Qt.AltModifier).gesture, "none");
        compare(Gestures.penPress(keys, 80, 11, 82, 2, 8, Qt.NoModifier).gesture, "pen");
        compare(Gestures.penSnapValue(143, Qt.ShiftModifier), 100);
        compare(Gestures.penSnapValue(143, Qt.NoModifier), 143);
    }

    function test_penToleranceFollowsClipPixels() {
        // 高さ 40px の音量 clip (最大 200%) で 12px = 60%。値の単位で固定しない。
        const tolerance = Gestures.penValueTolerance(12, 200, 40);
        compare(tolerance, 60);
        compare(Gestures.penPress([], 100, 20, 150, 2, tolerance).gesture, "pen");
        compare(Gestures.penPress([], 100, 20, 170, 2, tolerance).gesture, "select");
        compare(Gestures.penValueTolerance(12, 100, 0), 1200);
        const keys = [{ frame: 10, value: 80 }, { frame: 14, value: 90 }];
        compare(Gestures.penNearestKey(keys, 13, 88, 2, 8).frame, 14);
        compare(Gestures.penNearestKey(keys, 20, 80, 2, 8), null);
    }
}
