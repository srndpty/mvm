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

// ペンの座標系。geometry は clip 1 つ分の
// { pixelsPerFrame, maximum (100 または 200 %), height, inset, keyPixels, linePixels }。
// 線は上下 inset px を空けて描く。描画 (penY) と逆変換 (penValueAt) は必ずこの組で使い、
// 片方だけ inset を無視しない (水平にドラッグしただけで値が変わる)。
function penUsableHeight(geometry) {
    return Math.max(1, geometry.height - 2 * geometry.inset);
}

function penY(geometry, valuePercent) {
    const clamped = Math.max(0, Math.min(geometry.maximum, valuePercent));
    return geometry.inset + (1 - clamped / geometry.maximum) * penUsableHeight(geometry);
}

function penValueAt(geometry, y) {
    const top = geometry.inset;
    const bottom = top + penUsableHeight(geometry);
    const normalized = 1 - (Math.max(top, Math.min(bottom, y)) - top) / (bottom - top);
    return Math.round(normalized * geometry.maximum);
}

// 線の当たり判定 (上下 linePixels px) を値の単位へ直したもの。
function penValueTolerance(geometry) {
    return geometry.linePixels * geometry.maximum / penUsableHeight(geometry);
}

// 押した位置 (clip 内の x と値) に最も近い、画面上で左右 keyPixels・上下 linePixels 以内の
// キー。frame へ丸めてから比べると、高倍率で隣の frame のキーまで拾う。無ければ null。
function penNearestKey(keys, geometry, x, valuePercent) {
    const pixelsPerValue = penUsableHeight(geometry) / geometry.maximum;
    let nearest = null;
    let nearestDistance = Infinity;
    for (let index = 0; index < keys.length; ++index) {
        const dx = Math.abs(keys[index].frame * geometry.pixelsPerFrame - x);
        const dy = Math.abs(keys[index].value - valuePercent) * pixelsPerValue;
        if (dx <= geometry.keyPixels && dy <= geometry.linePixels && dx + dy < nearestDistance) {
            nearest = keys[index];
            nearestDistance = dx + dy;
        }
    }
    return nearest;
}

// キーの上ならそのキーの編集、線の近くならキーの追加、どちらでもなければ clip の選択。
// Alt+クリックはキーの削除。キー以外の Alt+クリックは何もしない (キーを増やさない)。
// x / y は clip 内の座標。キーを中心から外して掴んだときは grabOffset をドラッグ位置へ足し、
// 最初の移動で値や frame が跳ばないようにする。
function penPress(keys, lineValue, frame, x, y, geometry, modifiers) {
    if (!Number.isFinite(lineValue))
        return { "gesture": "select" };
    const valuePercent = penValueAt(geometry, y);
    const nearest = penNearestKey(keys, geometry, x, valuePercent);
    if (((modifiers || 0) & Qt.AltModifier) !== 0)
        return nearest ? { "gesture": "deleteKey", "frame": nearest.frame } : { "gesture": "none" };
    if (!nearest && Math.abs(lineValue - valuePercent) > penValueTolerance(geometry))
        return { "gesture": "select" };
    if (!nearest)
        return { "gesture": "pen", "originalFrame": -1, "frame": frame, "value": valuePercent,
                 "grabOffsetX": 0, "grabOffsetY": 0 };
    return { "gesture": "pen", "originalFrame": nearest.frame, "frame": nearest.frame,
             "value": nearest.value,
             "grabOffsetX": nearest.frame * geometry.pixelsPerFrame - x,
             "grabOffsetY": penY(geometry, nearest.value) - y };
}

// ドラッグ中の Shift は 100% (音量なら 0 dB、不透明度なら不透明) へ吸着する。
function penSnapValue(valuePercent, modifiers) {
    return ((modifiers || 0) & Qt.ShiftModifier) !== 0 ? 100 : valuePercent;
}

function penRelease(state, frame, valuePercent) {
    return { "action": "editKey", "originalFrame": state.originalFrame,
             "frame": frame, "value": valuePercent };
}
