param(
    [Parameter(Mandatory=$true)][string]$Json
)

$ErrorActionPreference = 'Stop'

function Assert-Oracle([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$raw = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json
Assert-Oracle ($raw.schema -eq 'mvm-p2-present-id-oracle-2') 'schemaが一致しません'
Assert-Oracle ($raw.swap_effect -eq 'FLIP_DISCARD') 'swap effectがFLIP_DISCARDではありません'
Assert-Oracle ([int64]$raw.buffer_count -eq 3) 'BufferCountが3ではありません'
Assert-Oracle ([int64]$raw.sync_interval -eq 1) 'SyncIntervalが1ではありません'
if ([bool]$raw.dwm_flush_used) {
    Assert-Oracle ($raw.dwm_flush_mode -eq 'ORACLE_ONLY_FALLBACK') `
        'DwmFlushがoracle-only fallbackとして明示されていません'
} else {
    Assert-Oracle ($raw.dwm_flush_mode -eq 'DISABLED') 'DwmFlush modeが不正です'
}

$submissions = @($raw.present_submissions)
$transitions = @($raw.statistics_transitions)
$oracle = @($raw.oracle_records)
$configured = [int64]$raw.configured_present_count
Assert-Oracle ($submissions.Count -eq $configured) '成功submission数がconfigured countと一致しません'
Assert-Oracle ([bool]$raw.configured_submissions_complete) 'submissionが完了していません'
Assert-Oracle ([int64]$raw.present_failure_count -eq 0) 'Present失敗があります'
Assert-Oracle ([int64]$raw.get_last_present_count_failure_count -eq 0) 'GetLastPresentCount失敗があります'

for ($index = 0; $index -lt $submissions.Count; ++$index) {
    $submission = $submissions[$index]
    Assert-Oracle ([int64]$submission.submission_index -eq $index) 'submission_indexが連続していません'
    Assert-Oracle ([int64]$submission.render_end_qpc -le [int64]$submission.present_return_qpc) `
        'render_end_qpcがpresent_return_qpcより後です'
    if ($index -gt 0) {
        Assert-Oracle ([int64]$submission.present_id -eq [int64]$submissions[$index - 1].present_id + 1) `
            'submitted Present IDがexact +1ではありません'
    }
}
Assert-Oracle ([bool]$raw.submitted_ids_consecutive) 'producerがPresent ID不連続を報告しました'

$firstId = [int64]$submissions[0].present_id
$lastId = [int64]$submissions[-1].present_id
$observed = @($transitions | Where-Object {
    [int64]$_.present_count -ge $firstId -and [int64]$_.present_count -le $lastId
})
Assert-Oracle ($observed.Count -eq $submissions.Count) 'ORACLE_SAMPLING_GAP: transition数が不足しています'
for ($index = 0; $index -lt $observed.Count; ++$index) {
    Assert-Oracle ([int64]$observed[$index].present_count -eq [int64]$submissions[$index].present_id) `
        'ORACLE_SAMPLING_GAP: PresentCountが飛んだか順序が崩れています'
    Assert-Oracle ([int64]$observed[$index].poll_qpc -gt 0) 'poll_qpcが不正です'
}
Assert-Oracle ([bool]$raw.observed_ids_complete) 'producerがobserved ID不完全を報告しました'
Assert-Oracle ([bool]$raw.final_drain_complete) 'final drainが完了していません'
Assert-Oracle ([int64]$raw.final_submitted_present_id -eq $lastId) 'final submitted IDが一致しません'
Assert-Oracle ([int64]$raw.final_observed_present_count -eq $lastId) 'final PresentCountが一致しません'

Assert-Oracle ([bool]$raw.sampler_high_priority) 'samplerを高優先度へ昇格できていません'
Assert-Oracle ([bool]$raw.sampler_baseline_ready) 'submission前にstatistics baselineを確立できませんでした'
Assert-Oracle ([int64]$raw.sampler_vblank_gap_count -eq 0) 'samplerがVBlank triggerを取りこぼしました'
Assert-Oracle ([int64]$raw.statistics_failure_count -eq 0) 'GetFrameStatistics失敗があります'
Assert-Oracle ([int64]$raw.statistics_disjoint_count -eq 0) 'frame statisticsがdisjointです'
Assert-Oracle ([bool]$raw.poll_interval_valid) 'poller intervalが大きすぎます'
Assert-Oracle ([int64]$raw.max_poll_interval_qpc * 2 -lt [int64]$raw.nominal_period_qpc) `
    'poller intervalの再計算が失敗しました'
Assert-Oracle ([bool]$raw.window_output_stable) 'window outputがrun中に変化しました'
Assert-Oracle ([bool]$raw.statistics_output_matches_window) 'statistics outputがwindow outputと一致しません'
Assert-Oracle ([int64]$raw.vblank_ring_overflow_count -eq 0) 'VBlank ring overflowがあります'
Assert-Oracle ([int64]$raw.vblank_wait_failure_count -eq 0) 'VBlank observer失敗があります'

Assert-Oracle ($oracle.Count -eq $submissions.Count) 'oracle join record数が一致しません'
for ($index = 0; $index -lt $oracle.Count; ++$index) {
    Assert-Oracle ([int64]$oracle[$index].submission_index -eq $index) 'oracleのsubmission_indexが不正です'
    Assert-Oracle ([int64]$oracle[$index].present_id -eq [int64]$submissions[$index].present_id) `
        'oracleのPresent ID joinが不正です'
    Assert-Oracle ([int64]$oracle[$index].present_refresh_count -eq `
                   [int64]$observed[$index].present_refresh_count) `
        'oracleのPresentRefreshCount joinが不正です'
}

$scenarios = @($raw.offline_notification_scenarios)
Assert-Oracle ($scenarios.Count -gt 0) 'offline notification scenarioがありません'
foreach ($scenario in $scenarios) {
    $expectedDelay = [int64]([math]::Truncate(
        ([decimal][int64]$scenario.delay_milli_periods * [decimal][int64]$raw.nominal_period_qpc) / 1000))
    Assert-Oracle ([int64]$scenario.delay_qpc -eq $expectedDelay) 'offline delay QPCが不正です'
    $records = @($scenario.records)
    Assert-Oracle ($records.Count -eq $submissions.Count) 'offline notification record数が不正です'
    for ($index = 0; $index -lt $records.Count; ++$index) {
        Assert-Oracle ([int64]$records[$index].submission_index -eq $index) `
            'offline notificationのsubmission_indexが不正です'
        Assert-Oracle ([int64]$records[$index].present_id -eq [int64]$submissions[$index].present_id) `
            'offline notificationのPresent IDが不正です'
        Assert-Oracle ([int64]$records[$index].notify_qpc -eq `
                       [int64]$submissions[$index].present_return_qpc + $expectedDelay) `
            'notification delayがlive時刻とは独立に加算されていません'
    }
}

Assert-Oracle ([bool]$raw.oracle_valid) 'producerがoracleをINVALIDと判定しました'
Assert-Oracle ($raw.oracle_status -eq 'VALID') 'oracle_statusがVALIDではありません'
Assert-Oracle ($raw.sampling_gap_code -eq 'NONE') 'sampling gapが報告されています'
Assert-Oracle ($raw.mapper_proof_status -eq 'NOT_YET_EVALUABLE') 'mapperを早期評価しています'
Assert-Oracle ($raw.flip_discard_frame_discard_claim -eq 'NOT_ESTABLISHED') `
    '未観測Presentをdiscardへ帰属しています'

Write-Host "Present-ID oracle contract: PASS ($($submissions.Count)/$configured)"
