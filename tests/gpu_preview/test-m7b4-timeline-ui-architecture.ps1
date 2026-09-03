$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$controller = Get-Content -LiteralPath $controllerPath -Raw

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
    'acceptedModifiers: Qt.NoModifier',
    'acceptedModifiers: Qt.ShiftModifier',
    'acceptedModifiers: Qt.AltModifier',
    'timelinePanel.setZoom(',
    'mvmController.hasGapAt(',
    'mvmController.rippleDeleteGap(',
    'mvmController.setTrackMuted(',
    'mvmController.addTrack(',
    'mvmController.videoTrackModel',
    'mvmController.audioTrackModel'
)

foreach ($needle in ($requiredQml + $requiredInteractions)) {
    if (-not $qml.Contains($needle)) {
        throw "timeline UI contractがありません: $needle"
    }
}
foreach ($needle in $forbiddenQml) {
    if ($qml.Contains($needle)) {
        throw "track数を固定する旧timeline UIが残っています: $needle"
    }
}

if ($controller.Contains('recomputeTimelineStarts(candidate)')) {
    throw 'controller編集経路がrecomputeTimelineStartsに依存しています'
}
# preview の layer 構成は mapTimelinePreviewFrame に一本化する。
if (-not $controller.Contains('mapTimelinePreviewFrame(project_, timelineFrame)')) {
    throw 'controllerがpreview layer mappingを経由していません'
}

Write-Output 'timeline UI architecture: PASS'
