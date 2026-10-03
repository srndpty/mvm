pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "SubtitleTime.js" as SubtitleTime

BoundedScrollView {
    id: panel
    objectName: "subtitlePanelScroll"
    clip: true
    padding: 8
    background: Rectangle {
        color: "#1c2127"
        radius: 6
    }
    contentWidth: availableWidth
    palette.text: "#f0f1f3"
    palette.windowText: "#c8cbd1"
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    required property var mvmController
    readonly property bool editable: !mvmController.busy && !mvmController.playing
    readonly property var subtitleStyle: panel.mvmController.subtitleStyle

    // 一覧の高さと書式欄の開閉は、プロジェクトではなくこの端末の表示設定として保存する。
    // listHeight が 0 なら、パネルの高さに合わせた既定の高さにする。
    Settings {
        id: layoutSettings
        category: "subtitlePanel"
        property real listHeight: 0
        property bool styleExpanded: true
    }
    readonly property real defaultListHeight: Math.max(180, panel.height * 0.5)

    function timeLabel(startFrame, endFrame) {
        return SubtitleTime.rangeLabel(startFrame, endFrame, panel.mvmController.timelineFpsNum,
                                       panel.mvmController.timelineFpsDen);
    }

    // 本文欄で最後に置いたカーソルの位置。「再生位置で分割」で本文をここで分ける。-1 は未指定。
    property int bodyCursor: -1
    function reloadEditor() {
        bodyCursor = -1;
        const cue = mvmController.selectedSubtitle;
        body.text = cue.content || "";
        startField.text = String(cue.startFrame === undefined ? mvmController.playheadFrame : cue.startFrame);
        endField.text = String(cue.endFrame === undefined ? mvmController.playheadFrame + Math.round(3 * mvmController.timelineFpsNum / mvmController.timelineFpsDen) : cue.endFrame);
    }
    Connections {
        target: panel.mvmController
        function onStateChanged() {
            if (!body.activeFocus && !startField.activeFocus && !endField.activeFocus)
                panel.reloadEditor();
        }
    }
    Component.onCompleted: reloadEditor()

    // 字幕の行揃え。押した向きに揃える排他ボタンで、行の並びを線で示す。
    component AlignButton: AbstractButton {
        id: alignButton
        required property string alignment
        required property string tip
        objectName: "subtitleAlign_" + alignButton.alignment
        readonly property bool current: panel.subtitleStyle.alignment === alignButton.alignment
        implicitWidth: 30
        implicitHeight: 24
        focusPolicy: Qt.NoFocus
        onClicked: {
            if (!alignButton.current)
                panel.mvmController.setSubtitleStyle({alignment: alignButton.alignment});
        }
        ToolTip.visible: hovered
        ToolTip.delay: 400
        ToolTip.text: alignButton.tip
        background: Rectangle {
            radius: 3
            color: alignButton.current ? "#2f4a66" : (alignButton.hovered ? "#2a2f37" : "transparent")
            border.color: alignButton.current || alignButton.hovered ? "#5b9bd5" : "#3c424c"
        }
        contentItem: Item {
            Repeater {
                model: [14, 9, 12]
                delegate: Rectangle {
                    required property int index
                    required property int modelData
                    width: modelData
                    height: 2
                    radius: 1
                    y: 6 + index * 5
                    x: alignButton.alignment === "left" ? 8
                       : alignButton.alignment === "right" ? 22 - modelData : 15 - modelData / 2
                    color: alignButton.current || alignButton.hovered ? "#e6e8ec" : "#9aa2ad"
                }
            }
        }
    }

    ColumnLayout {
        width: panel.availableWidth
        spacing: 8
        Flow {
            Layout.fillWidth: true
            spacing: 6
            ModernDialogButton {
                objectName: "transcribeOpenButton"
                text: "自動文字起こし"
                prominent: true
                enabled: panel.editable
                onClicked: transcribeDialog.open()
            }
            ModernDialogButton {
                text: "SRT読込"
                enabled: panel.editable
                onClicked: importDialog.open()
            }
            ModernDialogComboBox {
                id: importMode
                width: 92
                model: ["置換", "追加"]
                enabled: panel.editable
            }
            ModernDialogButton {
                text: "SRT出力"
                onClicked: exportDialog.open()
            }
        }
        ModernDialogCheckBox {
            text: "字幕を表示"
            checked: panel.mvmController.subtitlesVisible
            enabled: panel.editable
            onClicked: panel.mvmController.setSubtitlesVisible(checked)
        }
        // 全字幕の共通書式。文字 clip のエフェクトコントロールと同じ TextStyleEditor で編集し、
        // 値は 1 回ごとに確定する (Undo 1 回分)。見出しで開閉する。
        AbstractButton {
            id: styleHeader
            objectName: "subtitleStyleHeader"
            Layout.fillWidth: true
            implicitHeight: 28
            focusPolicy: Qt.NoFocus
            onClicked: layoutSettings.styleExpanded = !layoutSettings.styleExpanded
            background: Rectangle {
                radius: 3
                color: styleHeader.hovered ? "#252b33" : "transparent"
            }
            contentItem: RowLayout {
                spacing: 8
                ChevronIcon {
                    Layout.leftMargin: 6
                    implicitWidth: 9
                    implicitHeight: 9
                    direction: layoutSettings.styleExpanded ? "down" : "right"
                }
                Label {
                    text: "共通書式"
                    color: "#e6e8ec"
                    font.pixelSize: 12
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: panel.subtitleStyle.fontFamily + "・" + panel.subtitleStyle.fontSize + "px"
                    color: "#7d8590"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
        TextStyleEditor {
            objectName: "subtitleStyleEditor"
            Layout.fillWidth: true
            Layout.leftMargin: 4
            visible: layoutSettings.styleExpanded
            enabled: panel.editable
            styleData: panel.subtitleStyle
            namePrefix: "subtitle"
            maximumFontSize: panel.mvmController.outputHeight
            onValueCommitted: (key, value) => panel.mvmController.setSubtitleStyle({[key]: value})
            onValuesPreviewed: values => panel.mvmController.previewSubtitleStyle(values)
            onPreviewCanceled: panel.mvmController.cancelSubtitleStylePreview()
            boldRowItems: [
                AlignButton {
                    alignment: "left"
                    tip: "左揃え"
                },
                AlignButton {
                    alignment: "center"
                    tip: "中央揃え"
                },
                AlignButton {
                    alignment: "right"
                    tip: "右揃え"
                }
            ]
            middleItems: [
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        text: "余白"
                        color: "#9aa2ad"
                        font.pixelSize: 11
                        Layout.preferredWidth: 38
                    }
                    StyleNumberField {
                        styleData: panel.subtitleStyle
                        key: "sideMargin"
                        namePrefix: "subtitle"
                        scale: 100
                        labelText: "左右"
                        inlineLabelWidth: 28
                        suffix: " %"
                        Layout.fillWidth: true
                        minimumValue: 0
                        maximumValue: 49
                        stepPerPixel: 0.2
                        onPreviewed: (key, value) => panel.mvmController.previewSubtitleStyle({[key]: value})
                        onCommitted: (key, value) => panel.mvmController.setSubtitleStyle({[key]: value})
                        onCanceled: panel.mvmController.cancelSubtitleStylePreview()
                    }
                    StyleNumberField {
                        styleData: panel.subtitleStyle
                        key: "bottomMargin"
                        namePrefix: "subtitle"
                        scale: 100
                        labelText: "下"
                        suffix: " %"
                        Layout.fillWidth: true
                        minimumValue: 0
                        maximumValue: 99
                        stepPerPixel: 0.2
                        onPreviewed: (key, value) => panel.mvmController.previewSubtitleStyle({[key]: value})
                        onCommitted: (key, value) => panel.mvmController.setSubtitleStyle({[key]: value})
                        onCanceled: panel.mvmController.cancelSubtitleStylePreview()
                    }
                }
            ]
        }
        // 一覧の枠。一覧がパネルの幅いっぱいだと、ホイールが常に一覧へ渡り、パネルの下へ
        // 進めない。左右に少し余白を残し、そこではパネル全体をスクロールできるようにする。
        // 枠を描いて、一覧の範囲と余白の境目を見せる。
        Rectangle {
            id: listFrame
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            Layout.preferredHeight: layoutSettings.listHeight > 0 ? layoutSettings.listHeight
                                                                  : panel.defaultListHeight
            Layout.minimumHeight: 96
            color: "#181c21"
            border.color: "#2c323b"
            radius: 4
            BoundedListView {
                id: list
                objectName: "subtitleList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: panel.mvmController.subtitleModel
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    id: row
                    required property string cueId
                    required property var startFrame
                    required property var endFrame
                    required property string content
                    readonly property bool selected: panel.mvmController.selectedSubtitleId === row.cueId
                    readonly property bool active: panel.mvmController.playheadFrame >= row.startFrame
                                                   && panel.mvmController.playheadFrame < row.endFrame
                    width: list.width
                    height: 30
                    leftPadding: 10
                    rightPadding: 8
                    contentItem: RowLayout {
                        spacing: 10
                        Label {
                            // 秒の表示は桁が揃うよう等幅にし、本文より控えめな色にする。
                            Layout.preferredWidth: 92
                            text: panel.timeLabel(row.startFrame, row.endFrame)
                            color: row.selected ? "#d6e6f7" : "#9aa2ad"
                            font.family: "Consolas"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: row.content.replace(/\n/g, " / ")
                            color: "#eeeeee"
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                    background: Rectangle {
                        color: row.selected ? "#30455c" : (row.hovered ? "#262c34" : "transparent")
                        // 再生位置の字幕は左端の帯で示す。選択の色とは独立に見える。
                        Rectangle {
                            width: 3
                            height: parent.height
                            color: "#5fb37a"
                            visible: row.active
                        }
                    }
                    onClicked: {
                        panel.mvmController.selectSubtitle(row.cueId);
                        panel.reloadEditor();
                    }
                }
            }
        }
        // 一覧の高さをドラッグで変える。Flickable にドラッグを渡さない。
        Item {
            objectName: "subtitleListResizeHandle"
            Layout.fillWidth: true
            Layout.topMargin: -6
            implicitHeight: 10
            Rectangle {
                anchors.centerIn: parent
                width: 36
                height: 3
                radius: 1.5
                color: resizeArea.containsMouse || resizeArea.pressed ? "#5b9bd5" : "#4a515c"
            }
            MouseArea {
                id: resizeArea
                anchors.fill: parent
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeVerCursor
                property real pressY: 0
                property real pressHeight: 0
                onPressed: mouse => {
                    pressY = mapToItem(panel, mouse.x, mouse.y).y;
                    pressHeight = listFrame.height;
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return;
                    const y = mapToItem(panel, mouse.x, mouse.y).y;
                    layoutSettings.listHeight = Math.max(96, Math.min(1600, pressHeight + y - pressY));
                }
                onDoubleClicked: layoutSettings.listHeight = 0
            }
            ToolTip.visible: resizeArea.containsMouse && !resizeArea.pressed
            ToolTip.delay: 600
            ToolTip.text: "ドラッグで一覧の高さを変更 (ダブルクリックで既定に戻す)"
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "開始／終了フレーム"
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: startField.acceptableInput && endField.acceptableInput
                      ? panel.timeLabel(Number(startField.text), Number(endField.text)) + " 秒" : ""
                color: "#9aa2ad"
                font.family: "Consolas"
                font.pixelSize: 12
            }
        }
        RowLayout {
            Layout.fillWidth: true
            ModernDialogField {
                id: startField
                Layout.fillWidth: true
                enabled: panel.editable
                validator: RegularExpressionValidator {
                    regularExpression: /[0-9]+/
                }
            }
            ModernDialogField {
                id: endField
                Layout.fillWidth: true
                enabled: panel.editable
                validator: RegularExpressionValidator {
                    regularExpression: /[0-9]+/
                }
            }
        }
        ModernDialogTextArea {
            id: body
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            enabled: panel.editable
            wrapMode: TextEdit.Wrap
            placeholderText: "字幕本文"
            onCursorPositionChanged: {
                if (body.activeFocus)
                    panel.bodyCursor = body.cursorPosition;
            }
        }
        Keys.onEscapePressed: panel.reloadEditor()
        Flow {
            objectName: "subtitleEditButtons"
            Layout.fillWidth: true
            spacing: 6
            ModernDialogButton {
                text: "更新"
                prominent: true
                enabled: panel.editable && panel.mvmController.selectedSubtitleId !== ""
                onClicked: {
                    if (panel.mvmController.updateSubtitle(panel.mvmController.selectedSubtitleId, body.text, Number(startField.text), Number(endField.text)))
                        panel.reloadEditor();
                }
            }
            ModernDialogButton {
                text: "追加"
                enabled: panel.editable
                onClicked: {
                    if (panel.mvmController.addSubtitle(body.text, Number(startField.text), Number(endField.text)))
                        panel.reloadEditor();
                }
            }
            ModernDialogButton {
                text: "削除"
                destructive: true
                enabled: panel.editable
                onClicked: panel.mvmController.deleteSelectedSubtitle()
            }
            ModernDialogButton {
                text: "再生位置で分割"
                enabled: panel.editable
                // 本文欄を編集していなければ、置いたカーソルの位置で本文も分ける。
                onClicked: panel.mvmController.splitSelectedSubtitle(
                               body.text === (panel.mvmController.selectedSubtitle.content || "")
                               ? panel.bodyCursor : -1)
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: "本文は、本文欄に置いたカーソルの位置で分けます。置いていなければ再生位置に近い句読点で分けます"
            }
            ModernDialogButton {
                text: "次と結合"
                enabled: panel.editable
                onClicked: panel.mvmController.mergeSelectedSubtitle()
            }
        }

    }
    TranscribeDialog {
        id: transcribeDialog
        mvmController: panel.mvmController
    }
    FileDialog {
        id: importDialog
        title: "SRTを読み込む"
        nameFilters: ["字幕 (*.srt)"]
        onAccepted: panel.mvmController.importSubtitles(selectedFile, importMode.currentIndex === 0)
    }
    FileDialog {
        id: exportDialog
        title: "SRTを書き出す"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "srt"
        nameFilters: ["字幕 (*.srt)"]
        onAccepted: panel.mvmController.exportSubtitles(selectedFile)
    }
}
