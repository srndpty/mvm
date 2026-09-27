.pragma library

// タイムラインの clip に対するマウス操作を、どの編集として確定するかの判定。
// QML の delegate は press / release でここを呼び、返った内容どおりに controller を呼ぶだけにする。
// 判定を delegate から切り出し、tests/qml の Qt Quick Test で検査できるようにしている。

// Alt を押しながらの操作はリンク相手へ適用しない (Premiere のリンクされた選択)。
function linkedFor(modifiers) {
    return (modifiers & Qt.AltModifier) === 0;
}

// press の時点で、操作の種類と release で使う値をすべて決める。
//   tool       : TimelineToolPanel.tools の tool
//   modifiers  : 押した時点の Qt.KeyboardModifiers
//   pressFrame : 押した位置の timeline frame
// release 時の位置や modifier で結果を変えない。クリックの間にマウスが少し動いても、
// 分割位置や Alt / Shift の解釈がぶれないようにする。
function bodyPress(tool, modifiers, pressFrame) {
    const shift = (modifiers & Qt.ShiftModifier) !== 0;
    let gesture = "move";
    if (tool === "razor" || tool === "slip" || tool === "slide")
        gesture = tool;
    else if (tool === "trackForward" || tool === "trackBackward")
        gesture = "trackSelect";
    return {
        "gesture": gesture,
        "pressFrame": pressFrame,
        "linked": linkedFor(modifiers),
        // レーザーの Shift は全 track、選択ツールの Shift は選択への追加。
        "allTracks": gesture === "razor" && shift,
        "additive": gesture === "move" && shift
    };
}

// release で行う編集。action に応じて delegate が controller を呼ぶ。
//   state         : bodyPress の戻り値
//   moved         : 移動ドラッグとして扱うだけ動いたか
//   movedToFrame  : 移動ドラッグの行き先 (clip 先頭の frame)
//   releaseFrame  : 離した位置の timeline frame
//   toolDragFrames: slip / slide のドラッグ量 (project frame)
function bodyRelease(state, moved, movedToFrame, releaseFrame, toolDragFrames) {
    switch (state.gesture) {
    case "razor":
        return { "action": "split", "frame": state.pressFrame, "allTracks": state.allTracks,
                 "linked": state.linked };
    case "slip":
    case "slide":
        // 動かさずに離したら何もしない。slip の preview の後始末は delegate が必ず行う。
        if (toolDragFrames === 0)
            return { "action": "none" };
        return { "action": state.gesture, "delta": toolDragFrames, "linked": state.linked };
    }
    if (moved)
        return { "action": "move", "frame": movedToFrame, "linked": state.linked };
    if (state.additive)
        return { "action": "toggle", "frame": releaseFrame };
    // トラックの選択ツールは press で選択済み。離しただけで選択を 1 つに戻さない。
    if (state.gesture === "trackSelect")
        return { "action": "none" };
    return { "action": "select", "frame": releaseFrame, "linked": state.linked };
}

// clip の端のドラッグを、現在のツールの編集として確定する。
function edgeRelease(tool, edge, delta, linked) {
    if (delta === 0)
        return { "action": "none" };
    const action = tool === "ripple" ? "rippleTrim" : (tool === "rolling" ? "roll" : "trim");
    return { "action": action, "edge": edge, "delta": delta, "linked": linked };
}
