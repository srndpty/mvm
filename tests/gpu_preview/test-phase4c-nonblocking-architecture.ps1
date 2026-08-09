[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$probe = Get-Content -Raw -LiteralPath (Join-Path $SourceRoot 'src/media/gpu_preview/transition_probe.cpp')
$renderer = Get-Content -Raw -LiteralPath (Join-Path $SourceRoot 'src/app/preview/compositor_rhi_item.cpp')

foreach ($pattern in @('sleep_for', 'WaitFor[A-Za-z0-9_]*\s*\(')) {
    if ($probe -match $pattern) {
        Write-Error "transition probe product pathにblocking operationが残っています: $pattern"
        exit 3
    }
}
$mapCount = ([regex]::Matches($probe, '->Map\s*\(')).Count
$doNotWaitCount = ([regex]::Matches($probe, 'D3D11_MAP_FLAG_DO_NOT_WAIT')).Count
if ($mapCount -ne $doNotWaitCount) {
    Write-Error "transition probeのMapがすべてDO_NOT_WAITではありません"
    exit 3
}
if ($renderer -match 'transitionProbeReadback\.drain\s*\(' -or
    $renderer -match 'compositor\.shutdown\s*\(') {
    Write-Error "render-thread teardownに同期drain/shutdownが残っています"
    exit 3
}
Write-Host '[phase4c-architecture] PASS render-thread probe teardownは非blockingです'
