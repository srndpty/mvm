<#
.SYNOPSIS
    P3-3B: 下地の alpha が 255 未満でも数式 group を使う診断用の経路を、製品の MLT の pre-YUV と
    全 frame・全画素で比べ、対照と時間の計測も行う。

.DESCRIPTION
    基準は、同じ process で先に書き出した製品の経路の pre-YUV (許容差なし)。candidate は
    --compare math-group --math-group-rule 1 (合成の順だけを守って group を分ける)。合成器の下地の
    下限は契約の値 (kMathGroupMinimumDestinationAlpha) のままで、実験用の上書きはしない。

    段階:
      1. 既存の 9 fixture。
      2. 下地の alpha の対照。対象 (alpha 1 以上) は全 frame 一致、対象外 (alpha 0 を含む) は
         合成器が拒否して candidate の書き出しが失敗すること (一致に数えない)。
      3. Project 全体の graph (v4-full) の 4 区間 (P3-3A と同じ)。
      4. 対照: frame の飛ばし・対応・不透明度・画像の取り違え・layer の欠落 (画素の不一致)、
         下地の拒否 (書き出しの失敗)、取消、失敗時に出力 file が残らないこと、画像の上限。
      5. 時間: Project 全体の graph の各区間を、製品の経路 (B) と candidate (C) を別々の process で
         B, C, C, B の順に書き出す (事前に固定。どちらも process の最初の書き出しになる)。

    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-transition-p33b-20261012',
    [string]$Label = 'verify',
    [string[]]$Fixtures = @('cmp-integer', 'cmp-fraction-fade', 'cmp-layers-16', 'cmp-layers-48',
        'cmp-region-motion', 'cmp-region-scale', 'p2-window-early', 'p2-window-middle', 'p2-window-late'),
    [string[]]$SupportedAlphaFixtures = @('alpha-255', 'alpha-254', 'alpha-253', 'alpha-128', 'alpha-1', 'alpha-all-nonzero'),
    [string[]]$RejectedAlphaFixtures = @('alpha-0', 'alpha-ramp'),
    [int[]]$Windows = @(1200, 3400, 5280, 6500),
    [switch]$SkipFixtures,
    [switch]$SkipAlpha,
    [switch]$SkipWindows,
    [switch]$SkipControls,
    [switch]$SkipTiming
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
    (Join-Path $repo 'tests/core/test_math_group_compose.cpp'), (Join-Path $repo 'scripts/math-transition-p33b-verify.ps1'))
