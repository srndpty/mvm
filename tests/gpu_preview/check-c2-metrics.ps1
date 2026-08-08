param(
    [Parameter(Mandatory)][string]$Json,
    [Parameter(Mandatory)]
    [ValidateSet('device','copy','probe','shutdown','seek','layout')]
    [string]$Kind
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Json)) { throw "C2 metricsがありません: $Json" }
$m = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json
switch ($Kind) {
    'device' {
        if (-not $m.same_device_a -or -not $m.same_device_b) { throw 'A/B deviceがQt deviceと一致しません' }
    }
    'copy' {
        if ($m.full_frame_gpu_copy_count -ne 0 -or $m.cpu_full_frame_readback_count -ne 0) {
            throw 'full-frame copy/readbackが0ではありません'
        }
    }
    'probe' {
        if ($m.output_probe_readback_count -ne 4 -or $m.actual_target_probe_mismatch -ne 0) {
            throw 'actual target probe契約が不成立です'
        }
    }
    'shutdown' {
        if (-not $m.teardown_success -or -not $m.final_report_written_after_teardown -or
            $m.retirement_depth_after_drain -ne 0 -or $m.lifecycle_order_violation_count -ne 0) {
            throw 'shutdown契約が不成立です'
        }
    }
    'seek' {
        if ($m.dual_seek_displayed_ms.Count -ne 64 -or $m.seek_display_mismatch -ne 0 -or
            $m.seek_timeout_count -ne 0) { throw '64点actual-display seek契約が不成立です' }
    }
    'layout' {
        if ($m.layout_epoch_mismatch -ne 0 -or $m.stale_composition_epoch_count -ne 0) {
            throw 'layout epoch契約が不成立です'
        }
    }
}
