import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "SubtitleTime.js" as SubtitleTime

ModernDialog {
    id: dialog
    objectName: "subtitleTranscribeDialog"
    required property var mvmController
    property url modelUrl: transcribeSettings.modelUrl
    property string requestedClipId: ""
    // 前回使ったモデル・実行経路・言語を次回の既定にする (この端末の設定として保存)。
    // 大きいモデルは数 GB あり、毎回選び直させない。
    Settings {
        id: transcribeSettings
        category: "transcribe"
        property url modelUrl
        property int backendIndex: 0
        property string language: "ja"
    }
    function openForClip(clipId) {
        requestedClipId = clipId;
        open();
    }
    onOpened: {
        if (requestedClipId === "")
            return;
        const sources = mvmController.transcriptionSources;
        for (let index = 0; index < sources.length; ++index) {
            if (sources[index].timelineClip && sources[index].id === requestedClipId) {
                source.currentIndex = index;
                return;
            }
        }
    }
    title: "ローカル文字起こし"
    parent: Overlay.overlay
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    width: Math.min(650, parent.width - 24)
    height: Math.min(640, parent.height - 24)
    palette.windowText: "#c8cbd1"
    footer: ModernDialogFooter {
        ModernDialogComboBox {
            id: mode
            objectName: "transcriptionApplyMode"
            width: 120
            enabled: dialog.mvmController.canApplyTranscription
            model: ["字幕を置換", "字幕へ追加"]
        }
        ModernDialogButton {
            implicitWidth: 60
            text: "閉じる"
            onClicked: dialog.reject()
        }
        ModernDialogButton {
            implicitWidth: 82
            text: "候補を適用"
            prominent: true
            enabled: dialog.mvmController.canApplyTranscription
            onClicked: {
                if (dialog.mvmController.applyTranscription(mode.currentIndex === 0))
                    dialog.close();
            }
        }
    }
    onRejected: {
        if (mvmController.transcribing)
            mvmController.cancelTranscription();
    }
    contentItem: BoundedScrollView {
        id: scroll
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            // 候補一覧が余った高さを使う。収まらない低いダイアログでは全体をスクロールする。
            width: scroll.availableWidth
            height: Math.max(scroll.availableHeight, implicitHeight)
            spacing: 8
            RowLayout {
                Label {
                    text: "対象"
                }
                ModernDialogComboBox {
                    id: source
                    objectName: "transcriptionSource"
                    Layout.fillWidth: true
                    model: dialog.mvmController.transcriptionSources
                    textRole: "label"
                    enabled: !dialog.mvmController.transcribing
                }
            }
            RowLayout {
                ModernDialogButton {
                    text: "モデルを選択"
                    enabled: !dialog.mvmController.transcribing
                    onClicked: modelDialog.open()
                }
                Label {
                    Layout.fillWidth: true
                    text: dialog.modelUrl.toString() || "多言語モデルを指定してください (精度重視なら ggml-large-v3.bin と Vulkan)"
                    elide: Text.ElideMiddle
                }
            }
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                Label {
                    text: "実行経路"
                }
                ModernDialogComboBox {
                    id: backend
                    Layout.fillWidth: true
                    model: ["CPU", "Vulkan"]
                    currentIndex: transcribeSettings.backendIndex
                    enabled: !dialog.mvmController.transcribing
                }
                Label {
                    text: "言語コード"
                }
                ModernDialogField {
                    id: language
                    text: transcribeSettings.language
                    placeholderText: "ja / auto / en"
                    Layout.fillWidth: true
                    enabled: !dialog.mvmController.transcribing
                }
                Label {
                    text: "語彙ヒント"
                }
                ModernDialogField {
                    id: initialPrompt
                    objectName: "transcriptionInitialPrompt"
                    Layout.fillWidth: true
                    maximumLength: 200
                    placeholderText: "固有名詞・専門用語など (任意、200文字まで)"
                    enabled: !dialog.mvmController.transcribing
                }
                Label {
                    text: "素材の配置フレーム"
                }
                ModernDialogField {
                    id: insertion
                    Layout.fillWidth: true
                    text: String(dialog.mvmController.playheadFrame)
                    validator: RegularExpressionValidator {
                        regularExpression: /[0-9]+/
                    }
                    enabled: !dialog.mvmController.transcribing
                }
            }
            Label {
                text: "クリップはトリム・速度・配置を反映し、作った字幕はクリップの移動に追従します。素材は全音声を処理します。"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            RowLayout {
                ModernDialogButton {
                    prominent: true
                    text: dialog.mvmController.transcribing ? "認識中…" : "認識開始"
                    enabled: !dialog.mvmController.transcribing && source.currentIndex >= 0 && dialog.modelUrl.toString() !== ""
                    onClicked: {
                        const item = dialog.mvmController.transcriptionSources[source.currentIndex];
                        const languageCode = ({
                                "日本語": "ja",
                                "自動検出": "auto",
                                "英語": "en"
                            })[language.text] || language.text;
                        transcribeSettings.modelUrl = dialog.modelUrl;
                        transcribeSettings.backendIndex = backend.currentIndex;
                        transcribeSettings.language = language.text;
                        dialog.mvmController.startTranscription(item.id, item.timelineClip, dialog.modelUrl, backend.currentIndex === 0 ? "cpu" : "vulkan", languageCode, Number(insertion.text), initialPrompt.text);
                    }
                }
                ModernDialogButton {
                    text: "キャンセル"
                    enabled: dialog.mvmController.transcribing
                    onClicked: dialog.mvmController.cancelTranscription()
                }
                ModernDialogProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    value: dialog.mvmController.transcriptionProgress
                }
            }
            Label {
                text: dialog.mvmController.transcriptionError
                visible: text.length > 0
                color: "#e89a96"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: "候補の開始・終了フレームと本文を修正し、適用してください。"
                color: "#9aa2ad"
            }
            BoundedListView {
                id: candidates
                objectName: "transcriptionCandidates"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 180
                Layout.preferredHeight: 180
                clip: true
                model: dialog.mvmController.transcriptionModel
                ScrollBar.vertical: ScrollBar {}
                delegate: ColumnLayout {
                    required property string cueId
                    required property var startFrame
                    required property var endFrame
                    required property string content
                    width: candidates.width
                    RowLayout {
                        Layout.fillWidth: true
                        ModernDialogField {
                            id: start
                            Layout.preferredWidth: 78
                            text: String(startFrame)
                            enabled: dialog.mvmController.canApplyTranscription
                            validator: RegularExpressionValidator {
                                regularExpression: /[0-9]+/
                            }
                        }
                        ModernDialogField {
                            id: end
                            Layout.preferredWidth: 78
                            text: String(endFrame)
                            enabled: dialog.mvmController.canApplyTranscription
                            validator: RegularExpressionValidator {
                                regularExpression: /[0-9]+/
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: start.acceptableInput && end.acceptableInput
                                  ? SubtitleTime.rangeLabel(Number(start.text), Number(end.text), dialog.mvmController.timelineFpsNum, dialog.mvmController.timelineFpsDen) + " 秒" : ""
                            color: "#9aa2ad"
                            font.family: "Consolas"
                            font.pixelSize: 12
                        }
                    }
                    ModernDialogField {
                        id: body
                        Layout.fillWidth: true
                        text: content
                        enabled: dialog.mvmController.canApplyTranscription
                    }
                    ModernDialogButton {
                        text: "更新"
                        enabled: dialog.mvmController.canApplyTranscription
                        onClicked: dialog.mvmController.updateTranscriptionCue(cueId, body.text, Number(start.text), Number(end.text))
                    }
                }
            }
        }
    }
    FileDialog {
        id: modelDialog
        title: "Whisperのローカルモデルを選択"
        nameFilters: ["認識モデル (*.bin)"]
        onAccepted: dialog.modelUrl = selectedFile
    }
}
