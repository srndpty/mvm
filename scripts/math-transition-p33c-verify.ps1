<#
.SYNOPSIS
    P3-3C: 準備済みの画像を「中身のある範囲だけ」で持つ診断用の数式 group を、製品の MLT の pre-YUV と
    全 frame・全画素で比べる。全画面で持つ P3-3B の形も同じ条件で比べ、3 つの経路が一致することを見る。

.DESCRIPTION
    経路:
      B  製品の経路 (変更なし)
      F  数式 group、準備済みの画像は全画面 (--prepared-storage full。P3-3B の形)
      R  数式 group、準備済みの画像は中身のある範囲だけ (--prepared-storage roi)
    fixture ごとに、F と R をそれぞれ別の process で「B -> candidate -> B」の順に書き出して
    candidate を B と byte 単位で比べる (許容差なし)。2 つの process の B が同じであることは、
    B の全 frame の digest (…-pre-yuv.tsv) が一致することで確かめる。

    段階:
      1. 既存の 9 fixture (従来の layer と混ざる cmp-region-scale を含む)。
      2. 下地の alpha の対照 (alpha 1 以上)。
      3. Project 全体の graph (v4-full) の 4 区間。
      4. 対照 (R): frame の飛ばし・対応・layer の欠落 (画素の不一致)、下地の拒否 (失敗)、画像の上限 (F)、
         失敗時に出力 file が残らないこと、取消。
      5. 時間: Project 全体の graph の各区間を、別々の process で B, F, R, R, F, B の順に書き出す
         (事前に固定。どれもその process の最初の書き出しになる)。

    結果は run ごとの directory に残し、既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-transition-p33c-20261012',
    [string]$Label = 'verify',
    [string[]]$Fixtures = @('cmp-integer', 'cmp-fraction-fade', 'cmp-layers-16', 'cmp-layers-48',
        'cmp-region-motion', 'cmp-region-scale', 'p2-window-early', 'p2-window-middle', 'p2-window-late'),
    [string[]]$AlphaFixtures = @('alpha-255', 'alpha-254', 'alpha-253', 'alpha-128', 'alpha-1', 'alpha-all-nonzero'),
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
    (Join-Path $repo 'tests/core/test_math_group_compose.cpp'), (Join-Path $repo 'scripts/math-transition-p33c-verify.ps1'))
$identity = [ordered]@{ git_head = (git -C $repo rev-parse HEAD); sha256 = [ordered]@{} }
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$candidate = @('--compare', 'math-group', '--math-group-rule', '1')
$storages = [ordered]@{ F = 'full'; R = 'roi' }

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
            storage = Get-Field $compare 'candidate_storage'
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
    Write-Host ("{0,-24} exit={1} {2,-8} 一致={3}/{4} 未比較={5} group={6} まとめた={7} 画像={8} 枚 {9:n2} MB (走査 {10:n0} MB) 終了後の画素={11} 下地の最小 alpha={12} render 秒 B/C/B={13:n1}/{14:n1}/{15:n1} peak={16:n0} MB" -f `
            $label, $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.frames_uncompared,
        $row.groups, $row.grouped_clips, $row.raster_count, ($row.raster_bytes / 1MB), ($row.source_bytes_read / 1MB),
        $row.raster_live_bytes_after_export, $row.destination_minimum_alpha,
        $row.baseline_render_seconds, $row.candidate_render_seconds, $row.repeat_render_seconds, $row.peak_working_set_mb)
}

# 製品の経路 (B) の全 frame の digest。2 つの process の B が同じであることの確認に使う。
function Get-BaselineDigest([string]$Name, [string]$Fixture) {
    $path = Join-Path (Join-Path $runDirectory $Name) "$Fixture-pre-yuv.tsv"
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    # 列は frame・要求された回数・画素の digest・最小の alpha。回数は process ごとに変わりうるので除く。
    $lines = Get-Content -LiteralPath $path | ForEach-Object { $c = $_ -split "`t"; "$($c[0]) $($c[2]) $($c[3])" }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($bytes)).Replace('-', '')
}

$summary = [ordered]@{ fixtures = @(); alpha = @(); windows = @(); controls = @(); timing = @() }
$failures = 0

# 1 つの fixture を F と R で比べ、3 つの経路が一致したかを返す。
function Invoke-ThreePath([string]$Fixture, [string]$Name, [string[]]$Extra, [string]$Section) {
    $digests = @{}
    $ok = $true
    foreach ($key in $storages.Keys) {
        $runName = "$Name-$key"
        $row = ConvertTo-Row (Invoke-Harness $Fixture $runName ($candidate + @('--prepared-storage', $storages[$key]) + $Extra))
        $digests[$key] = Get-BaselineDigest $runName $Fixture
        $row | Add-Member -NotePropertyName path -NotePropertyValue $key
        $row | Add-Member -NotePropertyName baseline_digest -NotePropertyValue $digests[$key]
        $script:summary[$Section] += $row
        Write-Row $row $runName
        if (-not (Test-Exact $row)) { $ok = $false }
    }
    # 2 つの process の製品の経路が同じ画素を出していること (3 つの経路を 1 つの基準へつなぐ)。
    $same = $null -ne $digests['F'] -and $digests['F'] -eq $digests['R']
    if (-not $same) { Write-Host "  $Name : 2 つの process の製品の経路の digest が一致しません"; $ok = $false }
    if (-not $ok) { $script:failures++ }
}

