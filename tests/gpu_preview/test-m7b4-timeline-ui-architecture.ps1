$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$controllerHeaderPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.h'
$mainPath = Join-Path $PSScriptRoot '..\..\apps\mvm\main.cpp'
$previewItemPath = Join-Path $PSScriptRoot '..\..\src\app\preview\preview_engine_rhi_item.cpp'
$compositorPath = Join-Path $PSScriptRoot '..\..\src\media\gpu_preview\gpu_compositor.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$controller = Get-Content -LiteralPath $controllerPath -Raw
$controllerHeader = Get-Content -LiteralPath $controllerHeaderPath -Raw
$main = Get-Content -LiteralPath $mainPath -Raw
$previewItem = Get-Content -LiteralPath $previewItemPath -Raw
$compositor = Get-Content -LiteralPath $compositorPath -Raw

# timeline UI は clip の配置を track/start から引く。vector 順を authority にしない。
$requiredQml = @(
    'x: timelineStartFrame * timelinePanel.pixelsPerFrame',
    'y: timelinePanel.rowY(trackKind, trackIndex)',
    'function trackAtY(y)',
    'mvmController.selectTimelineClip(clipItem.clipId, frame)',
    'mvmController.moveTimelineClip(',
    'releasedClipId, destinationKind, destinationIndex',
    'mvmController.selectTimelineClips(selectedIds)',
    'required property bool selected'
)
# track 数は固定しない。model から引き、行位置は rowY() だけが決める。
$forbiddenQml = @(
    'timelineList.indexAt',
    'mvmController.reorderClip',
    'model: ["V2", "V1"]',
    'videoTrack === 1'
)
# premiere 相当の操作。どれか 1 つでも消えたら UI 契約が崩れている。
$requiredInteractions = @(
    'mvmController.beginScrub()',
    'mvmController.scrubToFrame(',
    'mvmController.endScrub()',
    'height: timelineFlick.contentHeight',
    'y: timelineFlick.contentY',
    'id: ruler',
    'z: 200',
    'function handleNativeAltWheel(',
    'function handleNativeCtrlWheel(',
    'function handleNativeShiftWheel(',
    'function handleNativePlainWheel(',
    'setZoom(wheelDelta > 0 ? 1 : -1,',
    'readonly property var zoomLevels:',
    'property int zoomIndex:',
    'readonly property real fitPixelsPerFrame:',
    'Math.min(zoomLevels[0], requestedFitPixelsPerFrame)',
    'readonly property int minimumZoomIndex:',
    'timelineFlick.width * 0.7 / mvmController.totalTimelineFrames',
    'zoomIndex === minimumZoomIndex ? fitPixelsPerFrame : zoomLevels[zoomIndex]',
    'onObservedTimelineFramesChanged:',
    'zoomIndex = Math.max(minimumZoomIndex,',
    'Math.max(minimumZoomIndex, Math.min(zoomLevels.length - 1,',
    'const anchorFrame = (timelineFlick.contentX + anchorItemX) / pixelsPerFrame;',
    'const desiredContentX = anchorFrame * pixelsPerFrame - anchorItemX;',
    'Math.min(nextMaxContentX, desiredContentX)',
    'timelinePanel.activeDragOffsetX = clipItem.bodyDragOffsetX',
    '-clipItem.timelineStartFrame * timelinePanel.pixelsPerFrame',
    'clipItem.linkGroupId === timelinePanel.activeDragLinkGroup',
    '(mouse.modifiers & Qt.ShiftModifier) !== 0',
    'mvmController.toggleTimelineClipSelection(',
    'timelineFlick.contentY',
    'id: timelineHorizontalScrollBar',
    'policy: ScrollBar.AlwaysOn',
    'ScrollBar.vertical:',
    'id: timelineVerticalScrollBar',
    'active: timelineFlick.contentHeight > timelineFlick.height',
    'interactive: true',
    'const maxY = Math.max(0, timelineFlick.contentHeight - timelineFlick.height);',
    'function trackForDrag(kind, trackAreaY)',
    'preventStealing: true',
    'acceptedButtons: Qt.LeftButton',
    'mvmController.hasGapAt(',
    'mvmController.hasClipAt(',
    'mvmController.rippleDeleteGap(',
    'mvmController.deleteTimelineClip(',
    'mvmController.unlinkTimelineClip(',
    'mvmController.setTrackMuted(',
    'mvmController.addTrack(',
    'mvmController.videoTrackModel',
    'mvmController.audioTrackModel'
)

$requiredShortcuts = @(
    'sequence: "Delete"',
    'sequence: "Space"',
    'autoRepeat: false',
    'sequence: "Ctrl+Z"',
    'enabled: mvmController.canUndo',
    'onActivated: mvmController.undoLastEdit()'
)

