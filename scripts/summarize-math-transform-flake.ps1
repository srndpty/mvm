<#
.SYNOPSIS
    保存した console ログから比較値を再計算する。元の証拠は変更しない。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$rows = @()
$workloads = @()
foreach ($file in Get-ChildItem $EvidenceDirectory -Filter '*.log') {
    if ($file.Name -match 'LastTest|historical-overlap|^pre-') { continue }
    $lines = @(Get-Content $file.FullName)
    $active = @{}
    $targetActive = $false
    $overlap = @{}
    $initialActive = @{}
    $maximumActive = 0
    foreach ($line in $lines) {
        if ($line -match '^\s*Start\s+(\d+):\s+(\S+)') {
            $testId = $Matches[1]
            $testName = $Matches[2]
            $active[$testId] = $testName
            if ($testName -eq 'math_transform_native_playback') {
                $targetActive = $true
                $overlap = @{}
                $initialActive = @{}
                $maximumActive = $active.Count
                foreach ($key in $active.Keys) {
                    $overlap[$key] = $active[$key]
                    $initialActive[$key] = $active[$key]
                }
            }
            if ($targetActive) { $overlap[$testId] = $testName }
        }
        if ($targetActive) { $maximumActive = [Math]::Max($maximumActive, $active.Count) }
        if ($line -match '^\s*\d+/\d+ Test\s+#(\d+):') {
            $testId = $Matches[1]
            if ($active[$testId] -eq 'math_transform_native_playback') {
                $workloads += [ordered]@{ log = $file.Name; result = $line.Trim();
                    active_at_start = $initialActive; maximum_active = $maximumActive;
                    overlap = $overlap }
                $targetActive = $false
            }
            $active.Remove($testId)
        }
    }
    $durations = @()
    foreach ($line in $lines) {
        if ($line -match 'Test\s+#\d+: math_transform_native_playback\s+\.+(?:\*\*\*Failed|\s+Passed)\s+([0-9.]+) sec') {
            $durations += [double]$Matches[1]
        }
    }
    if ($durations.Count -eq 0) { continue }
    $stages = @{}
    foreach ($line in $lines) {
        if ($line -match '変形診断: 経過 (\d+) ms、thread (\S+)、(.+)$') {
            $stages[$Matches[3]] = [ordered]@{ elapsed_ms = [long]$Matches[1]; thread_id = $Matches[2] }
        }
    }
    $waitMs = $null
    $publicationMs = $null
    if ($stages.ContainsKey('GUI disk 待機終了') -and $stages.ContainsKey('GUI 選択・disk 待機開始')) {
        $waitMs = $stages['GUI disk 待機終了'].elapsed_ms - $stages['GUI 選択・disk 待機開始'].elapsed_ms
    }
    if ($stages.ContainsKey('変形 worker 描画終了') -and
        $stages.ContainsKey('変形 worker 検証終了・provenance 公開直前')) {
        $publicationMs = $stages['変形 worker 検証終了・provenance 公開直前'].elapsed_ms -
                         $stages['変形 worker 描画終了'].elapsed_ms
    }
    $rows += [ordered]@{
        log = $file.Name
        total_seconds = $durations
        failing_assertions = @($lines | Where-Object { $_ -match 'FAIL: native 変形:' })
        frame_observations = @($lines | Where-Object { $_ -match '1: 記録|2: 届けた時|26 検査中' })
        gui_disk_wait_ms = $waitMs
        validation_and_frame_cache_ms = $publicationMs
        stages = $stages
    }
}
if ($rows.Count -eq 0) { throw '比較対象の試験が 0 件です' }
$rows | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $EvidenceDirectory 'comparison.json') -Encoding utf8
$workloads | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $EvidenceDirectory 'workload-comparison.json') -Encoding utf8
$report = @('# 数式変形 release flake のログ比較', '',
    'この表は scripts/summarize-math-transform-flake.ps1 が保存済みログから生成した。', '',
    '| ログ | 実行秒数 | GUI の disk 待機 ms | 描画終了から検証・frame cache 書き込み終了 ms |',
    '| --- | --- | --- | --- |')
foreach ($row in $rows) {
    $report += "| $($row.log) | $($row.total_seconds -join ', ') | $($row.gui_disk_wait_ms) | $($row.validation_and_frame_cache_ms) |"
}
$report += @('', 'null / 空欄は未記録。0 とみなさない。thread ID と各段階の時刻は comparison.json に保存。')
$report | Set-Content (Join-Path $EvidenceDirectory 'comparison.md') -Encoding utf8
Write-Host "比較結果: $EvidenceDirectory/comparison.json"
