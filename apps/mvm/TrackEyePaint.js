// .pragma library は付けない (docs/premiere-like-editing.md §19.8)。状態を持たない関数だけを置く。

// video track の目玉のドラッグ塗り (Photoshop のレイヤーの目玉と同じ)。
// 押した track の表示を反転し、その値を「塗る値」としてドラッグで通った track を同じ値にする。
// QML は press / move / release でここを呼び、release で通った track をまとめて確定する。

// 押した track の現在の mute (非表示) から塗る値を決める。
function paintMutedFor(pressedMuted) {
    return !pressedMuted;
}

// timeline の content 座標の y にある video track の index。video の範囲外は端の track に寄せる
// (audio の行や ruler へはみ出しても塗りが途切れないように)。video track が無ければ -1。
function videoIndexAtY(contentY, tracksTop, trackHeight, videoCount) {
    if (videoCount <= 0 || trackHeight <= 0)
        return -1;
    const row = Math.max(0, Math.min(videoCount - 1, Math.floor((contentY - tracksTop) / trackHeight)));
    return videoCount - 1 - row;
}

// 前回の index から今回の index までを (両端を含めて) 塗った集合へ足した新しい配列を返す。
// 速く動かすと move event が行を飛ばすので、間の track も通ったものとして扱う。
function addPassed(painted, fromIndex, toIndex) {
    const result = painted.slice();
    if (fromIndex < 0 || toIndex < 0)
        return result;
    const step = toIndex >= fromIndex ? 1 : -1;
    for (let index = fromIndex; ; index += step) {
        if (result.indexOf(index) < 0)
            result.push(index);
        if (index === toIndex)
            break;
    }
    return result;
}

// 目玉に見せる mute。塗っている最中は、通った track に塗る値を先に見せる。
function displayedMuted(index, actualMuted, painting, painted, paintMuted) {
    return painting && painted.indexOf(index) >= 0 ? paintMuted : actualMuted;
}
