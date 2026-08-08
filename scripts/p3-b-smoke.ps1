[CmdletBinding()]
param(
    [string]$BuildDir = 'build\ucrt64-release',
    [string]$OutputDir = 'build\p3-b-results'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "$BuildDir\bin\mvm_p3_av_sync_spike.exe"
$sourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4'
$sourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4'
$checker = Join-Path $repo 'scripts\check-p3-b-contract.ps1'
$out = Join-Path $repo $OutputDir
foreach ($path in @($exe,$sourceA,$sourceB,$checker)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "P3-B 必須ファイルがありません: $path`nfixture は pwsh scripts/make-p3-fixture.ps1 で生成してください。"
    }
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$runs = @()
foreach ($index in 1..3) {
    $json = Join-Path $out "playback-$index.json"
    & $exe --source-a $sourceA --source-b $sourceB --metrics $json `
        --mode playback --duration-seconds 15
    if ($LASTEXITCODE -ne 0) { throw "P3-B playback run $index が失敗しました" }
    & pwsh -NoProfile -File $checker -Json $json
    if ($LASTEXITCODE -ne 0) { throw "P3-B playback run $index の契約検査が失敗しました" }
    $runs += Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
}

$seekJson = Join-Path $out 'seek-64.json'
& $exe --source-a $sourceA --source-b $sourceB --metrics $seekJson `
    --mode seek --seek-count 64 --seed 20260808
if ($LASTEXITCODE -ne 0) { throw 'P3-B integrated seek 64 が失敗しました' }
& pwsh -NoProfile -File $checker -Json $seekJson
if ($LASTEXITCODE -ne 0) { throw 'P3-B integrated seek JSON contract が失敗しました' }

$pauseJson = Join-Path $out 'pause-resume.json'
& $exe --source-a $sourceA --source-b $sourceB --metrics $pauseJson --mode pause-resume
if ($LASTEXITCODE -ne 0) { throw 'P3-B pause/resume が失敗しました' }
& pwsh -NoProfile -File $checker -Json $pauseJson
if ($LASTEXITCODE -ne 0) { throw 'P3-B pause/resume JSON contract が失敗しました' }

$summary = [ordered]@{
    schema_version = 1
    phase = 'P3-B'
    formal_verdict = 'NOT_RUN'
    playback_runs = 3
    playback_all_pass = @($runs | Where-Object { -not $_.pass }).Count -eq 0
    playback = $runs
    integrated_seek = Get-Content -Raw -LiteralPath $seekJson | ConvertFrom-Json
    pause_resume = Get-Content -Raw -LiteralPath $pauseJson | ConvertFrom-Json
}
$summaryPath = Join-Path $out 'summary.json'
$summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "PASS: P3-B playback 15 sec x3 / seek 64 / pause-resume" -ForegroundColor Green
Write-Host "summary: $summaryPath"
