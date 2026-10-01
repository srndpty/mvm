import QtQuick

// video track の表示切り替えの目玉。非表示ならスラッシュを重ねる。画像アセットを持たず、
// 16x16 の座標系へ線で描く。
Canvas {
    id: root

    property bool hidden: false
    property color color: "#c9ccd2"

    implicitWidth: 16
    implicitHeight: 16

    onHiddenChanged: requestPaint()
    onColorChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        ctx.scale(width / 16, height / 16);
        ctx.strokeStyle = root.color;
        ctx.fillStyle = root.color;
        ctx.lineWidth = 1.4;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        // まぶた (上下の弧) と瞳。
        ctx.beginPath();
        ctx.moveTo(1.5, 8);
        ctx.quadraticCurveTo(8, 1, 14.5, 8);
        ctx.quadraticCurveTo(8, 15, 1.5, 8);
        ctx.closePath();
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(8, 8, 2.2, 0, Math.PI * 2);
        ctx.fill();

        if (root.hidden) {
            ctx.lineWidth = 1.6;
            ctx.beginPath();
            ctx.moveTo(2.5, 14);
            ctx.lineTo(13.5, 2);
            ctx.stroke();
        }
    }
}
