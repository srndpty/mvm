// .pragma library は付けない。QML 専用の指示なので VS Code の JavaScript 検査が構文エラー
// ("Unexpected keyword or identifier") にする。ここは状態を持たない関数だけなので、import した
// QML ごとに別の instance になっても振る舞いは変わらない。

// プレビュー上で素材を動かす・拡縮するときの計算。座標はすべて出力画素で、
// 矩形は {x, y, width, height}。QML から切り離して tst_preview_transform.qml で検査する。

function finite(value) {
    return typeof value === "number" && isFinite(value);
}

function copyRect(rect) {
    return { "x": rect.x, "y": rect.y, "width": rect.width, "height": rect.height };
}

// 回転した矩形を囲む軸平行の矩形。回転は pivot を中心に時計回り (画面の y 下向き)。
function rotatedBounds(rect, pivotX, pivotY, degrees) {
    if (!finite(degrees) || degrees === 0)
        return copyRect(rect);
    const corners = [[rect.x, rect.y], [rect.x + rect.width, rect.y],
                     [rect.x, rect.y + rect.height], [rect.x + rect.width, rect.y + rect.height]];
    let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity;
    for (let index = 0; index < corners.length; ++index) {
        const point = rotatePoint(corners[index][0], corners[index][1], pivotX, pivotY, degrees);
        left = Math.min(left, point.x);
        right = Math.max(right, point.x);
        top = Math.min(top, point.y);
        bottom = Math.max(bottom, point.y);
    }
    return { "x": left, "y": top, "width": right - left, "height": bottom - top };
}

// 吸着先の線。出力の端と中央、他の素材の端と中央 (Photoshop のスマートガイドと同じ)。
function snapLines(canvasWidth, canvasHeight, rects) {
    const xs = [0, canvasWidth / 2, canvasWidth];
    const ys = [0, canvasHeight / 2, canvasHeight];
    for (let index = 0; index < (rects ? rects.length : 0); ++index) {
        const rect = rects[index];
        xs.push(rect.x, rect.x + rect.width / 2, rect.x + rect.width);
        ys.push(rect.y, rect.y + rect.height / 2, rect.y + rect.height);
    }
    return { "xs": xs, "ys": ys };
}

// values のどれかを lines のどれかへ寄せる、閾値以内で最小のずれ。無ければ null。
function nearestSnap(values, lines, threshold) {
    let best = null;
    for (let valueIndex = 0; valueIndex < values.length; ++valueIndex) {
        for (let lineIndex = 0; lineIndex < lines.length; ++lineIndex) {
            const delta = lines[lineIndex] - values[valueIndex];
            if (Math.abs(delta) <= threshold && (best === null || Math.abs(delta) < Math.abs(best.delta)))
                best = { "delta": delta, "line": lines[lineIndex] };
        }
    }
    return best;
}

// rect を (dx, dy) 動かしたとき、端か中央が線に近ければ吸着させた移動量を返す。
// enabled = false (Ctrl を押している) なら吸着しない。
function snapMove(rect, dx, dy, lines, threshold, enabled) {
    if (!finite(dx) || !finite(dy))
        return { "dx": 0, "dy": 0, "guidesX": [], "guidesY": [] };
    if (!enabled)
        return { "dx": dx, "dy": dy, "guidesX": [], "guidesY": [] };
    const x = rect.x + dx;
    const y = rect.y + dy;
    const snapX = nearestSnap([x, x + rect.width / 2, x + rect.width], lines.xs, threshold);
    const snapY = nearestSnap([y, y + rect.height / 2, y + rect.height], lines.ys, threshold);
    return {
        "dx": dx + (snapX ? snapX.delta : 0),
        "dy": dy + (snapY ? snapY.delta : 0),
        "guidesX": snapX ? [snapX.line] : [],
        "guidesY": snapY ? [snapY.line] : []
    };
}

function handleSides(handle) {
    return {
        "left": handle.indexOf("l") >= 0,
        "right": handle.indexOf("r") >= 0,
        "top": handle.indexOf("t") >= 0,
        "bottom": handle.indexOf("b") >= 0
    };
}

// 拡縮で動かさない点。Alt (fromCenter) なら素材の回転の中心 (pivot) があればそれ、
// それ以外 (Ctrl だけで縦横比を保つときの、もう一方の軸を含む) は見えている矩形の中心。
// 回転の中心は crop 範囲の中心なので、非対称な crop では見えている矩形の中心と違う
// (矩形の外のこともある)。Alt で描画 (preview / 書き出し) と同じ点を固定しないと、
// 回転した素材が動いてしまう。
function fixedPoint(start, options) {
    const pivot = options && options.fromCenter ? options.pivot : null;
    return {
        "x": pivot && finite(pivot.x) ? pivot.x : start.x + start.width / 2,
        "y": pivot && finite(pivot.y) ? pivot.y : start.y + start.height / 2
    };
}

