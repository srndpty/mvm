// .pragma library は付けない (TimelineGestures.js と同じ理由)。状態を持たない関数だけを置く。

// トランジションの長さと配置の計算。配置は cut の前 (before) と後 (after) の frame 数の比
// そのもので、別の値を持たない。エフェクトコントロールの数値欄・配置・ミニタイムラインの
// ドラッグはここで before / after を求め、controller.setTransitionSpan を 1 回だけ呼ぶ。
// 置けるかどうか (素材 frame・不透明度) の最終判定は project 層が行い、ここでは丸めない。
// ここで揃えるのは余白の上限 (maxBefore / maxAfter) と、合計 1 frame 以上だけである。

// "start" = cut で開始、"end" = cut で終了、"center" = 中央 (奇数なら 1 frame ずれる)、
// それ以外は "custom"。
function alignmentOf(before, after) {
    if (before === 0)
        return "start";
    if (after === 0)
        return "end";
    if (Math.abs(before - after) <= 1)
        return "center";
    return "custom";
}

function clamp(value, low, high) {
    return Math.max(low, Math.min(high, value));
}

// 片側が上限を超えた分はもう片側へ寄せる (既定のトランジションを置くときと同じ規則)。
// 合計は両側の上限の和で頭打ちにする。
function fitSpan(before, after, maxBefore, maxAfter) {
    const total = clamp(before + after, 1, Math.max(1, maxBefore + maxAfter));
    let fittedBefore = clamp(before, 0, total);
    let fittedAfter = total - fittedBefore;
    if (fittedBefore > maxBefore) {
        fittedAfter += fittedBefore - maxBefore;
        fittedBefore = maxBefore;
    }
    if (fittedAfter > maxAfter) {
        fittedBefore += fittedAfter - maxAfter;
        fittedAfter = maxAfter;
    }
    return { "before": fittedBefore, "after": fittedAfter };
}

// 長さを duration にする。配置は alignment に従い、"custom" は cut の前の割合を保つ。
function spanForDuration(duration, alignment, before, after, maxBefore, maxAfter) {
    const total = Math.max(1, Math.round(duration));
    let wantedBefore;
    if (alignment === "start")
        wantedBefore = 0;
    else if (alignment === "end")
        wantedBefore = total;
    else if (alignment === "center" || before + after <= 0)
        wantedBefore = Math.floor(total / 2);
    else
        wantedBefore = Math.round(total * before / (before + after));
    return fitSpan(wantedBefore, total - wantedBefore, maxBefore, maxAfter);
}

// 長さを保ったまま配置だけを変える。
function spanForAlignment(alignment, before, after, maxBefore, maxAfter) {
    return spanForDuration(before + after, alignment, before, after, maxBefore, maxAfter);
}

// ミニタイムラインで A / B / cut 線をドラッグしたときの編集 (Premiere のエフェクトコントロールと同じ)。
//   "rippleA" A を動かす: A の終端のリップルトリム (後ろの clip がずれる)
//   "rippleB" B を動かす: B の先頭のリップルトリム
//   "roll"    cut 線を動かす: ローリング編集 (A の終端と B の先頭を一緒に動かす)
// edge / tool は controller.clampEdgeDrag と rippleTrimClip / rollClipEdge に渡す値。
// cutShift / incomingEndShift は、動かした量 delta に対する cut と B の終端の移動の係数。
function edgeEditFor(gesture) {
    if (gesture === "rippleA")
        return { "side": "outgoing", "edge": "right", "tool": "ripple",
                 "cutShift": 1, "incomingEndShift": 1 };
    if (gesture === "rippleB")
        return { "side": "incoming", "edge": "left", "tool": "ripple",
                 "cutShift": 0, "incomingEndShift": -1 };
    if (gesture === "roll")
        return { "side": "outgoing", "edge": "right", "tool": "rolling",
                 "cutShift": 1, "incomingEndShift": 0 };
    return null;
}

// ミニタイムラインのドラッグ。deltaFrames は右向きが正。
//   "left"  左端: 開始だけを動かす (cut の前の長さ)
//   "right" 右端: 終了だけを動かす (cut の後の長さ)
//   "body"  本体: 長さを保ってずらす (cut に対する位置)
// 余白の上限と、合計 1 frame 以上の範囲で止める。
function spanForDrag(handle, deltaFrames, before, after, maxBefore, maxAfter) {
    const delta = Math.round(deltaFrames);
    if (handle === "left")
        return { "before": clamp(before - delta, Math.max(0, 1 - after), maxBefore),
                 "after": after };
    if (handle === "right")
        return { "before": before,
                 "after": clamp(after + delta, Math.max(0, 1 - before), maxAfter) };
    const shift = clamp(delta, Math.max(before - maxBefore, -after),
                        Math.min(before, maxAfter - after));
    return { "before": before - shift, "after": after + shift };
}
