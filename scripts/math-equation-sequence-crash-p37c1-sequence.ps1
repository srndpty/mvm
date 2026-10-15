<#
.SYNOPSIS
    P3-7C.1 Task E: 元の SegFault (2026-10-12) が起きた 49 件の CTest の選定と順序を、今の build で
    dump を採れる形で実行する (docs/math-equation-sequence-crash-p37c.md の 9)。

.DESCRIPTION
    49 件の名前と順序は、元の失敗の binary を作った source (commit 4f83f2b。tests/CMakeLists.txt は
    685ba23 から変わっていない) を configure し、P3-1.2 の gate の正規表現
    `math|mlt_export|m7b_3_product_export|timeline_export` で ctest -N した結果である。49 件・対象が
    25 番目・直前が math_inspector_product_ui で、文書の記録と一致する。今の tree でも 49 件すべてが
    同じ相対順序で登録されている (今の正規表現は 54 件に当たるので、名前の一覧で選ぶ)。

    binary・runtime・環境は元と同じではない (build/ は容量のために消した)。これは「同じ選定と順序」で
    あって、元の実行の正確な再現ではない。

    回数は実行の前に固定する: 2 回。native の crash が起きたら、その時点で止める (根本原因の解析へ)。
    ctest は -j 1 で、試験ごとに別の process (CTest の通常の分離)。native の試験は TEST_LAUNCHER
    (mvm_wer_launch) で起動され、落ちたら dump が archive に残る。記録は scripts/lib/test-run-record.ps1
    (build/ の外の archive の runs/)。実行中は 2 秒ごとに process と GPU の状態を記録する。
#>
[CmdletBinding()]
param([ValidateRange(1, 2)][int]$Repetitions = 2)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64'
$build = Join-Path $repo 'build\ucrt64-release'
$ctest = Join-Path $ucrt 'bin\ctest.exe'
. (Join-Path $PSScriptRoot 'lib\test-display-lease.ps1')
. (Join-Path $PSScriptRoot 'lib\test-run-record.ps1')
$env:PATH = "$ucrt\bin;$env:PATH"

$tests = @(
    'm4_timeline_export_focused_sequential', 'm4_timeline_export_focused_tractor', 'm4_timeline_export_focused_audio',
    'm7b_3_timeline_export_mapping_focused', 'm7b_3_product_export_architecture', 'mlt_export_capacity_overflow',
    'math_group_compose_oracle', 'mlt_export_timeout_accounting_deviation', 'math_equation_sequence_domain',
    'math_equation_sequence_authoring', 'math_equation_sequence_compile', 'math_project_json_focused',
    'math_transform_timeline_focused', 'math_raster_cache_focused', 'math_equation_sequence_render_contract',
    'math_equation_sequence_render_bridge', 'math_equation_sequence_preview_model', 'math_render_key_and_layout',
    'math_transform_contract', 'manim_math_tex_focused', 'math_smoke_existing_workspace_rejected',
    'math_inspector_qml', 'math_equation_sequence_inspector_qml', 'math_inspector_product_ui',
    'math_equation_sequence_product_ui', 'math_transform_inspector_product_ui', 'math_transform_authoring_product_ui',
    'math_controller_focused', 'math_equation_sequence_history', 'math_write_native_playback',
    'math_transform_native_playback', 'math_transform_preparation_backend-stall', 'math_transform_preparation_decode-fail',
    'math_transform_preparation_persist-fail', 'math_transform_preparation_publish-stall', 'math_transform_preparation_publish-fail',
    'math_transform_preparation_lost-notification', 'math_transform_preparation_missing', 'math_transform_preparation_corrupt',
    'math_transform_preparation_wrong-frame', 'math_transform_preparation_delayed', 'math_transform_preparation_wait_contract',
    'math_preparation_tools_contract', 'math_equation_sequence_preview_controller', 'math_equation_sequence_editor_controller',
    'math_equation_sequence_native_preview', 'math_equation_sequence_export_focused', 'math_transform_export_focused',
    'math_export_focused')
if ($tests.Count -ne 49 -or $tests[24] -ne 'math_equation_sequence_product_ui' -or $tests[23] -ne 'math_inspector_product_ui') {
    throw '49 件の一覧が元の選定と一致しません'
}

# 今の登録で 49 件がすべてあり、同じ相対順序であることを確かめる (違えば実行しない)。
$registered = @(& $ctest --test-dir $build -N | ForEach-Object { if ("$_" -match 'Test\s+#\d+: (\S+)') { $Matches[1] } })
$present = @($registered | Where-Object { $tests -contains $_ })
if (($present -join '|') -ne ($tests -join '|')) { throw '今の CTest の登録に 49 件が同じ順序でありません' }

