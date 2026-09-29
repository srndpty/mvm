pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// プロジェクトパネル。素材とフォルダをリスト表示で管理する。
// 並び・開閉は mediaBinModel が決め、ここは選択・ドラッグ・入力だけを持つ。
Item {
    id: panel
    required property MvmController mvmController

    readonly property var binModel: panel.mvmController.mediaBinModel
    // 選択は UI の状態であり Project には保存しない。
    property var selectedIds: []
    property string anchorId: ""
    property string renamingId: ""
    property var dragIds: []
    // 外部ファイルのドラッグがこのパネル上にあるか。Main.qml の DropArea が立てる。
    property bool externalDropHover: false
    // 素材のドラッグ表示の文言。timeline など panel の外の drop 先が差し替える。
    property string dragLabelOverride: ""

    readonly property int rowHeight: 22
    readonly property int indentWidth: 14
    readonly property int nameColumnWidth: 200
    readonly property int rateColumnWidth: 88
    readonly property int durationColumnWidth: 100
    readonly property int sizeColumnWidth: 96
    readonly property int columnsWidth: nameColumnWidth + rateColumnWidth
                                        + durationColumnWidth + sizeColumnWidth
    readonly property string dragKey: "mvm-media-bin"

    function isSelected(entryId) {
        return selectedIds.indexOf(entryId) >= 0;
    }

    function selectOnly(entryId) {
        selectedIds = entryId === "" ? [] : [entryId];
        anchorId = entryId;
    }

    function toggleSelection(entryId) {
        const next = selectedIds.slice();
        const position = next.indexOf(entryId);
        if (position >= 0)
            next.splice(position, 1);
        else
            next.push(entryId);
        selectedIds = next;
        anchorId = entryId;
    }

    // 表示中の行の範囲で選択する。折りたたまれた子孫は含めない。
    function selectRangeTo(entryId) {
        const from = binModel.rowOfEntry(anchorId);
        const to = binModel.rowOfEntry(entryId);
        if (from < 0 || to < 0) {
            selectOnly(entryId);
            return;
        }
        const next = [];
        for (let row = Math.min(from, to); row <= Math.max(from, to); ++row)
            next.push(binModel.entryIdAt(row));
        selectedIds = next;
    }

    // 編集で消えた entry と、折りたたみで見えなくなった entry を選択から外す。
    function pruneSelection() {
        selectedIds = selectedIds.filter(entryId => binModel.rowOfEntry(entryId) >= 0);
        if (binModel.rowOfEntry(anchorId) < 0)
            anchorId = "";
        if (renamingId !== "" && binModel.rowOfEntry(renamingId) < 0)
            renamingId = "";
    }

    // 読み込み・新規フォルダの行き先。folder を 1 つ選んでいればその中、
    // 素材を選んでいればその素材と同じ場所、それ以外は root。
    function targetFolder() {
        return selectedIds.length === 1 ? binModel.containingFolderOf(selectedIds[0]) : "";
    }

    function importUrls(urls) {
        panel.mvmController.importMediaFiles(urls, targetFolder());
    }

    function createFolder() {
        const folderId = panel.mvmController.createMediaFolder(targetFolder());
        if (folderId === "")
            return;
        selectOnly(folderId);
        renamingId = folderId;
        const row = binModel.rowOfEntry(folderId);
        if (row >= 0)
            binList.positionViewAtIndex(row, ListView.Contain);
    }

    // 使用中の素材は、それを使う timeline clip ごと消える。消える clip があるときだけ確認する。
    function removeSelected() {
        if (selectedIds.length === 0)
            return;
        const ids = selectedIds.slice();
        const clipCount = panel.mvmController.mediaBinRemovalClipCount(ids);
        if (clipCount < 0)
            return;
        if (clipCount === 0) {
            panel.mvmController.removeMediaBinEntries(ids);
            return;
        }
        removeConfirmDialog.entryIds = ids;
        removeConfirmDialog.clipCount = clipCount;
        removeConfirmDialog.open();
    }

    function startRename() {
        if (selectedIds.length === 1)
            renamingId = selectedIds[0];
    }

    function commitRename(entryId, originalName, text) {
        if (renamingId !== entryId)
            return;
        renamingId = "";
        binList.forceActiveFocus();
        if (text.trim() !== "" && text.trim() !== originalName)
            panel.mvmController.renameMediaBinEntry(entryId, text);
    }

    function openEntry(entryId, entryKind) {
        if (entryKind === "folder")
            binModel.toggleExpanded(entryId);
        else
            panel.mvmController.addMediaItemToTimeline(entryId);
    }

    // すでに行き先にある entry だけなら何もしない。空の移動を undo 履歴へ積まない。
    // 移動は model を作り直して drop 元の delegate を破棄するため、drag の後始末が
    // 終わってから実行する。
    function dropInto(folderId) {
        const moving = dragIds.filter(entryId => entryId !== folderId
                                      && binModel.parentFolderOf(entryId) !== folderId);
        if (moving.length > 0)
            Qt.callLater(() => panel.mvmController.moveMediaBinEntries(moving, folderId));
    }

    function finishDrag(drop) {
        if (drop)
            dragProxy.Drag.drop();
        dragProxy.Drag.active = false;
        dragIds = [];
        dragLabelOverride = "";
    }

    Connections {
        target: panel.binModel
        function onModelReset() { panel.pruneSelection(); }
        function onRowsRemoved() { panel.pruneSelection(); }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: panel.binModel.entryCount + " 項目"
                color: "#9aa2ad"
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            ToolButton {
                text: "読み込み…"
                font.pixelSize: 11
                enabled: !panel.mvmController.busy
                onClicked: importDialog.open()
            }
            ToolButton {
                text: "新規フォルダ"
                font.pixelSize: 11
                enabled: !panel.mvmController.busy
                onClicked: panel.createFolder()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#15181d"
            border.width: panel.externalDropHover ? 2 : 1
            border.color: panel.externalDropHover ? "#64a8e8" : "#2c3139"

            // 行の無い場所へ落とすと root へ移す。行の DropArea より下に置く。
            DropArea {
                anchors.fill: parent
                keys: [panel.dragKey]
                onDropped: panel.dropInto("")
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: panel.binModel.entryCount === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "ファイルをここへドロップするか「読み込み…」で素材を追加"
                color: "#6f7681"
                font.pixelSize: 11
            }

            ListView {
                id: binList
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                focus: true
                model: panel.binModel
                contentWidth: Math.max(width, panel.columnsWidth)
                flickableDirection: Flickable.AutoFlickIfNeeded
                boundsBehavior: Flickable.StopAtBounds
                headerPositioning: ListView.OverlayHeader
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

                // リストにfocusがある間は削除・選択移動をtimelineのshortcutより優先する。
                // Delete は素材を選んでいる時だけ受ける。選んでいなければ window の
                // Shortcut (clip の削除) へ回す。
                Keys.onShortcutOverride: event => {
                    event.accepted = panel.renamingId === ""
                        && ((event.key === Qt.Key_Delete && panel.selectedIds.length > 0)
                            || event.key === Qt.Key_F2
                            || event.key === Qt.Key_Left || event.key === Qt.Key_Right
                            || event.key === Qt.Key_Up || event.key === Qt.Key_Down);
                }
                Keys.onPressed: event => {
                    if (panel.renamingId !== "")
                        return;
                    if (event.key === Qt.Key_Delete) {
                        panel.removeSelected();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_F2) {
                        panel.startRename();
                        event.accepted = true;
                    }
                }

                // 行より下の空白。クリックで選択解除、右クリックでメニュー。
                // ListView は左 press を flick 用に受け取るため、背面ではなく footer に置く。
                footer: MouseArea {
                    width: Math.max(binList.width, panel.columnsWidth)
                    height: Math.max(panel.rowHeight,
                                     binList.height - panel.rowHeight * (binList.count + 1))
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: mouse => {
                        binList.forceActiveFocus();
                        panel.selectOnly("");
                        if (mouse.button === Qt.RightButton) {
                            contextMenu.entryId = "";
                            contextMenu.entryKind = "";
                            contextMenu.popup();
                        }
                    }
                }

                header: Rectangle {
                    z: 3
                    width: Math.max(binList.width, panel.columnsWidth)
                    height: panel.rowHeight
                    color: "#23272e"

                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: [
                                { "title": "名前", "width": panel.nameColumnWidth },
                                { "title": "フレームレート", "width": panel.rateColumnWidth },
                                { "title": "デュレーション", "width": panel.durationColumnWidth },
                                { "title": "解像度", "width": panel.sizeColumnWidth }
                            ]
                            Label {
                                required property var modelData
                                width: modelData.width
                                height: parent.height
                                leftPadding: 8
                                verticalAlignment: Text.AlignVCenter
                                text: modelData.title
                                color: "#9aa2ad"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 1
                        color: "#2c3139"
                    }
                }

                delegate: Rectangle {
                    id: rowItem

                    required property int index
                    required property string entryId
                    required property string entryKind
                    required property string name
                    required property int depth
                    required property string parentId
                    required property bool expanded
                    required property bool hasChildren
                    required property string rateText
                    required property string durationText
                    required property string sizeText
                    required property string mediaPath
                    required property bool inUse

                    readonly property bool isFolder: entryKind === "folder"
                    readonly property bool selected: panel.selectedIds.indexOf(entryId) >= 0

                    width: Math.max(binList.width, panel.columnsWidth)
                    height: panel.rowHeight
                    color: rowDrop.containsDrag ? "#2d4a6b"
                           : selected ? "#34506e"
                           : index % 2 === 1 ? "#1a1d22" : "#15181d"

                    ToolTip.visible: rowMouse.containsMouse && mediaPath !== ""
                                     && !rowMouse.pressed
                    ToolTip.delay: 800
                    ToolTip.text: mediaPath + (inUse ? "\n(タイムラインで使用中)" : "")

                    Row {
                        anchors.fill: parent

                        Item {
                            width: panel.nameColumnWidth
                            height: parent.height
                            clip: true

                            // 開閉の三角。子を持たない folder にも場所だけ確保して揃える。
                            Label {
                                id: disclosure
                                x: 4 + rowItem.depth * panel.indentWidth
                                width: 12
                                anchors.verticalCenter: parent.verticalCenter
                                visible: rowItem.isFolder
                                text: rowItem.expanded ? "▾" : "▸"
                                color: rowItem.hasChildren ? "#c3c8d0" : "#4a505a"
                                font.pixelSize: 11

                                MouseArea {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    onClicked: panel.binModel.toggleExpanded(rowItem.entryId)
                                }
                            }
                            MediaKindIcon {
                                id: kindIcon
                                x: disclosure.x + 14
                                anchors.verticalCenter: parent.verticalCenter
                                kind: rowItem.entryKind
                            }
                            Label {
                                x: kindIcon.x + kindIcon.width + 5
                                width: parent.width - x - 4
                                anchors.verticalCenter: parent.verticalCenter
                                visible: panel.renamingId !== rowItem.entryId
                                text: rowItem.name
                                color: "#e6e8ec"
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            TextField {
                                id: renameField
                                x: kindIcon.x + kindIcon.width + 3
                                width: parent.width - x - 2
                                height: parent.height - 2
                                anchors.verticalCenter: parent.verticalCenter
                                visible: panel.renamingId === rowItem.entryId
                                padding: 2
                                font.pixelSize: 12
                                color: "#e6e8ec"
                                selectionColor: "#3d6ea5"
                                selectedTextColor: "white"
                                selectByMouse: true
                                background: Rectangle {
                                    color: "#0f1114"
                                    border.color: "#64a8e8"
                                    radius: 2
                                }
                                onVisibleChanged: {
                                    if (!visible)
                                        return;
                                    text = rowItem.name;
                                    selectAll();
                                    forceActiveFocus();
                                }
                                onAccepted: panel.commitRename(rowItem.entryId, rowItem.name, text)
                                onEditingFinished: panel.commitRename(rowItem.entryId, rowItem.name, text)
                                Keys.onEscapePressed: {
                                    panel.renamingId = "";
                                    binList.forceActiveFocus();
                                }
                            }
                        }
                        Label {
                            width: panel.rateColumnWidth
                            height: parent.height
                            leftPadding: 8
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.rateText
                            color: "#b7bdc6"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Label {
                            width: panel.durationColumnWidth
                            height: parent.height
                            leftPadding: 8
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.durationText
                            color: "#b7bdc6"
                            font.pixelSize: 11
                            font.family: "Consolas"
                            elide: Text.ElideRight
                        }
                        Label {
                            width: panel.sizeColumnWidth
                            height: parent.height
                            leftPadding: 8
                            verticalAlignment: Text.AlignVCenter
                            text: rowItem.sizeText
                            color: "#b7bdc6"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        z: -1
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        // ListView の flick に drag を奪わせない。
                        preventStealing: true
                        property point pressPoint
                        property bool dragging: false

                        onPressed: mouse => {
                            binList.forceActiveFocus();
                            pressPoint = Qt.point(mouse.x, mouse.y);
                            dragging = false;
                            const additive = (mouse.modifiers & Qt.ControlModifier) !== 0;
                            const range = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                            if (range)
                                panel.selectRangeTo(rowItem.entryId);
                            else if (additive)
                                panel.toggleSelection(rowItem.entryId);
                            else if (!rowItem.selected)
                                panel.selectOnly(rowItem.entryId);
                            if (mouse.button === Qt.RightButton) {
                                contextMenu.entryId = rowItem.entryId;
                                contextMenu.entryKind = rowItem.entryKind;
                                contextMenu.popup();
                            }
                        }
                        onPositionChanged: mouse => {
                            if (!(mouse.buttons & Qt.LeftButton))
                                return;
                            const point = mapToItem(dragProxy.parent, mouse.x, mouse.y);
                            if (!dragging) {
                                if (Math.abs(mouse.x - pressPoint.x) + Math.abs(mouse.y - pressPoint.y) < 6)
                                    return;
                                if (!rowItem.selected)
                                    panel.selectOnly(rowItem.entryId);
                                panel.dragIds = panel.selectedIds.slice();
                                dragging = true;
                                dragProxy.x = point.x + 6;
                                dragProxy.y = point.y + 6;
                                dragProxy.Drag.active = true;
                            }
                            dragProxy.x = point.x + 6;
                            dragProxy.y = point.y + 6;
                        }
                        onReleased: mouse => {
                            if (dragging) {
                                dragging = false;
                                panel.finishDrag(true);
                                return;
                            }
                            // 複数選択中の行を単クリックしたら、その行だけへ絞る。
                            const modified = (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0;
                            if (mouse.button === Qt.LeftButton && !modified && panel.selectedIds.length > 1)
                                panel.selectOnly(rowItem.entryId);
                        }
                        onCanceled: {
                            dragging = false;
                            panel.finishDrag(false);
                        }
                        onDoubleClicked: mouse => {
                            if (mouse.button === Qt.LeftButton)
                                panel.openEntry(rowItem.entryId, rowItem.entryKind);
                        }
                    }

                    // folder の行へ落とすとその中、素材の行へ落とすとその素材と同じ場所へ移す。
                    DropArea {
                        id: rowDrop
                        anchors.fill: parent
                        keys: [panel.dragKey]
                        onEntered: drag => {
                            drag.accepted = panel.dragIds.indexOf(rowItem.entryId) < 0;
                        }
                        onDropped: panel.dropInto(rowItem.isFolder ? rowItem.entryId : rowItem.parentId)
                    }
                }
            }
        }
    }

    // drag 中にカーソルへ付いてくる札。Drag.active の間だけ見える。
    Rectangle {
        id: dragProxy
        // list は clip するため、timeline まで運べるよう window 全体の overlay に描く。
        parent: Overlay.overlay
        z: 100
        visible: Drag.active
        width: dragLabel.implicitWidth + 16
        height: 20
        radius: 3
        color: "#34506e"
        border.color: "#64a8e8"
        Drag.keys: [panel.dragKey]
        Drag.hotSpot.x: -6
        Drag.hotSpot.y: -6

        Label {
            id: dragLabel
            anchors.centerIn: parent
            text: panel.dragLabelOverride !== "" ? panel.dragLabelOverride
                                                 : panel.dragIds.length + " 件を移動"
            color: "white"
            font.pixelSize: 11
        }
    }

    CompactMenu {
        id: contextMenu
        property string entryId: ""
        property string entryKind: ""

        CompactMenuItem {
            text: "タイムラインに追加"
            enabled: contextMenu.entryKind === "video" || contextMenu.entryKind === "audio"
                     || contextMenu.entryKind === "image"
            onTriggered: panel.mvmController.addMediaItemToTimeline(contextMenu.entryId)
        }
        CompactMenuSeparator {}
        CompactMenuItem {
            text: "読み込み…"
            onTriggered: importDialog.open()
        }
        CompactMenuItem {
            text: "新規フォルダ"
            onTriggered: panel.createFolder()
        }
        CompactMenuSeparator {}
        CompactMenuItem {
            text: "名前を変更\tF2"
            enabled: panel.selectedIds.length === 1
            onTriggered: panel.startRename()
        }
        CompactMenuItem {
            text: "削除\tDelete"
            enabled: panel.selectedIds.length > 0
            onTriggered: panel.removeSelected()
        }
    }

    FileDialog {
        id: importDialog
        title: "素材を読み込み"
        fileMode: FileDialog.OpenFiles
        nameFilters: panel.mvmController.mediaFileNameFilters
        onAccepted: panel.importUrls(selectedFiles)
    }
    ModernDialog {
        id: removeConfirmDialog
        property var entryIds: []
        property int clipCount: 0
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent ? parent.width - 40 : 460, 460)
        title: "素材を削除"

        contentItem: Label {
            width: removeConfirmDialog.availableWidth
            text: "選択した素材を削除すると、タイムライン上の "
                  + removeConfirmDialog.clipCount + " 個のクリップも削除されます。"
            wrapMode: Text.Wrap
        }
        footer: ModernDialogFooter {
            ModernDialogButton {
                text: "キャンセル"
                onClicked: removeConfirmDialog.close()
            }
            ModernDialogButton {
                text: "削除"
                prominent: true
                onClicked: {
                    removeConfirmDialog.close();
                    panel.mvmController.removeMediaBinEntries(removeConfirmDialog.entryIds);
                }
            }
        }
    }
}