// start の縦横比を保って rect を直す。drive は倍率を決める軸 ("x" / "y")。
// 省略時は角なら大きく変わった方、辺ならその辺の軸。
// fromCenter なら固定点 (fixedPoint) を、そうでなければ掴んだ側の反対の角・辺を固定する。
// 辺のハンドルでは、もう一方の軸は固定点を保って伸縮する。
function keepAspectRect(start, rect, handle, fromCenter, minSize, drive, fixed) {
    const sides = handleSides(handle);
    const horizontal = sides.left || sides.right;
    const vertical = sides.top || sides.bottom;
    let axis = drive;
    if (!axis) {
        if (horizontal && vertical)
            axis = rect.width / start.width >= rect.height / start.height ? "x" : "y";
        else
            axis = horizontal ? "x" : "y";
    }
    const minimumScale = minSize / Math.min(start.width, start.height);
    const scale = Math.max(minimumScale, axis === "x" ? rect.width / start.width
                                                      : rect.height / start.height);
    const width = start.width * scale;
    const height = start.height * scale;
    const point = fixed || fixedPoint(start, null);
    // 固定点との位置関係 (矩形の中の割合) を保って拡縮する。
    let x = point.x - (point.x - start.x) * scale;
    let y = point.y - (point.y - start.y) * scale;
    if (!fromCenter) {
        if (sides.left)
            x = start.x + start.width - width;
        else if (sides.right)
            x = start.x;
        if (sides.top)
            y = start.y + start.height - height;
        else if (sides.bottom)
            y = start.y;
    }
    return { "x": x, "y": y, "width": width, "height": height };
}

// ハンドル (tl, t, tr, r, br, b, bl, l) を pointer まで動かしたときの矩形。
//   既定       : 反対側の角・辺を固定する
//   fromCenter : 固定点 (options.pivot、無ければ中心) を動かさずに伸縮する (Alt)
//   keepAspect : 縦横比を保つ (Ctrl)
// 反対側を越えて裏返さず、minSize で止める。
function resizeRect(start, handle, pointer, options, minSize) {
    if (!pointer || !finite(pointer.x) || !finite(pointer.y))
        return copyRect(start);
    const minimum = finite(minSize) && minSize > 0 ? minSize : 1;
    const sides = handleSides(handle);
    const fixed = fixedPoint(start, options);
    let left = start.x, right = start.x + start.width;
    let top = start.y, bottom = start.y + start.height;
    // 両側を同じ倍率にする。固定点が矩形の外 (letterbox の余白を含む crop) にあることもあるので、距離の符号は
    // 決め打ちせず、固定点から掴んだ辺までと pointer までの比をそのまま倍率にする。
    const scaleAbout = (low, high, point, grabbedHigh, target, size) => {
        const reach = (grabbedHigh ? high : low) - point;
        const scale = Math.abs(reach) < 1e-9 ? minimum / size
                                              : Math.max(minimum / size, (target - point) / reach);
        return [point - (point - low) * scale, point + (high - point) * scale];
    };
    if (options.fromCenter) {
        if (sides.left || sides.right)
            [left, right] = scaleAbout(left, right, fixed.x, sides.right, pointer.x, start.width);
        if (sides.top || sides.bottom)
            [top, bottom] = scaleAbout(top, bottom, fixed.y, sides.bottom, pointer.y, start.height);
    } else {
        if (sides.right)
            right = Math.max(pointer.x, left + minimum);
        if (sides.left)
            left = Math.min(pointer.x, right - minimum);
        if (sides.bottom)
            bottom = Math.max(pointer.y, top + minimum);
        if (sides.top)
            top = Math.min(pointer.y, bottom - minimum);
    }
    const rect = { "x": left, "y": top, "width": right - left, "height": bottom - top };
    if (options.keepAspect)
        return keepAspectRect(start, rect, handle, options.fromCenter, minimum, undefined, fixed);
    return rect;
}

