import QtQuick
import QtQuick.Controls

// 数値を左右ドラッグで変更できるフィールド。ダブルクリック (clickToEdit ならシングルクリック)
// で直接入力へ切り替わる。
// ドラッグ中は commit=false で通知し、離した時だけ commit=true にする。
// 保存のたびに Project を書き直さないための区別であり、表示上の都合ではない。
Item {
    id: root

    property string labelText
    property real value: 0
    property real minimumValue: -100000
    property real maximumValue: 100000
    // 1 px ドラッグあたりの変化量。
    property real stepPerPixel: 0.5
    property int decimals: 0
    property string suffix: ""
    // 0 より大きいと label を左に置き、1 行 (高さ 24) にする。値は label の幅。
    property real inlineLabelWidth: 0
    readonly property bool inlineLabel: root.inlineLabelWidth > 0
    // true ならドラッグせずに離したクリック 1 回で直接入力へ切り替える (ダイアログ用)。
    // 既定の false は、パネル上で値を誤って入力モードにしないためダブルクリックを要求する。
    property bool clickToEdit: false
    // 直接入力を閉じたときに focus を返す先。null なら window。ダイアログ内では
    // ダイアログの中を指定する。window へ返すと focus が popup の外へ出て、Esc で
    // ダイアログを閉じられなくなる。
    property Item focusReturnItem: null

    // 直接入力の文字列全体を数値として読む。末尾の単位 (suffix) だけは付いていてよい。
    // parseFloat は "50foo" を 50 と読んでしまうので使わない。読めなければ NaN。
    function parseEditorText(text) {
        let body = text.trim();
        const unit = root.suffix.trim();
        if (unit.length > 0 && body.endsWith(unit))
            body = body.slice(0, body.length - unit.length).trim();
        if (body.length === 0)
            return NaN;
        const parsed = Number(body);
        return isFinite(parsed) ? parsed : NaN;
    }

    // 直接入力中の文字列を確定する。入力中に別のボタンで確定するダイアログは、
    // Enter を待たずにこれを呼んで値を取り込む。
    // 入力していなければ、または数値として読めて確定したら true。読めなければ入力欄を
    // 開いたまま false を返す (古い値のまま閉じて、呼び出し側が古い値で処理を続けないように)。
    function commitEditing() {
        if (!editor.visible)
            return true;
        const parsed = root.parseEditorText(editor.text);
        if (isNaN(parsed))
            return false;
        root.valueEdited(root.clampValue(parsed), true);
        editor.finish();
        return true;
    }

    function beginEditing() {
        editor.text = root.value.toFixed(root.decimals);
        editor.visible = true;
        editor.forceActiveFocus();
        editor.selectAll();
    }
    // enabled は Item から継承したものをそのまま使う。同名 property を足すと
    // 親 (GridLayout) の enabled が伝わらず、無効化しても drag できてしまう。

    signal valueEdited(real newValue, bool commit)
    // grab を奪われた等で release が来なかった場合。編集を確定させない。
    signal editCanceled()

    implicitWidth: root.inlineLabel ? root.inlineLabelWidth + 64 : 116
    implicitHeight: root.inlineLabel ? 24 : 38

    function clampValue(candidate) {
        return Math.max(root.minimumValue, Math.min(root.maximumValue, candidate));
    }

    function formatted(candidate) {
        return candidate.toFixed(root.decimals) + root.suffix;
    }

    Label {
        id: caption
        x: 2
        width: root.inlineLabel ? root.inlineLabelWidth - 4 : parent.width - 4
        height: root.inlineLabel ? parent.height : implicitHeight
        verticalAlignment: Text.AlignVCenter
        text: root.labelText
        color: "#9aa2ad"
        font.pixelSize: 10
        elide: Text.ElideRight
    }

    Rectangle {
        id: box
        x: root.inlineLabel ? root.inlineLabelWidth : 0
        y: root.inlineLabel ? 0 : caption.height + 2
        width: parent.width - x
        height: root.inlineLabel ? parent.height : parent.height - caption.height - 2
        radius: 3
        color: root.enabled ? (dragArea.pressed ? "#2f3945" : "#232830") : "#1c2026"
        border.color: dragArea.containsMouse || editor.visible ? "#5b9bd5" : "#3c424c"

        Label {
            anchors.fill: parent
            anchors.leftMargin: 8
            visible: !editor.visible
            verticalAlignment: Text.AlignVCenter
            text: root.formatted(root.value)
            color: root.enabled ? "#e6e8ec" : "#6a707a"
            font.pixelSize: 13
        }

        TextField {
            id: editor
            anchors.fill: parent
            visible: false
            selectByMouse: true
            font.pixelSize: 13
            topPadding: 0
            bottomPadding: 0
            leftPadding: 7
            verticalAlignment: TextInput.AlignVCenter
            onAccepted: root.commitEditing()
            Keys.onEscapePressed: editor.finish()

            // 非表示にした editor に focus が残ると、Space の再生や tool の単キー操作が
            // 止まったままになる。確定・取り消しで window へ focus を戻す。
            function finish() {
                const hadFocus = editor.activeFocus;
                editor.visible = false;
                if (!hadFocus)
                    return;
                if (root.focusReturnItem)
                    root.focusReturnItem.forceActiveFocus();
                else if (root.Window.window)
                    root.Window.window.contentItem.forceActiveFocus();
            }
            // ダイアログ (clickToEdit) では、入力中に「適用」を押すと先に focus が外れる。
            // そこで捨てると入力した値が失われるので確定する。パネルでは従来どおり取り消す。
            onActiveFocusChanged: {
                if (activeFocus || !editor.visible)
                    return;
                if (root.clickToEdit)
                    root.commitEditing();
                else
                    editor.visible = false;
            }
        }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            enabled: root.enabled && !editor.visible
            hoverEnabled: true
            cursorShape: Qt.SizeHorCursor
            preventStealing: true

            property real pressX: 0
            property real pressValue: 0
            property bool dragged: false

            onPressed: mouse => {
                pressX = mouse.x;
                pressValue = root.value;
                dragged = false;
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return;
                const delta = mouse.x - pressX;
                if (!dragged && Math.abs(delta) < 3)
                    return;
                dragged = true;
                root.valueEdited(root.clampValue(pressValue + delta * root.stepPerPixel), false);
            }
            onReleased: {
                if (dragged)
                    root.valueEdited(root.value, true);
                else if (root.clickToEdit)
                    root.beginEditing();
                dragged = false;
            }
            onCanceled: {
                if (dragged)
                    root.editCanceled();
                dragged = false;
            }
            onDoubleClicked: root.beginEditing()
        }
    }
}
