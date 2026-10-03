import QtQuick
import QtQuick.Shapes

// 開閉・送り・一覧の矢印。文字 (v ⌄ ▾ ▸ ‹ ›) で代用すると、フォントの置き換えで
// 「v」や豆腐に崩れ、太さも周りの線と揃わない。矢印は必ずこの図形で描く。
// direction: "down" / "up" / "left" / "right" / "updown" (一覧を開く ComboBox の印)
Item {
    id: icon
    property string direction: "down"
    property color color: "#c8cbd1"
    property real lineWidth: 1.5
    implicitWidth: 10
    implicitHeight: icon.direction === "updown" ? 12 : 10

    // 1 本の「く」の字。tipX, tipY が先端、armX, armY が先端から両端までの差分。
    component Chevron: ShapePath {
        id: chevron
        required property real tipX
        required property real tipY
        required property real armX
        required property real armY
        required property bool horizontal
        strokeColor: icon.color
        strokeWidth: icon.lineWidth
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        joinStyle: ShapePath.RoundJoin
        startX: chevron.tipX - chevron.armX
        startY: chevron.tipY - chevron.armY
        PathLine {
            x: chevron.tipX
            y: chevron.tipY
        }
        PathLine {
            x: chevron.horizontal ? chevron.tipX - chevron.armX : chevron.tipX + chevron.armX
            y: chevron.horizontal ? chevron.tipY + chevron.armY : chevron.tipY - chevron.armY
        }
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        // 下向き (updown では下半分)。
        Chevron {
            tipX: icon.width / 2
            tipY: icon.direction === "updown" ? icon.height - icon.lineWidth : icon.height * 0.72
            armX: icon.width / 2 - icon.lineWidth
            armY: icon.direction === "updown" ? icon.height * 0.28 : icon.height * 0.44
            horizontal: false
            strokeColor: icon.direction === "down" || icon.direction === "updown" ? icon.color
                                                                                  : "transparent"
        }
        // 上向き (updown では上半分)。
        Chevron {
            tipX: icon.width / 2
            tipY: icon.direction === "updown" ? icon.lineWidth : icon.height * 0.28
            armX: icon.width / 2 - icon.lineWidth
            armY: icon.direction === "updown" ? -icon.height * 0.28 : -icon.height * 0.44
            horizontal: false
            strokeColor: icon.direction === "up" || icon.direction === "updown" ? icon.color
                                                                                : "transparent"
        }
        // 右向き。
        Chevron {
            tipX: icon.width * 0.72
            tipY: icon.height / 2
            armX: icon.width * 0.44
            armY: icon.height / 2 - icon.lineWidth
            horizontal: true
            strokeColor: icon.direction === "right" ? icon.color : "transparent"
        }
        // 左向き。
        Chevron {
            tipX: icon.width * 0.28
            tipY: icon.height / 2
            armX: -icon.width * 0.44
            armY: icon.height / 2 - icon.lineWidth
            horizontal: true
            strokeColor: icon.direction === "left" ? icon.color : "transparent"
        }
    }
}
