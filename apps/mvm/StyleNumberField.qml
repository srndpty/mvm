import QtQuick

// 書式の数値欄 (文字 clip と字幕の共通書式で共用)。ドラッグ中は pending を表示しつつ
// previewed で確定前の見た目だけを描き直させ、離したとき (commit) に committed で保存させる。
// 値が元に戻っていれば保存せず canceled で preview を取り消させる。
// scale は表示値と保存値の倍率 (字幕の余白は保存値 0.05 を 5 % と表示する)。
DragNumberField {
    id: field
    required property var styleData
    required property string key
    property string namePrefix: "text"
    property real scale: 1
    property var pending: undefined
    signal previewed(string key, var value)
    signal committed(string key, var value)
    signal canceled
    objectName: field.namePrefix + "NumberField_" + field.key
    inlineLabelWidth: 20
    value: field.pending !== undefined ? field.pending
                                       : (Number(field.styleData[field.key]) || 0) * field.scale
    onValueEdited: (newValue, commit) => {
        const rounded = Math.round(newValue);
        const stored = field.scale === 1 ? rounded : rounded / field.scale;
        if (!commit) {
            field.pending = rounded;
            field.previewed(field.key, stored);
            return;
        }
        field.pending = undefined;
        if (field.styleData[field.key] === stored)
            field.canceled();
        else
            field.committed(field.key, stored);
    }
    onEditCanceled: {
        field.pending = undefined;
        field.canceled();
    }
}
