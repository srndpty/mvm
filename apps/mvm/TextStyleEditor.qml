pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// 文字の書式 (フォント・サイズ・太字・文字色・縁・背景)。文字 clip のエフェクトコントロールと
// 字幕パネルの共通書式が同じ部品を使い、見た目と操作を揃える。
// 値は styleData から読み、変更は signal で返す。保存先の違いは呼び出し側が吸収する。
//   valueCommitted  値を確定する (Undo 1 回分)。元と同じ値では呼ばない。
//   valuesPreviewed Project を変えずに preview だけを描き直す (ドラッグ中・フォントの hover)。
//   previewCanceled 確定せずに preview を元の値へ戻す。
ColumnLayout {
    id: editor
    required property var styleData
    // objectName の接頭辞。文字 clip ("text") と字幕 ("subtitle") の部品を区別する。
    property string namePrefix: "text"
    property int maximumFontSize: 1000
    // 太字ボタンの右に並べる操作 (文字 clip の配置ボタン、字幕の揃えなど)。
    property alias boldRowItems: boldRowExtras.data
    // 太字の行と色の間に置く行 (文字 clip の X / Y、字幕の余白など)。
    property alias middleItems: middleSlot.data
    // 書体の一覧。おすすめ (字幕向きの太く読みやすい書体で、この PC に入っているもの) を先頭に置く。
    readonly property var allFonts: Qt.fontFamilies()
    readonly property var recommendedFonts: {
        const candidates = ["源真ゴシックP Bold", "源真ゴシックP Heavy", "LINE Seed JP_OTF Bold",
                            "LINE Seed JP OTF ExtraBold", "BIZ UDPGothic", "BIZ UDPゴシック",
                            "Noto Sans JP Black", "Noto Sans JP Medium", "源暎ゴシックN Bold",
                            "UD Digi Kyokasho NP-B", "UD デジタル 教科書体 NP", "Meiryo", "メイリオ"];
        return candidates.filter(name => editor.allFonts.indexOf(name) >= 0);
    }
    property string fontFilter: ""
    readonly property var visibleFonts: {
        const filter = editor.fontFilter.trim().toLowerCase();
        if (filter === "")
            return editor.recommendedFonts.concat(editor.allFonts);
        return editor.allFonts.filter(name => name.toLowerCase().indexOf(filter) >= 0);
    }
    signal valueCommitted(string key, var value)
    signal valuesPreviewed(var values)
    signal previewCanceled
    spacing: 4

    function setValue(key, value) {
        if (editor.styleData[key] !== value)
            editor.valueCommitted(key, value);
    }

    // 確定前の値で preview だけを描き直す。色のドラッグやフォントの hover は短い間に何度も来る。
    // 最初の値はすぐ描き、その後は動かしている間も一定間隔で最新の値を描く (throttle)。
    // 止まるのを待つ debounce にすると、ドラッグ中に色が追従しない。
    function schedulePreview(key, value) {
        previewTimer.values = {[key]: value};
        if (previewTimer.running) {
            previewTimer.pending = true;
            return;
        }
        editor.sendPreview();
        previewTimer.start();
    }

    function sendPreview() {
        previewTimer.pending = false;
        editor.valuesPreviewed(previewTimer.values);
    }

    // preview 中の値を確定する。元の値と同じなら保存せず preview を取り消す。
    function commitPreview(key, value) {
        previewTimer.stop();
        previewTimer.pending = false;
        if (editor.styleData[key] === value)
            editor.previewCanceled();
        else
            editor.valueCommitted(key, value);
    }

    function cancelPreview() {
        previewTimer.stop();
        previewTimer.pending = false;
        editor.previewCanceled();
    }

    // 描き直しの最短間隔。1080p の文字画像の描画と preview の seek を毎回行うので、
    // マウスの move ごとには描かない。間隔中に来た最新の値は間隔の終わりに描く。
    Timer {
        id: previewTimer
        property var values: ({})
        property bool pending: false
        interval: 200
        onTriggered: {
            if (!previewTimer.pending)
                return;
            editor.sendPreview();
            previewTimer.start();
        }
    }

    // 入力欄を抜けたら window へ focus を戻す。残すと Space や V などの単キー操作が止まる。
    function releaseFocus() {
        if (editor.Window.window)
            editor.Window.window.contentItem.forceActiveFocus();
    }

    component SectionLabel: Label {
        color: "#9aa2ad"
        font.pixelSize: 11
        Layout.preferredWidth: 44
    }

    // 色の見本。押すと color picker を開く。
    component ColorSwatch: Rectangle {
        id: swatch
        required property string key
        required property string title
        objectName: editor.namePrefix + "ColorSwatch_" + swatch.key
        implicitWidth: 34
        implicitHeight: 20
        radius: 2
        color: "#2a2f37"
        border.color: swatchArea.containsMouse ? "#5b9bd5" : "#5a616c"
        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            color: editor.styleData[swatch.key] || "transparent"
        }
        // 完全に透明な色は見本が背景と区別できないので斜線で示す。
        Rectangle {
            anchors.centerIn: parent
            width: parent.width - 6
            height: 1
            rotation: -30
            color: "#d05050"
            // 保存形式は "#AARRGGBB" なので、先頭 2 桁が alpha。
            visible: String(editor.styleData[swatch.key] || "#00").substring(1, 3) === "00"
        }
        MouseArea {
            id: swatchArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: editor.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                colorPicker.targetKey = swatch.key;
                colorPicker.title = swatch.title;
                colorPicker.initialColor = editor.styleData[swatch.key] || "#FFFFFFFF";
                colorPicker.anchorItem = swatch;
                colorPicker.open();
            }
        }
    }

    // 16 進の直接入力。見本と同じ値を編集する。
    component ColorText: TextField {
        id: colorField
        required property string key
        Layout.fillWidth: true
        implicitHeight: 24
        topPadding: 0
        bottomPadding: 0
        font.pixelSize: 12
        font.family: "Consolas"
        color: "#e6e8ec"
        background: Rectangle {
            radius: 3
            color: "#232830"
            border.color: colorField.activeFocus ? "#5b9bd5" : "#3c424c"
        }
        text: editor.styleData[colorField.key] || ""
        onEditingFinished: editor.setValue(colorField.key, text.toUpperCase())
        Keys.onReturnPressed: editor.releaseFocus()
        Keys.onEscapePressed: {
            text = editor.styleData[colorField.key] || "";
            editor.releaseFocus();
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        ComboBox {
            id: fontBox
            objectName: editor.namePrefix + "FontBox"
            Layout.fillWidth: true
            implicitHeight: 26
            model: editor.allFonts
            displayText: editor.styleData.fontFamily || ""
            currentIndex: fontBox.find(editor.styleData.fontFamily || "")
            font.pixelSize: 12
            focusPolicy: Qt.NoFocus
            // OS の配色に任せず、周りの入力欄と同じ暗い枠と上下の矢印で描く。
            contentItem: Label {
                leftPadding: 8
                rightPadding: 24
                text: fontBox.displayText
                color: "#e6e8ec"
                font: fontBox.font
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            indicator: ChevronIcon {
                x: fontBox.width - width - 9
                y: (fontBox.height - height) / 2
                width: 8
                height: 12
                direction: "updown"
                color: fontBox.enabled ? "#c8cbd1" : "#5b616b"
            }
            background: Rectangle {
                radius: 3
                color: fontBox.pressed ? "#2a2f37" : "#232830"
                border.color: fontBox.hovered || fontBox.popup.visible ? "#5b9bd5" : "#3c424c"
            }
            // 書体は数百あるので、一覧の上の検索欄で部分一致に絞り込む (大文字小文字は区別しない)。
            // Photoshop と同じく、一覧で hover した書体を preview へ仮に反映する。
            // 選べば確定、選ばずに閉じれば元の書体へ戻す。
            popup: Popup {
                id: fontPopup
                y: fontBox.height + 2
                width: Math.max(fontBox.width, 320)
                height: Math.min(460, fontList.contentHeight + fontSearch.implicitHeight + 16)
                padding: 4
                // 検索欄に入力 focus を渡す。渡さないと文字を打てず、Esc でも閉じられない。
                focus: true
                onOpened: {
                    fontSearch.text = "";
                    fontSearch.forceActiveFocus();
                    fontList.currentIndex = editor.visibleFonts.indexOf(editor.styleData.fontFamily || "");
                    fontList.positionViewAtIndex(Math.max(0, fontList.currentIndex), ListView.Center);
                }
                // 確定 (choose) の後にも来る。確定済みなら override は無いので何もしない。
                // 検索欄の focus を window へ戻す。残すと Space や V などの単キー操作が止まる。
                onClosed: {
                    editor.cancelPreview();
                    editor.releaseFocus();
                }
                function choose(family) {
                    editor.commitPreview("fontFamily", family);
                    fontPopup.close();
                }
                background: Rectangle {
                    color: "#292b2f"
                    border.color: "#555960"
                    radius: 4
                }
                contentItem: ColumnLayout {
                    spacing: 4
                    TextField {
                        id: fontSearch
                        objectName: editor.namePrefix + "FontSearch"
                        Layout.fillWidth: true
                        implicitHeight: 26
                        topPadding: 0
                        bottomPadding: 0
                        leftPadding: 8
                        font.pixelSize: 12
                        color: "#e6e8ec"
                        placeholderText: "フォントを検索"
                        placeholderTextColor: "#7d8590"
                        background: Rectangle {
                            radius: 3
                            color: "#1c2026"
                            border.color: fontSearch.activeFocus ? "#5b9bd5" : "#3c424c"
                        }
                        onTextChanged: {
                            editor.fontFilter = text;
                            fontList.currentIndex = editor.visibleFonts.length > 0 ? 0 : -1;
                            fontList.positionViewAtBeginning();
                        }
                        Keys.onDownPressed: {
                            fontList.hoveredIndex = -1;
                            fontList.incrementCurrentIndex();
                        }
                        Keys.onUpPressed: {
                            fontList.hoveredIndex = -1;
                            fontList.decrementCurrentIndex();
                        }
                        Keys.onReturnPressed: {
                            if (fontList.currentIndex >= 0)
                                fontPopup.choose(editor.visibleFonts[fontList.currentIndex]);
                        }
                        Keys.onEnterPressed: {
                            if (fontList.currentIndex >= 0)
                                fontPopup.choose(editor.visibleFonts[fontList.currentIndex]);
                        }
                        Keys.onEscapePressed: fontPopup.close()
                    }
                    BoundedListView {
                        id: fontList
                        objectName: editor.namePrefix + "FontList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        implicitHeight: contentHeight
                        clip: true
                        model: editor.visibleFonts
                        highlightMoveDuration: 0
                        ScrollIndicator.vertical: ScrollIndicator {}
                        // キー操作で選んでいる行。キー操作では一覧を追従させる。
                        onCurrentIndexChanged: {
                            if (fontPopup.visible && currentIndex >= 0)
                                editor.schedulePreview("fontFamily", editor.visibleFonts[currentIndex]);
                        }
                        // マウスで指している行。currentIndex を動かすと ListView が
                        // その行へ寄せて一覧が勝手に動くので、hover は別に持つ。
                        property int hoveredIndex: -1
                        // Premiere と同じく、名前の右にその書体の見本を出す。おすすめと全書体の
                        // 境目には見出しを出す (絞り込み中は出さない)。
                        delegate: ItemDelegate {
                            id: fontItem
                            required property int index
                            required property string modelData
                            readonly property string caption:
                                editor.fontFilter !== "" ? ""
                                : fontItem.index === 0 && editor.recommendedFonts.length > 0 ? "おすすめ (字幕向き)"
                                : fontItem.index === editor.recommendedFonts.length ? "すべてのフォント" : ""
                            width: ListView.view ? ListView.view.width : fontBox.width
                            height: 26 + (fontItem.caption !== "" ? 20 : 0)
                            topPadding: fontItem.caption !== "" ? 20 : 0
                            hoverEnabled: true
                            highlighted: fontList.hoveredIndex >= 0 ? fontList.hoveredIndex === fontItem.index
                                                                    : fontList.currentIndex === fontItem.index
                            onHoveredChanged: {
                                if (fontItem.hovered) {
                                    fontList.hoveredIndex = fontItem.index;
                                    editor.schedulePreview("fontFamily", fontItem.modelData);
                                } else if (fontList.hoveredIndex === fontItem.index) {
                                    fontList.hoveredIndex = -1;
                                }
                            }
                            onClicked: fontPopup.choose(fontItem.modelData)
                            background: Rectangle {
                                y: fontItem.topPadding
                                width: parent.width
                                height: parent.height - fontItem.topPadding
                                radius: 3
                                color: fontItem.highlighted ? "#30455c" : "transparent"
                            }
                            Label {
                                visible: fontItem.caption !== ""
                                x: 8
                                y: 3
                                text: fontItem.caption
                                color: "#7d8590"
                                font.pixelSize: 11
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: fontItem.modelData
                                    elide: Text.ElideRight
                                    font.pixelSize: 12
                                    color: "#e6e8ec"
                                }
                                Label {
                                    text: "文字もじモジ"
                                    font.family: fontItem.modelData
                                    font.pixelSize: 14
                                    color: "#c8ced6"
                                }
                            }
                        }
                    }
                    Label {
                        visible: editor.visibleFonts.length === 0
                        Layout.fillWidth: true
                        Layout.margins: 8
                        text: "「" + fontSearch.text + "」に一致するフォントはありません"
                        color: "#9aa2ad"
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        StyleNumberField {
            styleData: editor.styleData
            key: "fontSize"
            namePrefix: editor.namePrefix
            labelText: ""
            inlineLabelWidth: 1
            Layout.preferredWidth: 58
            minimumValue: 1
            maximumValue: editor.maximumFontSize
            stepPerPixel: 0.5
            onPreviewed: (key, value) => editor.valuesPreviewed({[key]: value})
            onCommitted: (key, value) => editor.valueCommitted(key, value)
            onCanceled: editor.previewCanceled()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Button {
            id: boldButton
            objectName: editor.namePrefix + "BoldButton"
            checkable: true
            checked: !!editor.styleData.bold
            implicitWidth: 28
            implicitHeight: 24
            focusPolicy: Qt.NoFocus
            text: "B"
            font.bold: true
            font.pixelSize: 13
            contentItem: Label {
                text: boldButton.text
                font: boldButton.font
                color: boldButton.checked ? "#ffffff" : "#c8cbd1"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 3
                color: boldButton.checked ? "#2f4a66" : (boldButton.hovered ? "#2a2f37" : "transparent")
                border.color: boldButton.checked || boldButton.hovered ? "#5b9bd5" : "#3c424c"
            }
            onClicked: editor.setValue("bold", boldButton.checked)
            ToolTip.visible: hovered
            ToolTip.text: "太字"
        }
        Item {
            implicitWidth: 6
        }
        RowLayout {
            id: boldRowExtras
            spacing: 4
        }
        Item {
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        id: middleSlot
        Layout.fillWidth: true
        spacing: 4
        visible: middleSlot.children.length > 0
    }

    GridLayout {
        Layout.fillWidth: true
        columns: 3
        columnSpacing: 6
        rowSpacing: 4
        SectionLabel {
            text: "文字色"
        }
        ColorSwatch {
            key: "color"
            title: "文字色"
        }
        ColorText {
            key: "color"
        }

        SectionLabel {
            text: "縁"
        }
        ColorSwatch {
            key: "outlineColor"
            title: "縁の色"
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            ColorText {
                key: "outlineColor"
            }
            StyleNumberField {
                styleData: editor.styleData
                key: "outlineWidth"
                namePrefix: editor.namePrefix
                labelText: "幅"
                Layout.preferredWidth: 70
                minimumValue: 0
                maximumValue: 32
                stepPerPixel: 0.2
                onPreviewed: (key, value) => editor.valuesPreviewed({[key]: value})
                onCommitted: (key, value) => editor.valueCommitted(key, value)
                onCanceled: editor.previewCanceled()
            }
        }

        SectionLabel {
            text: "背景"
        }
        ColorSwatch {
            key: "backgroundColor"
            title: "背景色"
        }
        ColorText {
            key: "backgroundColor"
        }
    }

    ModernColorPicker {
        id: colorPicker
        objectName: editor.namePrefix + "ColorPicker"
        property string targetKey: ""
        onColorEdited: argb => editor.schedulePreview(colorPicker.targetKey, argb)
        onColorAccepted: argb => editor.commitPreview(colorPicker.targetKey, argb)
        onColorCanceled: editor.cancelPreview()
    }
}
