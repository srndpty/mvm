<#
.SYNOPSIS
    音声グラフの最小切り分けを 1 ケース 1 プロセスで実行し、表にまとめる (S5)。

.DESCRIPTION
    S5 で「MP4/AAC の音声が MLT を通すと壊れる」「volume filter でクラッシュする」
    という 2 つの症状が出た。完全構成のまま推測しても切り分けられないので、
    最小構成から 1 段ずつ足して最初に壊れる境界を特定する。

    アクセス違反が起きるケースがあるため、各ケースは子プロセスとして実行し、
    終了コードとして観測する。親プロセスは落とさない。

.EXAMPLE
    pwsh scripts/audio-graph-matrix.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$SmokeDir = Join-Path $RepoRoot 'tests\assets\smoke'
$DiagDir  = Join-Path $SmokeDir '_diag'

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\audio-graph" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench`n  pwsh scripts/build.ps1" }
if (-not (Test-Path $DiagDir)) {
    throw "切り分け用素材がありません: $DiagDir`n  pwsh scripts/make-testmedia.ps1 -Mode Smoke"
}

$env:PATH = "$Ucrt64\bin;$env:PATH"

# ケースの説明。最初に壊れる境界を読み取るため順序が意味を持つ。
$cases = [ordered]@{
    'A'  = 'producer -> consumer (playlist/tractor/transition なし)'
    'B'  = 'producer -> playlist -> tractor -> consumer'
    'C'  = 'B + video_index=-1'
    'D'  = '別 resource: 映像のみ + 音声のみ'
    'E'  = '同一 resource を別 producer で stream 分離'
    'F'  = 'A1 + A2 mix(sum=1)'
    'G'  = 'F + A2 volume gain -6dB'
    'H'  = '5 トラック相当'
    'V0' = 'volume なし (基準)'
    'V1' = 'attach してから playlist へ append'
    'V2' = 'append してから attach'
    'V3' = 'level=-6 + filter in/out 明示'
    'V4' = 'tractor/playlist なし producer + volume'
}

function Describe-Exit([int]$code) {
    switch ($code) {
        0          { 'OK' }
        3          { '検証不一致' }
        4          { 'TIMEOUT' }
        -1073741819 { 'CRASH 0xC0000005 (アクセス違反)' }
        -1073740940 { 'CRASH 0xC0000374 (heap 破壊)' }
        default    { "exit $code" }
    }
}

$results = @()

foreach ($c in $cases.Keys) {
    $wav = Join-Path $OutDir "$c.wav"
    $log = Join-Path $OutDir "$c.log"
    if (Test-Path $wav) { Remove-Item $wav -Force }

    # 子プロセスとして実行する。クラッシュしても親は続行する。
    $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
        -ArgumentList @('audio-graph-probe', '--case', $c, '--output', $wav,
                        '--diag-dir', $DiagDir, '--smoke-dir', $SmokeDir,
                        '--timeout-ms', '60000') `
        -RedirectStandardError $log -RedirectStandardOutput "$log.out"
    $code = $p.ExitCode

    $row = [ordered]@{
        Case = $c
        Desc = $cases[$c]
        Exit = $code
        Status = Describe-Exit $code
        Wav = if (Test-Path $wav) { (Get-Item $wav).Length } else { 0 }
        DomL = ''; DomR = ''; RmsL = ''; RmsR = ''; SnrL = ''; SnrR = ''; Clip = ''
    }

    if ($code -eq 0 -and (Test-Path $wav) -and (Get-Item $wav).Length -gt 1000) {
        $j = & $Bench analyze-wav $wav 2>$null | Out-String
        try {
            $o = $j | ConvertFrom-Json
            $row.DomL = "$($o.L.dominant_hz)Hz=$([math]::Round($o.L.dominant,4))"
            $row.DomR = "$($o.R.dominant_hz)Hz=$([math]::Round($o.R.dominant,4))"
            $row.RmsL = [math]::Round($o.L.rms, 4)
            $row.RmsR = [math]::Round($o.R.rms, 4)
            $row.SnrL = [math]::Round($o.L.snr, 1)
            $row.SnrR = [math]::Round($o.R.snr, 1)
            $row.Clip = [math]::Round([math]::Max($o.L.clip_ratio, $o.R.clip_ratio), 5)
            $j | Set-Content (Join-Path $OutDir "$c.analysis.json") -Encoding UTF8
        } catch {
            $row.DomL = '(解析失敗)'
        }
    }
    $results += [pscustomobject]$row
    Write-Host ("{0,-3} {1,-28} {2}" -f $c, $row.Status, $cases[$c]) `
        -ForegroundColor $(if ($code -eq 0) { 'Green' } else { 'Red' })
}

Write-Host "`n=== 結果 ===" -ForegroundColor Cyan
$results | Format-Table Case, Status, Wav, RmsL, RmsR, DomL, DomR, SnrL, SnrR -AutoSize

$results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
Write-Host "詳細: $OutDir"

# 最初に壊れた境界を明示する
$firstBad = $results | Where-Object { $_.Exit -ne 0 } | Select-Object -First 1
if ($firstBad) {
    Write-Host "`n最初に失敗したケース: $($firstBad.Case) — $($firstBad.Desc)" -ForegroundColor Yellow
    Write-Host "  $($firstBad.Status)" -ForegroundColor Yellow
} else {
    Write-Host "`n全ケース成功" -ForegroundColor Green
}