$requiredVideoDrop = @(
    'id: videoDropArea',
    'drag.hasUrls',
    'root.isLocalFileUrl(drag.urls[index])',
    'mvmController.addVideoClip(url)',
    'drop.accept(Qt.CopyAction)',
    'drag.accept(Qt.CopyAction)',
    '(drag.supportedActions & Qt.CopyAction) !== 0',
    'videoDropArea.acceptingVideoDrag'
)

# drop は参照登録であり移動ではない。source が提示した action (Move を含む) を
# そのまま受理しない。
$forbiddenVideoDrop = @(
    'acceptProposedAction',
    'isSupportedVideoUrl',
    '/\.(mp4|mov|mkv|ts)$/i'
)

$requiredExportProgress = @(
    'visible: mvmController.exporting',
    'value: mvmController.exportProgress',
    'text: mvmController.exportProgressText',
    'mvmController.cancelTimelineExport()'
)

$requiredExportFailure = @(
    'id: exportFailureDialog',
    'title: "書き出しに失敗しました"',
    'function onExportFailed(message)',
    'exportFailureDialog.message = message',
    'exportFailureDialog.open()'
)

$requiredExportSettings = @(
    'id: exportSettingsDialog',
    'id: qualityCombo',
    'key: "high"',
    'key: "standard"',
    'key: "compact"',
    'mvmController.exportSettingsSummary()',
    'mvmController.exportTimelineWithQuality(',
    '品質は圧縮率だけを変更します。出力の解像度とfpsはプロジェクト設定のままです。'
)

$requiredProjectSettings = @(
    'id: projectSettingsDialog',
    'id: matchClipSettingsDialog',
    'プロジェクト設定をこの素材に合わせる',
    'mvmController.projectSettingsForClip(clipId)',
    'mvmController.setProjectVideoSettings(',
    '既存クリップの開始位置は秒位置を維持して換算します。素材のin/outは変更しません。続行しますか？'
)

foreach ($needle in @('id: rectangleSelectionArea')) {
    if (-not $qml.Contains($needle)) {
        throw "矩形選択の契約がありません: $needle"
    }
}

# audio clip の波形。可視範囲だけに置かないと、長い clip の高倍率表示で巨大な texture を作る。
foreach ($needle in @('import mvm.timeline 1.0', 'WaveformView {', 'cache: waveformCache',
                      'mediaPath: clipItem.clipKind === "audio" ? clipItem.mediaPath : ""',
                      'Math.max(0, timelineFlick.contentX - clipContentX)',
                      'x: clipItem.renderOffsetX')) {
    if (-not $qml.Contains($needle)) {
        throw "audio波形の契約がありません: $needle"
    }
}
foreach ($needle in @('qmlRegisterType<mvm::app::WaveformView>("mvm.timeline", 1, 0, "WaveformView")',
                      'setContextProperty(QStringLiteral("waveformCache"), &waveformCache)')) {
    if (-not $main.Contains($needle)) {
        throw "audio波形の登録がありません: $needle"
    }
}

foreach ($removed in @('spaceMoveToolActive', 'id: moveToolArea', 'sequence: "Ctrl+Space"',
                       'TimelineSpaceMoveState', 'acceptsTextInput(QGuiApplication::focusObject())')) {
    if ($qml.Contains($removed) -or $main.Contains($removed)) {
        throw "削除したSpace move toolの契約が残っています: $removed"
    }
}

foreach ($needle in ($requiredQml + $requiredInteractions + $requiredShortcuts +
                     $requiredVideoDrop + $requiredExportProgress + $requiredExportFailure +
                     $requiredExportSettings + $requiredProjectSettings)) {
    if (-not $qml.Contains($needle)) {
        throw "timeline UI contractがありません: $needle"
    }
}
foreach ($needle in $forbiddenVideoDrop) {
    if ($qml.Contains($needle)) {
        throw "動画dropの旧契約 (拡張子による制限 / proposed action の受理) が残っています: $needle"
    }
}
foreach ($needle in $forbiddenQml) {
    if ($qml.Contains($needle)) {
        throw "track数を固定する旧timeline UIが残っています: $needle"
    }
}

