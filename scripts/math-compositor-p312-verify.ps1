<#
.SYNOPSIS
    P3-1.2: 数式の CPU 合成器 (src/media/math/math_group_compose) を、製品の MLT の pre-YUV と
    全 frame・全画素・4 channel で比べ、比較そのものの対照 (negative control) も実行する。

.DESCRIPTION
    fixture は固定 (P3-1.1 と同じ 9 つ)。fixture ごとに製品の経路で書き出し、全 frame の pre-YUV を
    保持して、同じ frame を合成器で作ったものと byte 単位で比べる
    (mvm_math_score_export_parity --compositor-all 1)。許容差は無い。

    1 frame の結果は COMPARED / UNSUPPORTED / ERROR に分ける。UNSUPPORTED は事前に決めた変形
    (拡縮・回転・shear・crop、Write、変形、数式でない layer) だけで、frame 番号を残す。
    成果物・mapping・準備の失敗は ERROR で、1 つでもあれば不成立になる。

    対照: 成果物なし・成果物を読めない・mapping なし・frame の欠落。どれも不成立になり、
    UNSUPPORTED に数えられてはならない。

    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-compositor-p312-20261012',
    [string]$Label = 'all-frames',
    [string[]]$Fixtures = @('cmp-integer', 'cmp-fraction-fade', 'cmp-layers-16', 'cmp-layers-48',
        'cmp-region-motion', 'cmp-region-scale', 'p2-window-early', 'p2-window-middle', 'p2-window-late'),
    [switch]$SkipControls
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
    (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'), (Join-Path $repo 'tests/harness/mlt_rgba_oracle.h'),
    (Join-Path $repo 'tests/core/test_math_group_compose.cpp'), (Join-Path $repo 'scripts/math-compositor-p312-verify.ps1'))
$identity = [ordered]@{ fixtures = $Fixtures; sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'

function Invoke-Comparison([string]$Fixture, [string]$Name, [string[]]$Extra) {
    $out = Join-Path $runDirectory $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    & $harness --project $project --manim $manim --out $out --ffprobe (Join-Path $ucrt 'ffprobe.exe') `
        --fixture $Fixture --cap-seconds 600 --compositor-all 1 @Extra 2> (Join-Path $out 'harness.log') | Out-Null
    $code = $LASTEXITCODE
    $reportPath = Join-Path $out "$Fixture-report.json"
    $all = if (Test-Path -LiteralPath $reportPath) { (Get-Content -Raw $reportPath | ConvertFrom-Json).compositor_all } else { $null }
    $row = [ordered]@{ name = $Name; fixture = $Fixture; exit_code = $code; verdict = ${all}?.verdict }
    foreach ($field in @('frames_expected', 'frames_observed', 'frames_compared', 'frames_missing', 'frames_unsupported',
            'frames_error', 'frames_with_math_pixels', 'frames_mismatching', 'differing_pixels', 'first_mismatch',
            'inputs_verified', 'mismatches_truncated', 'unsupported_frame_ids', 'unsupported_reasons',
            'error_frame_ids', 'missing_frame_ids', 'mismatching_frame_ids')) {
        $row[$field] = if ($null -ne $all -and $all.PSObject.Properties[$field]) { $all.$field } else { $null }
    }
    [pscustomobject]$row
}

function Format-Ids($ids) {
    # 連続する frame 番号を a-b の形へまとめる (表示用。report には全部の番号がある)。
    $values = @($ids | Where-Object { $null -ne $_ })
    if ($values.Count -eq 0) { return '-' }
    $parts = @(); $start = $values[0]; $previous = $values[0]
    foreach ($value in ($values | Select-Object -Skip 1)) {
        if ($value -ne $previous + 1) { $parts += $(if ($start -eq $previous) { "$start" } else { "$start-$previous" }); $start = $value }
        $previous = $value
    }
    $parts += $(if ($start -eq $previous) { "$start" } else { "$start-$previous" })
    $parts -join ','
}

$rows = @()
foreach ($fixture in $Fixtures) {
    $row = Invoke-Comparison $fixture $fixture @()
    $rows += $row
    Write-Host ("{0,-20} exit={1} {2,-8} 想定={3} 観測={4} 比較={5} 欠落={6} 対象外={7} ERROR={8} 不一致={9} 画素={10} 対象外の frame={11} {12}" -f `
            $fixture, $row.exit_code, $row.verdict, $row.frames_expected, $row.frames_observed, $row.frames_compared,
        $row.frames_missing, $row.frames_unsupported, $row.frames_error, $row.frames_mismatching, $row.differing_pixels,
        (Format-Ids $row.unsupported_frame_ids), $row.first_mismatch)
}
$bad = @($rows | Where-Object {
        $_.exit_code -ne 0 -or $_.verdict -ne 'exact' -or $_.frames_mismatching -ne 0 -or $_.frames_missing -ne 0 -or
        $_.frames_error -ne 0 -or $_.frames_compared -le 0 -or $_.inputs_verified -ne $true -or
        ($_.frames_compared + $_.frames_unsupported) -ne $_.frames_expected })

# --- 対照: 比較が壊れた入力を「対象外」や「一致」に数えないこと ---------------------------
$controls = @()
$controlFailures = 0
if (-not $SkipControls) {
    $cases = @(
        @{ name = 'control-missing-artifact'; extra = @('--compositor-fault', 'missing-artifact'); error = 120; missing = 0 },
        @{ name = 'control-unreadable-artifact'; extra = @('--compositor-fault', 'unreadable-artifact'); error = 120; missing = 0 },
        @{ name = 'control-missing-mapping'; extra = @('--compositor-fault', 'missing-mapping'); error = 120; missing = 0 },
        @{ name = 'control-missing-frame'; extra = @('--inject', 'drop-frame'); error = 0; missing = 1 }
    )
    foreach ($case in $cases) {
        $row = Invoke-Comparison 'cmp-integer' $case.name $case.extra
        # 期待: 不成立 (exit 8、verdict error)。ERROR・欠落の数が想定どおりで、対象外は 0。
        $detected = $row.exit_code -eq 8 -and $row.verdict -eq 'error' -and $row.frames_unsupported -eq 0 -and
        $row.frames_error -eq $case.error -and $row.frames_missing -eq $case.missing -and
        ($row.frames_compared + $row.frames_error + $row.frames_missing) -eq $row.frames_expected
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $controls += $row
        if (-not $detected) { ++$controlFailures }
        Write-Host ("{0,-28} exit={1} {2,-6} 比較={3} 欠落={4} 対象外={5} ERROR={6} 検出={7}" -f $case.name, $row.exit_code,
            $row.verdict, $row.frames_compared, $row.frames_missing, $row.frames_unsupported, $row.frames_error, $detected)
    }
}

[ordered]@{ fixtures = $rows; controls = $controls } | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
if ($bad.Count -gt 0 -or $controlFailures -gt 0) { exit 1 }
exit 0
