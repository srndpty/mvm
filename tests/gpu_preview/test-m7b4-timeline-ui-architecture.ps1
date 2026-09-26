$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$mainPath = Join-Path $PSScriptRoot '..\..\apps\mvm\main.cpp'
$previewItemPath = Join-Path $PSScriptRoot '..\..\src\app\preview\preview_engine_rhi_item.cpp'
$compositorPath = Join-Path $PSScriptRoot '..\..\src\media\gpu_preview\gpu_compositor.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$controller = Get-Content -LiteralPath $controllerPath -Raw
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
    'destination.kind, destination.index',
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
    'const wasFullyVisible = oldMaxContentX <= 0.5;',
    'const desiredContentX = wasFullyVisible',
    'Math.min(nextMaxContentX, desiredContentX)',
    'timelinePanel.activeDragOffsetX = clipItem.bodyDragOffsetX',
    'clipItem.linkGroupId === timelinePanel.activeDragLinkGroup',
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
    'root.isSupportedVideoUrl(drag.urls[index])',
    'mvmController.addVideoClip(url)',
    'drop.acceptProposedAction()',
    'videoDropArea.acceptingVideoDrag'
)

$requiredExportProgress = @(
    'visible: mvmController.exporting',
    'value: mvmController.exportProgress',
    'text: mvmController.exportProgressText',
    'mvmController.cancelTimelineExport()'
)

foreach ($needle in @('id: rectangleSelectionArea')) {
    if (-not $qml.Contains($needle)) {
        throw "矩形選択の契約がありません: $needle"
    }
}

foreach ($removed in @('spaceMoveToolActive', 'id: moveToolArea', 'sequence: "Ctrl+Space"',
                       'TimelineSpaceMoveState', 'acceptsTextInput(QGuiApplication::focusObject())')) {
    if ($qml.Contains($removed) -or $main.Contains($removed)) {
        throw "削除したSpace move toolの契約が残っています: $removed"
    }
}

foreach ($needle in ($requiredQml + $requiredInteractions + $requiredShortcuts +
                     $requiredVideoDrop + $requiredExportProgress)) {
    if (-not $qml.Contains($needle)) {
        throw "timeline UI contractがありません: $needle"
    }
}
foreach ($needle in $forbiddenQml) {
    if ($qml.Contains($needle)) {
        throw "track数を固定する旧timeline UIが残っています: $needle"
    }
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
                      'UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_};',
                      'project::saveProjectJsonTransaction(project_, undo.project, projectPath_)',
                      'undoHistory_.pop_back();')) {
    if (-not $controller.Contains($needle)) {
        throw "audio/video選択同期またはUndoの契約がありません: $needle"
    }
}
foreach ($needle in @('exportThread_ = exportThreadFactory_(',
                      'exportCancelRequested_.store(true, std::memory_order_release)',
                      'exportCancelling_ = true;',
                      'finishTimelineExport(std::move(exported))',
                      'Qt::QueuedConnection')) {
    if (-not $controller.Contains($needle)) {
        throw "非同期書き出しまたはキャンセル伝播の契約がありません: $needle"
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
