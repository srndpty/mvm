param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good', 'NegativeSubmittedGap', 'NegativeObservedGap', 'NegativeFinalDrain',
        'NegativePollInterval', 'NegativeOfflineDelay', 'NegativeEarlyMapper',
        'NegativeDiscardClaim', 'NegativeSamplerTriggerMode',
        'NegativeSamplerVBlankWaitFailure', 'NegativeSamplerPriorityMode',
        'NegativeSamplerAckTimeout', 'NegativeSamplerCycleTimeout',
        'NegativeFrameLatencyWaitFailure', 'NegativeWarmupIncomplete',
        'NegativeFrameLatencyMode',
        'GoodCorrectnessOnly', 'NegativeAuthorityModeMissing',
        'NegativeDebugTimingApplied', 'NegativeReleaseTimingDisabled',
        'NegativeCorrectnessRelaxedInDebug',
        'NegativeOccludedPresent', 'NegativeUnclassifiedPresentStatus',
        'NegativePresentOutcomeAuthority', 'NegativeWindowVisibility')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Output
)

$ErrorActionPreference = 'Stop'
$submissions = @()
$transitions = @()
$oracle = @()
$offline = @()
for ($index = 0; $index -lt 4; ++$index) {
    $presentId = 100 + $index
    $submissions += [ordered]@{
        submission_index=$index; present_id=$presentId
        render_end_qpc=10000 + 1000 * $index; present_return_qpc=10100 + 1000 * $index
    }
    $transitions += [ordered]@{
        poll_qpc=10500 + 1000 * $index; present_count=$presentId
        present_refresh_count=500 + $index; sync_refresh_count=500 + $index
        sync_qpc_time=10500 + 1000 * $index
    }
    $oracle += [ordered]@{
        submission_index=$index; present_id=$presentId; present_refresh_count=500 + $index
    }
    $offline += [ordered]@{
        submission_index=$index; present_id=$presentId; notify_qpc=10100 + 1000 * $index + 100
    }
}
$raw = [ordered]@{
    schema='mvm-p2-present-id-oracle-2'; oracle_status='VALID'; oracle_valid=$true
    authority_mode='FULL_RELEASE'; correctness_verdict='PASS'; timing_verdict='PASS'
    acquisition_liveness_verdict='PASS'; acquisition_wait_budget_ms=1000
    mapper_proof_status='NOT_YET_EVALUABLE'; flip_discard_frame_discard_claim='NOT_ESTABLISHED'
    sampling_gap_code='NONE'; qpc_frequency=10000000; nominal_period_qpc=1000
    configured_present_count=4; swap_effect='FLIP_DISCARD'; buffer_count=3; sync_interval=1
    frame_latency_waitable=$true; maximum_frame_latency=1
    warmup_present_count=99; warmup_complete=$true; warmup_present_failure_count=0
    warmup_frame_latency_wait_failure_count=0; final_warmup_present_id=99
    measurement_follows_warmup=$true
    dwm_flush_used=$false; dwm_flush_mode='DISABLED'; configured_submissions_complete=$true
    submitted_ids_consecutive=$true; observed_ids_complete=$true; final_drain_complete=$true
    final_submitted_present_id=103; final_observed_present_count=103
    present_failure_count=0; get_last_present_count_failure_count=0
    present_occluded_count=0; present_unclassified_status_count=0
    present_outcome_authority_exact=$true; present_outcome_code='PRESENT_OUTCOME_EXACT'
    window_visibility_precondition=$true
    frame_latency_wait_failure_count=0
    sampler_ack_timeout_count=0
    sampler_cycle_timeout_count=0
    sampler_high_priority=$true; sampler_priority_mode='TIME_CRITICAL'
    sampler_trigger_mode='OBSERVER_PUBLICATION_EVENT'
    sampler_baseline_ready=$true; sampler_vblank_gap_count=0
    sampler_vblank_wait_failure_count=0
    statistics_failure_count=0; statistics_disjoint_count=0; baseline_disjoint_count=0
    max_poll_interval_qpc=100; poll_interval_valid=$true
    window_output_stable=$true; statistics_output_matches_window=$true
    vblank_ring_overflow_count=0; vblank_wait_failure_count=0
    present_submissions=$submissions; statistics_transitions=$transitions; oracle_records=$oracle
    offline_notification_scenarios=@([ordered]@{
        delay_milli_periods=100; delay_qpc=100; records=$offline
    })
}

