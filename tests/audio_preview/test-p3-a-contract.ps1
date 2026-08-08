param(
    [ValidateSet('Good','NegativeCounter')][string]$Case,
    [string]$Checker,
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$base = [ordered]@{
    schema_version=1; phase='P3-A'; mode='playback'; pass=$true
    device_failure_count=0; clock_regression_count=0; clock_generation_mismatch_count=0
    clock_query_failure_count=0; overflow_reject_count=0; stale_generation_reject_count=0
    audio_render_thread_join_leak=0; audio_decode_thread_join_leak=0
    audio_device_release_before_join=0; audio_lifecycle_violation=0
    queue_target_ms=250; queue_hard_max_ms=500; audio_preroll_ms=100
    elapsed_seconds=15.0; underflow_count=0; seek_requested_count=0
    exact_seek_count=0; seek_timeout_count=0; pause_clock_frozen=$false
    pause_resume_continuous=$false; audio_marker_matches=0
    source_sample_rate=48000; source_channels=2; seeks=@()
}
function Write-Case([string]$name, [string]$mode) {
    $value = [ordered]@{}
    foreach ($entry in $base.GetEnumerator()) { $value[$entry.Key] = $entry.Value }
    $value.mode = $mode
    if ($mode -eq 'seek') {
        $value.seek_requested_count=64; $value.exact_seek_count=64
        $value.seeks = @(1..64 | ForEach-Object {
            [ordered]@{ requested_sample=$_; first_output_sample=$_; seek_generation=$_ + 1
                discarded_preroll_samples=0; latency_ms=1.0 }
        })
    }
    if ($mode -eq 'pause-resume') { $value.pause_clock_frozen=$true; $value.pause_resume_continuous=$true }
    if ($mode -eq 'fixture') { $value.audio_marker_matches=6 }
    if ($Case -eq 'NegativeCounter' -and $name -eq 'playback-2') {
        $value.clock_regression_count = 1
    }
    $path = Join-Path $OutputDir "$name.json"
    $value | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $path -Encoding utf8
    return $path
}
$plays = @(1..3 | ForEach-Object { Write-Case "playback-$_" 'playback' })
$seek = Write-Case 'seek' 'seek'
$pause = Write-Case 'pause' 'pause-resume'
$fixture = Write-Case 'fixture' 'fixture'
$summary = Join-Path $OutputDir 'summary.json'
& pwsh -NoProfile -File $Checker -PlaybackDirectory $OutputDir -Seek $seek `
    -PauseResume $pause -Fixture $fixture -Output $summary
$actual = $LASTEXITCODE
$expected = if ($Case -eq 'Good') { 0 } else { 3 }
if ($actual -ne $expected) {
    throw "checker exit 不一致: expected=$expected actual=$actual case=$Case"
}
