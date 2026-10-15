<#
.SYNOPSIS
    P3-6A: 製品の入口 (MvmController の内部の選択 → 書き出しの要求 → mvm::app::exportTimeline) から、
    数式 group の graph を選べること、既定 (off) では従来の graph のままであることを確かめる。

.DESCRIPTION
    検証 harness (mvm_math_score_export_parity.exe) を使う。harness は、製品の MvmController に
    書き出しをさせ、controller が作った要求をそのまま書き出しの関数へ渡す。
      - candidate (--compare product-math-group / product-off): controller の内部の選択を on / off にして
        書き出す。observer は渡さない (製品の要求と同じ)。同じ process で先に書き出した従来の経路
        (選択 off) と、encoder 直前の RGBA を全 frame 比べる。
      - 「observer あり」の場合 (--product-observer diagnostic): 製品の選択に、診断用の observer
        (故障の注入、証拠の差し替え) を組み合わせる。製品の要求ではなく、対照のための形である。
      - 主の書き出し (--product-math-group on): 取消・timeout・失敗・公開の確認。
    期待は実行の前にこの script に書いてある。build はしない。source を書き換えない。
#>
param(
    [string]$Label = 'verify',
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-group-product-p36a-20261014'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$harness = Join-Path $repo 'build\ucrt64-release\bin\mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$run = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null

$hashed = @($harness, (Join-Path $repo 'build\ucrt64-release\bin\mvm.exe'), (Join-Path $ucrt 'libmlt-7.dll'),
    'C:\msys64\ucrt64\lib\mlt\libmltcore.dll', 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll', 'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll', (Join-Path $ucrt 'Qt6Gui.dll'), (Join-Path $ucrt 'libx264-165.dll'), $project,
    (Join-Path $repo 'src/app/timeline_export.cpp'), (Join-Path $repo 'src/app/timeline_export.h'),
    (Join-Path $repo 'apps/mvm/mvm_controller_export.cpp'), (Join-Path $repo 'apps/mvm/mvm_controller.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_math_group.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_v1_background.c'),
    (Join-Path $repo 'src/media/math/math_group_compose.cpp'), (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'),
    (Join-Path $repo 'scripts/math-group-product-p36a-verify.ps1'))
$identity = [ordered]@{ label = $Label; git_head = (git -C $repo rev-parse HEAD); git_status = @(git -C $repo status --porcelain); sha256 = [ordered]@{} }
foreach ($file in $hashed) { $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash }
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'identity.json') -Encoding utf8
(git -C $repo diff HEAD) | Set-Content -LiteralPath (Join-Path $run 'git-diff.patch') -Encoding utf8

function Invoke-Harness([string]$Name, [string]$Fixture, [string[]]$Extra) {
    $out = Join-Path $run $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = @('--project', $project, '--manim', $manim, '--out', $out, '--ffprobe', (Join-Path $ucrt 'ffprobe.exe'),
        '--fixture', $Fixture, '--cap-seconds', '900') + $Extra
    & $harness @arguments 2> (Join-Path $out 'harness.log') | Out-Null
    $reportPath = Join-Path $out "$Fixture-report.json"
    [pscustomobject]@{ out = $out; exit_code = $LASTEXITCODE
        report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
    }
}

$on = @('--compare', 'product-math-group')
$off = @('--compare', 'product-off')
$observed = @('--product-observer', 'diagnostic')
# 区間の指定 (--range) は observer の診断用の入力なので、v4 の全体 graph の区間は「observer あり」で書き出す
# (observer が無いと、区間が無視されて Project 全体を書き出す)。
# candidate の場合 (実行の前に固定)。
#   expect: exact = 全 frame が従来の経路と一致、rejected = 実行時の拒否で書き出しが失敗、
#           differs = 画素の不一致を検出 (対照)
#   decision が '' なら、判定そのものをしない (選択 off)。
$cases = @(
    @{ name = 'off-layers-16'; fixture = 'cmp-layers-16'; extra = $off; requested = 'off'; decision = ''; reason = ''; graph = 'conventional'; expect = 'exact' },
    @{ name = 'off-v4-window-1200'; fixture = 'v4-full'; extra = $off + $observed + @('--range', '1200:60'); requested = 'off'; decision = ''; reason = ''; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-eligible-layers-16'; fixture = 'cmp-layers-16'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-eligible-layers-48'; fixture = 'cmp-layers-48'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-eligible-region-motion'; fixture = 'cmp-region-motion'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-mixed-geometry'; fixture = 'geom-mixed'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; affine = $true },
    @{ name = 'on-mixed-region-scale'; fixture = 'cmp-region-scale'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; affine = $true },
    @{ name = 'on-v1-opaque-still'; fixture = 'alpha-255'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-v1-alpha-1'; fixture = 'alpha-1'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-v1-gap-only'; fixture = 'base-gap'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-v1-half-gap'; fixture = 'base-half-gap'; extra = $on; requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact' },
    @{ name = 'on-v1-alpha-zero'; fixture = 'alpha-0'; extra = $on; requested = 'auto'; decision = 'INELIGIBLE'; reason = 'base-alpha-zero'; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-no-eligible-clip'; fixture = 'geom-all-unsupported'; extra = $on; requested = 'auto'; decision = 'INELIGIBLE'; reason = 'no-eligible-math-clip'; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-v1-dynamic-effect'; fixture = 'base-transformed'; extra = $on; requested = 'auto'; decision = 'UNKNOWN'; reason = 'base-clip-transformed'; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-evidence-missing'; fixture = 'cmp-layers-16'; extra = $on + $observed + @('--base-alpha-proof', 'none'); requested = 'auto'; decision = 'UNKNOWN'; reason = 'base-alpha-unproven'; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-evidence-out-of-range'; fixture = 'cmp-layers-16'; extra = $on + $observed + @('--base-alpha-proof', 'forge:999'); requested = 'auto'; decision = 'UNKNOWN'; reason = 'base-alpha-unproven'; graph = 'conventional'; expect = 'exact' },
    @{ name = 'on-evidence-false-opaque'; fixture = 'alpha-0'; extra = $on + $observed + @('--base-alpha-proof', 'forge:256'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'rejected' },
    @{ name = 'on-runtime-rejection'; fixture = 'cmp-layers-16'; extra = $on + $observed + @('--math-group-fault', '4'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'rejected' },
    @{ name = 'on-artifact-identity-mismatch'; fixture = 'cmp-layers-16'; extra = $on + $observed + @('--math-group-fault', '5'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'differs' },
    @{ name = 'on-repeated-3'; fixture = 'cmp-layers-16'; extra = $on + @('--candidate-repeats', '3'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; repeats = 3 },
    @{ name = 'on-v4-window-1200'; fixture = 'v4-full'; extra = $on + $observed + @('--range', '1200:60'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; affine = $true },
    @{ name = 'on-v4-window-5280'; fixture = 'v4-full'; extra = $on + $observed + @('--range', '5280:60'); requested = 'auto'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; affine = $true }
)
$rows = @()
$failures = 0
foreach ($case in $cases) {
    $result = Invoke-Harness $case.name $case.fixture $case.extra
    $compare = $result.report.compare
    $product = $result.report.product_selection
    $recorded = $product.candidate_report
    $baselineAffine = $product.main_report.affine_transitions
    $observerExpected = $case.extra -contains 'diagnostic'
    $repeats = if ($case.ContainsKey('repeats')) { $case.repeats } else { 1 }
    $row = [ordered]@{
        name = $case.name; fixture = $case.fixture; exit_code = $result.exit_code; verdict = $compare.verdict
        frames_exact = $compare.frames_exact; expected_frames = $result.report.expected_frames; frames_uncompared = $compare.frames_uncompared
        baseline_repeat_mismatches = $compare.baseline_repeat_mismatches; group_clean = $compare.group_clean
        requested = $recorded.requested; request_mode_seen = $product.candidate_request_mode_seen
        controller_supplied_observer = $product.candidate_controller_supplied_observer; observer_attached = $product.candidate_observer_attached
        decision = $recorded.decision; reason = $recorded.reason; graph = $recorded.graph; groups = $recorded.groups
        grouped_clips = $recorded.grouped_clips; retained_clips = $recorded.retained_clips; affine_transitions = $recorded.affine_transitions
        baseline_affine_transitions = $baselineAffine; composed_frames = $recorded.composed_frames; errors = $recorded.errors
        live_transitions_after = $recorded.live_transitions_after; active_callbacks_after = $recorded.active_callbacks_after
        raster_live_bytes_after = $compare.candidate_raster_live_bytes_after_export; candidate_exports = $compare.candidate_exports
        candidate_error = $compare.candidate_error; candidate_output_sha256 = $product.candidate_output_sha256; expect = $case.expect
    }
    # どの場合も: controller が要求へ入れた選択、observer の有無、判定・理由・graph、残りが 0。
    $common = $row.requested -eq $case.requested -and $row.request_mode_seen -eq $(if ($case.requested -eq 'auto') { 1 } else { 0 }) -and
    $row.controller_supplied_observer -eq $false -and $row.observer_attached -eq $observerExpected -and
    $row.decision -eq $case.decision -and $row.reason -eq $case.reason -and $row.graph -eq $case.graph -and
    $row.live_transitions_after -eq 0 -and $row.active_callbacks_after -eq 0 -and $row.raster_live_bytes_after -eq 0 -and
    $row.candidate_exports -eq $repeats
    $ok = switch ($case.expect) {
        'exact' {
            $common -and $row.exit_code -eq 0 -and $row.verdict -eq 'EXACT' -and $row.frames_exact -eq $row.expected_frames -and
            $row.frames_uncompared -eq 0 -and $row.baseline_repeat_mismatches -eq 0 -and $row.group_clean -eq $true -and $row.errors -eq 0 -and
            $(if ($case.graph -eq 'math-group') {
                    # 数式 group を選んだなら、実際に合成していること (選んだと記録しただけではない)。
                    $row.grouped_clips -gt 0 -and $row.composed_frames -gt 0 -and $row.groups -gt 0 -and
                    $row.affine_transitions -lt $baselineAffine * $repeats -and
                    (-not $case.ContainsKey('affine') -or $row.affine_transitions -gt 0)
                } else {
                    # 従来の graph なら、group を 1 つも使わず、affine の数が従来と同じこと。
                    $row.grouped_clips -eq 0 -and $row.composed_frames -eq 0 -and $row.groups -eq 0 -and
                    $row.affine_transitions -eq $baselineAffine * $repeats
                })
        }
        'rejected' {
            $common -and $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
            "$($row.candidate_error)" -match '合成器が拒否' -and -not $row.candidate_output_sha256
        }
        'differs' {
            $common -and $row.exit_code -in @(13, 14) -and $row.verdict -in @('WITHIN_ONE_NOT_EXACT', 'DIFFERS_BEYOND_ONE') -and
            $row.frames_exact -lt $row.expected_frames -and $row.frames_uncompared -eq 0
        }
    }
    $row.as_expected = [bool]$ok
    $rows += [pscustomobject]$row
    if (-not $ok) { ++$failures }
    Write-Host ("{0,-32} exit={1} {2,-16} 一致={3}/{4} 要求={5} 判定={6} ({7}) graph={8} まとめた={9} affine={10}/{11} 合成={12} 期待どおり={13}" -f `
            $case.name, $row.exit_code, $row.verdict, $row.frames_exact, $row.expected_frames, $row.requested, $row.decision, $row.reason,
        $row.graph, $row.grouped_clips, $row.affine_transitions, $baselineAffine, $row.composed_frames, [bool]$ok)
}

# 同じ fixture で、選択 off と on の MP4 が同じであること (observer なしの製品の要求どうし)。
$pairs = @(@('off-layers-16', 'on-eligible-layers-16'), @('off-v4-window-1200', 'on-v4-window-1200'))
$outputs = @()
foreach ($pair in $pairs) {
    $a = @($rows | Where-Object { $_.name -eq $pair[0] })[0]; $b = @($rows | Where-Object { $_.name -eq $pair[1] })[0]
    $same = $a.candidate_output_sha256 -and $a.candidate_output_sha256 -eq $b.candidate_output_sha256
    $outputs += [pscustomobject]@{ off = $pair[0]; on = $pair[1]; off_sha256 = $a.candidate_output_sha256; on_sha256 = $b.candidate_output_sha256; same = [bool]$same }
    Write-Host ("MP4 {0} 対 {1}: 同じ={2}" -f $pair[0], $pair[1], [bool]$same)
    if (-not $same) { ++$failures }
}

# 主の書き出し (製品の選択 on)。取消・timeout・実行時の拒否・公開。
$mainCases = @(
    @{ name = 'main-on-publish'; extra = @('--product-math-group', 'on'); exit = 0; success = $true; error = '' },
    @{ name = 'main-on-cancel'; extra = @('--product-math-group', 'on', '--inject', 'cancel'); exit = 11; success = $false; error = 'キャンセル' },
    @{ name = 'main-on-timeout'; extra = @('--product-math-group', 'on', '--request-timeout-ms', '300'); exit = 6; success = $false; error = 'timeout' },
    @{ name = 'main-on-runtime-rejection'; extra = @('--product-math-group', 'on', '--math-group-fault', '4'); exit = 6; success = $false; error = '合成器が拒否' },
    @{ name = 'main-off-publish'; extra = @('--product-math-group', 'off'); exit = 0; success = $true; error = '' }
)
$mainRows = @()
foreach ($case in $mainCases) {
    $result = Invoke-Harness $case.name 'cmp-layers-16' $case.extra
    $report = $result.report
    $product = $report.product_selection
    $recorded = $product.main_report
    $selected = $case.extra -contains 'on'
    $left = @(Get-ChildItem $result.out -File | Where-Object { $_.Name -match '\.(mp4|mkv|mvmtmp|tmp|part)' } | ForEach-Object { $_.Name })
    $ok = $result.exit_code -eq $case.exit -and $report.export_success -eq $case.success -and $report.final_file_exists -eq $case.success -and
    $report.temporary_file_exists -eq $false -and ($case.success -or $left.Count -eq 0) -and
    ($case.success -or "$($report.export_error)" -match $case.error) -and
    $product.main_request_mode_seen -eq $(if ($selected) { 1 } else { 0 }) -and $product.main_controller_supplied_observer -eq $false -and
    $recorded.requested -eq $(if ($selected) { 'auto' } else { 'off' }) -and $recorded.live_transitions_after -eq 0 -and $recorded.active_callbacks_after -eq 0 -and
    $report.math_group_main.raster_live_bytes_after_export -eq 0 -and
    $(if ($selected) { $recorded.decision -eq 'ELIGIBLE' -and $recorded.graph -eq 'math-group' } else { $recorded.decision -eq '' -and $recorded.graph -eq 'conventional' }) -and
    (-not ($case.success -and $selected) -or $recorded.composed_frames -gt 0) -and
    $report.request_timeout_is_product -eq (-not ($case.extra -contains '--request-timeout-ms'))
    $mainRows += [pscustomobject]@{ name = $case.name; exit_code = $result.exit_code; verdict = $report.verdict; export_error = $report.export_error
        final_file_exists = $report.final_file_exists; temporary_file_exists = $report.temporary_file_exists; media_files_left = $left
        requested = $recorded.requested; decision = $recorded.decision; graph = $recorded.graph; composed_frames = $recorded.composed_frames
        errors = $recorded.errors; live_transitions_after = $recorded.live_transitions_after; active_callbacks_after = $recorded.active_callbacks_after
        raster_live_bytes_after = $report.math_group_main.raster_live_bytes_after_export; request_timeout_ms = $report.request_timeout_ms
        controller_last_report_graph = $product.controller_last_report.graph; as_expected = [bool]$ok
    }
    if (-not $ok) { ++$failures }
    Write-Host ("{0,-28} exit={1} {2} error='{3}' 最終 file={4} 一時 file={5} 判定={6} graph={7} 合成={8} 期待どおり={9}" -f $case.name,
        $result.exit_code, $report.verdict, $report.export_error, $report.final_file_exists, $report.temporary_file_exists,
        $recorded.decision, $recorded.graph, $recorded.composed_frames, [bool]$ok)
}
[ordered]@{ candidates = $rows; outputs = $outputs; main = $mainRows } | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
Write-Host "結果: $run"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
