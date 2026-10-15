<#
.SYNOPSIS
    P3-5B: 書き出しの timeout を実時間で数えることの確認。

.DESCRIPTION
    build はしない。source を書き換えない。
      1. mvm_test_mlt_export_timeout.exe (実物の待ち loop) を -Repeats 回。全部成立すること。
         1 回ごとに、scenario ごとの「待ちの開始から timeout を見つけるまで」の時間を記録する。
      2. 以前の数え方の記録 (mvm_test_mlt_export_capacity.exe timeout-accounting、
         mvm_test_mlt_export_timeout.exe legacy)。どちらも終了コード 3 (実時間で 1.5 倍より遅い)。
      3. 上位の層を通した書き出し (検証 harness) で、timeout・取消のときに最終 file・一時 file が
         残らないこと、数式 group の transition・callback・画像が残らないこと。
#>
param(
    [string]$Label = 'verify',
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/mlt-export-timeout-p35b-20261013',
    [int]$Repeats = 5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$bin = Join-Path $repo 'build\ucrt64-release\bin'
$test = Join-Path $bin 'mvm_test_mlt_export_timeout.exe'
$capacity = Join-Path $bin 'mvm_test_mlt_export_capacity.exe'
$harness = Join-Path $bin 'mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$run = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null

$hashed = @($test, $capacity, $harness, (Join-Path $ucrt 'libmlt-7.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltcore.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll', (Join-Path $ucrt 'libx264-165.dll'),
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'tests/harness/test_mlt_export_timeout.cpp'), (Join-Path $repo 'tests/harness/test_mlt_export_capacity.cpp'),
    (Join-Path $repo 'tests/assets/smoke/v1080p60_h264.mp4'), (Join-Path $repo 'scripts/mlt-export-timeout-p35b-verify.ps1'))
$identity = [ordered]@{ label = $Label; git_head = (git -C $repo rev-parse HEAD); git_status = @(git -C $repo status --porcelain); sha256 = [ordered]@{} }
foreach ($file in $hashed) { $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash }
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'identity.json') -Encoding utf8
(git -C $repo diff HEAD) | Set-Content -LiteralPath (Join-Path $run 'git-diff.patch') -Encoding utf8

$failures = 0
$summary = [ordered]@{ contract = @(); legacy = @(); application = @() }

# 1. 契約の検査 (くり返し)。
for ($index = 1; $index -le $Repeats; ++$index) {
    $log = Join-Path $run "contract-$index.log"
    & $test *> $log
    $code = $LASTEXITCODE
    $rows = @(Get-Content -LiteralPath $log -Encoding utf8 | ForEach-Object {
            $m = [regex]::Match($_, '^(\S+)\s+timeout=(\d+) ms status=(-?\d+) 待ちの開始から検出まで=(-?\d+) ms 待ち=(-?\d+) ms 全体=(\d+) ms')
            if ($m.Success) {
                [pscustomobject]@{ scenario = $m.Groups[1].Value; timeout_ms = [int]$m.Groups[2].Value; status = [int]$m.Groups[3].Value
                    detected_after_ms = [int]$m.Groups[4].Value; waited_ms = [int]$m.Groups[5].Value; total_ms = [int]$m.Groups[6].Value }
            } })
    $fails = @(Get-Content -LiteralPath $log -Encoding utf8 | Where-Object { $_ -match '^FAIL' })
    $summary.contract += [pscustomobject]@{ index = $index; exit_code = $code; failed_checks = $fails; scenarios = $rows }
    # 1 秒の timeout を見つけるまでの時間 (遅い進行 callback・止められた thread の場合を除く)。
    $plain = @($rows | Where-Object { $_.timeout_ms -eq 1000 -and $_.status -eq 1 -and
            $_.scenario -in @('slow-consumer', 'poll-30ms', 'slow-consumer-8-workers', 'sequence-poll-30ms', 'repeat-timeout') } |
        ForEach-Object { $_.detected_after_ms })
    Write-Host ("契約 #{0}: exit={1} 不成立={2} 1 秒の timeout の検出 {3}〜{4} ms ({5} 件)" -f $index, $code, $fails.Count,
        ($plain | Measure-Object -Minimum).Minimum, ($plain | Measure-Object -Maximum).Maximum, $plain.Count)
    if ($code -ne 0) { ++$failures }
}

