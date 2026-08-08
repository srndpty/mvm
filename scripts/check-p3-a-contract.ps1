<# P3-A smoke の raw JSON 契約を一箇所で検査し、raw から summary を生成する。 #>
[CmdletBinding()]
param(
    [string[]]$Playback = @(),
    [string]$PlaybackDirectory = '',
    [Parameter(Mandatory)][string]$Seek,
    [Parameter(Mandatory)][string]$PauseResume,
    [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($PlaybackDirectory) {
    $Playback = @(Get-ChildItem -LiteralPath $PlaybackDirectory -Filter 'playback-*.json' |
        Sort-Object Name | ForEach-Object FullName)
}

$errors = [Collections.Generic.List[string]]::new()
function Read-Result([string]$path, [string]$mode) {
    if (-not (Test-Path -LiteralPath $path)) { throw "raw JSON がありません: $path" }
    $value = Get-Content -Raw -LiteralPath $path -Encoding utf8 | ConvertFrom-Json
    if ($value.schema_version -ne 1 -or $value.phase -ne 'P3-A' -or $value.mode -ne $mode) {
        $script:errors.Add("schema/phase/mode 不一致: $path")
    }
    if (-not $value.pass) { $script:errors.Add("smoke が pass ではありません: $path") }
    foreach ($field in @('device_failure_count','clock_regression_count',
            'clock_generation_mismatch_count','clock_query_failure_count',
            'overflow_reject_count','stale_generation_reject_count',
            'audio_render_thread_join_leak','audio_decode_thread_join_leak',
            'audio_device_release_before_join','audio_lifecycle_violation')) {
        if ([long]$value.$field -ne 0) { $script:errors.Add("$field が 0 ではありません: $path") }
    }
    if ($value.queue_target_ms -ne 250 -or $value.queue_hard_max_ms -ne 500 -or
        $value.audio_preroll_ms -ne 100) {
        $script:errors.Add("固定 queue/pre-roll contract 不一致: $path")
    }
    return $value
}

if ($Playback.Count -ne 3) { $errors.Add("playback raw は 3 process 必須です: $($Playback.Count)") }
$playbackValues = @($Playback | ForEach-Object { Read-Result $_ 'playback' })
foreach ($value in $playbackValues) {
    if ([double]$value.elapsed_seconds -lt 15.0) { $errors.Add('playback が 15 秒未満です') }
}
$seekValue = Read-Result $Seek 'seek'
if ($seekValue.seek_requested_count -ne 64 -or $seekValue.exact_seek_count -ne 64 -or
    $seekValue.seek_timeout_count -ne 0 -or $seekValue.underflow_count -ne 0) {
    $errors.Add('exact seek の 64/64・timeout・underflow contract 不一致です')
}
if (@($seekValue.seeks).Count -ne 64) {
    $errors.Add("per-seek telemetry は 64 entry 必須です: $(@($seekValue.seeks).Count)")
} else {
    foreach ($entry in $seekValue.seeks) {
        if ($entry.requested_sample -ne $entry.first_output_sample -or
            [long]$entry.seek_generation -le 1 -or
            [long]$entry.discarded_preroll_samples -lt 0) {
            $errors.Add('per-seek exact/generation/preroll telemetry 不一致です')
            break
        }
    }
}
$pauseValue = Read-Result $PauseResume 'pause-resume'
if (-not $pauseValue.pause_clock_frozen -or -not $pauseValue.pause_resume_continuous) {
    $errors.Add('pause/resume continuity contract 不一致です')
}
$fixtureValue = Read-Result $Fixture 'fixture'
if ($fixtureValue.audio_marker_matches -ne 6 -or $fixtureValue.source_sample_rate -ne 48000 -or
    $fixtureValue.source_channels -ne 2) {
    $errors.Add('fixture marker/source format contract 不一致です')
}

$summary = [ordered]@{
    schema_version = 1
    phase = 'P3-A'
    verdict = if ($errors.Count -eq 0) { 'PASS' } else { 'FAIL' }
    formal_p3_verdict = 'NOT_RUN'
    playback = $playbackValues
    exact_seek = $seekValue
    pause_resume = $pauseValue
    fixture = $fixtureValue
    errors = @($errors)
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
    exit 3
}
Write-Host "P3-A smoke contract: PASS -> $Output" -ForegroundColor Green
