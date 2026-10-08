# 生記録から P4-3.2 の件数と結果を再計算する。既存の歴史的結果は変更しない。
[CmdletBinding()]
param([string]$EvidencePrefix = 'math-p432-', [string]$Output = 'docs/math-graph-p432-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$directories = @(Get-ChildItem -LiteralPath (Join-Path $repo 'build') -Directory | Where-Object { $_.Name.StartsWith($EvidencePrefix) } | Sort-Object Name)
if ($directories.Count -eq 0) { throw 'P4-3.2 の証拠がありません' }
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-3.2 の実行結果')
$lines.Add('')
$lines.Add('この文書は `scripts/math-p432-report.ps1` が生ログ・JSON・TSV から生成する。過去の FAIL と P4-3.1 の PASS は元の証拠に保持する。')
$lines.Add('')
$lines.Add('|証拠|gate|終了コード|結果|')
$lines.Add('|---|---|---:|---|')
foreach ($directory in $directories) {
    $commandsPath = Join-Path $directory.FullName 'commands.json'
    if (Test-Path -LiteralPath $commandsPath) {
        $commands = @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)
        foreach ($record in $commands) {
            $result = if ($record.exit -eq 0) { 'PASS' } else { 'FAIL' }
            $lines.Add("|[$($directory.Name)](../build/$($directory.Name)/commands.json)|$($record.name)|$($record.exit)|$result|")
        }
        $raw = Join-Path $directory.FullName 'ctest-raw.log'
        if (@($commands | Where-Object { $_.name -in @('regressions', 'independent', 'release') }).Count -gt 0 -and (Test-Path -LiteralPath $raw)) {
            $content = Get-Content -LiteralPath $raw -Raw
            $passed = [regex]::Matches($content, '(?m)^Test Passed\.\r?$').Count
            $failed = [regex]::Matches($content, '(?m)^Test Failed\.\r?$').Count
            if ($passed + $failed -eq 0) { throw 'CTest の比較対象が0件です' }
            $lines.Add("|[$($directory.Name)](../build/$($directory.Name)/ctest-raw.log)|CTest 生記録|—|通過 $passed、失敗 $failed|")
        }
    }
}
$lines.Add('')
$lines.Add('## 再利用と毎 frame 新規構築の比較')
foreach ($directory in $directories) {
    $tsv = Join-Path $directory.FullName 'native/continuous-effects.tsv'
    if (-not (Test-Path -LiteralPath $tsv)) { continue }
    $rows = @(Import-Csv -LiteralPath $tsv -Delimiter "`t")
    if ($rows.Count -eq 0) { throw '効果の比較対象が0件です' }
    $mismatch = @($rows | Where-Object { [int]$_.mismatch_pixels -ne 0 }).Count
    $oracle = @($rows | Where-Object { [int]$_.oracle_mismatch -ne 0 }).Count
    $changed = @($rows | Group-Object case | Where-Object { @($_.Group.composition_revision | Select-Object -Unique).Count -ne 1 }).Count
    $state = Get-Content -LiteralPath (Join-Path $directory.FullName 'native/continuous-effects.json') -Raw | ConvertFrom-Json
    $lines.Add('')
    $lines.Add("[$($directory.Name)](../build/$($directory.Name)/native/continuous-effects.tsv): 比較 $($rows.Count) frame、非再利用基準との不一致 $mismatch frame、独立 oracle との不一致 $oracle frame、case 内の revision 変化 $changed 群。検査 $($state.checks)、失敗 $($state.failures)、seek request $($state.seek_requests)。")
    $draw = Join-Path $directory.FullName 'native/continuous-native.tsv'
    if (Test-Path -LiteralPath $draw) {
        $drawRows = @(Import-Csv -LiteralPath $draw -Delimiter "`t")
        $wrong = @($drawRows | Where-Object { [int]$_.mismatch_pixels -ne 0 }).Count
        $native = Get-Content -LiteralPath (Join-Path $directory.FullName 'native/results.json') -Raw | ConvertFrom-Json
        $lines.Add("Draw の既存連続試験: 比較 $($drawRows.Count) frame、不一致 $wrong frame。Manim・動画 alpha・ClipEffects を含む native 全体: 検査 $($native.checks)、失敗 $($native.failures)。")
    }
}
$lines.Add('')
$lines.Add('## 負例と復元')
foreach ($directory in $directories) {
    $path = Join-Path $directory.FullName 'mutation.json'
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $mutation = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $same = $mutation.original_sha256 -eq $mutation.restored_sha256
    $lines.Add('')
    $lines.Add("[$($directory.Name)](../build/$($directory.Name)/mutation.json): $($mutation.name)、終了 $($mutation.exit)、検出 $($mutation.killed)、位置・拡大の不一致 $($mutation.mismatched_position_frames) frame、復元 hash 一致 $same、復元試験の終了 $($mutation.restored_exit)。")
}
$lines.Add('')
$lines.Add('Effects-01 の FAIL は初回 animation record 作成直後で memo がまだ確立していない試験条件による再利用確認の失敗である。生記録を保持し、環境干渉へ分類変更しない。Effects-02 以降は memo 確立後の snapshot を比較する。')
$lines.Add('')
$lines.Add('P4-3 の BuildIndependent 1079/1080 と Release 1477/1479 は [歴史的結果](math-graph-p43-results.md) に保持する。[P4-3.1 の PASS 証拠](math-graph-p431-results.md) も変更しない。')
$lines | Set-Content -LiteralPath (Join-Path $repo $Output) -Encoding utf8NoBOM
Write-Host "集計を生成: $Output"
