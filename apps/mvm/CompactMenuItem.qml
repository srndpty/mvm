import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

MenuItem {
    id: item
    readonly property string shortcutLabel: {
        const parts = text.split("\t");
        if (parts.length > 1)
            return parts[1];
        return action && action.shortcut ? String(action.shortcut) : "";
    }
    // Windows のアクセスキー。menu が開いている間、英字 1 文字 (Alt を押したままでもよい) で
    // この項目を選ぶ。同じ menu の中で重ねない。キーの処理は CompactMenu 内で行う。
    property string mnemonic: ""
    readonly property string label: {
        const base = text.split("\t")[0];
        if (mnemonic.length === 0)
            return base;
        // 「…」で終わる項目は Windows の慣習どおり、その前に (X) を置く。
        const tail = base.match(/(…|\.\.\.)$/);
        return tail ? base.slice(0, -tail[0].length) + "(" + mnemonic + ")" + tail[0]
                    : base + "(" + mnemonic + ")";
    }

    implicitWidth: Math.max(206, contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: 27
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: 12

    contentItem: RowLayout {
        spacing: 20
        Label {
            Layout.fillWidth: true
            text: item.label
            color: item.enabled ? "#f0f1f3" : "#85888f"
            font: item.font
            elide: Text.ElideRight
        }
        Label {
            visible: item.shortcutLabel.length > 0
            text: item.shortcutLabel
            color: item.enabled ? "#b8bbc2" : "#74777e"
            font: item.font
        }
    }

    background: Rectangle {
        color: item.highlighted && item.enabled ? "#414750" : "transparent"
        radius: 4
    }
}