$expectedViolation = @{
    NegativeSubmittedGap='submitted Present IDがexact +1ではありません'
    NegativeObservedGap='ORACLE_SAMPLING_GAP: transition数が不足しています'
    NegativeFinalDrain='final PresentCountが一致しません'
    NegativePollInterval='poller intervalの再計算が失敗しました'
    NegativeOfflineDelay='notification delayがlive時刻とは独立に加算されていません'
    NegativeEarlyMapper='mapperを早期評価しています'
    NegativeDiscardClaim='未観測Presentをdiscardへ帰属しています'
    NegativeSamplerTriggerMode='samplerがevent-based VBlank waitを使用していません'
    NegativeSamplerVBlankWaitFailure='samplerのVBlank wait失敗があります'
    NegativeSamplerPriorityMode='samplerがTIME_CRITICAL priorityを使用していません'
    NegativeSamplerAckTimeout='sampler ack timeoutがあります'
    NegativeSamplerCycleTimeout='sampler cycle timeoutがあります'
    NegativeFrameLatencyWaitFailure='frame latency wait失敗があります'
    NegativeWarmupIncomplete='warmupが完了していません'
    NegativeFrameLatencyMode='maximum frame latencyが1ではありません'
    NegativeAuthorityModeMissing='authority mode provenanceがありません'
    NegativeDebugTimingApplied='CORRECTNESS_ONLYでtiming verdictがNOT_AUTHORITY_IN_DEBUGではありません: PASS'
    NegativeReleaseTimingDisabled='timing verdictがPASSではありません'
    NegativeCorrectnessRelaxedInDebug='ORACLE_SAMPLING_GAP: transition数が不足しています'
    NegativeOccludedPresent='OCCLUDED_NOT_AUTHORITY: occludedなPresentがあります'
    NegativeUnclassifiedPresentStatus='UNCLASSIFIED_PRESENT_STATUS: 未分類のPresent statusがあります'
    NegativePresentOutcomeAuthority='present outcome authorityがexactではありません'
    NegativeWindowVisibility='window visibility preconditionが成立していません'
}

# S2-e2: caseごとにcheckerへ渡すauthority modeを決める。build typeは推測しない。
$caseAuthorityMode = @{
    GoodCorrectnessOnly='CORRECTNESS_ONLY'
    NegativeDebugTimingApplied='CORRECTNESS_ONLY'
    NegativeCorrectnessRelaxedInDebug='CORRECTNESS_ONLY'
}
$authorityMode = if ($caseAuthorityMode.ContainsKey($Case)) { $caseAuthorityMode[$Case] } else { 'FULL_RELEASE' }
$beforeMutation = $raw | ConvertTo-Json -Depth 10 -Compress

