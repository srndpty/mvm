[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$StopOnFailure,
    [string]$OutputDirectory,
    [string]$SourceA,
    [string]$SourceB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\ucrt64-release'
$exe = Join-Path $build 'bin\mvm_p3_av_sync_spike.exe'
$checker = Join-Path $PSScriptRoot 'check-p3-c-contract.ps1'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $build 'p3-matrix' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
foreach ($path in @($exe,$checker,$SourceA,$SourceB)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "P3 matrix 必須 file がありません: $path`nfixture は pwsh scripts/make-p3-fixture.ps1 で生成してください。"
    }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

function Git-Text([string[]]$Arguments) {
    $value = & git @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n")
}
function Get-Provenance {
    $status = Git-Text @('status','--porcelain')
    return [ordered]@{
        git_commit = Git-Text @('rev-parse','HEAD')
        dirty_worktree = -not [string]::IsNullOrWhiteSpace($status)
        git_status_porcelain = @($status -split "`n" | Where-Object {$_})
        fixture_a_sha256 = (Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
        fixture_b_sha256 = (Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
        executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        contract_version = 'P3-C-1'
    }
}
function Same-Provenance([object]$A, [object]$B) {
    return $A.git_commit -eq $B.git_commit -and
        $A.dirty_worktree -eq $B.dirty_worktree -and
        ($A.git_status_porcelain -join "`n") -eq ($B.git_status_porcelain -join "`n") -and
        $A.fixture_a_sha256 -eq $B.fixture_a_sha256 -and
        $A.fixture_b_sha256 -eq $B.fixture_b_sha256 -and
        $A.executable_sha256 -eq $B.executable_sha256 -and
        $A.contract_version -eq $B.contract_version
}

$start = Get-Provenance
if (-not $DryRun -and $start.dirty_worktree) {
    throw 'P3 formal matrix は clean worktree でのみ実行できます。commit 後に再実行してください'
}
$runCount = if ($DryRun) {1} else {3}
$warmup = if ($DryRun) {1} else {5}
$measurement = if ($DryRun) {5} else {60}
$seekCount = if ($DryRun) {64} else {1000}
$entries = [System.Collections.Generic.List[object]]::new()
$failed = $false

foreach ($mode in @('playback','seek','pause-resume')) {
    foreach ($index in 1..$runCount) {
        $path = Join-Path $OutputDirectory "$mode-run$index.json"
        if (Test-Path -LiteralPath $path) {
            $stamp = Get-Date -Format 'yyyyMMdd-HHmmssfff'
            Move-Item -LiteralPath $path -Destination "$path.previous-$stamp"
        }
        $arguments = @('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$path,
            '--mode',$mode,'--duration-seconds',$measurement,'--warmup-seconds',$warmup,
            '--seek-count',$seekCount,'--seed','20260808','--display-timeout-ms','3000',
            '--formal-contract')
        Write-Host "P3-C $mode run $index/$runCount を開始します" -ForegroundColor Cyan
        & $exe @arguments
        $processExit = $LASTEXITCODE
        $contractExit = -1
        if (Test-Path -LiteralPath $path) {
            if ($DryRun) {
                & pwsh -NoProfile -File $checker -Json $path -Mode $mode `
                    -ProcessExitCode $processExit -DryRun
            } else {
                & pwsh -NoProfile -File $checker -Json $path -Mode $mode `
                    -ProcessExitCode $processExit
            }
            $contractExit = $LASTEXITCODE
        }
        $raw = if (Test-Path -LiteralPath $path) {
            Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        } else {$null}
        $pass = $processExit -eq 0 -and $contractExit -eq 0
        if (-not $pass) {$failed=$true}
        $entries.Add([pscustomobject]@{mode=$mode;run=$index;raw_path=$path
            process_exit_code=$processExit;contract_exit_code=$contractExit;pass=$pass;raw=$raw})
        if (-not $pass -and $StopOnFailure) {break}
    }
    if ($failed -and $StopOnFailure) {break}
}

$end = Get-Provenance
$allExpected = $entries.Count -eq 3 * $runCount
$allPass = $allExpected -and @($entries | Where-Object {-not $_.pass}).Count -eq 0
$rawEntries = @($entries | Where-Object raw)
$firstRaw = @($rawEntries | Select-Object -First 1).raw
$lastRaw = @($rawEntries | Select-Object -Last 1).raw
$hardwareSignatures = @($rawEntries | ForEach-Object {
    $raw=$_.raw
    foreach ($name in @('adapter','audio_endpoint_sample_rate','audio_endpoint_channels',
                         'audio_endpoint_sample_format')) {
        if ($null -eq $raw.PSObject.Properties[$name] -or $null -eq $raw.$name) {
            throw "P3 raw に provenance field がありません: $name ($($_.raw_path))"
        }
    }
    "$($raw.adapter)|$($raw.audio_endpoint_sample_rate)|$($raw.audio_endpoint_channels)|$($raw.audio_endpoint_sample_format)"
} | Select-Object -Unique)
$hardwareUnchanged = $rawEntries.Count -eq $entries.Count -and $hardwareSignatures.Count -eq 1
if ($firstRaw) {
    $start['gpu_adapter']=$firstRaw.adapter
    $start['audio_endpoint']=[ordered]@{sample_rate=$firstRaw.audio_endpoint_sample_rate
        channels=$firstRaw.audio_endpoint_channels;sample_format=$firstRaw.audio_endpoint_sample_format}
}
if ($lastRaw) {
    $end['gpu_adapter']=$lastRaw.adapter
    $end['audio_endpoint']=[ordered]@{sample_rate=$lastRaw.audio_endpoint_sample_rate
        channels=$lastRaw.audio_endpoint_channels;sample_format=$lastRaw.audio_endpoint_sample_format}
}
$unchanged = (Same-Provenance $start $end) -and $hardwareUnchanged
if (-not $unchanged) {$failed=$true}
$summary = [ordered]@{
    schema='mvm-p3-matrix-summary-1'; contract_version='P3-C-1'
    formal_verdict=$(if ($DryRun) {'NOT_RUN'} elseif ($allPass -and $unchanged) {'PASS'} else {'FAIL'})
    dry_run=[bool]$DryRun; expected_processes=3*$runCount; completed_processes=$entries.Count
    start_provenance=$start; end_provenance=$end; provenance_unchanged=$unchanged
    hardware_provenance_unchanged=$hardwareUnchanged
    adapter=$(if ($firstRaw) {$firstRaw.adapter} else {$null})
    audio_endpoint=$(if ($firstRaw) {[ordered]@{sample_rate=$firstRaw.audio_endpoint_sample_rate
        channels=$firstRaw.audio_endpoint_channels;sample_format=$firstRaw.audio_endpoint_sample_format}} else {$null})
    runs=@($entries | ForEach-Object {[ordered]@{mode=$_.mode;run=$_.run;raw_path=$_.raw_path
        process_exit_code=$_.process_exit_code;contract_exit_code=$_.contract_exit_code;pass=$_.pass}})
    all_runs_pass=$allPass; p3_c_pass=[bool]((-not $DryRun)-and $allPass-and $unchanged)
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "P3 matrix summary: $summaryPath"
if ($failed -or -not $allPass) {exit 3}
exit 0
