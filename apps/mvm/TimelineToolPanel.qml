pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// タイムライン左端に縦 1 列で並べるツールパネル。
// ツールの一覧・ショートカット・有効/無効はここの tools だけが決める。
// Main.qml のショートカットと操作ヒントもこの配列から引く。
Rectangle {
    id: root

    property string currentTool: "select"
    // false のツールはボタンを出すが選べない。理由は unavailableReason に書く。
    readonly property var tools: [
        { tool: "select", group: "select", key: "V", name: "選択ツール",
          hint: "クリック: 選択 / ドラッグ: 移動 / 端のドラッグ: トリミング / Shift+クリック: 複数選択",
          available: true },
        { tool: "trackForward", group: "select", key: "A", name: "前方トラック選択ツール",
          hint: "クリック位置以降のclipを全trackで選択 / Shift+クリック: そのtrackだけ / そのままドラッグで移動",
          available: true },
        { tool: "trackBackward", group: "select", key: "Shift+A", name: "後方トラック選択ツール",
          hint: "クリック位置以前のclipを全trackで選択 / Shift+クリック: そのtrackだけ / そのままドラッグで移動",
          available: true },
        { tool: "razor", group: "cut", key: "C", name: "レーザーツール",
          hint: "クリック: その位置でclipを分割 / Shift+クリック: 全trackを分割",
          available: true },
        { tool: "ripple", group: "cut", key: "B", name: "リップルツール",
          hint: "clipの端をドラッグ: 尺を変え、後ろのclipを詰める・押し出す",
          available: true },
        { tool: "rolling", group: "cut", key: "N", name: "ローリングツール",
          hint: "接している2つのclipの境界をドラッグ: 合計尺を変えずにつなぎ目を動かす",
          available: true },
        { tool: "rate", group: "cut", key: "R", name: "レート調整ツール",
          hint: "clipの端をドラッグ: 素材の範囲を変えずに速度を変えて長さを伸縮する (10%〜1000%)",
          available: true },
        { tool: "slip", group: "view", key: "Y", name: "スリップツール",
          hint: "clipを左右にドラッグ: 位置と長さを保ったまま素材のイン/アウトをずらす",
          available: true },
        { tool: "slide", group: "view", key: "U", name: "スライドツール",
          hint: "前後のclipに接しているclipを左右にドラッグ: 長さを保って動かし、前後のclipの長さを追従させる",
          available: true },
        { tool: "pen", group: "view", key: "P", name: "ペンツール",
          hint: "黄色い線の近くをクリック: キーを追加 / 青いキーをドラッグ: 時刻と値を調整 / 線から離れた所をクリック: clipを選択",
          available: true },
        { tool: "hand", group: "view", key: "H", name: "ハンドツール",
          hint: "ドラッグ: タイムラインの表示をスクロール (素材は動かない)",
          available: true },
        { tool: "zoom", group: "view", key: "Z", name: "ズームツール",
          hint: "クリック: 拡大 / Alt+クリック: 縮小",
          available: true },
        { tool: "text", group: "text", key: "T", name: "横書き文字ツール",
          hint: "プログラムモニターをクリックして入力 / Ctrl+Enter: 確定 / Esc: 取消",
          available: true }
    ]

    signal toolRequested(string tool)

    function toolInfo(tool) {
        for (let index = 0; index < tools.length; ++index) {
            if (tools[index].tool === tool)
                return tools[index];
        }
        return null;
    }

    function requestTool(tool) {
        const info = toolInfo(tool);
        if (info && info.available)
            toolRequested(tool);
    }

    color: "#191c21"
    border.color: "#3c424c"
    implicitWidth: 34

    BoundedFlickable {
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        contentHeight: toolColumn.height
        clip: true


        Column {
            id: toolColumn
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 2

            Repeater {
                model: root.tools

                Column {
                    id: toolEntry
                    required property var modelData
                    required property int index
                    // 前のツールと種類が変わるところに区切り線を入れる。
                    readonly property bool startsGroup:
                        toolEntry.index > 0
                        && root.tools[toolEntry.index - 1].group !== toolEntry.modelData.group
                    spacing: 2

                    Rectangle {
                        visible: toolEntry.startsGroup
                        width: 22
                        height: 1
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: "#3c424c"
                    }

                    AbstractButton {
                        id: toolButton
                        readonly property bool active: root.currentTool === toolEntry.modelData.tool
                        width: 28
                        height: 26
                        enabled: toolEntry.modelData.available
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.delay: 400
                        ToolTip.text: toolEntry.modelData.name + " (" + toolEntry.modelData.key + ")"
                                      + (toolEntry.modelData.available
                                         ? "\n" + toolEntry.modelData.hint
                                         : "\n" + toolEntry.modelData.unavailableReason)
                        onClicked: root.requestTool(toolEntry.modelData.tool)

                        background: Rectangle {
                            radius: 4
                            color: toolButton.active ? "#315f86"
                                                     : (toolButton.hovered && toolButton.enabled
                                                        ? "#2f353e" : "transparent")
                            border.color: toolButton.active ? "#65a8dc" : "transparent"
                        }
                        contentItem: Item {
                            TimelineToolIcon {
                                anchors.centerIn: parent
                                tool: toolEntry.modelData.tool
                                color: !toolButton.enabled ? "#4c525b"
                                                           : (toolButton.active ? "white" : "#c9ccd2")
                            }
                        }
                    }
                }
            }
        }
    }
}
