// .pragma library は付けない (TimelineGestures.js と同じ理由)。状態を持たない関数だけを置く。

// 字幕一覧の時刻表示。frame だけでは位置が分かりにくいので秒で示す。
// 1 分未満は "35.1"、1 時間未満は "1:05.3"、それ以上は "1:02:05.3"。
// 表示用の丸めであり、保存・換算には使わない (正確な値は frame の欄で編集する)。
function secondsLabel(frame, fpsNum, fpsDen) {
    if (!(fpsNum > 0) || !(fpsDen > 0) || !(frame >= 0))
        return "";
    // 小数第 1 位で丸めてから桁を分ける。59.96 秒を "59.10" ではなく "1:00.0" と書く。
    const tenths = Math.round(frame * fpsDen * 10 / fpsNum);
    const totalSeconds = Math.floor(tenths / 10);
    const fraction = String(tenths % 10);
    const seconds = totalSeconds % 60;
    const minutes = Math.floor(totalSeconds / 60) % 60;
    const hours = Math.floor(totalSeconds / 3600);
    if (hours > 0)
        return hours + ":" + String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0")
            + "." + fraction;
    if (minutes > 0)
        return minutes + ":" + String(seconds).padStart(2, "0") + "." + fraction;
    return seconds + "." + fraction;
}

function rangeLabel(startFrame, endFrame, fpsNum, fpsDen) {
    return secondsLabel(startFrame, fpsNum, fpsDen) + "–" + secondsLabel(endFrame, fpsNum, fpsDen);
}
