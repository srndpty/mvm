<#
.SYNOPSIS
    P3-1.1: 数式の CPU 合成器 (src/media/math/math_group_compose) を、製品の MLT の pre-YUV と
    全 frame・全画素・4 channel で比べる。

.DESCRIPTION
    fixture は固定。fixture ごとに製品の経路で書き出し、全 frame の pre-YUV を保持して、同じ frame を
    合成器で作ったものと byte 単位で比べる (mvm_math_score_export_parity --compositor-all 1)。
    位置と不透明度は書き出しの計画の値、画像は MLT が受け取るものと同じ (motion のある clip は
    qtcrop と同じ描き直しの後) を使う。許容差は無い。
    拡縮・回転・crop を含む frame は「対象外」として数え、一致には数えない。
    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-compositor-p311-20261012',
    [string]$Label = 'all-frames',
    [string[]]$Fixtures = @('cmp-integer', 'cmp-fraction-fade', 'cmp-layers-16', 'cmp-layers-48',
        'cmp-region-motion', 'cmp-region-scale', 'p2-window-early', 'p2-window-middle', 'p2-window-late')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$harness = Join-Path $repo 'build\ucrt64-release\bin\mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
foreach ($required in @($harness, $manim, $latex, $project)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$runDirectory = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $runDirectory) { throw "出力先が既にあります: $runDirectory" }
New-Item -ItemType Directory -Force $runDirectory | Out-Null

$hashed = @($harness, (Join-Path $ucrt 'libmlt-7.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll', (Join-Path $ucrt 'Qt6Gui.dll'), $project,
    (Join-Path $repo 'src/media/math/math_group_compose.cpp'), (Join-Path $repo 'src/media/math/math_group_compose.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/app/timeline_export.cpp'),
    (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'), (Join-Path $repo 'tests/core/test_math_group_compose.cpp'),
    (Join-Path $repo 'scripts/math-compositor-p311-verify.ps1'))
$identity = [ordered]@{ fixtures = $Fixtures; sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$rows = @()
foreach ($fixture in $Fixtures) {
    $out = Join-Path $runDirectory $fixture
    New-Item -ItemType Directory -Force $out | Out-Null
    & $harness --project $project --manim $manim --out $out --ffprobe (Join-Path $ucrt 'ffprobe.exe') `
        --fixture $fixture --cap-seconds 600 --compositor-all 1 2> (Join-Path $out 'harness.log') | Out-Null
    $code = $LASTEXITCODE
    $reportPath = Join-Path $out "$fixture-report.json"
    $all = if (Test-Path -LiteralPath $reportPath) { (Get-Content -Raw $reportPath | ConvertFrom-Json).compositor_all } else { $null }
    $row = [pscustomobject]@{
        fixture = $fixture; exit_code = $code
        expected = ${all}?.frames_expected; observed = ${all}?.frames_observed; compared = ${all}?.frames_compared
        missing = ${all}?.frames_missing; unsupported = ${all}?.frames_unsupported
        with_math_pixels = ${all}?.frames_with_math_pixels; mismatching = ${all}?.frames_mismatching
        differing_pixels = ${all}?.differing_pixels; first_mismatch = ${all}?.first_mismatch
    }
    $rows += $row
    Write-Host ("{0,-20} exit={1} 想定={2} 観測={3} 比較={4} 欠落={5} 対象外={6} 不一致={7} 画素={8} {9}" -f $fixture, $code,
        $row.expected, $row.observed, $row.compared, $row.missing, $row.unsupported, $row.mismatching, $row.differing_pixels, $row.first_mismatch)
}
$rows | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
$bad = @($rows | Where-Object { $_.exit_code -ne 0 -or $null -eq $_.compared -or $_.mismatching -ne 0 -or $_.missing -ne 0 -or $_.compared -eq 0 })
if ($bad.Count -gt 0) { exit 1 }
exit 0
