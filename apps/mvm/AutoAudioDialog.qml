pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ModernDialog {
    id: dialog
    objectName: "autoAudioDialog"
    required property var mvmController
    property var invalidFields: []
    property var options: ({voiceTracks: [], bgmTrack: 0, normalize: true, duck: true,
        voiceLufs: -16, bgmLufs: -24, reductionDb: 12, thresholdDb: -35,
        attackMs: 150, holdMs: 300, releaseMs: 600})
    function change(key, value) {
        mvmController.cancelAudioAdjustment();
        const next = Object.assign({}, options);
        next[key] = value;
        options = next;
    }
    onOpened: {
        const saved = mvmController.savedAudioAdjustmentSettings;
        options = Object.assign({}, options, saved.bgmTrack !== undefined ? saved : {});
        invalidFields = [];
    }
    onClosed: mvmController.cancelAudioAdjustment()
    Connections {
        target: dialog.mvmController
        function onAudioAdjustmentApplied() { dialog.close(); }
    }
    title: "BGM の自動音量調整"
    parent: Overlay.overlay
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    width: Math.max(1, Math.min(680, parent.width - 24))
    height: Math.max(1, Math.min(760, parent.height - 24))
    palette.windowText: "#c8cbd1"
    footer: Item {
        implicitHeight: footerButtons.implicitHeight + 24
        Rectangle { anchors.fill: parent; color: "#25272b"; radius: 8 }
        Flow {
            id: footerButtons
            anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 12; anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            ModernDialogButton {
                text: "キャンセル"
                onClicked: dialog.reject()
            }
            ModernDialogButton {
                text: dialog.mvmController.audioAdjustmentAuditioning ? "試聴を停止" : "音声を試聴"
                enabled: dialog.mvmController.canApplyAudioAdjustment || dialog.mvmController.audioAdjustmentAuditioning
                onClicked: {
                    if (dialog.mvmController.audioAdjustmentAuditioning) dialog.mvmController.stopAudioAdjustmentAudition();
                    else dialog.mvmController.auditionAudioAdjustment();
                }
            }
            ModernDialogButton {
                text: dialog.mvmController.audioAdjustmentApplying ? "素材を確認中…" : "適用"
                prominent: true
                enabled: dialog.mvmController.canApplyAudioAdjustment
                onClicked: { if (dialog.mvmController.applyAudioAdjustment()) dialog.close(); }
            }
        }
    }
    contentItem: BoundedScrollView {
        id: scroll
        objectName: "autoAudioScroll"
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 10
            Label {
                Layout.fillWidth: true
                text: "声の音量を検出して BGM を下げます。声トラックの効果音や雑音にも反応します。試聴は現在の再生位置から音声だけを再生します。"
                color: "#c8cbd1"; wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: dialog.mvmController.audioAdjustmentNeedsRegeneration
                text: "音声の編集後です。自動調整を再生成してください。"
                color: "#e3bc7c"; wrapMode: Text.Wrap
            }
            Label { text: "声トラック（複数選択可）"; color: "#e2e5eb" }
            BoundedListView {
                id: voiceList
                objectName: "autoAudioVoiceTracks"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(130, Math.max(40, count * 36))
                clip: true
                model: dialog.mvmController.audioTrackModel
                delegate: ModernDialogCheckBox {
                    required property int trackIndex
                    required property string trackName
                    width: voiceList.width
                    text: trackName
                    enabled: !dialog.mvmController.audioAdjusting
                    checked: dialog.options.voiceTracks.indexOf(trackIndex) >= 0
                    onClicked: {
                        let tracks = dialog.options.voiceTracks.slice();
                        if (checked) tracks.push(trackIndex); else tracks = tracks.filter(value => value !== trackIndex);
                        dialog.change("voiceTracks", tracks);
                    }
                }
            }
            Label { text: "BGM トラック"; color: "#e2e5eb" }
            ModernDialogComboBox {
                objectName: "autoAudioBgmTrack"
                Layout.fillWidth: true
                model: dialog.mvmController.audioTrackModel
                textRole: "trackName"
                currentIndex: dialog.options.bgmTrack
                enabled: !dialog.mvmController.audioAdjusting
                onActivated: dialog.change("bgmTrack", currentIndex)
            }
            Flow {
                Layout.fillWidth: true
                spacing: 12
                ModernDialogCheckBox {
                    text: "ラウドネス正規化"
                    checked: dialog.options.normalize
                    enabled: !dialog.mvmController.audioAdjusting
                    onClicked: dialog.change("normalize", checked)
                }
                ModernDialogCheckBox {
                    text: "自動ダッキング"
                    checked: dialog.options.duck
                    enabled: !dialog.mvmController.audioAdjusting
                    onClicked: dialog.change("duck", checked)
                }
            }
            Repeater {
                model: [
                    {key: "voiceLufs", label: "声の目標 (LUFS)", min: -40, max: -5},
                    {key: "bgmLufs", label: "BGM の目標 (LUFS)", min: -40, max: -5},
                    {key: "reductionDb", label: "下げる量 (dB)", min: 0, max: 60},
                    {key: "thresholdDb", label: "検出しきい値 (dBFS)", min: -96, max: 0},
                    {key: "attackMs", label: "下げる時間 (ms)", min: 1, max: 10000},
                    {key: "holdMs", label: "保持する時間 (ms)", min: 0, max: 10000},
                    {key: "releaseMs", label: "戻す時間 (ms)", min: 1, max: 10000}]
                delegate: ColumnLayout {
                    id: fieldRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 3
                    Label { text: fieldRow.modelData.label; color: "#c8cbd1" }
                    ModernDialogField {
                        objectName: "autoAudioField_" + fieldRow.modelData.key
                        Layout.fillWidth: true
                        text: String(dialog.options[fieldRow.modelData.key])
                        enabled: !dialog.mvmController.audioAdjusting
                        validator: DoubleValidator { bottom: fieldRow.modelData.min; top: fieldRow.modelData.max; decimals: fieldRow.modelData.key.endsWith("Ms") ? 0 : 1; locale: "C" }
                        onTextEdited: {
                            dialog.invalidFields = dialog.invalidFields.filter(key => key !== fieldRow.modelData.key);
                            if (acceptableInput) dialog.change(fieldRow.modelData.key, Number(text));
                            else {
                                dialog.mvmController.cancelAudioAdjustment();
                                dialog.invalidFields = dialog.invalidFields.concat([fieldRow.modelData.key]);
                            }
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                color: "#aab2bd"
                text: "正規化はクリップごとに測定します。−1 dBTP を超える増幅は制限します。複数トラックを合成した最終ピークは別途確認してください。"
            }
            Flow {
                Layout.fillWidth: true; spacing: 8
                ModernDialogButton {
                    objectName: "autoAudioAnalyze"
                    text: dialog.mvmController.audioAdjusting ? "解析を中止" : "解析・カーブ生成"
                    enabled: dialog.mvmController.audioAdjusting || dialog.invalidFields.length === 0
                    onClicked: {
                        if (dialog.mvmController.audioAdjusting) dialog.mvmController.cancelAudioAdjustment();
                        else dialog.mvmController.startAudioAdjustment(dialog.options);
                    }
                }
                Label { text: dialog.mvmController.audioAdjusting ? dialog.mvmController.audioAdjustmentProgress + " %" : ""; color: "#c8cbd1" }
            }
            ModernDialogProgressBar {
                Layout.fillWidth: true
                visible: dialog.mvmController.audioAdjusting
                value: dialog.mvmController.audioAdjustmentProgress / 100
            }
            Label {
                objectName: "autoAudioError"
                Layout.fillWidth: true; wrapMode: Text.Wrap
                visible: text.length > 0; color: "#ffb5ac"
                text: dialog.mvmController.audioAdjustmentError
            }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#c8cbd1"
                text: "解析結果（測定不能な無音・短い音声は正規化を適用しません）"
                visible: dialog.mvmController.audioAdjustmentResults.length > 0
            }
            BoundedListView {
                id: results
                objectName: "autoAudioResults"
                Layout.fillWidth: true
                Layout.preferredHeight: 240
                visible: count > 0
                clip: true
                spacing: 6
                model: dialog.mvmController.audioAdjustmentResults
                delegate: Rectangle {
                    id: resultRow
                    required property var modelData
                    width: results.width
                    height: resultContent.implicitHeight + 16
                    radius: 5; color: "#20242b"; border.color: "#414750"
                    ColumnLayout {
                        id: resultContent
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.top: parent.top; anchors.margins: 8
                        Label { Layout.fillWidth: true; text: resultRow.modelData.name; wrapMode: Text.Wrap; color: "#e2e5eb" }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#b8c3d1"
                            text: resultRow.modelData.measurable ? resultRow.modelData.lufs.toFixed(1) + " LUFS / ピーク " + resultRow.modelData.truePeakDb.toFixed(1) + " dBTP / 補正 " + resultRow.modelData.correctionDb.toFixed(1) + " dB"
                                + (resultRow.modelData.peakLimited ? "（ピーク制限で目標未達）" : "")
                                + (resultRow.modelData.gainLimited ? "（補正量の上限に到達）" : "") : "測定不能：正規化は未適用"
                        }
                        Canvas {
                            Layout.fillWidth: true; Layout.preferredHeight: 55
                            visible: resultRow.modelData.keys.length > 0
                            onPaint: {
                                const ctx = getContext("2d");
                                ctx.clearRect(0, 0, width, height);
                                const keys = resultRow.modelData.keys;
                                if (!keys.length) return;
                                ctx.strokeStyle = "#72b4f4"; ctx.lineWidth = 1.5; ctx.beginPath();
                                const last = Math.max(1, keys[keys.length - 1].frame);
                                for (let i = 0; i < keys.length; ++i) {
                                    const x = keys[i].frame / last * width;
                                    const y = 4 + Math.min(1, -keys[i].db / Math.max(1, dialog.options.reductionDb)) * (height - 8);
                                    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
                                }
                                ctx.stroke();
                            }
                            onWidthChanged: requestPaint()
                        }
                        Label {
                            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#aab2bd"
                            visible: resultRow.modelData.keys.length > 0
                            text: resultRow.modelData.keys.length + " キー / " + (resultRow.modelData.keys.length ? resultRow.modelData.keys[resultRow.modelData.keys.length - 1].seconds.toFixed(2) : "0") + " 秒（クリップ内）"
                        }
                    }
                }
            }
            Label { text: "声の検出区間"; color: "#c8cbd1"; visible: ranges.count > 0 }
            BoundedListView {
                id: ranges
                objectName: "autoAudioDetectedRanges"
                Layout.fillWidth: true; Layout.preferredHeight: 100
                visible: count > 0; clip: true
                model: dialog.mvmController.audioAdjustmentRanges
                delegate: Label {
                    required property var modelData
                    width: ranges.width; height: 28; color: "#b8c3d1"
                    text: modelData.startSeconds.toFixed(2) + "〜" + modelData.endSeconds.toFixed(2) + " 秒"
                }
            }
        }
    }
}
