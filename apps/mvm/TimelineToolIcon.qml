import QtQuick

// タイムラインツールのアイコン。画像アセットを持たず、16x16 の座標系へ線で描く。
Canvas {
    id: root

    // TimelineToolPanel.tools の tool 名。
    property string tool: "select"
    property color color: "#c9ccd2"

    implicitWidth: 16
    implicitHeight: 16

    onToolChanged: requestPaint()
    onColorChanged: requestPaint()

    function arrowHead(ctx, x, y, direction) {
        ctx.beginPath();
        ctx.moveTo(x, y);
        ctx.lineTo(x - 3 * direction, y - 3);
        ctx.lineTo(x - 3 * direction, y + 3);
        ctx.closePath();
        ctx.fill();
    }

    function horizontalArrow(ctx, fromX, toX, y) {
        ctx.beginPath();
        ctx.moveTo(fromX, y);
        ctx.lineTo(toX, y);
        ctx.stroke();
        arrowHead(ctx, toX + (toX > fromX ? 1 : -1), y, toX > fromX ? 1 : -1);
    }

    // "]" / "[" 形の編集点。open が 1 なら右へ開く "["。
    function bracket(ctx, x, open) {
        ctx.beginPath();
        ctx.moveTo(x + 2.5 * open, 2.5);
        ctx.lineTo(x, 2.5);
        ctx.lineTo(x, 13.5);
        ctx.lineTo(x + 2.5 * open, 13.5);
        ctx.stroke();
    }

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        ctx.scale(width / 16, height / 16);
        ctx.strokeStyle = root.color;
        ctx.fillStyle = root.color;
        ctx.lineWidth = 1.4;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";

        switch (root.tool) {
        case "select":
            ctx.beginPath();
            ctx.moveTo(4, 1.5);
            ctx.lineTo(4, 13);
            ctx.lineTo(6.8, 10.3);
            ctx.lineTo(8.8, 14.5);
            ctx.lineTo(10.6, 13.6);
            ctx.lineTo(8.6, 9.6);
            ctx.lineTo(12.3, 9.6);
            ctx.closePath();
            ctx.fill();
            break;
        case "trackForward":
        case "trackBackward": {
            const forward = root.tool === "trackForward";
            const base = forward ? 2 : 14;
            const direction = forward ? 1 : -1;
            ctx.beginPath();
            ctx.moveTo(base, 2);
            ctx.lineTo(base, 14);
            ctx.stroke();
            horizontalArrow(ctx, base + 2 * direction, base + 9 * direction, 5);
            horizontalArrow(ctx, base + 2 * direction, base + 9 * direction, 11);
            break;
        }
        case "razor":
            ctx.translate(8, 8);
            ctx.rotate(-Math.PI / 4);
            ctx.strokeRect(-3, -7, 6, 10);
            ctx.beginPath();
            ctx.moveTo(0, -4.5);
            ctx.lineTo(0, 0.5);
            ctx.stroke();
            ctx.fillRect(-1.5, 3, 3, 4.5);
            break;
        case "ripple":
            bracket(ctx, 6, -1);
            horizontalArrow(ctx, 8.5, 13, 8);
            break;
        case "rolling":
            bracket(ctx, 7, -1);
            bracket(ctx, 9, 1);
            horizontalArrow(ctx, 5, 1.5, 8);
            horizontalArrow(ctx, 11, 14.5, 8);
            break;
        case "rate":
            bracket(ctx, 2, 1);
            bracket(ctx, 14, -1);
            ctx.beginPath();
            ctx.arc(8, 8, 3.5, 0, Math.PI * 2);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(8, 8);
            ctx.lineTo(8, 5.8);
            ctx.moveTo(8, 8);
            ctx.lineTo(9.8, 8);
            ctx.stroke();
            break;
        case "slip":
            bracket(ctx, 2, 1);
            bracket(ctx, 14, -1);
            horizontalArrow(ctx, 7, 11, 8);
            horizontalArrow(ctx, 9, 5, 8);
            break;
        case "slide":
            ctx.strokeRect(5, 4, 6, 8);
            horizontalArrow(ctx, 4, 1.5, 8);
            horizontalArrow(ctx, 12, 14.5, 8);
            break;
        case "pen":
            ctx.beginPath();
            ctx.moveTo(3, 13);
            ctx.lineTo(4.2, 8.8);
            ctx.lineTo(11, 2);
            ctx.lineTo(14, 5);
            ctx.lineTo(7.2, 11.8);
            ctx.closePath();
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(3, 13);
            ctx.lineTo(5.6, 10.4);
            ctx.stroke();
            break;
        case "hand":
            ctx.beginPath();
            ctx.moveTo(4.5, 9);
            ctx.lineTo(4.5, 11);
            ctx.quadraticCurveTo(5, 14.5, 8.5, 14.5);
            ctx.quadraticCurveTo(12.5, 14.5, 12.5, 10.5);
            ctx.lineTo(12.5, 5.5);
            ctx.stroke();
            for (const finger of [[6, 4.5], [8.3, 2.5], [10.5, 3.5], [12.5, 5.5]]) {
                ctx.beginPath();
                ctx.moveTo(finger[0], 9.5);
                ctx.lineTo(finger[0], finger[1]);
                ctx.stroke();
            }
            ctx.beginPath();
            ctx.moveTo(4.5, 9);
            ctx.lineTo(2.5, 7.5);
            ctx.stroke();
            break;
        case "zoom":
            ctx.beginPath();
            ctx.arc(6.5, 6.5, 4.3, 0, Math.PI * 2);
            ctx.stroke();
            ctx.lineWidth = 2;
            ctx.beginPath();
            ctx.moveTo(9.7, 9.7);
            ctx.lineTo(14, 14);
            ctx.stroke();
            break;
        case "text":
            ctx.lineWidth = 1.8;
            ctx.beginPath();
            ctx.moveTo(3, 3);
            ctx.lineTo(13, 3);
            ctx.moveTo(8, 3);
            ctx.lineTo(8, 14);
            ctx.moveTo(6, 14);
            ctx.lineTo(10, 14);
            ctx.stroke();
            break;
        }
    }
}
