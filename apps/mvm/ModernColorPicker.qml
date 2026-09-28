pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 色選択。ModernDialog の見た目に揃え、alpha まで選べるようにする。
// 値は Project の保存形式と同じ "#AARRGGBB" で受け渡す。
//
// 押したボタンの横に、背景を暗くせずに出す (非 modal)。選んでいる途中の色は colorEdited で
// 知らせるので、呼び出し側は preview へそのまま反映できる。OK で colorAccepted、
// それ以外 (キャンセル・Esc・外側の押下) で閉じたら colorCanceled を出す。
ModernDialog {
    id: picker
    // 開く前に設定する。
    property string initialColor: "#FFFFFFFF"
    // この item の右横に出す。未設定なら window の中央。
    property Item anchorItem: null
    signal colorEdited(string argb)
    signal colorAccepted(string argb)
    signal colorCanceled()
    property bool accepting: false
    property bool loading: false

    width: 360
    modal: false
    dim: false
    // Esc で閉じられるよう、開いたら focus を取る。
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: picker.anchorItem ? picker.anchorItem : Overlay.overlay
    x: picker.anchorItem ? picker.anchorItem.width + 8 : (parent.width - width) / 2
    y: picker.anchorItem ? -12 : (parent.height - height) / 2
    // window の外へはみ出さないように寄せる。
    margins: 8

    // 編集中の色は HSV + alpha で持つ。RGB で持つと彩度 0 のときに色相が失われる。
    property real hue: 0
    property real saturation: 0
    property real brightness: 1
    property real alpha: 1
    property color parsedColor: "#FFFFFFFF"
    readonly property color currentColor: Qt.hsva(picker.hue, picker.saturation, picker.brightness,
                                                  picker.alpha)

    function argbText(value) {
        const hex = component => ("0" + Math.round(component * 255).toString(16)).slice(-2);
        return ("#" + hex(value.a) + hex(value.r) + hex(value.g) + hex(value.b)).toUpperCase();
    }

    // "#AARRGGBB" / "#RRGGBB" を読む。読めなければ false (値は変えない)。
    function load(text) {
        const trimmed = String(text).trim();
        if (!/^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(trimmed))
            return false;
        // color property への代入で "#AARRGGBB" を解釈させる。
        picker.parsedColor = trimmed.length === 7 ? "#FF" + trimmed.substring(1) : trimmed;
        const value = picker.parsedColor;
        // 無彩色では hsvHue が -1 になる。直前の色相を残して、戻したときに色が飛ばないようにする。
        if (value.hsvHue >= 0)
            picker.hue = value.hsvHue;
        picker.saturation = value.hsvSaturation;
        picker.brightness = value.hsvValue;
        picker.alpha = value.a;
        return true;
    }

    onAboutToShow: {
        picker.accepting = false;
        picker.loading = true;
        picker.load(picker.initialColor);
        picker.loading = false;
        hexField.text = picker.argbText(picker.currentColor);
    }
    onCurrentColorChanged: {
        if (!hexField.activeFocus)
            hexField.text = picker.argbText(picker.currentColor);
        if (picker.visible && !picker.loading)
            picker.colorEdited(picker.argbText(picker.currentColor));
    }
    onClosed: {
        if (!picker.accepting)
            picker.colorCanceled();
        picker.accepting = false;
    }

    // 市松模様。alpha の見本の下地にする。
    component Checker: Canvas {
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const context = getContext("2d");
            const cell = 6;
            for (let y = 0; y < height; y += cell)
                for (let x = 0; x < width; x += cell) {
                    context.fillStyle = ((x + y) / cell) % 2 === 0 ? "#3a3f46" : "#23272d";
                    context.fillRect(x, y, cell, cell);
                }
        }
    }

    contentItem: ColumnLayout {
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 彩度 (横) と明度 (縦)。
            Rectangle {
                id: field
                objectName: "colorPickerField"
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                radius: 4
                color: Qt.hsva(picker.hue, 1, 1, 1)
                border.color: "#51565e"
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: "#ffffffff" }
                        GradientStop { position: 1; color: "#00ffffff" }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#00000000" }
                        GradientStop { position: 1; color: "#ff000000" }
                    }
                }
                Rectangle {
                    x: picker.saturation * field.width - width / 2
                    y: (1 - picker.brightness) * field.height - height / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: "transparent"
                    border.color: picker.brightness > 0.5 ? "#000000" : "#ffffff"
                    border.width: 2
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function pick(mouse) {
                        picker.saturation = Math.max(0, Math.min(1, mouse.x / width));
                        picker.brightness = 1 - Math.max(0, Math.min(1, mouse.y / height));
                    }
                    onPressed: mouse => pick(mouse)
                    onPositionChanged: mouse => pick(mouse)
                }
            }

            // 色相 (縦)。
            Rectangle {
                id: hueBar
                Layout.preferredWidth: 16
                Layout.preferredHeight: 200
                radius: 3
                border.color: "#51565e"
                gradient: Gradient {
                    GradientStop { position: 0.000; color: "#ff0000" }
                    GradientStop { position: 0.167; color: "#ffff00" }
                    GradientStop { position: 0.333; color: "#00ff00" }
                    GradientStop { position: 0.500; color: "#00ffff" }
                    GradientStop { position: 0.667; color: "#0000ff" }
                    GradientStop { position: 0.833; color: "#ff00ff" }
                    GradientStop { position: 1.000; color: "#ff0000" }
                }
                Rectangle {
                    x: -2
                    y: picker.hue * hueBar.height - height / 2
                    width: hueBar.width + 4
                    height: 4
                    radius: 2
                    color: "transparent"
                    border.color: "#ffffff"
                    border.width: 1.5
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    onPressed: mouse => picker.hue = Math.max(0, Math.min(0.9999, mouse.y / height))
                    onPositionChanged: mouse => picker.hue = Math.max(0, Math.min(0.9999, mouse.y / height))
                }
            }
        }

        // 不透明度。
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Label {
                text: "不透明度"
                color: "#aeb4bf"
                Layout.preferredWidth: 56
            }
            Item {
                id: alphaBar
                Layout.fillWidth: true
                Layout.preferredHeight: 14
                Checker {
                    anchors.fill: parent
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 3
                    border.color: "#51565e"
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Qt.hsva(picker.hue, picker.saturation, picker.brightness, 0) }
                        GradientStop { position: 1; color: Qt.hsva(picker.hue, picker.saturation, picker.brightness, 1) }
                    }
                }
                Rectangle {
                    x: picker.alpha * alphaBar.width - width / 2
                    y: -2
                    width: 4
                    height: alphaBar.height + 4
                    radius: 2
                    color: "transparent"
                    border.color: "#ffffff"
                    border.width: 1.5
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    onPressed: mouse => picker.alpha = Math.max(0, Math.min(1, mouse.x / width))
                    onPositionChanged: mouse => picker.alpha = Math.max(0, Math.min(1, mouse.x / width))
                }
            }
            Label {
                text: Math.round(picker.alpha * 100) + "%"
                color: "#e6e8ec"
                Layout.preferredWidth: 38
                horizontalAlignment: Text.AlignRight
            }
        }

        // テロップでよく使う色。押すと alpha は 100% になる。
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: ["#FFFFFFFF", "#FF000000", "#FFFFE600", "#FFFF3B30", "#FF2F80ED",
                        "#FF27AE60", "#FFFF8C00", "#FFE040FB"]
                delegate: Rectangle {
                    id: preset
                    required property string modelData
                    objectName: "colorPreset_" + preset.modelData
                    implicitWidth: 24
                    implicitHeight: 24
                    radius: 4
                    color: preset.modelData
                    border.color: presetArea.containsMouse ? "#6ca9e6" : "#51565e"
                    MouseArea {
                        id: presetArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picker.load(preset.modelData)
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            // 変更前 (左) と変更後 (右) の見本。
            Item {
                implicitWidth: 64
                implicitHeight: 34
                Checker { anchors.fill: parent }
                Row {
                    anchors.fill: parent
                    Rectangle {
                        width: parent.width / 2
                        height: parent.height
                        color: picker.initialColor
                    }
                    Rectangle {
                        width: parent.width / 2
                        height: parent.height
                        color: picker.currentColor
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: "#51565e"
                    radius: 3
                }
            }
            ModernDialogField {
                id: hexField
                Layout.fillWidth: true
                font.family: "Consolas"
                onEditingFinished: {
                    if (!picker.load(text))
                        text = picker.argbText(picker.currentColor);
                }
            }
        }
    }

    footer: ModernDialogFooter {
        ModernDialogButton {
            text: "キャンセル"
            onClicked: picker.reject()
        }
        ModernDialogButton {
            text: "OK"
            objectName: "colorPickerOk"
            prominent: true
            onClicked: {
                picker.load(hexField.text);
                picker.accepting = true;
                picker.colorAccepted(picker.argbText(picker.currentColor));
                picker.accept();
            }
        }
    }
}