# zoom式はQMLがauthority。ここは同じ式を独立に評価し、短いtimelineと長いtimelineの
# 契約がソース上の式と一致することを確認する。
$zoomMatch = [regex]::Match($qml, 'readonly property var zoomLevels:\s*\[(?<levels>[\s\S]*?)\]')
if (-not $zoomMatch.Success) {
    throw 'zoomLevelsを読み取れません'
}
$zoomLevels = @()
foreach ($token in ($zoomMatch.Groups['levels'].Value -split ',')) {
    $trimmed = $token.Trim()
    if ($trimmed.Length -eq 0) { continue }
    $zoomLevels += [double]::Parse($trimmed, [cultureinfo]::InvariantCulture)
}
if ($zoomLevels.Count -lt 2) {
    throw 'zoom段階が複数ありません'
}
function Get-FitPixelsPerFrame([double]$viewportWidth, [double]$totalFrames) {
    $rawFit = $viewportWidth * 0.7 / $totalFrames
    return [Math]::Min($zoomLevels[0], $rawFit)
}
$shortFit = Get-FitPixelsPerFrame 1000 10
if ([Math]::Abs($shortFit - $zoomLevels[0]) -gt 0.0000001) {
    throw "10 frameの最小zoomが通常presetから外れています: $shortFit"
}
if ($zoomLevels[-1] -le $zoomLevels[0]) {
    throw '10 frameで複数段階のzoomへ進めません'
}
$longFrames = 1000000
$longViewport = 1000
$longFit = Get-FitPixelsPerFrame $longViewport $longFrames
if ($longFit -ge $zoomLevels[0]) {
    throw '非常に長いtimelineでも通常presetより広くzoom outできません'
}
if ($longFit * $longFrames -gt $longViewport * 0.7 + 0.001) {
    throw '最大zoom-outでtimeline全体がviewportの70%に収まりません'
}
if ($qml -notmatch 'const anchorFrame = \(timelineFlick\.contentX \+ anchorItemX\) / pixelsPerFrame;' -or
    $qml -notmatch 'const desiredContentX = anchorFrame \* pixelsPerFrame - anchorItemX;') {
    throw 'zoomのcursor anchorが維持されません'
}

# moveTimelineClipは同期的にmodelを更新してdelegateを破棄し得る。
# 呼び出し後にdelegate contextのtimelinePanelを参照するとReferenceErrorになる。
$bodyAreaIndex = $qml.IndexOf('id: bodyArea')
$bodyReleaseIndex = $qml.IndexOf('onReleased: mouse => {', $bodyAreaIndex)
$bodyMoveIndex = $qml.IndexOf('mvmController.moveTimelineClip(', $bodyReleaseIndex)
$bodyDragResetIndex = $qml.IndexOf('timelinePanel.activeDragLinkGroup = "";', $bodyReleaseIndex)
if ($bodyAreaIndex -lt 0 -or $bodyReleaseIndex -lt 0 -or $bodyMoveIndex -lt 0 -or
    $bodyDragResetIndex -lt 0 -or $bodyDragResetIndex -gt $bodyMoveIndex) {
    throw 'delegateを破棄し得るmoveTimelineClipより前にdrag状態をresetしていません'
}

if (-not $main.Contains('class TimelineWheelEventFilter final') -or
    -not $main.Contains('window->installEventFilter(&timelineWheelFilter)') -or
    -not $main.Contains('testFlag(Qt::AltModifier)') -or
    -not $main.Contains('testFlag(Qt::ControlModifier)') -or
    -not $main.Contains('testFlag(Qt::ShiftModifier)') -or
    -not $main.Contains('method = "handleNativePlainWheel"') -or
    -not $main.Contains('angleDelta.x()') -or
    -not $main.Contains('pixelDelta.x()') -or
    -not $main.Contains('if (delta == 0)') -or
    -not $qml.Contains('if (wheelDelta === 0)')) {
    throw 'modifier付きwheelがQQuickWindowのevent filterで先取りされていません'
}

# native surfaceはQML scene graphへ宣言し、C++からwindow表示後に動的追加しない。
if (-not $qml.Contains('PreviewSurface {') -or
    $main.IndexOf('qmlRegisterType<mvm::app::PreviewEngineRhiItem>') -lt 0 -or
    $main.IndexOf('qmlRegisterType<mvm::app::PreviewEngineRhiItem>') -gt
        $main.IndexOf('engine.load(') -or
    $main.Contains('new mvm::app::PreviewEngineRhiItem')) {
    throw 'product GUIのnative preview surfaceがQML scene graphへ事前登録されていません'
}