// 掴んだ辺だけを線へ吸着させる。fromCenter なら固定点を保って反対の辺も同じ倍率で動かし、
// keepAspect なら近い方の軸だけを吸着させてもう一方は比率で決める。
function snapResize(start, rect, handle, lines, threshold, options, minSize) {
    const minimum = finite(minSize) && minSize > 0 ? minSize : 1;
    const sides = handleSides(handle);
    const right = rect.x + rect.width;
    const bottom = rect.y + rect.height;
    const snapX = sides.left ? nearestSnap([rect.x], lines.xs, threshold)
                : sides.right ? nearestSnap([right], lines.xs, threshold) : null;
    const snapY = sides.top ? nearestSnap([rect.y], lines.ys, threshold)
                : sides.bottom ? nearestSnap([bottom], lines.ys, threshold) : null;
    let useX = snapX !== null;
    let useY = snapY !== null;
    if (options.keepAspect && useX && useY) {
        useX = Math.abs(snapX.delta) <= Math.abs(snapY.delta);
        useY = !useX;
    }
    if (!useX && !useY)
        return { "rect": copyRect(rect), "guidesX": [], "guidesY": [] };

    const fixed = fixedPoint(start, options);
    // 掴んだ辺を delta だけ動かす。fromCenter なら固定点との比を保って反対の辺も動かす。
    const moveEdge = (low, high, point, grabbedHigh, delta) => {
        if (!options.fromCenter)
            return grabbedHigh ? [low, high + delta] : [low + delta, high];
        const edge = grabbedHigh ? high : low;
        if (edge === point)
            return [low, high];
        const scale = (edge + delta - point) / (edge - point);
        return [point - (point - low) * scale, point + (high - point) * scale];
    };
    let left = rect.x, newRight = right, top = rect.y, newBottom = bottom;
    if (useX)
        [left, newRight] = moveEdge(left, newRight, fixed.x, sides.right, snapX.delta);
    if (useY)
        [top, newBottom] = moveEdge(top, newBottom, fixed.y, sides.bottom, snapY.delta);
    // 吸着で裏返る・潰れるなら吸着しない。
    if (newRight - left < minimum || newBottom - top < minimum)
        return { "rect": copyRect(rect), "guidesX": [], "guidesY": [] };
    let snapped = { "x": left, "y": top, "width": newRight - left, "height": newBottom - top };
    if (options.keepAspect)
        snapped = keepAspectRect(start, snapped, handle, options.fromCenter, minimum,
                                 useX ? "x" : "y", fixed);
    return {
        "rect": snapped,
        "guidesX": useX ? [snapX.line] : [],
        "guidesY": useY ? [snapY.line] : []
    };
}

// (x, y) を pivot を中心に degrees だけ回した点 (時計回り、画面の y 下向き)。
function rotatePoint(x, y, pivotX, pivotY, degrees) {
    const radians = degrees * Math.PI / 180;
    const dx = x - pivotX;
    const dy = y - pivotY;
    return { "x": pivotX + Math.cos(radians) * dx - Math.sin(radians) * dy,
             "y": pivotY + Math.sin(radians) * dx + Math.cos(radians) * dy };
}

// 回転した素材の拡縮。画面上の pointer を回転前 (素材の座標) へ戻してから resizeRect で
// 拡縮し、回転の中心が動いた分をずらして、固定する側 (反対の角・辺、Alt なら中心) を
// 画面上で動かさない。回転の中心は矩形の中の同じ割合の位置にある (crop 範囲の中心)。
// 回転した辺は画面の軸と揃わないので吸着はしない。
function resizeRotatedRect(start, pivotX, pivotY, degrees, handle, pointer, options, minSize) {
    if (!pointer || !finite(pointer.x) || !finite(pointer.y) || !finite(degrees))
        return copyRect(start);
    const local = rotatePoint(pointer.x, pointer.y, pivotX, pivotY, -degrees);
    // Alt で固定するのは回転の中心そのもの (動かなければ下の補正は 0 になる)。
    const aboutPivot = Object.assign({}, options, { "pivot": { "x": pivotX, "y": pivotY } });
    const resized = resizeRect(start, handle, local, aboutPivot, minSize);
    // 新しい矩形の回転の中心 P'。画面上の位置を保つには (I - R)(P - P') だけずらす。
    const fractionX = (pivotX - start.x) / start.width;
    const fractionY = (pivotY - start.y) / start.height;
    const dx = pivotX - (resized.x + fractionX * resized.width);
    const dy = pivotY - (resized.y + fractionY * resized.height);
    const turned = rotatePoint(dx, dy, 0, 0, degrees);
    return { "x": resized.x + dx - turned.x, "y": resized.y + dy - turned.y,
             "width": resized.width, "height": resized.height };
}

// 画面上の吸着距離 (px) を出力画素へ換算する。
function thresholdOutputPx(screenPx, outputWidth, hostWidth) {
    return hostWidth > 0 ? screenPx * outputWidth / hostWidth : screenPx;
}

// 各ハンドルの名前と、矩形に対する位置 (0..1)。
function handles() {
    return [
        { "name": "tl", "fx": 0, "fy": 0 }, { "name": "t", "fx": 0.5, "fy": 0 },
        { "name": "tr", "fx": 1, "fy": 0 }, { "name": "r", "fx": 1, "fy": 0.5 },
        { "name": "br", "fx": 1, "fy": 1 }, { "name": "b", "fx": 0.5, "fy": 1 },
        { "name": "bl", "fx": 0, "fy": 1 }, { "name": "l", "fx": 0, "fy": 0.5 }
    ];
}