switch ($Case) {
    'NegativeSubmittedGap' { $raw.present_submissions[2].present_id = 104 }
    'NegativeObservedGap' { $raw.statistics_transitions[2].present_count = 104 }
    'NegativeFinalDrain' { $raw.final_observed_present_count = 102 }
    'NegativePollInterval' { $raw.max_poll_interval_qpc = 600 }
    'NegativeOfflineDelay' { $raw.offline_notification_scenarios[0].records[2].notify_qpc += 1 }
    'NegativeEarlyMapper' { $raw.mapper_proof_status = 'PASS' }
    'NegativeDiscardClaim' { $raw.flip_discard_frame_discard_claim = 'ESTABLISHED' }
    'NegativeSamplerTriggerMode' { $raw.sampler_trigger_mode = 'YIELD_BUSY_WAIT' }
    'NegativeSamplerVBlankWaitFailure' { $raw.sampler_vblank_wait_failure_count = 1 }
    'NegativeSamplerPriorityMode' { $raw.sampler_priority_mode = 'HIGHEST' }
    'NegativeSamplerAckTimeout' { $raw.sampler_ack_timeout_count = 1 }
    'NegativeSamplerCycleTimeout' { $raw.sampler_cycle_timeout_count = 1 }
    'NegativeFrameLatencyWaitFailure' { $raw.frame_latency_wait_failure_count = 1 }
    'NegativeWarmupIncomplete' { $raw.warmup_complete = $false }
    'NegativeFrameLatencyMode' { $raw.maximum_frame_latency = 2 }
    # CORRECTNESS_ONLYではtiming判定を行わない。timing verdictをPASSと主張する
    # artifactはauthority scopeの詐称なのでrejectする。
    'GoodCorrectnessOnly' {
        $raw.authority_mode = 'CORRECTNESS_ONLY'
        $raw.timing_verdict = 'NOT_AUTHORITY_IN_DEBUG'
        # timing thresholdを外れた値でもCORRECTNESS_ONLYでは判定されない。
        $raw.max_poll_interval_qpc = 600
    }
    'NegativeAuthorityModeMissing' { $raw.Remove('authority_mode') }
    # occludedなPresentがsubmission成功として数えられていないこと。
    # sampler_ack_timeout_countは0のままで、OCCLUDED_NOT_AUTHORITYとして落ちること。
    'NegativeOccludedPresent' {
        $raw.present_occluded_count = 1
        $raw.present_outcome_authority_exact = $false
        $raw.present_outcome_code = 'OCCLUDED_NOT_AUTHORITY'
    }
    'NegativeUnclassifiedPresentStatus' {
        $raw.present_unclassified_status_count = 1
        $raw.present_outcome_authority_exact = $false
        $raw.present_outcome_code = 'UNCLASSIFIED_PRESENT_STATUS'
    }
    # countは0なのにauthority flagだけfalse。自己申告の不整合を捕まえる。
    'NegativePresentOutcomeAuthority' { $raw.present_outcome_authority_exact = $false }
    'NegativeWindowVisibility' { $raw.window_visibility_precondition = $false }
    'NegativeDebugTimingApplied' {
        $raw.authority_mode = 'CORRECTNESS_ONLY'
        $raw.timing_verdict = 'PASS'
    }
    'NegativeReleaseTimingDisabled' {
        $raw.authority_mode = 'FULL_RELEASE'
        $raw.timing_verdict = 'NOT_AUTHORITY_IN_DEBUG'
    }
    # debug splitを入れてもcorrectnessは緩まないことの証明。
    # correctness_verdict fieldはPASSのままにし、checkerが再計算で捕まえることを要求する。
    'NegativeCorrectnessRelaxedInDebug' {
        $raw.authority_mode = 'CORRECTNESS_ONLY'
        $raw.timing_verdict = 'NOT_AUTHORITY_IN_DEBUG'
        $raw.statistics_transitions[2].present_count = 104
    }
}

$afterMutation = $raw | ConvertTo-Json -Depth 10 -Compress
if ($Case -notlike 'Good*' -and $beforeMutation -ceq $afterMutation) {
    throw "negative mutationが適用されませんでした: $Case"
}

$raw | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Output -Encoding utf8
try {
    & $Checker -Json $Output -AuthorityMode $authorityMode *> $null
    $actualViolation = $null
} catch {
    $actualViolation = $_.Exception.Message
}
if ($Case -like 'Good*') {
    if ($null -ne $actualViolation) { throw "対照群が失敗しました: $actualViolation" }
} elseif ($null -eq $actualViolation) {
    throw "negative caseをcheckerが受理しました: $Case"
} elseif ($actualViolation -cne $expectedViolation[$Case]) {
    throw "意図しないviolationです: case=$Case expected=$($expectedViolation[$Case]) actual=$actualViolation"
}
