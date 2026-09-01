$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$controller = Get-Content -LiteralPath $controllerPath -Raw

$requiredQml = @(
    'model: ["V2", "V1"]',
    'x: timelineStartFrame * root.pixelsPerFrame',
    'y: (videoTrack === 1 ? 0 : 68) + 4',
    'mvmController.selectTimelineClip(clipItem.clipId, frame)',
    'mvmController.moveTimelineClip(clipItem.clipId,',
    'Math.max(',
    '0, Math.round(candidateX / root.pixelsPerFrame))'
)
foreach ($needle in $requiredQml) {
    if (-not $qml.Contains($needle)) {
        throw "M7b-4 timeline UI contractがありません: $needle"
    }
}

if ($qml.Contains('timelineList.indexAt') -or $qml.Contains('mvmController.reorderClip')) {
    throw 'vector順をplacement authorityにする旧timeline dragが残っています'
}
if ($controller.Contains('recomputeTimelineStarts(candidate)')) {
    throw 'M7b-4 controller編集経路がrecomputeTimelineStartsに依存しています'
}

Write-Output 'M7b-4 timeline UI architecture: PASS'
