param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good', 'NegativeCount', 'NegativeOrder', 'NegativeSwapChain', 'NegativeSyncInterval',
        'NegativeWindow', 'NegativeLostEvent', 'NegativeRawQpc', 'NegativeBoundary',
        'NegativePhysicalAuthority', 'CollisionNotObservedGood')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$VBlankChecker,
    [Parameter(Mandatory=$true)][string]$Directory
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Directory | Out-Null
$appPath = Join-Path $Directory 'app.json'
$etwPath = Join-Path $Directory 'etw.json'
$oraclePath = Join-Path $Directory 'oracle.json'

$samples = @()
for ($index = 0; $index -lt 130; ++$index) {
    $samples += [ordered]@{ ordinal=$index; qpc=1000 + 100 * $index }
}
$identity = [ordered]@{
    available=$true; monitor_handle='1'; output_index=0; adapter_luid_low=1; adapter_luid_high=0
    gdi_device_name='DISPLAY1'; output_device_name='DISPLAY1'; refresh_numerator=10
    refresh_denominator=1; desktop_left=0; desktop_top=0; desktop_right=1920; desktop_bottom=1080
}
$app = [ordered]@{
    schema='mvm-p2-formal-2'
    presentation_opportunity=[ordered]@{
        enabled=$true; measurement_start_qpc=1100; measurement_end_qpc_exclusive=1300
        qpc_frequency=1000; swap_record_count=2; swap_overflow_count=0
        physical_vblank=[ordered]@{
            enabled=$true; observer_started=$true; observer_error=''; time_critical_priority=$true
            window_output_start=$identity; window_output_end=$identity; window_output_stable=$true
            sample_count=130; ring_overflow_count=0; wait_failure_count=0; sequence_status='OK'
            interval_report_ok=$true; interval_count=129; long_interval_count=0
            short_interval_count=0; nominal_period_qpc=100; cumulative_consistent=$true
            samples=$samples
        }
        swap_records=@(
            [ordered]@{swap_qpc=1150;swap_ordinal=0;completed_render_ordinal=0;submitted_render_ordinal=0;presented_output_frame=10},
            [ordered]@{swap_qpc=1160;swap_ordinal=1;completed_render_ordinal=1;submitted_render_ordinal=1;presented_output_frame=11}
        )
    }
}
$etw = [ordered]@{
    schema='mvm-p2-etw-present-history-1'; raw_displayed_qpc=$true
    qpc_frequency=1000; target_process_id=42; etw_events_lost=0; etw_buffers_lost=0
    present_event_overflow_count=0; collision_evidence_mode='ACTUAL_QT'
    cadence_diagnostic=[ordered]@{ traced_swaps_per_second=10.0; baseline_swaps_per_second=$null; ratio=$null; extreme_change=$null }
    events=@(
        [ordered]@{
            sequence_index=0;present_start_qpc=1140;process_id=42;thread_id=7
            swap_chain_address='0xabc';window_handle='0x123';sync_interval=1;present_flags=0
            displayed=@([ordered]@{frame_type='Application';qpc=1100})
            present_ids=@([ordered]@{vidpn_layer_id='0x1';present_id=100})
        },
        [ordered]@{
            sequence_index=1;present_start_qpc=1155;process_id=42;thread_id=7
            swap_chain_address='0xabc';window_handle='0x123';sync_interval=1;present_flags=0
            displayed=@([ordered]@{frame_type='Application';qpc=1200})
            present_ids=@([ordered]@{vidpn_layer_id='0x1';present_id=101})
        }
    )
}

switch ($Case) {
    'NegativeCount' { $etw.events = @($etw.events[0]) }
    'NegativeOrder' { $etw.events[1].sequence_index = 3 }
    'NegativeSwapChain' { $etw.events[1].swap_chain_address = '0xdef' }
    'NegativeWindow' { $etw.events[1].window_handle = '0x456' }
    'NegativeSyncInterval' { $etw.events[1].sync_interval = 0 }
    'NegativeLostEvent' { $etw.etw_events_lost = 1 }
    'NegativeRawQpc' { $etw.raw_displayed_qpc = $false }
    'NegativeBoundary' { $etw.events[0].displayed[0].qpc = 1150 }
    'NegativePhysicalAuthority' { $app.presentation_opportunity.physical_vblank.short_interval_count = 1 }
    'CollisionNotObservedGood' { $etw.events[1].displayed[0].qpc = 1100 }
}

$app | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $appPath -Encoding utf8
$etw | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $etwPath -Encoding utf8
& pwsh -NoProfile -File $Checker -AppJson $appPath -EtwJson $etwPath -Output $oraclePath `
    -VBlankChecker $VBlankChecker *> $null
$exitCode = $LASTEXITCODE
if ($Case -in @('Good', 'CollisionNotObservedGood')) {
    if ($exitCode -ne 0) { throw "対照群が失敗しました: exit=$exitCode" }
    $oracle = Get-Content -Raw -LiteralPath $oraclePath | ConvertFrom-Json
    if ($oracle.oracle_status -ne 'ORACLE_VALID' -or @($oracle.records).Count -ne 2 -or
        $oracle.mapper_proof_status -ne 'NOT_YET_EVALUABLE') {
        throw '対照群のoracle出力が契約と一致しません'
    }
    $expectedCollision = if ($Case -eq 'Good') { 'COLLISION_RESOLVED' } else {
        'COLLISION_NOT_OBSERVED'
    }
    if ($oracle.collision_evidence_status -ne $expectedCollision) {
        throw "collision状態が一致しません: $($oracle.collision_evidence_status)"
    }
} elseif ($exitCode -eq 0) {
    throw "negative caseをcheckerが受理しました: $Case"
}