if (-not $SkipFixtures) { foreach ($fixture in $Fixtures) { Invoke-ThreePath $fixture $fixture @() 'fixtures' } }
if (-not $SkipAlpha) { foreach ($fixture in $AlphaFixtures) { Invoke-ThreePath $fixture $fixture @() 'alpha' } }
if (-not $SkipWindows) {
    foreach ($start in $Windows) { Invoke-ThreePath 'v4-full' "full-graph-$start" @('--range', "${start}:60") 'windows' }
}

if (-not $SkipControls) {
    $fixture = 'cmp-layers-16'
    $roi = $candidate + @('--prepared-storage', 'roi')
    foreach ($fault in @(1, 2, 3, 5, 6)) {
        $name = "control-fault-$fault"
        $row = ConvertTo-Row (Invoke-Harness $fixture $name ($roi + @('--math-group-fault', "$fault")))
        $detected = $row.exit_code -in @(13, 14) -and $row.verdict -in @('WITHIN_ONE_NOT_EXACT', 'DIFFERS_BEYOND_ONE') -and
        $row.frames_uncompared -eq 0 -and $row.group_clean -eq $true -and $row.frames_exact -lt $row.expected_frames
        $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
        $summary.controls += $row
        if (-not $detected) { ++$failures }
        Write-Host ("{0,-32} exit={1} {2} 一致={3}/{4} 画素差={5} 検出={6}" -f $name, $row.exit_code, $row.verdict,
            $row.frames_exact, $row.expected_frames, $row.differing_pixels, $detected)
    }
    $row = ConvertTo-Row (Invoke-Harness $fixture 'control-destination-rejected' ($roi + @('--math-group-fault', '4')))
    $detected = $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
    "$($row.candidate_error)" -match '合成器が拒否' -and $row.live_after_export -eq 0 -and $row.active_after_export -eq 0 -and
    $row.raster_live_bytes_after_export -eq 0
    $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
    $summary.controls += $row
    if (-not $detected) { ++$failures }
    Write-Host ("control-destination-rejected     exit={0} {1} errors={2} 終了後の画素={3} error='{4}' 検出={5}" -f $row.exit_code,
        $row.verdict, $row.errors, $row.raster_live_bytes_after_export, $row.candidate_error, $detected)
    # 画像の上限: 全画面の持ち方で上限を 50 MB にすると、準備の途中で止まる (黙って増やさない)。
    # 範囲だけの持ち方は 16 層で 1 MB に満たず、MB 単位の上限では止められないので、ここでは試さない。
    $row = ConvertTo-Row (Invoke-Harness $fixture 'control-raster-budget-full' ($candidate + @('--prepared-storage', 'full', '--math-group-budget-mb', '50')))
    $detected = $row.exit_code -eq 15 -and "$($row.candidate_error)" -match '上限' -and $row.raster_live_bytes_after_export -eq 0
    $row | Add-Member -NotePropertyName detected -NotePropertyValue $detected
    $summary.controls += $row
    if (-not $detected) { ++$failures }
    Write-Host ("control-raster-budget-full       exit={0} {1} error='{2}' 検出={3}" -f $row.exit_code, $row.verdict, $row.candidate_error, $detected)
    $result = Invoke-Harness $fixture 'control-failure-not-published' @('--math-group', '1', '--math-group-rule', '1', '--prepared-storage', 'roi', '--math-group-fault', '4')
    $main = Get-Field $result.report 'math_group_main'
    $detected = $result.exit_code -eq 6 -and (Get-Field $result.report 'export_success') -eq $false -and
    (Get-Field $result.report 'final_file_exists') -eq $false -and (Get-Field $result.report 'temporary_file_exists') -eq $false -and
    (Get-Field $main 'errors') -gt 0 -and (Get-Field $main 'live_after_export') -eq 0 -and (Get-Field $main 'active_after_export') -eq 0 -and
    (Get-Field $main 'raster_live_bytes_after_export') -eq 0
    $summary.controls += [pscustomobject]@{ name = 'control-failure-not-published'; fixture = $fixture; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; export_error = Get-Field $result.report 'export_error'
        final_file_exists = Get-Field $result.report 'final_file_exists'; temporary_file_exists = Get-Field $result.report 'temporary_file_exists'
        errors = Get-Field $main 'errors'; live_after_export = Get-Field $main 'live_after_export'
        raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'; detected = $detected
    }
    if (-not $detected) { ++$failures }
    Write-Host ("control-failure-not-published    exit={0} {1} 出力 file={2} 一時 file={3} 終了後の画素={4} 検出={5}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $result.report 'final_file_exists'),
        (Get-Field $result.report 'temporary_file_exists'), (Get-Field $main 'raster_live_bytes_after_export'), $detected)
    $result = Invoke-Harness $fixture 'control-cancel' @('--math-group', '1', '--math-group-rule', '1', '--prepared-storage', 'roi', '--inject', 'cancel')
    $main = Get-Field $result.report 'math_group_main'
    $detected = $result.exit_code -eq 11 -and (Get-Field $main 'grouped_clips') -gt 0 -and
    (Get-Field $main 'live_after_export') -eq 0 -and (Get-Field $main 'active_after_export') -eq 0 -and
    (Get-Field $main 'raster_live_bytes_after_export') -eq 0 -and (Get-Field $result.report 'final_file_exists') -eq $false
    $summary.controls += [pscustomobject]@{ name = 'control-cancel'; fixture = $fixture; exit_code = $result.exit_code
        verdict = Get-Field $result.report 'verdict'; grouped_clips = Get-Field $main 'grouped_clips'
        composed_frames = Get-Field $main 'composed_frames'; live_after_export = Get-Field $main 'live_after_export'
        active_after_export = Get-Field $main 'active_after_export'
        raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
        working_set_after_export_mb = Get-Field $main 'working_set_after_export_mb'
        final_file_exists = Get-Field $result.report 'final_file_exists'; detected = $detected
    }
    if (-not $detected) { ++$failures }
    Write-Host ("control-cancel                   exit={0} {1} 合成した frame={2} 残った transition={3} 終了後の画素={4} 検出={5}" -f $result.exit_code,
        (Get-Field $result.report 'verdict'), (Get-Field $main 'composed_frames'), (Get-Field $main 'live_after_export'),
        (Get-Field $main 'raster_live_bytes_after_export'), $detected)
}

