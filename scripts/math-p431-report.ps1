# 新規 gate の生データだけから集計する。過去の FAIL と変異の負例は不変保存する。
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p431-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$directories = @(Get-ChildItem (Join-Path $repo 'build') -Directory -Filter 'math-p431-*' | Sort-Object Name)
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-3.1 検証の機械集計')
$lines.Add('')
$lines.Add('`scripts/math-p431-report.ps1` が新規証拠の command JSON と native TSV から生成する。初回失敗も保持する。')
$lines.Add('')
$lines.Add('|証拠|gate|終了コード|結果|')
$lines.Add('|---|---|---:|---|')
foreach ($directory in $directories) {
    $commandFile = Join-Path $directory.FullName 'commands.json'
    if (-not (Test-Path $commandFile)) { continue }
    $commands = @(Get-Content $commandFile -Raw | ConvertFrom-Json)
    foreach ($record in $commands) {
        $outcome = if ($record.exit -eq 0) { 'PASS' } else { 'FAIL' }
        $lines.Add("|[$($directory.Name)](../build/$($directory.Name)/commands.json)|$($record.name)|$($record.exit)|$outcome|")
    }
    $raw = Join-Path $directory.FullName 'ctest-raw.log'
    $hasCTest = @($commands | Where-Object { $_.name -in @('regressions', 'independent', 'release') }).Count -gt 0
    if ($hasCTest -and (Test-Path $raw)) {
        $content = Get-Content $raw -Raw
        $passed = [regex]::Matches($content, '(?m)^Test Passed\.\r?$').Count
        $failed = [regex]::Matches($content, '(?m)^Test Failed\.\r?$').Count
        if ($passed + $failed -gt 0) {
            $lines.Add("|[$($directory.Name)](../build/$($directory.Name)/ctest-raw.log)|CTest 生記録|—|通過 $passed、失敗 $failed|")
        }
    }
}
$lines.Add('')
$lines.Add('## 固定 composition の native clock')
foreach ($directory in $directories) {
    $path = Join-Path $directory.FullName 'native/continuous-native.tsv'
    if (-not (Test-Path $path)) { continue }
    $rows = @(Import-Csv $path -Delimiter "`t")
    $wrong = @($rows | Where-Object { [int]$_.mismatch_pixels -ne 0 }).Count
    $changed = @($rows | Group-Object case | Where-Object { @($_.Group.composition_epoch | Select-Object -Unique).Count -ne 1 }).Count
    $lines.Add('')
    $lines.Add("[$($directory.Name)](../build/$($directory.Name)/native/continuous-native.tsv): 比較 $($rows.Count) frame、不一致 $wrong frame、case 内で revision が変わった群 $changed。")
    $result = Join-Path $directory.FullName 'native/results.json'
    if (Test-Path $result) {
        $native = Get-Content $result -Raw | ConvertFrom-Json
        $lines.Add("native 全体: 検査 $($native.checks)、失敗 $($native.failures)。")
    }
    $clockPath = Join-Path $directory.FullName 'native/continuous-native.json'
    if (Test-Path $clockPath) {
        $clockState = Get-Content $clockPath -Raw | ConvertFrom-Json
        $lines.Add("clock 試験の seek request $($clockState.seek_requests)、提示 $($clockState.presented_frames) frame、最終 composition revision $($clockState.composition_revision)。")
    }
}
$lines.Add('')
$lines.Add('## 変異と A/B')
foreach ($directory in $directories) {
    $mutation = Join-Path $directory.FullName 'mutations.json'
    if (Test-Path $mutation) {
        foreach ($record in @(Get-Content $mutation -Raw | ConvertFrom-Json)) {
            $lines.Add("[$($directory.Name)](../build/$($directory.Name)/mutations.json): $($record.name)、負例の終了 $($record.exit)、検出 $($record.killed)。")
        }
    }
    $comparison = Join-Path $directory.FullName 'comparison.json'
    if (Test-Path $comparison) {
        foreach ($record in @(Get-Content $comparison -Raw | ConvertFrom-Json)) {
            $lines.Add("[$($directory.Name)](../build/$($directory.Name)/comparison.json): $($record.arm)、終了 $($record.exit)、fixture SHA256 $($record.fixture)。")
        }
    }
}
$lines.Add('')
$lines.Add('P4-3 の BuildIndependent 1079/1080、Release 1477/1479 は [元の結果](math-graph-p43-results.md) と元 directory を保持する。')
$lines | Set-Content (Join-Path $repo $OutputPath) -Encoding utf8NoBOM
