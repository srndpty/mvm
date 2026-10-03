// .pragma library は付けない。QML 専用の指示なので VS Code の JavaScript 検査が構文エラー
// ("Unexpected keyword or identifier") にする。ここは状態を持たない関数だけなので、import した
// QML ごとに別の instance になっても振る舞いは変わらない。

// タイムラインの clip に対するマウス操作を、どの編集として確定するかの判定。
// QML の delegate は press / release でここを呼び、返った内容どおりに controller を呼ぶだけにする。
// 判定を delegate から切り出し、tests/qml の Qt Quick Test で検査できるようにしている。

// Alt を押しながらの操作はリンク相手へ適用しない (Premiere のリンクされた選択)。
function linkedFor(modifiers) {
    return (modifiers & Qt.AltModifier) === 0;
}

// ルーラーのマーカーだけは右クリック位置の近くにあるものを選ぶ。
function markerNearRulerX(x, pixelsPerFrame, markers, radius) {
    let nearest = radius + 1;
    let result = -1;
    for (const frame of markers) {
        const distance = Math.abs(x - frame * pixelsPerFrame);
        if (distance <= radius && distance < nearest) {
            nearest = distance;
            result = frame;
        }
    }
    return result;
}

// ドラッグ中の clip 群 (controller.timelineDragBounds) を丸ごと同じ量だけ動かせる範囲へ
// 横のドラッグ量 (px) を丸める。最も左の clip が 0 frame に接したところで止める。
function groupDragOffsetX(offsetX, pixelsPerFrame, bounds) {
    if (bounds.minStartFrame === undefined)
        return offsetX;
    return Math.max(-bounds.minStartFrame * pixelsPerFrame, offsetX);
}

// clip 移動の吸着。動かす clip 群 (movingIds) の両端と、吸着先 (他の clip の両端・再生ヘッド・
// timeline 先頭) の frame を集める。spans は timelineModel.clipSpans() の値。
function dragSnapFrames(spans, movingIds, playheadFrame) {
    const moving = [];
    const targets = [0, playheadFrame];
    for (const span of spans) {
        if (movingIds.indexOf(span.clipId) >= 0)
            moving.push(span.start, span.end);
        else
            targets.push(span.start, span.end);
    }
    return { "moving": moving, "targets": targets };
}

// 横のドラッグ量 (px) を、動かす端のどれかが吸着先から thresholdPx 以内に来たら、端が吸着先に
// ちょうど重なる量へ寄せる。最も近いものを選ぶ。戻り値は {offsetX, frame} で、frame は吸着した
// 吸着先の frame (吸着しなければ -1)。
function snapDragOffsetX(offsetX, pixelsPerFrame, snap, thresholdPx) {
    let result = { "offsetX": offsetX, "frame": -1 };
    let nearest = thresholdPx;
    for (const edge of snap.moving) {
        for (const target of snap.targets) {
            const candidate = (target - edge) * pixelsPerFrame;
            const distance = Math.abs(candidate - offsetX);
            if (distance <= nearest) {
                nearest = distance;
                result = { "offsetX": candidate, "frame": target };
            }
        }
    }
    return result;
}

// 縦も同じく、群の最下段・最上段の clip が既存 track からはみ出さない index へ丸める。
function groupDragTrackIndex(kind, anchorIndex, snappedIndex, trackCount, bounds) {
    const minTrack = bounds[kind + "MinTrack"];
    const maxTrack = bounds[kind + "MaxTrack"];
    if (minTrack === undefined || maxTrack === undefined)
        return snappedIndex;
    const delta = Math.max(-minTrack, Math.min(trackCount - 1 - maxTrack, snappedIndex - anchorIndex));
    return anchorIndex + delta;
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
        "duplicate": tool === "select" && (modifiers & Qt.AltModifier) !== 0,
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
        return { "action": state.duplicate ? "duplicate" : "move",
                 "frame": movedToFrame, "linked": state.linked };
    if (state.additive)
        return { "action": "toggle", "frame": releaseFrame };
    // トラックの選択ツールは press で選択済み。離しただけで選択を 1 つに戻さない。
    if (state.gesture === "trackSelect")
        return { "action": "none" };
    return { "action": "select", "frame": releaseFrame, "linked": state.linked };
}

// clip の端のドラッグを、現在のツールの編集として確定する。
// 動かさずに離したら、選択・リップル・ローリングでは編集点を選ぶ (Shift+D のクロスディゾルブの
// 対象になる)。レート調整では何もしない。
function edgeRelease(tool, edge, delta, linked) {
    if (delta === 0) {
        if (tool === "select" || tool === "ripple" || tool === "rolling")
            return { "action": "selectEdit", "edge": edge };
        return { "action": "none" };
    }
    const action = tool === "ripple" ? "rippleTrim"
                 : tool === "rolling" ? "roll"
                 : tool === "rate" ? "rateStretch"
                 : "trim";
    return { "action": action, "edge": edge, "delta": delta, "linked": linked };
}

// ペンの座標系。geometry は clip 1 つ分の
// { pixelsPerFrame, maximum (100 または 200 %), width, height, inset, keyPixels, linePixels }。
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

// 描画する線の頂点 (clip 内の座標)。キーの間は直線、キーの外側は端の値を保つ。
// 当たり判定も同じ頂点を使い、見えている線と判定をずらさない。
function penLinePoints(keys, baseValue, geometry) {
    if (keys.length === 0) {
        const y = penY(geometry, baseValue);
        return [{ "x": 0, "y": y }, { "x": geometry.width, "y": y }];
    }
    const points = [{ "x": 0, "y": penY(geometry, keys[0].value) }];
    for (let index = 0; index < keys.length; ++index) {
        points.push({ "x": keys[index].frame * geometry.pixelsPerFrame,
                      "y": penY(geometry, keys[index].value) });
        for (const sample of (keys[index].samples || []))
            points.push({ "x": sample.frame * geometry.pixelsPerFrame, "y": penY(geometry, sample.value) });
    }
    points.push({ "x": geometry.width, "y": penY(geometry, keys[keys.length - 1].value) });
    return points;
}

// x の位置で線が通る y。frame へ丸めず、描画と同じ折れ線上で補間する。
function penLineYAt(points, x) {
    if (x <= points[0].x)
        return points[0].y;
    for (let index = 1; index < points.length; ++index) {
        const left = points[index - 1];
        const right = points[index];
        if (x <= right.x) {
            if (right.x <= left.x)
                return right.y;
            return left.y + (right.y - left.y) * (x - left.x) / (right.x - left.x);
        }
    }
    return points[points.length - 1].y;
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
// x / y は clip 内の座標、frame はキーを置く frame (x を frame へ丸めたもの)。
// 「線を掴んだか」は描画された線の位置で決め、「どの frame へ置くか」だけを frame へ丸める。
// キーを中心から外して掴んだときは grabOffset をドラッグ位置へ足し、
// 最初の移動で値や frame が跳ばないようにする。
function penPress(keys, baseValue, frame, x, y, geometry, modifiers) {
    const valuePercent = penValueAt(geometry, y);
    const nearest = penNearestKey(keys, geometry, x, valuePercent);
    if (((modifiers || 0) & Qt.AltModifier) !== 0)
        return nearest ? { "gesture": "deleteKey", "frame": nearest.frame } : { "gesture": "none" };
    const lineY = penLineYAt(penLinePoints(keys, baseValue, geometry), x);
    if (!nearest && Math.abs(lineY - y) > geometry.linePixels)
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
