[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$SourceA,
    [Parameter(Mandatory)][string]$SourceB,
    [Parameter(Mandatory)][string]$OutputDir,
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$metrics = Join-Path $OutputDir 'metrics.json'
$stdout = Join-Path $OutputDir 'stdout.txt'
$stderr = Join-Path $OutputDir 'stderr.txt'
Remove-Item -LiteralPath @($metrics, $stdout, $stderr) -Force -ErrorAction SilentlyContinue
$arguments = @(
    '--source-a', $SourceA,
    '--source-b', $SourceB,
    '--metrics', $metrics,
    '--mode', 'playback',
    '--duration-seconds', '60',
    '--warmup-seconds', '0',
    '--seek-count', '1000',
    '--seed', '20260808',
    '--display-timeout-ms', '3000',
    '--formal-contract-c2',
    '--inject-render-fault-after-playing'
)

$process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
$timedOut = $false
try {
    Wait-Process -Id $process.Id -Timeout $TimeoutSeconds -ErrorAction Stop
} catch {
    $timedOut = $true
} finally {
    if (!$process.HasExited) {
        # このtest自身が起動したPIDだけを停止する。
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $process.Id -Timeout 10 -ErrorAction SilentlyContinue
    }
}
if ($timedOut) {
    throw "async WASAPI failure後もprocessが${TimeoutSeconds}秒以内に終了しませんでした"
}
$process.Refresh()
if ($process.ExitCode -eq 0) {
    throw 'injected async WASAPI failureを成功終了として扱いました'
}
if (!(Test-Path -LiteralPath $metrics)) {
    throw 'async WASAPI failure後にmetrics JSONが生成されませんでした'
}

$record = Get-Content -Raw -LiteralPath $metrics | ConvertFrom-Json
$stderrText = Get-Content -Raw -LiteralPath $stderr
if ($record.pass -ne $false) { throw 'formal passがfalseではありません' }
if ($record.audio_device_failure_count -ne 1) {
    throw "deviceFailureCountが1ではありません: $($record.audio_device_failure_count)"
}
if ([string]::IsNullOrWhiteSpace([string]$record.audio_device_last_error) -or
    $record.audio_device_last_error -notmatch 'injected WASAPI render fault') {
    throw "injected render failure identityがmetricsにありません: $($record.audio_device_last_error)"
}
if ($record.detail -notmatch '^WASAPI runtime failure: .*injected WASAPI render fault') {
    throw "controller failure reasonが不正です: $($record.detail)"
}
if ($record.test_render_fault_injected_after_playing -ne $true) {
    throw 'sinkがplayingになった後のfault injectionを確認できません'
}
if ($record.shutdown_enter_observed -ne $true -or
    $record.shutdown_entered_from_playback -ne $true -or
    $stderrText -notmatch '\[p3\] shutdown \(failure\): WASAPI runtime failure:') {
    throw 'Playbackからfailure shutdownへ入った証拠がありません'
}
if ($record.teardown_success -ne $true -or $record.final_report_after_teardown -ne $true -or
    $record.audio_sink_joined -ne $true -or $record.video_worker_a_joined -ne $true -or
    $record.video_worker_b_joined -ne $true) {
    throw 'async WASAPI failure後のterminal teardownが完了していません'
}

Write-Host "PASS: async WASAPI failureをbounded fatal shutdownとして処理しました (exit=$($process.ExitCode))"
