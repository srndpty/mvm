[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$CandidateWorktree = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\p2-q1-candidate'),
    [string]$CandidateExecutable,
    [ValidateRange(1, 6)]
    [int]$MaxRuns = 6,
    [switch]$PreflightOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'P2-Q2 ETW採取には管理者権限が必要です。昇格したPowerShellから実行してください'
}

$repo = Split-Path -Parent $PSScriptRoot
$candidateSha = '31eda0d8d080dcf4b1680149d85b8293f618cd57'
$q1ExecutedExeSha = '78cde287e6a91a6413adf90723eea87c36bae9471b7fff87e8879aa822bd283a'
$candidateExeSha = 'efd2e11b55d421fd6b5c4766f19ca1732c932f07038ad9909eb9a940c5f8b6b7'
$candidateTextSha = '273ef3d76b2df29b212e11dd9ba959e136002c15957b765f67ca61bd75665af0'
$checkerSha = '83e9e812c61b6c7cf611a08ccdd12fac4126367b2dcd580269f068f04e85f381'
$sourceASha = '4282e53bbab814962410e4bc99ee0ae6015aeb11b39c65076d15879c6180cbe4'
$sourceBSha = 'ea528d0c1739d85e40521af2f16c3d6e3a5aee6df68ebeedda424302ad70ffc1'
$candidateWorktree = (Resolve-Path -LiteralPath $CandidateWorktree).Path
if (-not $CandidateExecutable) {
    $CandidateExecutable = Join-Path $repo 'build\p2-q2-pinned-31eda0d\mvm_compositor_spike.exe'
}
$exe = $CandidateExecutable
$checker = Join-Path $PSScriptRoot 'check-p2-contract.ps1'
$sourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'

foreach ($required in @($exe, $checker, $sourceA, $sourceB)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2-Q2必須ファイルがありません: $required" }
}
$exe = (Resolve-Path -LiteralPath $exe).Path
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "既存artifactを上書きしません: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-GitText([string[]]$Arguments) {
    $value = & git -C $candidateWorktree @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

function Assert-Identity {
    $actualSha = Invoke-GitText @('rev-parse', 'HEAD')
    $status = Invoke-GitText @('status', '--porcelain')
    if ($actualSha -ne $candidateSha) { throw "candidate SHAが不一致です: $actualSha" }
    if ($status) { throw "candidate worktreeがcleanではありません: $status" }
    $identities = [ordered]@{
        executable = @{ path = $exe; expected = $candidateExeSha; actual = Get-Sha256 $exe }
        checker = @{ path = $checker; expected = $checkerSha; actual = Get-Sha256 $checker }
        source_a = @{ path = $sourceA; expected = $sourceASha; actual = Get-Sha256 $sourceA }
        source_b = @{ path = $sourceB; expected = $sourceBSha; actual = Get-Sha256 $sourceB }
    }
    foreach ($name in $identities.Keys) {
        if ($identities[$name].actual -ne $identities[$name].expected) {
            throw "$name SHA-256がQ1 provenanceと一致しません"
        }
    }
    return $identities
}