if (-not $SkipTiming) {
    # 事前に固定した順: 区間ごとに B, F, R, R, F, B。別々の process で、どれも最初の書き出しになる。
    foreach ($start in $Windows) {
        $order = @('B', 'F', 'R', 'R', 'F', 'B')
        for ($index = 0; $index -lt $order.Count; ++$index) {
            $kind = $order[$index]
            $extra = @('--range', "${start}:60")
            if ($kind -ne 'B') { $extra += @('--math-group', '1', '--math-group-rule', '1', '--prepared-storage', $storages[$kind]) }
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
                    muxed_packets = Get-Field $report 'main_muxed_packets'
                    lowest_alpha = Get-Field $report 'pre_yuv_lowest_alpha'
                    baseline_digest = Get-BaselineDigest $result.name 'v4-full'
                    peak_working_set_mb = Get-Field $report 'process_peak_working_set_mb'
                    working_set_before_prepare_mb = Get-Field $main 'working_set_before_prepare_mb'
                    working_set_after_prepare_mb = Get-Field $main 'working_set_after_prepare_mb'
                    working_set_after_export_mb = Get-Field $main 'working_set_after_export_mb'
                    raster_count = Get-Field $main 'raster_count'; raster_bytes = Get-Field $main 'raster_bytes'
                    source_bytes_read = Get-Field $main 'source_bytes_read'
                    raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
                    prepare_ms = Get-Field $main 'prepare_ms'
                    groups = Get-Field $main 'transitions'; grouped_clips = Get-Field $main 'grouped_clips'
                    composed_frames = Get-Field $main 'composed_frames'
                    copy_ms_per_frame = Get-Field $main 'copy_ms_per_frame'
                    compose_ms_per_frame = Get-Field $main 'compose_ms_per_frame'
                    callback_ms_per_frame = Get-Field $main 'callback_ms_per_frame'
                    callback_seconds_total = Get-Field $main 'callback_seconds_total'
                    errors = Get-Field $main 'errors'; live_after_export = Get-Field $main 'live_after_export'
                })
            $summary.timing += $row
            if ($row.exit_code -ne 0) { ++$failures }
            Write-Host ("timing v4-full@{0} {1}{2} exit={3} render={4:n1}s 書き出し全体={5:n1}s CPU={6:n1}s 画像={7:n2} MB 準備={8:n0} ms 合成={9:n2} ms/frame 複製={10:n2} ms/frame peak={11:n0} MB digest={12}" -f `
                    $start, $kind, ($index + 1), $row.exit_code, $row.render_seconds, $row.export_call_wall_seconds, $row.cpu_seconds,
                ($row.raster_bytes / 1MB), $row.prepare_ms, $row.compose_ms_per_frame, $row.copy_ms_per_frame, $row.peak_working_set_mb,
                "$($row.baseline_digest)".Substring(0, [Math]::Min(8, "$($row.baseline_digest)".Length)))
        }
        # 3 つの経路の全 frame の digest が同じであること (別々の process の出力どうし)。
        $digests = @($summary.timing | Where-Object { $_.window -eq $start } | ForEach-Object { $_.baseline_digest } | Sort-Object -Unique)
        if ($digests.Count -ne 1 -or $null -eq $digests[0]) { Write-Host "  timing v4-full@$start : 経路ごとの出力の digest が一致しません"; ++$failures }
    }
}

$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
Write-Host "結果: $runDirectory"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
exit 0