# 2. 以前の数え方の記録 (終了コード 3 = 実時間で 1.5 倍より遅い)。
foreach ($case in @(@{ name = 'historical-capacity-timeout-accounting'; exe = $capacity; args = @('timeout-accounting') },
        @{ name = 'legacy-mode'; exe = $test; args = @('legacy') })) {
    $log = Join-Path $run "$($case.name).log"
    & $case.exe @($case.args) *> $log
    $code = $LASTEXITCODE
    $line = (Get-Content -LiteralPath $log -Encoding utf8 | Where-Object { $_ -match 'timeout' } | Select-Object -Last 1)
    $summary.legacy += [pscustomobject]@{ name = $case.name; exit_code = $code; line = "$line" }
    Write-Host ("{0}: exit={1} (期待 3) {2}" -f $case.name, $code, $line)
    if ($code -ne 3) { ++$failures }
}

# 3. 上位の層: timeout・取消のときに出力を残さない。
$cases = @(
    @{ name = 'timeout-conventional'; extra = @('--request-timeout-ms', '1000'); exit = 6; error = 'timeout' },
    @{ name = 'timeout-math-group'; extra = @('--math-group', 'auto', '--math-group-rule', '1', '--request-timeout-ms', '300'); exit = 6; error = 'timeout' },
    @{ name = 'cancel-math-group'; extra = @('--math-group', 'auto', '--math-group-rule', '1', '--inject', 'cancel'); exit = 11; error = 'キャンセル' },
    @{ name = 'success-math-group'; extra = @('--math-group', 'auto', '--math-group-rule', '1'); exit = 0; error = '' }
)
foreach ($case in $cases) {
    $out = Join-Path $run $case.name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = @('--project', $project, '--manim', $manim, '--out', $out, '--ffprobe', (Join-Path $ucrt 'ffprobe.exe'),
        '--fixture', 'cmp-layers-16', '--cap-seconds', '600') + $case.extra
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    & $harness @arguments 2> (Join-Path $out 'harness.log') | Out-Null
    $code = $LASTEXITCODE
    $report = Get-Content -Raw (Join-Path $out 'cmp-layers-16-report.json') | ConvertFrom-Json
    $main = $report.math_group_main
    $left = @(Get-ChildItem $out -File | Where-Object { $_.Name -match '\.(mp4|mkv|mvmtmp|tmp|part)' } | ForEach-Object { $_.Name })
    $success = $case.exit -eq 0
    $ok = $code -eq $case.exit -and $report.export_success -eq $success -and $report.final_file_exists -eq $success -and
    $report.temporary_file_exists -eq $false -and ($success -or $left.Count -eq 0) -and
    ($success -or "$($report.export_error)" -match $case.error) -and $main.live_after_export -eq 0 -and
    $main.active_after_export -eq 0 -and $main.raster_live_bytes_after_export -eq 0
    $summary.application += [pscustomobject]@{ name = $case.name; exit_code = $code; verdict = $report.verdict; export_error = $report.export_error
        request_timeout_ms = $report.request_timeout_ms; final_file_exists = $report.final_file_exists
        temporary_file_exists = $report.temporary_file_exists; media_files_left = $left; graph = $main.graph
        live_after_export = $main.live_after_export; active_after_export = $main.active_after_export
        raster_live_bytes_after_export = $main.raster_live_bytes_after_export; wall_seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1); ok = $ok
    }
    if (-not $ok) { ++$failures }
    Write-Host ("{0}: exit={1} {2} error='{3}' 最終 file={4} 一時 file={5} 残り transition={6} callback={7} ok={8}" -f $case.name, $code,
        $report.verdict, $report.export_error, $report.final_file_exists, $report.temporary_file_exists, $main.live_after_export,
        $main.active_after_export, $ok)
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
Write-Host "結果: $run"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
