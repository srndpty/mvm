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
    'destination.kind, destination.index'
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
    'ScrollBar.vertical:',
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
    'sequence: "Space"'
)

foreach ($needle in ($requiredQml + $requiredInteractions + $requiredShortcuts)) {
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
