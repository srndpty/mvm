<#
.SYNOPSIS
    P3-3A: 診断専用の数式 group transition を、製品の MLT の pre-YUV と全 frame・全画素で比べ、
    対照 (negative control) と時間の計測も行う。

.DESCRIPTION
    fixture は固定 (P3-1.1・P3-1.2 と同じ 9 つ)。fixture ごとに同じ process で
      baseline (製品の経路) -> candidate (数式 group) -> baseline をもう一度
    の順に書き出し (mvm_math_score_export_parity --compare math-group)、candidate の全 frame の
    RGBA を baseline と byte 単位で比べる。基準は製品の経路の pre-YUV で、許容差は無い。
    2 回目の baseline が 1 回目と違えば比較は無効になる。

    group にまとめるのは、等倍・回転なし・crop なしの静止の数式 clip で、合成の順で連続する分だけ。
    さらに、並びで自分より前にある従来の経路の clip と時間が重なる clip はまとめない
    (順序と、不透明な下地の条件を守るため)。まとめない clip は従来の affine のまま書き出す
    (数と最初の理由を report に残す)。

    対照 (cmp-layers-16):
      fault 1 合成を 1 frame 飛ばす / 2 1 つ後の frame の値 / 3 不透明度を半分 / 5 別の画像 /
      6 一番上の layer を落とす  -> 画素の不一致として検出されること
      fault 4 下地に不透明でない画素 -> 合成器が拒否し、candidate の書き出しが失敗すること
      cancel  group を使う書き出しを途中で取り消す -> 取消で終わり、transition が残らないこと

    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-transition-p33a-20261012',
    [string]$Label = 'parity',
    [string[]]$Fixtures = @('cmp-integer', 'cmp-fraction-fade', 'cmp-layers-16', 'cmp-layers-48',
        'cmp-region-motion', 'cmp-region-scale', 'p2-window-early', 'p2-window-middle', 'p2-window-late'),
    [switch]$SkipControls,
    # Project 全体の graph (v4-full) のまま、60 frame の区間だけを書き出して比べる。
    [switch]$FullGraphWindows,
    # fixture の比較へ足す harness の引数 (例: --render-threads 8)。製品の条件ではなくなる。
    [string[]]$ExtraArguments = @()
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
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_math_group.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_math_group.h'),
    (Join-Path $repo 'src/app/timeline_export.cpp'), (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'),
    (Join-Path $repo 'scripts/math-transition-p33a-verify.ps1'))