if (-not ('MvmSystemPowerStatus' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MvmSystemPowerStatus {
    [StructLayout(LayoutKind.Sequential)]
    public struct Status {
        public byte ACLineStatus;
        public byte BatteryFlag;
        public byte BatteryLifePercent;
        public byte SystemStatusFlag;
        public int BatteryLifeTime;
        public int BatteryFullLifeTime;
    }
    [DllImport("kernel32.dll")]
    public static extern bool GetSystemPowerStatus(out Status status);
}
'@
}

function Get-ExternalProvenance {
    $power = [MvmSystemPowerStatus+Status]::new()
    $powerOk = [MvmSystemPowerStatus]::GetSystemPowerStatus([ref]$power)
    $video = @(Get-CimInstance Win32_VideoController | ForEach-Object {
        [ordered]@{
            name = $_.Name
            pnp_device_id = $_.PNPDeviceID
            driver_version = $_.DriverVersion
            driver_date = if ($_.DriverDate) { $_.DriverDate.ToUniversalTime().ToString('o') } else { $null }
            current_refresh_rate_hz = $_.CurrentRefreshRate
            current_width = $_.CurrentHorizontalResolution
            current_height = $_.CurrentVerticalResolution
            status = $_.Status
        }
    })
    $scheme = (& powercfg.exe /getactivescheme 2>&1) -join "`n"
    return [ordered]@{
        utc = (Get-Date).ToUniversalTime().ToString('o')
        qpc = [Diagnostics.Stopwatch]::GetTimestamp()
        qpc_frequency = [Diagnostics.Stopwatch]::Frequency
        user = $identity.Name
        elevated = $true
        os = [ordered]@{
            version = [Environment]::OSVersion.VersionString
            build = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
        }
        power = [ordered]@{
            query_succeeded = $powerOk
            ac_line_status = if ($powerOk) { [int]$power.ACLineStatus } else { $null }
            battery_flag = if ($powerOk) { [int]$power.BatteryFlag } else { $null }
            battery_percent = if ($powerOk) { [int]$power.BatteryLifePercent } else { $null }
            active_scheme = $scheme.Trim()
        }
        video_controllers = $video
    }
}

function Start-Etw([string]$Label) {
    & wpr.exe -start GeneralProfile -start GPU -start DesktopComposition -filemode
    if ($LASTEXITCODE -ne 0) { throw "WPR startに失敗しました: $Label" }
    & wpr.exe -marker "P2-Q2 $Label process-start" | Out-Null
}

function Stop-Etw([string]$Label, [string]$EtlPath) {
    & wpr.exe -marker "P2-Q2 $Label process-end" | Out-Null
    & wpr.exe -stop $EtlPath "P2-Q2 $Label"
    if ($LASTEXITCODE -ne 0) { throw "WPR stopに失敗しました: $Label" }
}

$identities = Assert-Identity
$runnerSha = Get-Sha256 $PSCommandPath
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $OutputDirectory 'runner.ps1')
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

if ($PreflightOnly) {
    $etl = Join-Path $OutputDirectory 'preflight.etl'
    Start-Etw 'preflight'
    Start-Sleep -Seconds 2
    Stop-Etw 'preflight' $etl
    [ordered]@{
        authority = 'diagnostic_preflight_only'
        binary_provenance_note = 'Q1実走後のsection抽出が元PEを再書込したため、Q1と同一.textを持つ31eda0d PEをQ2用に固定した'
        q1_executed_executable_sha256 = $q1ExecutedExeSha
        q1_and_q2_text_sha256 = $candidateTextSha
        runner_sha256 = $runnerSha
        identities = $identities
        provenance = Get-ExternalProvenance
        etl_sha256 = Get-Sha256 $etl
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'preflight.json') -Encoding utf8
    Write-Host "P2-Q2 ETW preflight完了: $OutputDirectory"
    exit 0
}

$runs = [System.Collections.Generic.List[object]]::new()
$passSeen = $false
$failSeen = $false

