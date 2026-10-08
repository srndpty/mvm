<#
.SYNOPSIS
    P4-2 の保存済み証拠から gate・変異・pixel 線幅を機械集計する。
#>
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p42-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-2 検証の機械集計')
$lines.Add('')
$lines.Add('`scripts/math-p42-report.ps1` が保存済み JSON・TSV・ログから生成する。失敗を含めて保存する。')
$lines.Add('')
$lines.Add('|証拠|gate|終了コード|結果|')
$lines.Add('|---|---|---:|---|')
$roots = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p42-*' | Sort-Object Name)
foreach ($root in $roots) {
    $commandsPath = Join-Path $root.FullName 'commands.json'
    if (-not (Test-Path -LiteralPath $commandsPath)) { continue }
    foreach ($entry in @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)) {
        $result = if ($entry.exit -eq 0) { 'PASS' } else { 'FAIL' }
        $lines.Add("|[$($root.Name)](../build/$($root.Name)/commands.json)|$($entry.name)|$($entry.exit)|$result|")
    }
}
foreach ($root in $roots) {
    $mutationsPath = Join-Path $root.FullName 'controls/mutations.json'
    if (Test-Path -LiteralPath $mutationsPath) {
        $entries = @(Get-Content -LiteralPath $mutationsPath -Raw | ConvertFrom-Json)
        $killed = @($entries | Where-Object killed).Count
        $lines.Add('')
        $lines.Add("## $($root.Name) の変異")
        $lines.Add('')
        $lines.Add("検出 $killed/17。ビルド終了0・検証終了1だけを検出として数える。")
        $lines.Add('')
        $lines.Add('|変異|build|test|検出|')
        $lines.Add('|---|---:|---:|---|')
        foreach ($entry in $entries) { $lines.Add("|$($entry.name)|$($entry.build_exit)|$($entry.test_exit)|$($entry.killed)|") }
    }
    $measurementsPath = Join-Path $root.FullName 'artifacts/measurements.tsv'
    if (Test-Path -LiteralPath $measurementsPath) {
        $lines.Add('')
        $lines.Add("## $($root.Name) の水平線被覆積分")
        $lines.Add('')
        $lines.Add('|canvas|指定 pixel 幅|被覆積分 pixel 幅|絶対差|')
        $lines.Add('|---|---:|---:|---:|')
        foreach ($row in @(Import-Csv -LiteralPath $measurementsPath -Delimiter "`t")) {
            $difference = [Math]::Abs([double]$row.指定線幅 - [double]$row.画素線幅)
            $lines.Add("|$($row.幅)×$($row.高さ)|$($row.指定線幅)|$($row.画素線幅)|$difference|")
        }
        $lines.Add('')
        $lines.Add('これは Cairo の固定した水平 fixture の被覆量子化を含む。preview/export の全画素同値に許容差を導入しない。')
    }
    foreach ($log in @(Get-ChildItem -LiteralPath $root.FullName -File -Filter '*.log')) {
        $content = Get-Content -LiteralPath $log.FullName -Raw
        $summaries = [regex]::Matches($content, '(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)')
        if ($summaries.Count -eq 0) { continue }
        $lines.Add('')
        $lines.Add("## $($root.Name)/$($log.Name)")
        $lines.Add('')
        foreach ($summary in $summaries) {
            $failed = [int]$summary.Groups[2].Value
            $total = [int]$summary.Groups[3].Value
            $lines.Add("通過 $($total - $failed)/$total、失敗 $failed。")
        }
        $section = $content.IndexOf('=== テスト種別ごとの結果 ===')
        if ($section -ge 0) {
            $tail = $content.Substring($section)
            $end = $tail.IndexOf('描画試験の電源前提を終了')
            if ($end -ge 0) { $tail = $tail.Substring(0, $end) }
            $lines.Add('')
            $lines.Add('```text')
            $lines.Add($tail.Trim())
            $lines.Add('```')
        }
    }
}
$lines | Set-Content -LiteralPath (Join-Path $repoRoot $OutputPath) -Encoding utf8NoBOM