$identity = [ordered]@{ git_head = (git -C $repo rev-parse HEAD); sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$candidate = @('--compare', 'math-group', '--math-group-rule', '1')

function Invoke-Harness([string]$Fixture, [string]$Name, [string[]]$Extra) {
    $out = Join-Path $runDirectory $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    & $harness --project $project --manim $manim --out $out --ffprobe (Join-Path $ucrt 'ffprobe.exe') `
        --fixture $Fixture --cap-seconds 900 @Extra 2> (Join-Path $out 'harness.log') | Out-Null
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
    $report = $result.report
    $compare = Get-Field $report 'compare'
    $group = Get-Field $compare 'candidate_group'
    $destination = Get-Field $report 'math_group_candidate_destination'
    $glyph = Get-Field $destination 'glyph_roi'
    $full = Get-Field $destination 'full_frame'
    $assignment = Get-Field $report 'math_group_candidate_assignment'
    $baseline = Get-Field $compare 'baseline'
    $candidateRun = Get-Field $compare 'candidate_run'
    $repeat = Get-Field $compare 'baseline_repeat'
    [pscustomobject]([ordered]@{
            name = $result.name; fixture = $result.fixture; exit_code = $result.exit_code
            verdict = Get-Field $compare 'verdict'
            expected_frames = Get-Field $report 'expected_frames'
            frames_exact = Get-Field $compare 'frames_exact'
            frames_within_one = Get-Field $compare 'frames_within_one'
            frames_beyond_one = Get-Field $compare 'frames_beyond_one'
            frames_uncompared = Get-Field $compare 'frames_uncompared'
            differing_pixels = Get-Field $compare 'differing_pixels'
            baseline_repeat_mismatches = Get-Field $compare 'baseline_repeat_mismatches'
            candidate_error = Get-Field $compare 'candidate_error'
            group_clean = Get-Field $compare 'group_clean'
            groups = Get-Field $group 'transitions'
            grouped_clips = Get-Field $group 'grouped_clips'
            retained_clips = Get-Field $group 'retained_clips'
            retained_geometry = Get-Field $group 'retained_geometry'
            retained_above = Get-Field $group 'retained_above_retained_layer'
            not_declared_layers = Get-Field $assignment 'conventional_not_declared'
            affine_transitions = Get-Field $group 'affine_transitions'
            baseline_affine_transitions = Get-Field (Get-Field $compare 'baseline_group') 'affine_transitions'
            destination_calls = Get-Field $destination 'calls'
            destination_minimum_alpha = Get-Field $full 'minimum'
            destination_alpha_254 = Get-Field $full 'alpha_254'
            destination_alpha_253 = Get-Field $full 'alpha_253'
            destination_alpha_below_253 = Get-Field $full 'alpha_below_253'
            glyph_roi_minimum_alpha = Get-Field $glyph 'minimum'
            calls_below_255_in_glyph_roi = Get-Field $destination 'calls_with_alpha_below_255_in_glyph_roi'
            composed_frames = Get-Field $group 'composed_frames'
            composed_layer_frames = Get-Field $group 'composed_layer_frames'
            raster_megabytes = if ($group) { [math]::Round($group.raster_bytes / 1MB, 1) } else { $null }
            prepare_ms = Get-Field $group 'prepare_ms'
            working_set_before_prepare_mb = Get-Field $compare 'candidate_working_set_before_prepare_mb'
            working_set_after_prepare_mb = Get-Field $group 'working_set_after_prepare_mb'
            copy_ms_per_frame = Get-Field $group 'copy_ms_per_frame'
            compose_ms_per_frame = Get-Field $group 'compose_ms_per_frame'
            callback_ms_per_frame = Get-Field $group 'callback_ms_per_frame'
            errors = Get-Field $group 'errors'
            live_after_export = Get-Field $group 'live_after_export'
            active_after_export = Get-Field $group 'active_after_export'
            baseline_render_seconds = Get-Field $baseline 'render_seconds'
            candidate_render_seconds = Get-Field $candidateRun 'render_seconds'
            repeat_render_seconds = Get-Field $repeat 'render_seconds'
            candidate_compose_ms_per_image = Get-Field $candidateRun 'compose_ms_per_image'
            repeat_compose_ms_per_image = Get-Field $repeat 'compose_ms_per_image'
        })
}

function Write-Row($row, [string]$label) {
    Write-Host ("{0,-20} exit={1} {2,-8} 一致={3}/{4} 未比較={5} 画素差={6} group={7} まとめた={8} 残した={9} (配置 {10}) 下地の最小 alpha={11} (字形の範囲 {12}) affine={13} render 秒 B/C/B={14:n1}/{15:n1}/{16:n1}" -f `
            $label, $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.frames_uncompared,
        $row.differing_pixels, $row.groups, $row.grouped_clips, $row.retained_clips, $row.retained_geometry,
        $row.destination_minimum_alpha, $row.glyph_roi_minimum_alpha, $row.affine_transitions,
        $row.baseline_render_seconds, $row.candidate_render_seconds, $row.repeat_render_seconds)
}

function Test-Exact($row) {
    $row.exit_code -eq 0 -and $row.verdict -eq 'EXACT' -and $row.frames_exact -eq $row.expected_frames -and
    $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.grouped_clips -gt 0 -and $row.errors -eq 0 -and
    $row.baseline_repeat_mismatches -eq 0
}

$summary = [ordered]@{ fixtures = @(); alpha = @(); windows = @(); controls = @(); timing = @() }
$failures = 0

if (-not $SkipFixtures) {
    foreach ($fixture in $Fixtures) {
        $row = ConvertTo-Row (Invoke-Harness $fixture $fixture $candidate)
        $summary.fixtures += $row
        Write-Row $row $fixture
        if (-not (Test-Exact $row)) { ++$failures }
    }
}

if (-not $SkipAlpha) {
    foreach ($fixture in $SupportedAlphaFixtures) {
        $row = ConvertTo-Row (Invoke-Harness $fixture $fixture $candidate)
        $row | Add-Member -NotePropertyName expectation -NotePropertyValue 'exact'
        $summary.alpha += $row
        Write-Row $row $fixture
        if (-not (Test-Exact $row)) { ++$failures }
    }
    foreach ($fixture in $RejectedAlphaFixtures) {
        # 対象外の下地: 合成器が拒否し、candidate の書き出しが失敗する (一致にも不一致にも数えない)。
        $row = ConvertTo-Row (Invoke-Harness $fixture $fixture $candidate)
        $rejected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
        "$($row.candidate_error)" -match '下限' -and $row.live_after_export -eq 0 -and $row.active_after_export -eq 0
        $row | Add-Member -NotePropertyName expectation -NotePropertyValue 'rejected'
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $rejected
        $summary.alpha += $row
        Write-Host ("{0,-20} exit={1} {2} errors={3} 下地の最小 alpha={4} error='{5}' 拒否={6}" -f $fixture, $row.exit_code,
            $row.verdict, $row.errors, $row.destination_minimum_alpha, $row.candidate_error, $rejected)
        if (-not $rejected) { ++$failures }
    }
}

