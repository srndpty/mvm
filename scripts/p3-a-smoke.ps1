<# P3-A の standalone smoke。P3 formal matrix ではない。 #>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$OutputDir = 'build\p3-a-smoke'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Preset\bin\mvm_p3_audio_smoke.exe"
$fixture = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4'
$raw = Join-Path $repo $OutputDir
New-Item -ItemType Directory -Force -Path $raw | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
if (-not (Test-Path $fixture)) { throw 'pwsh scripts/make-p3-fixture.ps1 を先に実行してください。' }
if (-not (Test-Path $exe)) { throw "P3-A smoke executable がありません: $exe" }

$playback = @()
foreach ($run in 1..3) {
    $path = Join-Path $raw "playback-$run.json"
    & $exe playback $fixture $path --duration-seconds 15
    if ($LASTEXITCODE -ne 0) { throw "playback run $run が失敗しました (exit $LASTEXITCODE)" }
    $playback += $path
    Start-Sleep -Milliseconds 250
}
$seek = Join-Path $raw 'seek.json'
& $exe seek $fixture $seek --seek-count 64 --seed 20260808
if ($LASTEXITCODE -ne 0) { throw "64 exact seek が失敗しました (exit $LASTEXITCODE)" }
Start-Sleep -Milliseconds 250
$pause = Join-Path $raw 'pause-resume.json'
& $exe pause-resume $fixture $pause
if ($LASTEXITCODE -ne 0) { throw "pause/resume が失敗しました (exit $LASTEXITCODE)" }
Start-Sleep -Milliseconds 250
$marker = Join-Path $raw 'fixture.json'
& $exe fixture $fixture $marker
if ($LASTEXITCODE -ne 0) { throw 'fixture marker 検証が失敗しました' }

& (Join-Path $PSScriptRoot 'check-p3-a-contract.ps1') -Playback $playback -Seek $seek `
    -PauseResume $pause -Fixture $marker -Output (Join-Path $raw 'summary.json')
if ($LASTEXITCODE -ne 0) { throw 'P3-A smoke contract が不合格です' }