function Write-Summary {
    [ordered]@{
        schema = 'mvm.p5-e4-p2-q2-etw.v1'
        authority = 'diagnostic_only_not_closure_evidence'
        candidate_sha = $candidateSha
        binary_provenance_note = 'Q1実走後のsection抽出が元PEを再書込したため、Q1 full-file hashは再利用不能。Q1と同一.textを持つ31eda0d PEをQ2開始前に別pathへ固定し、全Q2 runで同一full-file hashを検証する'
        q1_executed_executable_sha256 = $q1ExecutedExeSha
        q1_and_q2_text_sha256 = $candidateTextSha
        runner_sha256 = $runnerSha
        identities = $identities
        conditions = [ordered]@{
            mode = 'playback'
            warmup_seconds = 5
            measure_seconds = 60
            seed = 20260808
            seek_count = 1000
            display_timeout_ms = 2000
            gpu_completion = 'fence'
            formal_preflight = $true
            diagnostic_timing = $false
            wpr_profiles = @('GeneralProfile', 'GPU', 'DesktopComposition')
        }
        stop_rule = '最大6 run。PASS 1本とFAIL 1本を採取した時点で停止'
        pass_seen = $passSeen
        fail_seen = $failSeen
        runs = $runs
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
}

for ($index = 1; $index -le $MaxRuns; ++$index) {
    Assert-Identity | Out-Null
    $label = "run-$index"
    $runDirectory = Join-Path $OutputDirectory $label
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $rawPath = Join-Path $runDirectory 'playback.json'
    $etlPath = Join-Path $runDirectory 'trace.etl'
    $stdoutPath = Join-Path $runDirectory 'stdout.txt'
    $stderrPath = Join-Path $runDirectory 'stderr.txt'
    $start = Get-ExternalProvenance
    $start | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDirectory 'provenance-start.json') -Encoding utf8

    $arguments = @(
        '--source-a', $sourceA, '--source-b', $sourceB,
        '--metrics', $rawPath, '--warmup-seconds', '5',
        '--measure-seconds', '60', '--seed', '20260808',
        '--seek-count', '1000', '--display-timeout-ms', '2000',
        '--gpu-completion', 'fence', '--mode', 'playback',
        '--formal-preflight'
    )
    Write-Host "P2-Q2 candidate run $index/$MaxRuns を開始します"
    $process = $null
    $processExit = -1
    $timedOut = $false
    $etwStarted = $false
    try {
        Start-Etw $label
        $etwStarted = $true
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        if (-not $process.WaitForExit(180000)) {
            $timedOut = $true
            & taskkill.exe /PID $process.Id /T /F | Out-Null
            $process.WaitForExit()
        }
        $processExit = if ($timedOut) { 124 } else { $process.ExitCode }
    } finally {
        if ($etwStarted) { Stop-Etw $label $etlPath }
    }

    $end = Get-ExternalProvenance
    $end | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $runDirectory 'provenance-end.json') -Encoding utf8
    $contractExit = -1
    $checkerOutput = @()
    if (Test-Path -LiteralPath $rawPath) {
        $checkerOutput = @(& pwsh -NoProfile -File $checker -Json $rawPath -Mode Playback `
                -ProcessExitCode $processExit 2>&1)
        $contractExit = $LASTEXITCODE
    }
    $checkerOutput | Set-Content -LiteralPath (Join-Path $runDirectory 'checker.txt') -Encoding utf8
    if (-not (Test-Path -LiteralPath $rawPath)) { throw "$label はraw P2 JSONを生成しませんでした" }
    $raw = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
    $passed = $processExit -eq 0 -and $contractExit -eq 0
    $passSeen = $passSeen -or $passed
    $failSeen = $failSeen -or (-not $passed)
    $runs.Add([ordered]@{
        run = $index
        process_id = if ($process) { $process.Id } else { $null }
        process_exit_code = $processExit
        timed_out = $timedOut
        checker_exit_code = $contractExit
        contract_pass = $passed
        deadline_drop_count = [long]$raw.measurement_drop_scheduler_deadline
        drop_rate = [double]$raw.drop_rate
        effective_fps = [double]$raw.effective_fps
        present_callback_count = [long]$raw.measurement_present_callback_count
        repeated_present_count = [long]$raw.measurement_repeated_present_count
        decoded_a_count = [long]$raw.measurement_decoded_a_count
        decoded_b_count = [long]$raw.measurement_decoded_b_count
        start_qpc = $start.qpc
        end_qpc = $end.qpc
        etl_file = "$label/trace.etl"
        etl_sha256 = Get-Sha256 $etlPath
    })
    Write-Summary
    if ($passSeen -and $failSeen) { break }
}

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
$manifest = Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Where-Object FullName -ne $manifestPath | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    }
$manifest | Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "P2-Q2 ETW campaign完了: pass=$passSeen fail=$failSeen root=$OutputDirectory"