if (-not $SkipWindows) {
    foreach ($start in $Windows) {
        $row = ConvertTo-Row (Invoke-Harness 'v4-full' "full-graph-$start" ($candidate + @('--range', "${start}:60")))
        $summary.windows += $row
        Write-Row $row "v4-full@$start"
        if (-not (Test-Exact $row)) { ++$failures }
    }
}

if (-not $SkipControls) {
    $fixture = 'cmp-layers-16'
    $cases = @()
    foreach ($fault in @(1, 2, 3, 5, 6)) { $cases += @{ fixture = $fixture; fault = $fault } }
    # 従来の layer と複数の group が混ざる fixture でも、対応の誤りと layer の欠落が検出されること。
    $cases += @{ fixture = 'cmp-region-scale'; fault = 2 }
    $cases += @{ fixture = 'cmp-region-scale'; fault = 6 }
    foreach ($case in $cases) {
        $name = "control-$($case.fixture)-fault-$($case.fault)"
        $row = ConvertTo-Row (Invoke-Harness $case.fixture $name ($candidate + @('--math-group-fault', "$($case.fault)")))
        $detected = $row.exit_code -in @(13, 14) -and $row.verdict -in @('WITHIN_ONE_NOT_EXACT', 'DIFFERS_BEYOND_ONE') -and
        $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.frames_exact -lt $row.expected_frames
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $summary.controls += $row
        if (-not $detected) { ++$failures }
        Write-Host ("{0,-36} exit={1} {2} 一致={3}/{4} 画素差={5} 検出={6}" -f $name, $row.exit_code, $row.verdict,
            $row.frames_exact, $row.expected_frames, $row.differing_pixels, $detected)
    }
    # 下地の拒否 (fault 4 は下地に alpha 0 の画素を作る)。
    $row = ConvertTo-Row (Invoke-Harness $fixture 'control-destination-rejected' ($candidate + @('--math-group-fault', '4')))
    $detected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
    "$($row.candidate_error)" -match '合成器が拒否' -and $row.live_after_export -eq 0 -and $row.active_after_export -eq 0
    $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
    $summary.controls += $row
    if (-not $detected) { ++$failures }
    Write-Host ("control-destination-rejected         exit={0} {1} errors={2} error='{3}' 検出={4}" -f $row.exit_code, $row.verdict,
        $row.errors, $row.candidate_error, $detected)
    # 画像の上限: 上限を 50 MB にすると、準備の途中で止まる (黙って増やさない)。
    $row = ConvertTo-Row (Invoke-Harness $fixture 'control-raster-budget' ($candidate + @('--math-group-budget-mb', '50')))
    $detected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and "$($row.candidate_error)" -match '上限' -and
    $row.live_after_export -eq 0
    $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
    $summary.controls += $row
    if (-not $detected) { ++$failures }
    Write-Host ("control-raster-budget                exit={0} {1} error='{2}' 検出={3}" -f $row.exit_code, $row.verdict,
        $row.candidate_error, $detected)
    # 主の書き出しそのものを group で行い、失敗させる: 書き出しは失敗を返し、出力 file が残らない。
    $result = Invoke-Harness $fixture 'control-failure-not-published' @('--math-group', '1', '--math-group-rule', '1', '--math-group-fault', '4')
    $main = Get-Field $result.report 'math_group_main'
    $detected = $result.exit_code -eq 6 -and (Get-Field $result.report 'export_success') -eq $false -and
    (Get-Field $result.report 'final_file_exists') -eq $false -and (Get-Field $result.report 'temporary_file_exists') -eq $false -and
    (Get-Field $main 'errors') -gt 0 -and (Get-Field $main 'live_after_export') -eq 0 -and (Get-Field $main 'active_after_export') -eq 0
    $summary.controls += [pscustomobject]@{ name = 'control-failure-not-published'; fixture = $fixture; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; export_error = Get-Field $result.report 'export_error'
        final_file_exists = Get-Field $result.report 'final_file_exists'; temporary_file_exists = Get-Field $result.report 'temporary_file_exists'
        errors = Get-Field $main 'errors'; live_after_export = Get-Field $main 'live_after_export'; detected = $detected
    }
    if (-not $detected) { ++$failures }
    Write-Host ("control-failure-not-published        exit={0} {1} 出力 file={2} 一時 file={3} error='{4}' 検出={5}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $result.report 'final_file_exists'),
        (Get-Field $result.report 'temporary_file_exists'), (Get-Field $result.report 'export_error'), $detected)
    # 取消。
    $result = Invoke-Harness $fixture 'control-cancel' @('--math-group', '1', '--math-group-rule', '1', '--inject', 'cancel')
    $main = Get-Field $result.report 'math_group_main'
    $detected = $result.exit_code -eq 11 -and (Get-Field $main 'grouped_clips') -gt 0 -and
    (Get-Field $main 'live_after_export') -eq 0 -and (Get-Field $main 'active_after_export') -eq 0 -and
    (Get-Field $result.report 'final_file_exists') -eq $false
    $summary.controls += [pscustomobject]@{ name = 'control-cancel'; fixture = $fixture; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; grouped_clips = Get-Field $main 'grouped_clips'
        composed_frames = Get-Field $main 'composed_frames'; live_after_export = Get-Field $main 'live_after_export'
        active_after_export = Get-Field $main 'active_after_export'; final_file_exists = Get-Field $result.report 'final_file_exists'
        detected = $detected
    }
    if (-not $detected) { ++$failures }
    Write-Host ("control-cancel                       exit={0} {1} 合成した frame={2} 残った transition={3} 検出={4}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $main 'composed_frames'), (Get-Field $main 'live_after_export'), $detected)
}

if (-not $SkipTiming) {
    # 事前に固定した順: 区間ごとに B, C, C, B。別々の process で、どちらも最初の書き出しになる。
    foreach ($start in $Windows) {
        $order = @('B', 'C', 'C', 'B')
        for ($index = 0; $index -lt $order.Count; ++$index) {
            $kind = $order[$index]
            $extra = @('--range', "${start}:60")
            if ($kind -eq 'C') { $extra += @('--math-group', '1', '--math-group-rule', '1') }
            $result = Invoke-Harness 'v4-full' "timing-$start-$($index + 1)-$kind" $extra
            $report = $result.report
            $main = Get-Field $report 'math_group_main'
            $row = [pscustomobject]([ordered]@{
                    name = $result.name; window = $start; position = $index + 1; kind = $kind; exit_code = $result.exit_code
                    verdict = Get-Field $report 'verdict'
                    render_seconds = Get-Field $report 'main_render_seconds'
                    export_call_wall_seconds = Get-Field $report 'export_call_wall_seconds'
                    wall_seconds = Get-Field $report 'wall_seconds'
                    cpu_seconds = Get-Field $report 'cpu_seconds'
                    compose_ms_per_image = Get-Field $report 'compose_ms_per_image'
                    affine_transitions = Get-Field $report 'overlay_transitions'
                    encoder_submitted = Get-Field $report 'main_encoder_submitted'
                    encoded_packets = Get-Field $report 'main_encoded_packets'
                    muxed_packets = Get-Field $report 'main_muxed_packets'
                    observed_frames = Get-Field $report 'pre_yuv_observed_frames'
                    lowest_alpha = Get-Field $report 'pre_yuv_lowest_alpha'
                    peak_working_set_mb = Get-Field $report 'process_peak_working_set_mb'
                    working_set_before_prepare_mb = Get-Field $main 'working_set_before_prepare_mb'
                    working_set_after_prepare_mb = Get-Field $main 'working_set_after_prepare_mb'
                    raster_megabytes = if ($main -and (Get-Field $main 'raster_bytes')) { [math]::Round($main.raster_bytes / 1MB, 1) } else { 0 }
                    groups = Get-Field $main 'transitions'; grouped_clips = Get-Field $main 'grouped_clips'
                    composed_frames = Get-Field $main 'composed_frames'
                    callback_ms_per_frame = Get-Field $main 'callback_ms_per_frame'
                    callback_seconds_total = Get-Field $main 'callback_seconds_total'
                    errors = Get-Field $main 'errors'; live_after_export = Get-Field $main 'live_after_export'
                })
            $summary.timing += $row
            if ($row.exit_code -ne 0) { ++$failures }
            Write-Host ("timing v4-full@{0} {1}{2} exit={3} render={4:n1}s 書き出し全体={5:n1}s CPU={6:n1}s affine={7} encoder={8} mux={9} peak={10:n0} MB group の callback 合計={11:n2}s" -f `
                    $start, $kind, ($index + 1), $row.exit_code, $row.render_seconds, $row.export_call_wall_seconds, $row.cpu_seconds,
                $row.affine_transitions, $row.encoder_submitted, $row.muxed_packets, $row.peak_working_set_mb, $row.callback_seconds_total)
        }
    }
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
exit 0
