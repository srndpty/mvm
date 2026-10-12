<#
.SYNOPSIS
    P3-3D: 数式 group を使う graph にしてよいかを書き出しの前に判定し、ELIGIBLE のときだけ使う経路
    (--compare math-group-auto) を、製品の MLT の pre-YUV と全 frame・全画素で比べる。

.DESCRIPTION
    candidate は mode 3: 判定が ELIGIBLE なら数式 group の graph、INELIGIBLE・UNKNOWN なら従来の graph
    (製品の書き出しと同じもの) を作る。どちらの場合も、candidate の全 frame を、同じ process で先に
    書き出した製品の経路と byte 単位で比べる (許容差なし)。判定・理由・実際に作った graph が、
    fixture ごとに事前に決めた期待と一致することも確かめる。

    段階:
      1. 判定と parity: 既存の 9 fixture、下地の alpha の対照 (alpha 0 を含む)、対象外の配置
         (拡縮・回転・crop)、V1 に effect がある場合、alpha の証明が無い場合。
      2. Project 全体の graph (v4-full) の 4 区間。
      3. 対照: frame の飛ばし・対応・不透明度・画像の取り違え・layer の欠落 (画素の不一致)、
         下地の拒否 (mode 1 でも mode 3 でも書き出しが失敗する)、失敗時に出力 file が残らないこと、
         成功時にだけ出力 file ができること、取消。
      4. 並行と寿命: worker 1・4・8 本、同じ process での candidate の繰り返し、すぐの取消、
         画像の準備の失敗。procdump があれば、その下で実行して dump を採る。
      5. 時間と memory: Project 全体の graph の各区間を、別々の process で B, R, R, B の順に書き出す。

    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-transition-p33d-20261012',
    [string]$Label = 'verify',
    [int[]]$Windows = @(1200, 3400, 5280, 6500),
    [switch]$SkipDecisions,
    [switch]$SkipWindows,
    [switch]$SkipControls,
    [switch]$SkipStress,
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
$procdump = (Get-Command procdump -ErrorAction SilentlyContinue)?.Source
foreach ($required in @($harness, $manim, $latex, $project)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
$runDirectory = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $runDirectory) { throw "出力先が既にあります: $runDirectory" }
New-Item -ItemType Directory -Force $runDirectory | Out-Null

