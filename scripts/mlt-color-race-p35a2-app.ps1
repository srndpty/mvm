<#
.SYNOPSIS
    P3-5A.2: V1 に隙間のある fixture を、上位の層を通した書き出し (検証 harness) で確かめる。

.DESCRIPTION
    mvm_math_score_export_parity.exe を使う。build はしない。
      parity  base-gap (V1 に clip が無い)・base-half-gap (前半だけ clip) を、同じ process で
              製品の経路 → 判定つきの数式 group → 製品の経路 の順に書き出し、全 frame の RGBA が一致すること、
              背景の検査が違反 0 であること。-Before に修正の前の run (P3-5A の color の段階) を渡すと、
              製品の経路の frame ごとの digest が修正の前と同じかも調べる。
      fault   背景の cache を作り直させる故障 (従来の graph と数式 group の graph)。書き出しが失敗し、
              最終 file も一時 file も残らないこと。
#>
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/mlt-color-race-p35a2-20261013',
    [string]$Before = 'build/math-export-p35a-20261013/20261013-030712-color',
    [int]$Repeats = 3
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
$run = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-app')
New-Item -ItemType Directory -Force $run | Out-Null
"$((Get-FileHash -Algorithm SHA256 -LiteralPath $harness).Hash)  $harness" | Set-Content (Join-Path $run 'identity.txt') -Encoding utf8
$failures = 0
$rows = @()

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

# frame と画素の digest の列だけを比べる (呼ばれた回数は比べない)。
function Get-DigestText([string]$Path) {
    (Get-Content -LiteralPath $Path | ForEach-Object { $c = $_ -split "`t"; "$($c[0]) $($c[2]) $($c[3])" }) -join "`n"
}

foreach ($fixture in @('base-gap', 'base-half-gap')) {
    for ($index = 1; $index -le $Repeats; ++$index) {
        $result = Invoke-Harness "$fixture-$index" $fixture @('--compare', 'math-group-auto', '--math-group-rule', '1')
        $compare = $result.report.compare
        $background = $result.report.v1_background
        $sameAsBefore = $null
        $beforeTsv = Join-Path (Join-Path $repo $Before) "$fixture-1\$fixture-pre-yuv.tsv"
        $afterTsv = Join-Path $result.out "$fixture-pre-yuv.tsv"
        if ((Test-Path -LiteralPath $beforeTsv) -and (Test-Path -LiteralPath $afterTsv)) {
            $sameAsBefore = (Get-DigestText $beforeTsv) -eq (Get-DigestText $afterTsv)
        }
        $ok = $result.exit_code -eq 0 -and $compare.verdict -eq 'EXACT' -and $compare.frames_exact -eq $result.report.expected_frames -and
        $compare.frames_uncompared -eq 0 -and $compare.baseline_repeat_mismatches -eq 0 -and $compare.candidate_group.errors -eq 0 -and
        $compare.candidate_group.graph -eq 'math-group' -and $background.violations -eq 0 -and $background.checked_requests -gt 0 -and
        $result.report.pre_yuv_missing_count -eq 0 -and $sameAsBefore -eq $true
        $rows += [pscustomobject]@{ name = "$fixture-$index"; kind = 'parity'; exit_code = $result.exit_code; verdict = $compare.verdict
            frames_exact = $compare.frames_exact; expected_frames = $result.report.expected_frames
            baseline_repeat_mismatches = $compare.baseline_repeat_mismatches; graph = $compare.candidate_group.graph
            grouped_clips = $compare.candidate_group.grouped_clips; group_errors = $compare.candidate_group.errors
            background_checked = $background.checked_requests; background_violations = $background.violations
            conventional_digest_same_as_before_fix = $sameAsBefore; ok = $ok
        }
        if (-not $ok) { ++$failures }
        Write-Host ("{0}: exit={1} {2} 一致={3}/{4} 背景の検査={5} 違反={6} 修正の前と同じ digest={7} ok={8}" -f "$fixture-$index",
            $result.exit_code, $compare.verdict, $compare.frames_exact, $result.report.expected_frames, $background.checked_requests,
            $background.violations, $sameAsBefore, $ok)
    }
}

foreach ($case in @(
        @{ name = 'fault-conventional'; extra = @('--v1-background-fault', '2') },
        @{ name = 'fault-math-group'; extra = @('--math-group', 'auto', '--math-group-rule', '1', '--v1-background-fault', '2') })) {
    $result = Invoke-Harness $case.name 'base-gap' $case.extra
    $report = $result.report
    $left = @(Get-ChildItem $result.out -File | Where-Object { $_.Name -match '\.(mp4|mkv|mvmtmp|tmp|part)' })
    $ok = $result.exit_code -ne 0 -and $report.export_success -eq $false -and "$($report.export_error)" -match '作り直されました' -and
    $report.final_file_exists -eq $false -and $report.temporary_file_exists -eq $false -and $left.Count -eq 0 -and
    $report.v1_background.violations -gt 0
    $rows += [pscustomobject]@{ name = $case.name; kind = 'fault'; exit_code = $result.exit_code; export_error = $report.export_error
        final_file_exists = $report.final_file_exists; temporary_file_exists = $report.temporary_file_exists
        media_files_left = @($left | ForEach-Object { $_.Name }); background_violations = $report.v1_background.violations; ok = $ok
    }
    if (-not $ok) { ++$failures }
    Write-Host ("{0}: exit={1} error='{2}' 最終 file={3} 一時 file={4} 残った file={5} ok={6}" -f $case.name, $result.exit_code,
        $report.export_error, $report.final_file_exists, $report.temporary_file_exists, $left.Count, $ok)
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
Write-Host "結果: $run"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
