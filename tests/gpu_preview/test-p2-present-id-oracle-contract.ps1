param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good', 'NegativeSubmittedGap', 'NegativeObservedGap', 'NegativeFinalDrain',
        'NegativePollInterval', 'NegativeOfflineDelay', 'NegativeEarlyMapper',
        'NegativeDiscardClaim')][string]$Case,
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
    mapper_proof_status='NOT_YET_EVALUABLE'; flip_discard_frame_discard_claim='NOT_ESTABLISHED'
    sampling_gap_code='NONE'; qpc_frequency=10000000; nominal_period_qpc=1000
    configured_present_count=4; swap_effect='FLIP_DISCARD'; buffer_count=3; sync_interval=1
    dwm_flush_used=$false; dwm_flush_mode='DISABLED'; configured_submissions_complete=$true
    submitted_ids_consecutive=$true; observed_ids_complete=$true; final_drain_complete=$true
    final_submitted_present_id=103; final_observed_present_count=103
    present_failure_count=0; get_last_present_count_failure_count=0
    sampler_high_priority=$true; sampler_baseline_ready=$true; sampler_vblank_gap_count=0
    statistics_failure_count=0; statistics_disjoint_count=0; baseline_disjoint_count=0
    max_poll_interval_qpc=100; poll_interval_valid=$true
    window_output_stable=$true; statistics_output_matches_window=$true
    vblank_ring_overflow_count=0; vblank_wait_failure_count=0
    present_submissions=$submissions; statistics_transitions=$transitions; oracle_records=$oracle
    offline_notification_scenarios=@([ordered]@{
        delay_milli_periods=100; delay_qpc=100; records=$offline
    })
}

switch ($Case) {
    'NegativeSubmittedGap' { $raw.present_submissions[2].present_id = 104 }
    'NegativeObservedGap' { $raw.statistics_transitions[2].present_count = 104 }
    'NegativeFinalDrain' { $raw.final_observed_present_count = 102 }
    'NegativePollInterval' { $raw.max_poll_interval_qpc = 600 }
    'NegativeOfflineDelay' { $raw.offline_notification_scenarios[0].records[2].notify_qpc += 1 }
    'NegativeEarlyMapper' { $raw.mapper_proof_status = 'PASS' }
    'NegativeDiscardClaim' { $raw.flip_discard_frame_discard_claim = 'ESTABLISHED' }
}

$raw | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Output -Encoding utf8
& pwsh -NoProfile -File $Checker -Json $Output *> $null
$exitCode = $LASTEXITCODE
if ($Case -eq 'Good') {
    if ($exitCode -ne 0) { throw "対照群が失敗しました: exit=$exitCode" }
} elseif ($exitCode -eq 0) {
    throw "negative caseをcheckerが受理しました: $Case"
}