$identity = [ordered]@{ fixtures = $Fixtures; extra_arguments = $ExtraArguments; git_head = (git -C $repo rev-parse HEAD); sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'

function Invoke-Harness([string]$Fixture, [string]$Name, [string[]]$Extra) {
    $out = Join-Path $runDirectory $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    & $harness --project $project --manim $manim --out $out --ffprobe (Join-Path $ucrt 'ffprobe.exe') `
        --fixture $Fixture --cap-seconds 600 @Extra 2> (Join-Path $out 'harness.log') | Out-Null
    $code = $LASTEXITCODE
    $reportPath = Join-Path $out "$Fixture-report.json"
    $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
    [pscustomobject]@{ name = $Name; fixture = $Fixture; exit_code = $code; report = $report }
}

function Get-Field($object, [string]$name) {
    if ($null -ne $object -and $object.PSObject.Properties[$name]) { return $object.$name }
    return $null
}

function ConvertTo-Row($result) {
    $compare = Get-Field $result.report 'compare'
    $group = Get-Field $compare 'candidate_group'
    $baseline = Get-Field $compare 'baseline'
    $candidate = Get-Field $compare 'candidate_run'
    $repeat = Get-Field $compare 'baseline_repeat'
    [pscustomobject]([ordered]@{
            name = $result.name; fixture = $result.fixture; exit_code = $result.exit_code
            verdict = Get-Field $compare 'verdict'
            expected_frames = Get-Field $result.report 'expected_frames'
            frames_exact = Get-Field $compare 'frames_exact'
            frames_within_one = Get-Field $compare 'frames_within_one'
            frames_beyond_one = Get-Field $compare 'frames_beyond_one'
            frames_uncompared = Get-Field $compare 'frames_uncompared'
            differing_pixels = Get-Field $compare 'differing_pixels'
            candidate_error = Get-Field $compare 'candidate_error'
            group_clean = Get-Field $compare 'group_clean'
            transitions = Get-Field $group 'transitions'
            grouped_clips = Get-Field $group 'grouped_clips'
            retained_clips = Get-Field $group 'retained_clips'
            retained_first_reason = Get-Field $group 'retained_first_reason'
            retained_geometry = Get-Field $group 'retained_geometry'
            retained_above = Get-Field $group 'retained_above_retained_layer'
            affine_transitions = Get-Field $group 'affine_transitions'
            composed_frames = Get-Field $group 'composed_frames'
            composed_layer_frames = Get-Field $group 'composed_layer_frames'
            raster_megabytes = if ($group) { [math]::Round($group.raster_bytes / 1MB, 1) } else { $null }
            prepare_ms = Get-Field $group 'prepare_ms'
            copy_ms_per_frame = Get-Field $group 'copy_ms_per_frame'
            compose_ms_per_frame = Get-Field $group 'compose_ms_per_frame'
            callback_ms_per_frame = Get-Field $group 'callback_ms_per_frame'
            errors = Get-Field $group 'errors'
            live_after_export = Get-Field $group 'live_after_export'
            active_after_export = Get-Field $group 'active_after_export'
            baseline_render_seconds = Get-Field $baseline 'render_seconds'
            candidate_render_seconds = Get-Field $candidate 'render_seconds'
            repeat_render_seconds = Get-Field $repeat 'render_seconds'
            baseline_wall_seconds = Get-Field $baseline 'wall_seconds'
            candidate_wall_seconds = Get-Field $candidate 'wall_seconds'
            repeat_wall_seconds = Get-Field $repeat 'wall_seconds'
            baseline_compose_ms_per_image = Get-Field $baseline 'compose_ms_per_image'
            candidate_compose_ms_per_image = Get-Field $candidate 'compose_ms_per_image'
            repeat_compose_ms_per_image = Get-Field $repeat 'compose_ms_per_image'
        })
}

$rows = @()
foreach ($fixture in $Fixtures) {
    $row = ConvertTo-Row (Invoke-Harness $fixture $fixture (@('--compare', 'math-group') + $ExtraArguments))
    $rows += $row
    Write-Host ("{0,-18} exit={1} {2,-8} 一致={3}/{4} 未比較={5} 画素差={6} group={7} まとめた clip={8} 残した clip={9} (配置 {10} / 下に従来の layer {14}) render 秒 base/cand/base={11:n1}/{12:n1}/{13:n1}" -f `
            $fixture, $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.frames_uncompared,
        $row.differing_pixels, $row.transitions, $row.grouped_clips, $row.retained_clips, $row.retained_geometry,
        $row.baseline_render_seconds, $row.candidate_render_seconds, $row.repeat_render_seconds, $row.retained_above)
}
if ($FullGraphWindows) {
    foreach ($start in @(1200, 3400, 5280, 6500)) {
        $row = ConvertTo-Row (Invoke-Harness 'v4-full' "full-graph-$start" @('--compare', 'math-group', '--range', "${start}:60"))
        $rows += $row
        Write-Host ("{0,-18} exit={1} {2,-8} 一致={3}/{4} 未比較={5} 画素差={6} group={7} まとめた clip={8} 残した clip={9} (配置 {10} / 下に従来の layer {14}) render 秒 base/cand/base={11:n1}/{12:n1}/{13:n1}" -f `
                "v4-full@$start", $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.frames_uncompared,
            $row.differing_pixels, $row.transitions, $row.grouped_clips, $row.retained_clips, $row.retained_geometry,
            $row.baseline_render_seconds, $row.candidate_render_seconds, $row.repeat_render_seconds, $row.retained_above)
    }
}
$bad = @($rows | Where-Object {
        $_.exit_code -ne 0 -or $_.verdict -ne 'EXACT' -or $_.frames_exact -ne $_.expected_frames -or
        $_.frames_uncompared -ne 0 -or $_.group_clean -ne $true -or $_.grouped_clips -le 0 -or $_.errors -ne 0 })

$controls = @()
$controlFailures = 0
if (-not $SkipControls) {
    $fixture = 'cmp-layers-16'
    foreach ($fault in @(1, 2, 3, 5, 6)) {
        $row = ConvertTo-Row (Invoke-Harness $fixture "control-fault-$fault" @('--compare', 'math-group', '--math-group-fault', "$fault"))
        # 期待: 比較は成立し (全 frame を比べた)、画素の不一致として検出される (exit 13 または 14)。
        $detected = $row.exit_code -in @(13, 14) -and $row.verdict -in @('WITHIN_ONE_NOT_EXACT', 'DIFFERS_BEYOND_ONE') -and
        $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.frames_exact -lt $row.expected_frames
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $controls += $row
        if (-not $detected) { ++$controlFailures }
        Write-Host ("control-fault-{0}  exit={1} {2} 一致={3}/{4} 画素差={5} 検出={6}" -f $fault, $row.exit_code, $row.verdict,
            $row.frames_exact, $row.expected_frames, $row.differing_pixels, $detected)
    }
    # fault 4: 合成器が拒否して candidate の書き出しが失敗する (画素の比較ではなく、失敗として検出)。
    $row = ConvertTo-Row (Invoke-Harness $fixture 'control-fault-4' @('--compare', 'math-group', '--math-group-fault', '4'))
    $detected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
    "$($row.candidate_error)" -match '合成器が拒否' -and $row.live_after_export -eq 0 -and $row.active_after_export -eq 0
    $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
    $controls += $row
    if (-not $detected) { ++$controlFailures }
    Write-Host ("control-fault-4  exit={0} {1} errors={2} error='{3}' 検出={4}" -f $row.exit_code, $row.verdict, $row.errors,
        $row.candidate_error, $detected)
    # cancel: group を使う書き出しを途中で取り消す。
    $result = Invoke-Harness $fixture 'control-cancel' @('--math-group', '1', '--inject', 'cancel')
    $main = Get-Field $result.report 'math_group_main'
    $detected = $result.exit_code -eq 11 -and (Get-Field $main 'grouped_clips') -gt 0 -and
    (Get-Field $main 'live_after_export') -eq 0 -and (Get-Field $main 'active_after_export') -eq 0
    $controls += [pscustomobject]@{ name = 'control-cancel'; fixture = $fixture; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; grouped_clips = Get-Field $main 'grouped_clips'
        composed_frames = Get-Field $main 'composed_frames'; live_after_export = Get-Field $main 'live_after_export'
        active_after_export = Get-Field $main 'active_after_export'; detected = $detected
    }
    if (-not $detected) { ++$controlFailures }
    Write-Host ("control-cancel   exit={0} {1} まとめた clip={2} 合成した frame={3} 残った transition={4} 検出={5}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $main 'grouped_clips'), (Get-Field $main 'composed_frames'),
        (Get-Field $main 'live_after_export'), $detected)
}

[ordered]@{ fixtures = $rows; controls = $controls } | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
if ($bad.Count -gt 0 -or $controlFailures -gt 0) { exit 1 }
exit 0