$hashed = @($harness, (Join-Path $repo 'build\ucrt64-release\bin\mvm_test_math_group_compose.exe'),
    (Join-Path $ucrt 'libmlt-7.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll', (Join-Path $ucrt 'Qt6Gui.dll'), $project,
    (Join-Path $repo 'src/media/math/math_group_compose.cpp'), (Join-Path $repo 'src/media/math/math_group_compose.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_math_group.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_math_group.h'),
    (Join-Path $repo 'src/app/timeline_export.cpp'), (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'),
    (Join-Path $repo 'tests/core/test_math_group_compose.cpp'), (Join-Path $repo 'scripts/math-transition-p33d-verify.ps1'))
$identity = [ordered]@{ git_head = (git -C $repo rev-parse HEAD); procdump = "$procdump"; sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$auto = @('--compare', 'math-group-auto', '--math-group-rule', '1')
$forced = @('--compare', 'math-group', '--math-group-rule', '1')

function Invoke-Harness([string]$Fixture, [string]$Name, [string[]]$Extra, [switch]$Dump) {
    $out = Join-Path $runDirectory $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = @('--project', $project, '--manim', $manim, '--out', $out, '--ffprobe', (Join-Path $ucrt 'ffprobe.exe'),
        '--fixture', $Fixture, '--cap-seconds', '900') + $Extra
    $dumps = 0
    if ($Dump -and $procdump) {
        # 未処理の例外で full dump を採る。procdump 自身の終了コードは使わない (report の値を読む)。
        $dumpDirectory = Join-Path $out 'dumps'
        New-Item -ItemType Directory -Force $dumpDirectory | Out-Null
        & $procdump -accepteula -e -ma -x $dumpDirectory $harness @arguments *> (Join-Path $out 'harness.log')
        $code = $null
        $dumps = @(Get-ChildItem $dumpDirectory -Filter *.dmp -ErrorAction SilentlyContinue).Count
    } else {
        & $harness @arguments 2> (Join-Path $out 'harness.log') | Out-Null
        $code = $LASTEXITCODE
    }
    $reportPath = Join-Path $out "$Fixture-report.json"
    $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
    if ($null -eq $code) { $code = if ($report) { $report.exit_code } else { -1 } }
    [pscustomobject]@{ name = $Name; fixture = $Fixture; exit_code = $code; report = $report; dumps = $dumps }
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
            storage = Get-Field $compare 'candidate_storage'
            decision = Get-Field $group 'decision'
            decision_reason = Get-Field $group 'decision_reason'
            graph = Get-Field $group 'graph'
            base_alpha_proven = Get-Field $group 'base_alpha_proven'
            raster_shared = Get-Field $group 'raster_shared'
            raster_conflicts = Get-Field $group 'raster_conflicts'
            candidate_exports = Get-Field $compare 'candidate_exports'
            candidate_export_failures = Get-Field $compare 'candidate_export_failures'
            candidate_leaks = Get-Field $compare 'candidate_leaks'
            raster_count = Get-Field $group 'raster_count'
            raster_bytes = Get-Field $group 'raster_bytes'
            source_bytes_read = Get-Field $group 'source_bytes_read'
            raster_live_bytes_after_prepare = Get-Field $group 'raster_live_bytes_after_prepare'
            raster_live_bytes_after_export = Get-Field $compare 'candidate_raster_live_bytes_after_export'
            working_set_after_export_mb = Get-Field $compare 'candidate_working_set_after_export_mb'
            peak_working_set_mb = Get-Field $report 'process_peak_working_set_mb'
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

function Test-Exact($row) {
    $row.exit_code -eq 0 -and $row.verdict -eq 'EXACT' -and $row.frames_exact -eq $row.expected_frames -and
    $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.grouped_clips -gt 0 -and $row.errors -eq 0 -and
    $row.baseline_repeat_mismatches -eq 0 -and $row.raster_live_bytes_after_export -eq 0
}



function Write-Row($row, [string]$label) {
    Write-Host ("{0,-26} exit={1} {2,-8} 一致={3}/{4} 判定={5} ({6}) graph={7} group={8} まとめた={9} affine={10} V1 の alpha={11} 画像={12:n2} MB 終了後の画素={13} render 秒 B/C/B={14:n1}/{15:n1}/{16:n1}" -f `
            $label, $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.decision, $row.decision_reason,
        $row.graph, $row.groups, $row.grouped_clips, $row.affine_transitions, $row.base_alpha_proven, ($row.raster_bytes / 1MB),
        $row.raster_live_bytes_after_export, $row.baseline_render_seconds, $row.candidate_render_seconds, $row.repeat_render_seconds)
}

# candidate が全 frame 一致し、判定・理由・graph が期待どおりであること。
function Test-Decision($row, [string]$Decision, [string]$Reason) {
    $graph = if ($Decision -eq 'ELIGIBLE') { 'math-group' } else { 'conventional' }
    $row.exit_code -eq 0 -and $row.verdict -eq 'EXACT' -and $row.frames_exact -eq $row.expected_frames -and
    $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.errors -eq 0 -and
    $row.baseline_repeat_mismatches -eq 0 -and $row.raster_live_bytes_after_export -eq 0 -and
    $row.decision -eq $Decision -and $row.decision_reason -eq $Reason -and $row.graph -eq $graph -and
    ($Decision -ne 'ELIGIBLE' -or $row.grouped_clips -gt 0) -and
    ($Decision -eq 'ELIGIBLE' -or ($row.grouped_clips -eq 0 -and $row.affine_transitions -eq $row.baseline_affine_transitions))
}

$summary = [ordered]@{ decisions = @(); windows = @(); controls = @(); stress = @(); timing = @() }
$failures = 0

if (-not $SkipDecisions) {
    # fixture、追加の引数、期待する判定、期待する理由 (実行の前に固定)。
    $cases = @(
        @('cmp-integer', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('cmp-fraction-fade', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('cmp-layers-16', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('cmp-layers-48', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('cmp-region-motion', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('cmp-region-scale', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('p2-window-early', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('p2-window-middle', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('p2-window-late', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-255', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-254', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-253', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-128', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-1', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-all-nonzero', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('alpha-0', @(), 'INELIGIBLE', 'base-alpha-zero'),
        @('alpha-ramp', @(), 'INELIGIBLE', 'base-alpha-zero'),
        @('geom-mixed', @(), 'ELIGIBLE', 'base-alpha-proven'),
        @('geom-all-unsupported', @(), 'INELIGIBLE', 'no-eligible-math-clip'),
        @('base-transformed', @(), 'UNKNOWN', 'base-clip-transformed'),
        @('cmp-layers-16', @('--base-alpha-proof', 'none'), 'UNKNOWN', 'base-alpha-unproven')
    )
    foreach ($case in $cases) {
        $name = if ($case[1].Count -gt 0) { "$($case[0])-proof-none" } else { $case[0] }
        $row = ConvertTo-Row (Invoke-Harness $case[0] $name ($auto + $case[1]))
        $ok = Test-Decision $row $case[2] $case[3]
        $row | Add-Member -NotePropertyName expected_decision -NotePropertyValue $case[2]
        $row | Add-Member -NotePropertyName expected_reason -NotePropertyValue $case[3]
        $row | Add-Member -NotePropertyName as_expected -NotePropertyValue $ok
        $summary.decisions += $row
        Write-Row $row $name
        if (-not $ok) { ++$failures; Write-Host "  期待と違います: $($case[2]) ($($case[3]))" }
    }
}

if (-not $SkipWindows) {
    foreach ($start in $Windows) {
        $row = ConvertTo-Row (Invoke-Harness 'v4-full' "full-graph-$start" ($auto + @('--range', "${start}:60")))
        $ok = Test-Decision $row 'ELIGIBLE' 'base-alpha-proven'
        $row | Add-Member -NotePropertyName as_expected -NotePropertyValue $ok
        $summary.windows += $row
        Write-Row $row "v4-full@$start"
        if (-not $ok) { ++$failures }
    }
}

if (-not $SkipControls) {
    $fixture = 'cmp-layers-16'
    foreach ($fault in @(1, 2, 3, 5, 6)) {
        $name = "control-fault-$fault"
        $row = ConvertTo-Row (Invoke-Harness $fixture $name ($auto + @('--math-group-fault', "$fault")))
        $detected = $row.exit_code -in @(13, 14) -and $row.verdict -in @('WITHIN_ONE_NOT_EXACT', 'DIFFERS_BEYOND_ONE') -and
        $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.frames_exact -lt $row.expected_frames -and
        $row.graph -eq 'math-group'
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $summary.controls += $row
        if (-not $detected) { ++$failures }
        Write-Host ("{0,-36} exit={1} {2} 一致={3}/{4} 画素差={5} 検出={6}" -f $name, $row.exit_code, $row.verdict,
            $row.frames_exact, $row.expected_frames, $row.differing_pixels, $detected)
    }
    # 下地に alpha 0 の画素を作る故障は、判定が ELIGIBLE でも実行時に拒否されて失敗する (mode 3 と mode 1)。
    foreach ($mode in @(@('control-destination-rejected-auto', $auto), @('control-destination-rejected-forced', $forced))) {
        $row = ConvertTo-Row (Invoke-Harness $fixture $mode[0] ($mode[1] + @('--math-group-fault', '4')))
        $detected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
        "$($row.candidate_error)" -match '合成器が拒否' -and $row.live_after_export -eq 0 -and $row.active_after_export -eq 0 -and
        $row.raster_live_bytes_after_export -eq 0 -and $row.graph -eq 'math-group'
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $summary.controls += $row
        if (-not $detected) { ++$failures }
        Write-Host ("{0,-36} exit={1} {2} 判定={3} errors={4} 検出={5}" -f $mode[0], $row.exit_code, $row.verdict, $row.decision, $row.errors, $detected)
    }
    # 主の書き出しそのもの (mode 3): 出力 file ができるのは、書き出しが成功したときだけ。
    $publish = @(
        @{ name = 'publish-eligible'; fixture = 'cmp-layers-16'; extra = @(); exit = 0; success = $true; graph = 'math-group'; decision = 'ELIGIBLE' },
        @{ name = 'publish-unknown-conventional'; fixture = 'cmp-layers-16'; extra = @('--base-alpha-proof', 'none'); exit = 0; success = $true; graph = 'conventional'; decision = 'UNKNOWN' },
        @{ name = 'publish-ineligible-conventional'; fixture = 'alpha-0'; extra = @(); exit = 0; success = $true; graph = 'conventional'; decision = 'INELIGIBLE' },
        @{ name = 'publish-failure'; fixture = 'cmp-layers-16'; extra = @('--math-group-fault', '4'); exit = 6; success = $false; graph = 'math-group'; decision = 'ELIGIBLE' },
        @{ name = 'publish-cancel'; fixture = 'cmp-layers-16'; extra = @('--inject', 'cancel'); exit = 11; success = $false; graph = 'math-group'; decision = 'ELIGIBLE' }
    )
    foreach ($case in $publish) {
        $result = Invoke-Harness $case.fixture $case.name (@('--math-group', 'auto', '--math-group-rule', '1') + $case.extra)
        $main = Get-Field $result.report 'math_group_main'
        $final = Get-Field $result.report 'final_file_exists'
        $temporary = Get-Field $result.report 'temporary_file_exists'
        $detected = $result.exit_code -eq $case.exit -and (Get-Field $result.report 'export_success') -eq $case.success -and
        $final -eq $case.success -and $temporary -eq $false -and (Get-Field $main 'graph') -eq $case.graph -and
        (Get-Field $main 'decision') -eq $case.decision -and (Get-Field $main 'live_after_export') -eq 0 -and
        (Get-Field $main 'active_after_export') -eq 0 -and (Get-Field $main 'raster_live_bytes_after_export') -eq 0
        $summary.controls += [pscustomobject]@{ name = $case.name; fixture = $case.fixture; exit_code = $result.exit_code
            verdict = Get-Field $result.report 'verdict'; export_success = Get-Field $result.report 'export_success'
            export_error = Get-Field $result.report 'export_error'; final_file_exists = $final; temporary_file_exists = $temporary
            decision = Get-Field $main 'decision'; decision_reason = Get-Field $main 'decision_reason'; graph = Get-Field $main 'graph'
            errors = Get-Field $main 'errors'; composed_frames = Get-Field $main 'composed_frames'
            live_after_export = Get-Field $main 'live_after_export'
            raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'; detected = $detected
        }
        if (-not $detected) { ++$failures }
        Write-Host ("{0,-36} exit={1} {2} 判定={3} graph={4} 出力 file={5} 一時 file={6} 期待どおり={7}" -f $case.name, $result.exit_code,
            (Get-Field $result.report 'verdict'), (Get-Field $main 'decision'), (Get-Field $main 'graph'), $final, $temporary, $detected)
    }
}

if (-not $SkipStress) {
    # 並行: worker の数を変え、同じ process の中で candidate を 3 回続けて書き出す (どの回も全 frame を比べる)。
    foreach ($fixture in @('cmp-layers-48', 'cmp-region-scale')) {
        foreach ($threads in @(1, 4, 8)) {
            $name = "stress-$fixture-threads$threads"
            $result = Invoke-Harness $fixture $name ($auto + @('--render-threads', "$threads", '--candidate-repeats', '3')) -Dump
            $row = ConvertTo-Row $result
            $ok = (Test-Decision $row 'ELIGIBLE' 'base-alpha-proven') -and $row.candidate_exports -eq 3 -and
            $row.candidate_export_failures -eq 0 -and $row.candidate_leaks -eq 0 -and $result.dumps -eq 0
            $row | Add-Member -NotePropertyName threads -NotePropertyValue $threads
            $row | Add-Member -NotePropertyName dumps -NotePropertyValue $result.dumps
            $row | Add-Member -NotePropertyName as_expected -NotePropertyValue $ok
            $summary.stress += $row
            if (-not $ok) { ++$failures }
            Write-Host ("{0,-40} exit={1} {2} 一致={3}/{4} candidate={5} 回 失敗={6} 残り={7} dump={8} 期待どおり={9}" -f $name, $row.exit_code,
                $row.verdict, $row.frames_exact, $row.expected_frames, $row.candidate_exports, $row.candidate_export_failures,
                $row.candidate_leaks, $result.dumps, $ok)
        }
    }
    # 取消: 書き出しが始まってすぐ (合成済みの画像が 0・1・8 枚の時点)、worker 1・4・8 本。
    foreach ($threads in @(1, 4, 8)) {
        foreach ($after in @(0, 1, 8)) {
            $name = "stress-cancel-threads$threads-after$after"
            $result = Invoke-Harness 'cmp-layers-16' $name @('--math-group', 'auto', '--math-group-rule', '1', '--render-threads', "$threads",
                '--inject', 'cancel', '--cancel-after', "$after") -Dump
            $main = Get-Field $result.report 'math_group_main'
            $ok = $result.exit_code -eq 11 -and (Get-Field $result.report 'final_file_exists') -eq $false -and
            (Get-Field $result.report 'temporary_file_exists') -eq $false -and (Get-Field $main 'live_after_export') -eq 0 -and
            (Get-Field $main 'active_after_export') -eq 0 -and (Get-Field $main 'raster_live_bytes_after_export') -eq 0 -and
            $result.dumps -eq 0
            $summary.stress += [pscustomobject]@{ name = $name; fixture = 'cmp-layers-16'; threads = $threads; cancel_after = $after
                exit_code = $result.exit_code; verdict = Get-Field $result.report 'verdict'
                composed_frames = Get-Field $main 'composed_frames'; callbacks = Get-Field $main 'callbacks'
                live_after_export = Get-Field $main 'live_after_export'; active_after_export = Get-Field $main 'active_after_export'
                raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
                working_set_after_export_mb = Get-Field $main 'working_set_after_export_mb'; dumps = $result.dumps; as_expected = $ok
            }
            if (-not $ok) { ++$failures }
            Write-Host ("{0,-40} exit={1} {2} 合成した frame={3} 残った transition={4} 画素={5} dump={6} 期待どおり={7}" -f $name,
                $result.exit_code, (Get-Field $result.report 'verdict'), (Get-Field $main 'composed_frames'),
                (Get-Field $main 'live_after_export'), (Get-Field $main 'raster_live_bytes_after_export'), $result.dumps, $ok)
        }
    }
    # 画像の準備の失敗: 全画面の持ち方で上限を 50 MB にする。graph を作る前に失敗を返す。
    $result = Invoke-Harness 'cmp-layers-16' 'stress-prepare-failure' @('--math-group', 'auto', '--math-group-rule', '1',
        '--prepared-storage', 'full', '--math-group-budget-mb', '50') -Dump
    $main = Get-Field $result.report 'math_group_main'
    $ok = $result.exit_code -eq 6 -and "$(Get-Field $result.report 'export_error')" -match '上限' -and
    (Get-Field $result.report 'final_file_exists') -eq $false -and (Get-Field $main 'live_after_export') -eq 0 -and
    (Get-Field $main 'raster_live_bytes_after_export') -eq 0 -and $result.dumps -eq 0
    $summary.stress += [pscustomobject]@{ name = 'stress-prepare-failure'; fixture = 'cmp-layers-16'; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; export_error = Get-Field $result.report 'export_error'
        live_after_export = Get-Field $main 'live_after_export'
        raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'; dumps = $result.dumps; as_expected = $ok
    }
    if (-not $ok) { ++$failures }
    Write-Host ("stress-prepare-failure                   exit={0} {1} error='{2}' 画素={3} dump={4} 期待どおり={5}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $result.report 'export_error'), (Get-Field $main 'raster_live_bytes_after_export'),
        $result.dumps, $ok)
}

if (-not $SkipTiming) {
    # 事前に固定した順: 区間ごとに B, R, R, B。別々の process で、どれも最初の書き出しになる。
    foreach ($start in $Windows) {
        $order = @('B', 'R', 'R', 'B')
        for ($index = 0; $index -lt $order.Count; ++$index) {
            $kind = $order[$index]
            $extra = @('--range', "${start}:60")
            if ($kind -ne 'B') { $extra += @('--math-group', 'auto', '--math-group-rule', '1') }
            $result = Invoke-Harness 'v4-full' "timing-$start-$($index + 1)-$kind" $extra
            $report = $result.report
            $main = Get-Field $report 'math_group_main'
            $digestPath = Join-Path (Join-Path $runDirectory $result.name) 'v4-full-pre-yuv.tsv'
            $digest = $null
            if (Test-Path -LiteralPath $digestPath) {
                $lines = Get-Content -LiteralPath $digestPath | ForEach-Object { $c = $_ -split "`t"; "$($c[0]) $($c[2]) $($c[3])" }
                $digest = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-', '')
            }
            $row = [pscustomobject]([ordered]@{
                    name = $result.name; window = $start; position = $index + 1; kind = $kind; exit_code = $result.exit_code
                    verdict = Get-Field $report 'verdict'; output_digest = $digest
                    decision = Get-Field $main 'decision'; graph = Get-Field $main 'graph'
                    render_seconds = Get-Field $report 'main_render_seconds'
                    export_call_wall_seconds = Get-Field $report 'export_call_wall_seconds'
                    cpu_seconds = Get-Field $report 'cpu_seconds'
                    affine_transitions = Get-Field $report 'overlay_transitions'
                    encoder_submitted = Get-Field $report 'main_encoder_submitted'
                    muxed_packets = Get-Field $report 'main_muxed_packets'
                    peak_working_set_mb = Get-Field $report 'process_peak_working_set_mb'
                    raster_count = Get-Field $main 'raster_count'; raster_bytes = Get-Field $main 'raster_bytes'
                    raster_shared = Get-Field $main 'raster_shared'; raster_conflicts = Get-Field $main 'raster_conflicts'
                    prepare_ms = Get-Field $main 'prepare_ms'
                    grouped_clips = Get-Field $main 'grouped_clips'
                    compose_ms_per_frame = Get-Field $main 'compose_ms_per_frame'
                    copy_ms_per_frame = Get-Field $main 'copy_ms_per_frame'
                    callback_ms_per_frame = Get-Field $main 'callback_ms_per_frame'
                    errors = Get-Field $main 'errors'; live_after_export = Get-Field $main 'live_after_export'
                    raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
                })
            $summary.timing += $row
            if ($row.exit_code -ne 0 -or ($kind -eq 'R' -and $row.graph -ne 'math-group')) { ++$failures }
            Write-Host ("timing v4-full@{0} {1}{2} exit={3} graph={4} render={5:n1}s 書き出し全体={6:n1}s CPU={7:n1}s 画像={8:n2} MB ({9} 枚) 準備={10:n0} ms peak={11:n0} MB" -f `
                    $start, $kind, ($index + 1), $row.exit_code, $row.graph, $row.render_seconds, $row.export_call_wall_seconds, $row.cpu_seconds,
                ($row.raster_bytes / 1MB), $row.raster_count, $row.prepare_ms, $row.peak_working_set_mb)
        }
        $digests = @($summary.timing | Where-Object { $_.window -eq $start } | ForEach-Object { $_.output_digest } | Sort-Object -Unique)
        if ($digests.Count -ne 1 -or $null -eq $digests[0]) { Write-Host "  timing v4-full@$start : 経路ごとの出力の digest が一致しません"; ++$failures }
    }
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
exit 0
