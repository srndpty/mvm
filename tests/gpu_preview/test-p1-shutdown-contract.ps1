$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$checker = Join-Path $repo 'scripts/check-p1-contract.ps1'
$outDir = Join-Path $repo 'tmp/p1-shutdown-contract-test'
New-Item -ItemType Directory -Force $outDir | Out-Null

# まず teardown 後の正常 report が contract を通ることを対照群にする。
$d = [ordered]@{
    schema='mvm-p1-preview-4'; displayed_frames=10; decoded_frames=10; marker_checked=1
    frame_interval_samples=1; rhi_backend='D3D11'; multithread_protected=$true
    ffmpeg_adapter_known=$true; same_adapter=$true; same_device=$true
    qt_d3d11_device='0x1'; ffmpeg_d3d11_device='0x1'; cpu_full_frame_readback_count=0
    software_frame_rejects=0; marker_mismatch=0; seek_failures=0; seek_display_mismatch=0
    decode_errors=0; render_errors=0; device_lost_count=0; device_rejected=0; exit_code=0
    markers=@([ordered]@{requested=0; displayed=0; marker=0; sync_ok=$true; error=''})
    gpu_completion_backend='event_query'; payloads_released_before_completion=0
    untracked_submission_count=0; completion_poll_failure_count=0; gpu_completion_fatal=$false
    gpu_completion_fatal_reason=''; gpu_completion_device_removed_count=0
    retirement_timeout_count=0; forced_gpu_wait_count=0; gpu_completed_serial=10
    gpu_submitted_serial=10; retirement_depth_peak=3; retirement_depth_current=0
    retirement_depth_after_drain=0; teardown_success=$true
    lifecycle_order_violation_count=0; final_report_written_after_teardown=$true
    resource_epoch=1; srv_cache_entries_current=1; srv_cache_entries_peak=1
    srv_cache_texture_groups=1; source_id=1; source_generation=1; composition_epoch=1
    device_change_detected_count=0; device_change_handled_count=0
    device_change_fail_closed_count=0
    device_recovery_support='none (P1.2 は fail-closed のみ)'
    submitted_frames=10; pending_at_end=0; dropped_frames=0; stale_rejected=0
    future_rejected=0; invalid_rejected=0; generation_regression_rejected=0
    decode_failed=0; render_failed=0; retired_not_completed=0
    measure_elapsed_ms=1000; effective_fps=10
}

$good = Join-Path $outDir 'good.json'
$d | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $good
& $checker -Json $good
if ($LASTEXITCODE -ne 0) { throw "対照 JSON が contract を通りません (exit $LASTEXITCODE)" }

# shutdown 後の report だけを timeout 状態へ変える。旧 pre-shutdown 値を
# 使い続けていれば 0 のままなので、この negative test は落ちない。
$d.retirement_timeout_count = 1
$d.retirement_depth_after_drain = 2
$d.retirement_depth_current = 2
$d.retired_not_completed = 2
$d.teardown_success = $false
$bad = Join-Path $outDir 'drain-timeout.json'
$d | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $bad
& $checker -Json $bad
if ($LASTEXITCODE -ne 3) {
    throw "shutdown drain timeout の終了コードが 3 ではありません: $LASTEXITCODE"
}

Write-Host 'OK  teardown 後の timeout report を formal contract が拒否しました'
