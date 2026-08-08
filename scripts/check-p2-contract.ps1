param(
    [Parameter(Mandatory = $true)][string]$Json,
    [Parameter(Mandatory = $true)][ValidateSet('Playback', 'Seek')][string]$Mode,
    [int]$ProcessExitCode = 0,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

function Require-Property($Object, [string]$Name) {
    if ($Object.PSObject.Properties.Name -notcontains $Name) {
        Add-Failure "必須fieldがありません: $Name"
        return $null
    }
    return $Object.$Name
}

function Require-Equal($Actual, $Expected, [string]$Name) {
    if ($null -eq $Actual -or $Actual -ne $Expected) {
        Add-Failure "$Name は $Expected である必要があります (actual=$Actual)"
    }
}

function Require-Zero($Object, [string]$Name) {
    $value = Require-Property $Object $Name
    if ($null -ne $value -and [double]$value -ne 0) {
        Add-Failure "$Name は0である必要があります (actual=$value)"
    }
}

try {
    $raw = Get-Content -LiteralPath $Json -Raw -Encoding utf8 | ConvertFrom-Json
} catch {
    Write-Error "P2 raw JSONを読めません: $($_.Exception.Message)"
    exit 2
}

Require-Equal (Require-Property $raw 'schema') 'mvm-p2-formal-1' 'schema'
Require-Equal (Require-Property $raw 'formal_contract_version') 'P2-D1-1' 'formal_contract_version'
Require-Equal (Require-Property $raw 'mode') $Mode.ToLowerInvariant() 'mode'
Require-Equal (Require-Property $raw 'process_exit_code') 0 'JSON process_exit_code'
Require-Equal $ProcessExitCode 0 '実process exit code'
Require-Equal (Require-Property $raw 'formal_preflight') $true 'formal_preflight'
Require-Equal (Require-Property $raw 'same_device_a') $true 'same_device_a'
Require-Equal (Require-Property $raw 'same_device_b') $true 'same_device_b'
Require-Equal (Require-Property $raw 'actual_output_width') 1920 'actual_output_width'
Require-Equal (Require-Property $raw 'actual_output_height') 1080 'actual_output_height'
Require-Equal (Require-Property $raw 'actual_gpu_completion_backend') 'fence' `
    'actual_gpu_completion_backend'
Require-Equal (Require-Property $raw 'configured_seed') 20260808 'configured_seed'
$configuredWarmup = Require-Property $raw 'configured_warmup_seconds'
$configuredMeasure = Require-Property $raw 'configured_measure_seconds'
$configuredSeekCount = Require-Property $raw 'configured_seek_count'
if (-not $DryRun) {
    Require-Equal $configuredWarmup 5 'configured_warmup_seconds'
    Require-Equal $configuredMeasure 60 'configured_measure_seconds'
    Require-Equal $configuredSeekCount 1000 'configured_seek_count'
}

Require-Equal (Require-Property $raw 'marker_a_checked_count') 7 'marker_a_checked_count'
Require-Equal (Require-Property $raw 'marker_b_checked_count') 7 'marker_b_checked_count'
Require-Zero $raw 'marker_a_mismatch'
Require-Zero $raw 'marker_b_mismatch'
Require-Equal (Require-Property $raw 'actual_target_probe_checked_count') 4 'actual_target_probe_checked_count'
Require-Zero $raw 'actual_target_probe_mismatch'
Require-Zero $raw 'mixed_source_frame_count'
Require-Zero $raw 'mixed_generation_count'
Require-Zero $raw 'stale_composition_epoch_count'
Require-Zero $raw 'cpu_full_frame_readback_count'
Require-Zero $raw 'full_frame_gpu_copy_count'
Require-Zero $raw 'payloads_released_before_completion'
Require-Zero $raw 'retirement_timeout_count'
Require-Zero $raw 'retirement_depth_after_drain'
Require-Zero $raw 'device_lost_count'
Require-Zero $raw 'lifecycle_order_violation_count'
Require-Equal (Require-Property $raw 'teardown_success') $true 'teardown_success'
Require-Equal (Require-Property $raw 'final_report_written_after_teardown') $true 'final_report_written_after_teardown'

if ($Mode -eq 'Playback') {
    $scheduled = Require-Property $raw 'measurement_scheduled_output_count'
    $displayed = Require-Property $raw 'measurement_displayed_composition_count'
    $dropped = Require-Property $raw 'measurement_dropped_output_count'
    $submission = Require-Property $raw 'measurement_gpu_submission_count'
    $layers = Require-Property $raw 'measurement_layer_draw_count'
    $clears = Require-Property $raw 'measurement_logical_clear_count'
    $reasons = @(
        'measurement_drop_scheduler_deadline',
        'measurement_drop_missing_source_a',
        'measurement_drop_missing_source_b',
        'measurement_drop_missing_both',
        'measurement_drop_stale_generation',
        'measurement_drop_future_generation',
        'measurement_drop_stale_composition_epoch',
        'measurement_drop_render_failure'
    )
    $reasonSum = 0L
    foreach ($name in $reasons) {
        $value = Require-Property $raw $name
        if ($null -ne $value) { $reasonSum += [long]$value }
    }
    if ($null -ne $scheduled -and $null -ne $displayed -and $null -ne $dropped) {
        Require-Equal ([long]$scheduled) ([long]$displayed + [long]$dropped) 'scheduled == displayed + dropped'
        Require-Equal $reasonSum ([long]$dropped) 'drop reason sum == dropped'
    }
    if ($null -ne $submission -and $null -ne $displayed) {
        Require-Equal ([long]$submission) ([long]$displayed) 'gpu submission == displayed'
    }
    if ($null -ne $layers -and $null -ne $displayed) {
        Require-Equal ([long]$layers) ([long]$displayed * 2) 'layer draw == displayed * 2'
    }
    if ($null -ne $clears -and $null -ne $displayed) {
        Require-Equal ([long]$clears) ([long]$displayed) 'logical clear == displayed'
    }
    Require-Zero $raw 'measurement_untracked_submission_count'
    Require-Zero $raw 'measurement_completion_poll_failure_count'
    Require-Zero $raw 'measurement_partial_gpu_issue_failure_count'
    if (-not $DryRun) {
        $fps = Require-Property $raw 'effective_fps'
        $dropRate = Require-Property $raw 'drop_rate'
        if ($null -ne $fps -and [double]$fps -lt 55) {
            Add-Failure "effective_fpsは55以上が必要です (actual=$fps)"
        }
        if ($null -ne $dropRate -and [double]$dropRate -gt 0.02) {
            Add-Failure "drop_rateは0.02以下が必要です (actual=$dropRate)"
        }
    }
} else {
    $displayedValues = @(Require-Property $raw 'dual_seek_displayed_ms')
    $decodeValues = @(Require-Property $raw 'dual_seek_decode_ready_ms')
    $expectedCount = if ($DryRun) { [int]$configuredSeekCount } else { 1000 }
    Require-Equal $displayedValues.Count $expectedCount 'dual_seek_displayed_ms.Count'
    Require-Equal $decodeValues.Count $expectedCount 'dual_seek_decode_ready_ms.Count'
    Require-Zero $raw 'seek_display_mismatch'
    Require-Zero $raw 'seek_timeout_count'
    Require-Zero $raw 'untracked_submission_count'
    Require-Zero $raw 'completion_poll_failure_count'

    if ($displayedValues.Count -gt 0) {
        [double[]]$sorted = $displayedValues | ForEach-Object { [double]$_ } | Sort-Object
        $index = [math]::Ceiling($sorted.Count * 0.95) - 1
        $nearestRank = $sorted[$index]
        $recordedP95 = [double](Require-Property $raw 'dual_seek_displayed_p95_ms')
        $recordedMax = [double](Require-Property $raw 'dual_seek_displayed_observed_max_ms')
        if ([math]::Abs($nearestRank - $recordedP95) -gt 1e-9) {
            Add-Failure "nearest-rank p95がraw配列と一致しません (recalculated=$nearestRank recorded=$recordedP95)"
        }
        if ([math]::Abs($sorted[-1] - $recordedMax) -gt 1e-9) {
            Add-Failure "observed maxがraw配列と一致しません (recalculated=$($sorted[-1]) recorded=$recordedMax)"
        }
        if (-not $DryRun) {
            if ($recordedP95 -gt 150) {
                Add-Failure "dual seek p95は150ms以下が必要です (actual=$recordedP95)"
            }
            if ($recordedMax -gt 400) {
                Add-Failure "dual seek observed maxは400ms以下が必要です (actual=$recordedMax)"
            }
        }
    }
}

if ($failures.Count -ne 0) {
    foreach ($failure in $failures) { [Console]::Error.WriteLine($failure) }
    exit 3
}

$kind = if ($DryRun) { 'dry-run harness' } else { 'formal' }
Write-Host "P2 $kind contract: PASS ($Mode)"
exit 0
