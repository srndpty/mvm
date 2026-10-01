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
// *Shift はドラッグ中の表示で、動かした量 delta に対する A の終端・B の先頭・B の終端の移動の係数。
// 掴んだ端がマウスに付いてくるように描く (B のリップルは確定すると B の先頭が cut に残り後ろが
// 詰まるが、それをそのまま描くとドラッグ中に何も動いて見えない)。
function edgeEditFor(gesture) {
    if (gesture === "rippleA")
        return { "side": "outgoing", "edge": "right", "tool": "ripple",
                 "outgoingEndShift": 1, "incomingStartShift": 1, "incomingEndShift": 1 };
    if (gesture === "rippleB")
        return { "side": "incoming", "edge": "left", "tool": "ripple",
                 "outgoingEndShift": 0, "incomingStartShift": 1, "incomingEndShift": 0 };
    if (gesture === "roll")
        return { "side": "outgoing", "edge": "right", "tool": "rolling",
                 "outgoingEndShift": 1, "incomingStartShift": 1, "incomingEndShift": 0 };
    return null;
}

// 長さの入力単位の換算。秒は timeline の fps (fpsNum / fpsDen) で frame に丸める。
function framesToSeconds(frames, fpsNum, fpsDen) {
    return fpsNum > 0 && fpsDen > 0 ? frames * fpsDen / fpsNum : 0;
}

function secondsToFrames(seconds, fpsNum, fpsDen) {
    return fpsNum > 0 && fpsDen > 0 ? Math.round(seconds * fpsNum / fpsDen) : 0;
}

// ルーラーの目盛り。表示範囲 [viewStart, viewEnd] の frame を幅 width px に描くとき、
// 細かい目盛りは 8 px 以上、文字を付ける目盛りは labelSpacing px 以上空ける。間隔は
// 1 / 2 / 5 / 10 frame、0.5 / 1 / 2 / 5 / 10 / 30 / 60 秒 (公称 fps の倍数) から選ぶ。
// 文字の目盛りは細かい目盛りの倍数にする。[{frame, major}] を frame 順に返す (負の frame は除く)。
function rulerTicks(viewStart, viewEnd, width, nominalFps, labelSpacing) {
    const fps = Math.max(1, Math.round(nominalFps));
    const pixelsPerFrame = width / Math.max(1, viewEnd - viewStart);
    const candidates = [1, 2, 5, 10, Math.max(1, Math.round(fps / 2)), fps, fps * 2, fps * 5,
                        fps * 10, fps * 30, fps * 60]
        .filter((value, index, all) => all.indexOf(value) === index)
        .sort((left, right) => left - right);
    let minor = candidates[candidates.length - 1];
    for (const candidate of candidates) {
        if (candidate * pixelsPerFrame >= 8) {
            minor = candidate;
            break;
        }
    }
    let major = minor;
    for (const candidate of candidates) {
        if (candidate >= minor && candidate % minor === 0 && candidate * pixelsPerFrame >= labelSpacing) {
            major = candidate;
            break;
        }
    }
    if (major * pixelsPerFrame < labelSpacing)
        major = minor * Math.ceil(labelSpacing / (minor * pixelsPerFrame));
    const ticks = [];
    const first = Math.max(0, Math.ceil(viewStart / minor) * minor);
    for (let frame = first; frame <= viewEnd; frame += minor)
        ticks.push({ "frame": frame, "major": frame % major === 0 });
    return ticks;
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