function Start-Sampler([string]$Path) {
    $script = @'
param($Path)
$names = '^(mvm.*|ctest|WerFault|python.*|manim.*|ffmpeg|melt|pwsh)$'
while ($true) {
    $p = @(Get-Process | Where-Object { $_.ProcessName -match $names } | ForEach-Object {
        [ordered]@{ name = $_.ProcessName; pid = $_.Id; started = $(try { $_.StartTime.ToString('o') } catch { $null }); ws_mb = [math]::Round($_.WorkingSet64 / 1MB) } })
    $gpu = (& nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits 2>$null | Select-Object -First 1)
    $line = [ordered]@{ at = (Get-Date).ToString('o'); gpu = "$gpu"; processes = $p } | ConvertTo-Json -Compress -Depth 4
    Add-Content -LiteralPath $Path -Value $line -Encoding utf8
    Start-Sleep -Seconds 2
}
'@
    $file = Join-Path ([IO.Path]::GetTempPath()) "mvm-sampler-$PID.ps1"
    Set-Content -LiteralPath $file -Value $script -Encoding utf8
    Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $file, $Path) -WindowStyle Hidden -PassThru
}

$lease = Start-MvmTestDisplayLease
$record = $null
$crashed = $false
try {
    # 既存の dump を保全してから始める (失敗したら例外で、試験を始めない)。
    $record = Start-MvmTestRunRecord -Name 'p37c1-sequence49' -RepoRoot $repo -BinDirectory (Join-Path $build 'bin')
    $list = Join-Path $record.Run 'tests.txt'
    Set-Content -LiteralPath $list -Value $tests -Encoding utf8NoBOM
    $preflight = Join-Path $PSScriptRoot 'gui-environment-preflight.ps1'
    for ($r = 1; $r -le $Repetitions -and -not $crashed; ++$r) {
        $label = "rep$r"
        $valid = Invoke-MvmTestRunPreflight -Record $record -Label "$label-before" -Script $preflight -Arguments @('-BinDirectory', (Join-Path $build 'bin'))
        if (-not $valid) { Write-Host "${label}: preflight が VALID でないので実行しません"; break }
        $sampler = Start-Sampler (Join-Path $record.Run "$label.samples.jsonl")
        $lines = [Collections.Generic.List[string]]::new()
        try {
            $arguments = @('--test-dir', $build, '-j', '1', '--output-on-failure', '--tests-from-file', $list) + (Get-MvmRecordedCTestArguments -Record $record -Label $label)
            & $ctest @arguments 2>&1 | ForEach-Object { $line = "$_"; Write-Host $line; $lines.Add($line) }
            $code = $LASTEXITCODE
        } finally {
            Stop-Process -Id $sampler.Id -Force -ErrorAction SilentlyContinue
        }
        # 実際に実行した順序が元の順序と同じか (CTest は cost で順序を変えることがある)。
        $order = @($lines | ForEach-Object { if ($_ -match '^\s*Start\s+\d+: (\S+)') { $Matches[1] } })
        $sameOrder = ($order -join '|') -eq ($tests -join '|')
        Set-Content -LiteralPath (Join-Path $record.Run "$label.order.txt") -Value (@("same_order=$sameOrder") + $order) -Encoding utf8
        if (-not $sameOrder) { Write-Host "${label}: 実行の順序が元の順序と違います (記録だけ)" -ForegroundColor Yellow }
        Start-Sleep -Seconds 3
        # ctest の後に残った試験の process (孤児) を記録する。
        $left = @(Get-Process | Where-Object { $_.ProcessName -match '^mvm' } | ForEach-Object { "$($_.ProcessName) $($_.Id)" })
        Set-Content -LiteralPath (Join-Path $record.Run "$label.leftover-processes.txt") -Value $left -Encoding utf8
        Complete-MvmRecordedCTestGroup -Record $record -Label $label -OutputLines $lines -ExitCode $code -BuildDirectory $build -Description "49 件 ($r 回目)"
        Invoke-MvmTestRunPreflight -Record $record -Label "$label-after" -Script $preflight -Arguments @('-BinDirectory', (Join-Path $build 'bin')) | Out-Null
        $results = @(Get-Content -Raw -LiteralPath (Join-Path $record.Run "$label.tests.json") | ConvertFrom-Json)
        $crashes = @($results | Where-Object { $_.classification -eq 'NATIVE_CRASH' })
        $target = @($results | Where-Object { $_.name -eq 'math_equation_sequence_product_ui' })[0]
        Write-Host ("{0}: ctest exit {1}、49 件中 PASS {2}、対象 {3}、native の crash {4} 件、残った process {5}" -f $label, $code,
            @($results | Where-Object { $_.classification -eq 'PASS' }).Count, $target.classification, $crashes.Count, $left.Count)
        if ($crashes.Count -gt 0) {
            $crashed = $true
            Write-Host "native の crash が起きたので止めます: $($crashes.name -join ', ')" -ForegroundColor Red
        }
        $lease.AssertValid()
    }
} finally {
    if ($record) { Complete-MvmTestRunRecord -Record $record | Out-Null }
    $lease.Dispose()
}
if ($crashed) { exit 3 }
