<#
.SYNOPSIS
    保存した P4-3 の生データを再集計する。失敗と INVALID を消さない。
#>
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p43-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-3 検証の機械集計')
$lines.Add('')
$lines.Add('`scripts/math-p43-report.ps1` が保存済み JSON・TSV から生成する。過去の失敗も保持する。')
$lines.Add('')
$lines.Add('|証拠|gate|終了コード|結果|')
$lines.Add('|---|---|---:|---|')
$directories = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p43-*' | Sort-Object Name)
foreach ($directory in $directories) {
    $commandFile = Join-Path $directory.FullName 'commands.json'
    if (-not (Test-Path -LiteralPath $commandFile)) { continue }
    $commands = @(Get-Content -LiteralPath $commandFile -Raw | ConvertFrom-Json)
    foreach ($record in $commands) {
        $outcome = if ($record.exit -eq 0) { 'PASS' } else { 'FAIL' }
        $lines.Add("|[$($directory.Name)](../build/$($directory.Name)/commands.json)|$($record.name)|$($record.exit)|$outcome|")
    }
}
foreach ($directory in $directories) {
    $resultPath = Join-Path $directory.FullName 'native/results.json'
    if (-not (Test-Path -LiteralPath $resultPath)) { continue }
    $native = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    $lines.Add('')
    $lines.Add("## $($directory.Name) の native 検査")
    $lines.Add('')
    $lines.Add("検査 $($native.checks)、失敗 $($native.failures)。artifact Ready まで $($native.artifact_ready_ms) ms。")
    $table = @(Import-Csv -LiteralPath (Join-Path $directory.FullName 'native/observations.tsv') -Delimiter "`t")
    if ($table.Count -eq 0) { $lines.Add('画素比較の行が無いので native 合成の成功証拠にはしない。'); continue }
    $lines.Add('')
    $lines.Add('|source frame|decode・提示・取得 ms|常駐 byte|最大 byte|不一致 pixel|')
    $lines.Add('|---:|---:|---:|---:|---:|')
    foreach ($row in $table) {
        $lines.Add("|$($row.source_frame)|$($row.decode_and_submission_ms)|$($row.resident_bytes)|$($row.peak_bytes)|$($row.mismatch_pixels)|")
    }
    $lines.Add('')
    $lines.Add('所要時間は性能観測であり、新しい合否閾値にはしない。source PNG と共通の RGBA8 段階で比較する。')
}
$lines | Set-Content -LiteralPath (Join-Path $repoRoot $OutputPath) -Encoding utf8NoBOM