if ($controller.Contains('recomputeTimelineStarts(candidate)')) {
    throw 'controller編集経路がrecomputeTimelineStartsに依存しています'
}
if (-not $controller.Contains('QString::number(selectedClipIds_.size())')) {
    throw 'linked clip展開後の実選択数をstatusへ表示していません'
}
foreach ($needle in @('project::timelineClipIndexAt(project_, current.track, clamped)',
                      'setTimelineSelection({clipId.toStdString()});',
                      'UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_, currentRevision_};',
                      'std::vector<std::string> deletedIds = selectedClipIds_;',
                      'for (const auto& id : deletedIds)',
                      'project_ = undo.project;',
                      'scheduleRecoveryAutosave();',
                      'undoHistory_.pop_back();')) {
    if (-not $controller.Contains($needle)) {
        throw "audio/video選択同期またはUndoの契約がありません: $needle"
    }
}
foreach ($needle in @('FILE_FLAG_DELETE_ON_CLOSE',
                      'ERROR_SHARING_VIOLATION',
                      'project::saveProjectRecovery(',
                      'project::classifyRecovery(',
                      'savedCanonicalSha256_',
                      'canonicalBaseMatchesDisk(')) {
    if (-not $controller.Contains($needle)) {
        throw "Project lockまたはrecovery照合の契約がありません: $needle"
    }
}
foreach ($needle in @('currentRevision_ = nextRevision_++;',
                      'savedRevision_ = currentRevision_;')) {
    if (-not $controller.Contains($needle)) {
        throw "未保存変更のcontroller契約がありません: $needle"
    }
}
foreach ($needle in @('Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)',
                      'Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY stateChanged)',
                      'Q_INVOKABLE bool saveProject();',
                      'Q_INVOKABLE bool saveProjectOverwritingExternalChange();',
                      'Q_PROPERTY(bool recoveryForeign READ recoveryForeign NOTIFY stateChanged)',
                      'Q_INVOKABLE bool discardUnsavedChanges();',
                      'Q_INVOKABLE bool restoreRecovery();',
                      'Q_INVOKABLE bool discardRecovery();',
                      'Q_INVOKABLE bool dismissRecovery();',
                      'Q_PROPERTY(bool recoveryCanonicalChanged READ recoveryCanonicalChanged NOTIFY stateChanged)',
                      'Q_PROPERTY(bool recoveryCorrupt READ recoveryCorrupt NOTIFY stateChanged)')) {
    if (-not $controllerHeader.Contains($needle)) {
        throw "未保存変更のcontroller公開契約がありません: $needle"
    }
}
foreach ($needle in @('onClosing: close => {',
                      'if (!closeConfirmed && mvmController.dirty)',
                      'root.requestProjectAction("new")',
                      'root.requestProjectAction("open")',
                      'mvmController.saveProject()',
                      'mvmController.discardUnsavedChanges()',
                      'id: unsavedChangesDialog',
                      'id: recoveryDialog',
                      'id: externalSaveDialog',
                      'mvmController.saveProjectOverwritingExternalChange()',
                      'mvmController.recoveryForeign',
                      'function onExternalCanonicalChangeOnSave()',
                      'mvmController.recoveryCanonicalChanged',
                      'mvmController.dismissRecovery()',
                      'mvmController.recoveryCorrupt')) {
    if (-not $qml.Contains($needle)) {
        throw "未保存変更の終了確認UIがありません: $needle"
    }
}
foreach ($needle in @('exportThreadFactory_([this,',
                      'exportCancelRequested_.store(true, std::memory_order_release)',
                      'exportCancelling_ = true;',
                      'finishTimelineExport(std::move(exported))',
                      'Qt::QueuedConnection')) {
    if (-not $controller.Contains($needle)) {
        throw "非同期書き出しまたはキャンセル伝播の契約がありません: $needle"
    }
}
foreach ($needle in @('previewStatus.state == preview::PreviewEngineState::Error',
                      'else if (previewEngine_->status().state == preview::PreviewEngineState::Error)',
                      'scrubTimer_.stop();')) {
    if (-not $controller.Contains($needle)) {
        throw "Preview error時のscrub停止契約がありません: $needle"
    }
}
# preview の layer 構成は mapTimelinePreviewFrame に一本化する。
if (-not $controller.Contains('mapTimelinePreviewFrame(project_, timelineFrame)')) {
    throw 'controllerがpreview layer mappingを経由していません'
}
if (-not $controller.Contains('project::placeLinkedAvPairAt(')) {
    throw 'linked video/audioを単一transactionで配置していません'
}

if (-not $previewItem.Contains('setMirrorVertically(false)') -or
    $previewItem.Contains('setMirrorVertically(true)')) {
    throw '製品previewの上下方向がD3D11出力と一致していません'
}
if (-not $previewItem.Contains('PreviewRenderPort::renderFrameDue(*engine_)')) {
    throw '新しいoutput frameがない周期にもrender targetを黒でclearしています'
}
if (-not $qml.Contains('mvmController.outputWidth') -or
    -not $qml.Contains('mvmController.outputHeight')) {
    throw 'previewがProject output sizeの縦横比を使っていません'
}
if (-not $compositor.Contains('aspectFit(croppedWidth, croppedHeight, destinationBox.width,')) {
    throw '製品compositorが素材の縦横比を保持していません'
}

Write-Output 'timeline UI architecture: PASS'
