<#
.SYNOPSIS
    P4-1 の保存済み gate 証拠から結果を集計する。失敗 run も一覧に残す。
#>
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p41-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-1 検証の機械集計')
$lines.Add('')
$lines.Add('`scripts/math-p41-report.ps1` が保存済みログから生成する。過去の失敗・途中版を削除しない。')
$lines.Add('')
$lines.Add('|証拠 directory|検証|件数|結果|')
$lines.Add('|---|---|---:|---|')
$roots = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p41-*' | Sort-Object Name)
foreach ($root in $roots) {
    $commandsPath = Join-Path $root.FullName 'commands.json'
    $mutationsPath = Join-Path $root.FullName 'mutations.json'
    $relative = 'build/' + $root.Name
    if (Test-Path -LiteralPath $commandsPath) {
        $entries = @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)
        foreach ($entry in $entries) {
            $count = if ($entry.PSObject.Properties['test_count']) { [string]$entry.test_count } else { '—' }
            $lines.Add("|[$($root.Name)](../$relative/commands.json)|$($entry.name)|$count|$($entry.result)|")
        }
    }
    if (Test-Path -LiteralPath $mutationsPath) {
        $entries = @(Get-Content -LiteralPath $mutationsPath -Raw | ConvertFrom-Json)
        $detected = @($entries | Where-Object result -eq 'DETECTED').Count
        $complete = $detected -eq 15 -and $entries.Count -eq 15
        $result = if ($complete) { 'PASS' } else { '未完了/FAIL' }
        $lines.Add("|[$($root.Name)](../$relative/mutations.json)|変異|$detected/15|$result|")
    }
}
$lines.Add('')
$lines.Add('## CTest と正式スクリプトの集計')
$lines.Add('')
foreach ($root in $roots) {
    foreach ($log in @(Get-ChildItem -LiteralPath $root.FullName -File -Filter '*.log')) {
        $content = Get-Content -LiteralPath $log.FullName -Raw
        $summaries = [regex]::Matches($content, '(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)')
        if ($summaries.Count -eq 0) { continue }
        $lines.Add("### $($root.Name)/$($log.Name)")
        $lines.Add('')
        foreach ($summary in $summaries) {
            $failed = [int]$summary.Groups[2].Value
            $total = [int]$summary.Groups[3].Value
            $lines.Add("通過 $($total - $failed)/$total、失敗 $failed。")
        }
        $assertions = [regex]::Matches($content, '(検査 (\d+) 件、失敗 (\d+) 件|(\d+) 検査中 (\d+) 件失敗)')
        foreach ($assertion in $assertions) { $lines.Add($assertion.Value + '。') }
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
        $lines.Add('')
    }
}
$lines.Add('各 source-state.json と sources/ が source/revision の正。性能閾値・歴史的分類を変更していない。')
$lines.Add('通常 release は performance/stability を除外し、既存の display-power lease を通す。')
$lines.Add('初期 sandbox 制約・compile 失敗・変異の見逃しは専用記録と該当の新規 directory を参照する。')
$lines | Set-Content -LiteralPath (Join-Path $repoRoot $OutputPath) -Encoding utf8NoBOM
Write-Host "集計: $OutputPath"
