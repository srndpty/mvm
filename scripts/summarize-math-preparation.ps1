<#
.SYNOPSIS
    準備記録の wall / CPU・件数・旧閾値超過をログから再計算する。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$rows = @()
foreach ($file in Get-ChildItem $EvidenceDirectory -Filter '*.log') {
    if ($file.Name -match 'LastTest') { continue }
    $groups = @{}
    $names = @{}
    foreach ($line in Get-Content $file.FullName) {
        if ($line -match '^\s*Start\s+(\d+):\s+(\S+)') { $names[$Matches[1]] = $Matches[2] }
        if ($line -match '^(\d+): 準備記録: (\S+) wall_us=(\d+) cpu_100ns=(\d+) thread=(\d+) frame=(-?\d+) bytes=(\d+) success=(\d+) cpu_available=(\d+)') {
            $testId = $Matches[1]
            if (!$groups.ContainsKey($testId)) { $groups[$testId] = [System.Collections.Generic.List[object]]::new() }
            $groups[$testId].Add([ordered]@{ stage = $Matches[2]; wall_us = [long]$Matches[3];
                cpu_100ns = [long]$Matches[4]; thread = [long]$Matches[5]; frame = [long]$Matches[6];
                bytes = [long]$Matches[7]; success = $Matches[8] -eq '1'; cpu_available = $Matches[9] -eq '1' })
        }
    }
    foreach ($testId in $groups.Keys) {
        $records = $groups[$testId]
        $stageTotals = @{}
        $started = @{}
        foreach ($record in $records) {
            if ($record.stage -eq 'manifest-missing') {
                $started.Remove("$($record.thread)/$($record.frame)/manifest")
            }
            $key = "$($record.thread)/$($record.frame)/$($record.stage -replace '-(start|end)$','')"
            if ($record.stage -match '-start$') { $started[$key] = $record }
            if ($record.stage -match '-end$' -and $started.ContainsKey($key)) {
                $begin = $started[$key]
                $stage = $record.stage -replace '-end$', ''
                if (!$stageTotals.ContainsKey($stage)) { $stageTotals[$stage] = [ordered]@{ count = 0; wall_us = 0L; cpu_100ns = 0L; bytes = 0L; failures = 0 } }
                $total = $stageTotals[$stage]
                $total.count++
                $total.wall_us += $record.wall_us - $begin.wall_us
                if ($record.cpu_available -and $begin.cpu_available) { $total.cpu_100ns += $record.cpu_100ns - $begin.cpu_100ns }
                $total.bytes += $record.bytes
                if (!$record.success) { $total.failures++ }
                $started.Remove($key)
            }
        }
        $waitStart = @($records | Where-Object stage -eq 'wait-start')
        $ready = @($records | Where-Object stage -eq 'gui-ready')
        $waitUs = if ($waitStart.Count -eq 1 -and $ready.Count -eq 1) { $ready[0].wall_us - $waitStart[0].wall_us } else { $null }
        $worker = @($records | Where-Object stage -match '^worker-')
        $testName = $names[$testId]
        $failedStage = switch -Regex ($testName) {
            '^math_transform_preparation_(decode-fail|missing|corrupt)$' { 'decode-end'; break }
            '^math_transform_preparation_persist-fail$' { 'persist-end'; break }
            '^math_transform_preparation_publish-fail$' { 'provenance-end'; break }
            default { $null }
        }
        if ($failedStage) {
            $witness = @($records | Where-Object { $_.stage -eq $failedStage -and !$_.success -and
                ($failedStage -eq 'provenance-end' -or $_.frame -eq 1) })
            if ($witness.Count -ne 1 -or @($worker | Where-Object stage -eq 'worker-error').Count -ne 1) {
                throw "故障注入の対象段階で失敗していません: $($file.Name)/$testName"
            }
        }
        if ($testName -match '^math_transform_preparation_(backend-stall|publish-stall)$' -and
            (@($worker | Where-Object stage -eq 'worker-cancelled').Count -ne 1 -or $ready.Count -ne 0)) {
            throw "停止した worker の取消を確認できません: $testName"
        }
        if ($testName -eq 'math_transform_preparation_lost-notification' -and
            (@($worker | Where-Object stage -eq 'worker-ready').Count -ne 1 -or $ready.Count -ne 0)) {
            throw '通知欠落の故障対照が worker の完了を観測していません'
        }
        if ($testName -eq 'math_transform_preparation_delayed' -and
            ($null -eq $waitUs -or $waitUs -le 10000000)) { throw '旧閾値を超える正常完了の対照になっていません' }
        if ($ready.Count -eq 1) {
            foreach ($stage in @('decode', 'extract', 'persist', 'hash', 'manifest-frame')) {
                if (!$stageTotals.ContainsKey($stage) -or $stageTotals[$stage].count -ne 180 -or $stageTotals[$stage].failures -ne 0) {
                    throw "Ready の段階会計が不正です: $($file.Name)/$testId/$stage"
                }
            }
        }
        $rows += [ordered]@{ log = $file.Name; test = $names[$testId]; preparation_wait_us = $waitUs;
            old_10_second_threshold_exceeded = if ($null -eq $waitUs) { $null } else { $waitUs -gt 10000000 };
            worker_outcomes = @($worker | ForEach-Object stage); gui_ready_count = $ready.Count;
            stages = $stageTotals; incomplete_stages = @($started.Keys) }
    }
}
if ($rows.Count -eq 0) { throw '準備記録が 0 件です' }
$rows | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $EvidenceDirectory 'preparation-summary.json') -Encoding utf8
$table = @('# 準備段階の比較（ログから生成）', '',
    '| ログ / 試験 | 準備待機 ms | 旧 10 秒超過 | worker 結果 | GUI Ready 件数 |', '| --- | --- | --- | --- | --- |')
foreach ($row in $rows) {
    $ms = if ($null -eq $row.preparation_wait_us) { '未完了' } else { $row.preparation_wait_us / 1000 }
    $table += "| $($row.log) / $($row.test) | $ms | $($row.old_10_second_threshold_exceeded) | $($row.worker_outcomes -join ', ') | $($row.gui_ready_count) |"
}
$table | Set-Content (Join-Path $EvidenceDirectory 'preparation-summary.md') -Encoding utf8
Write-Host "段階集計: $EvidenceDirectory/preparation-summary.json"
