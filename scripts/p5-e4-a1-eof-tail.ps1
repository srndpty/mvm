[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$Executable,
    [string]$SourceA,
    [string]$SourceB,
    [int[]]$Targets = @(3890, 3891, 3892),
    [ValidateRange(1, 20)][int]$Attempts = 3,
    [string]$ExpectedDiagnosticSha,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-A1Classification {
    param([object]$Raw)
    if ($null -eq $Raw -or $Raw.measurement_audio_underflow_count -le 0) {
        return 'NO_UNDERFLOW'
    }
    $snapshot = $Raw.first_audio_underflow_snapshot
    if ($null -eq $snapshot) { return 'MISSING_SNAPSHOT' }
    $requestedStart = [long]$snapshot.requested_sample_start
    $requestedCount = [long]$snapshot.requested_sample_count
    $consumed = [long]$snapshot.actually_consumed_samples
    $queueEnd = [long]$snapshot.queue_last_available_sample_exclusive
    $actualEnd = [long]$snapshot.context.actual_audio_end_exclusive
    $eof = [bool]$snapshot.context.audio_decoder_eof
    if ($eof -and $actualEnd -ge 0 -and $queueEnd -eq $actualEnd -and
        ($actualEnd - $requestedStart) -eq $consumed -and $consumed -lt $requestedCount) {
        return 'EOF_TAIL_INSUFFICIENT'
    }
    if ($actualEnd -ge 0 -and $queueEnd -ge 0 -and $queueEnd -ne $actualEnd) {
        return 'QUEUE_ACCOUNTING_DISCONTINUITY'
    }
    if (-not $eof -or ($actualEnd - $requestedStart) -ge $requestedCount) {
        return 'PRODUCER_STARVATION'
    }
    return 'UNRESOLVED'
}

function Get-OptionalProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

if ($SelfTest) {
    $base = [pscustomobject]@{
        measurement_audio_underflow_count = 1
        first_audio_underflow_snapshot = [pscustomobject]@{
            requested_sample_start = 3119840; requested_sample_count = 480
            actually_consumed_samples = 288; queue_last_available_sample_exclusive = 3120128
            context = [pscustomobject]@{ actual_audio_end_exclusive = 3120128; audio_decoder_eof = $true }
        }
    }
    if ((Get-A1Classification $base) -ne 'EOF_TAIL_INSUFFICIENT') { throw 'EOF-tail分類に失敗しました' }
    $base.first_audio_underflow_snapshot.context.audio_decoder_eof = $false
    if ((Get-A1Classification $base) -ne 'PRODUCER_STARVATION') { throw 'starvation分類に失敗しました' }
    $base.first_audio_underflow_snapshot.context.audio_decoder_eof = $true
    $base.first_audio_underflow_snapshot.queue_last_available_sample_exclusive = 3120000
    if ((Get-A1Classification $base) -ne 'QUEUE_ACCOUNTING_DISCONTINUITY') { throw 'queue不連続分類に失敗しました' }
    $base.measurement_audio_underflow_count = 0
    if ((Get-A1Classification $base) -ne 'NO_UNDERFLOW') { throw '非underflow分類に失敗しました' }
    Write-Host 'PASS: ATTR-Q3-A1 classifier'
    exit 0
}

if (-not $OutputDirectory) { throw '-OutputDirectory は必須です' }
if (-not $ExpectedDiagnosticSha) { throw '-ExpectedDiagnosticSha は必須です' }
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo 'build\ucrt64-release\bin\mvm_p3_av_sync_spike.exe' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
foreach ($path in @($Executable, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "ATTR-Q3-A1必須fileがありません: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "ATTR-Q3-A1 evidenceを上書きしません: $OutputDirectory"
}
$status = @(& git -C $repo status --porcelain)
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { throw 'clean worktreeが必要です' }
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($commit -ne $ExpectedDiagnosticSha) { throw "diagnostic SHAが一致しません: $commit" }

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$provenance = [ordered]@{
    git_commit = $commit
    executable = (Resolve-Path -LiteralPath $Executable).Path
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_a_sha256 = (Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_b_sha256 = (Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
}
$runs = [System.Collections.Generic.List[object]]::new()
foreach ($target in $Targets) {
    foreach ($attempt in 1..$Attempts) {
        $rawPath = Join-Path $OutputDirectory "target-$target-attempt-$attempt.json"
        $arguments = @('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$rawPath,
            '--mode','seek','--duration-seconds','60','--warmup-seconds','5',
            '--seek-count','1','--seed','20260808','--display-timeout-ms','3000',
            '--formal-contract-c2','--diagnostic-fixed-seek-target',[string]$target)
        Write-Host "ATTR-Q3-A1 target=$target attempt=$attempt を開始します" -ForegroundColor Cyan
        & $Executable @arguments
        $processExit = $LASTEXITCODE
        $raw = if (Test-Path -LiteralPath $rawPath) {
            Get-Content -Raw -LiteralPath $rawPath | ConvertFrom-Json
        } else { $null }
        $classification = Get-A1Classification $raw
        $snapshot = if ($raw) { $raw.first_audio_underflow_snapshot } else { $null }
        $runs.Add([pscustomobject]@{
            target = $target; attempt = $attempt; raw_path = $rawPath
            process_exit_code = $processExit; classification = $classification
            actual_audio_end_exclusive = $(if ($snapshot) {$snapshot.context.actual_audio_end_exclusive} elseif ($raw) {$raw.actual_audio_end_exclusive} else {-1})
            audio_decoder_eof = $(if ($snapshot) {$snapshot.context.audio_decoder_eof} elseif ($raw) {$raw.audio_decoder_eof} else {$false})
            first_audio_underflow_snapshot = $snapshot
            display_provenance = $(if ($raw) {[ordered]@{
                start = Get-OptionalProperty $raw 'display_environment_start'
                end = Get-OptionalProperty $raw 'display_environment_end'
            }} else {$null})
            hardware_provenance = $(if ($raw) {[ordered]@{
                adapter = Get-OptionalProperty $raw 'adapter'
                audio_endpoint_sample_rate = Get-OptionalProperty $raw 'audio_endpoint_sample_rate'
                audio_endpoint_channels = Get-OptionalProperty $raw 'audio_endpoint_channels'
                audio_endpoint_sample_format = Get-OptionalProperty $raw 'audio_endpoint_sample_format'
            }} else {$null})
        })
    }
}
$endCommit = (& git -C $repo rev-parse HEAD).Trim()
$endStatus = @(& git -C $repo status --porcelain)
$endHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$complete = $runs.Count -eq ($Targets.Count * $Attempts) -and
    @($runs | Where-Object {$_.classification -in @('MISSING_SNAPSHOT','UNRESOLVED')}).Count -eq 0
$underflowClasses = @($runs | Where-Object {$_.classification -ne 'NO_UNDERFLOW'} |
    Select-Object -ExpandProperty classification -Unique)
$campaignClassification = if ($underflowClasses.Count -eq 0) {'NO_UNDERFLOW_REPRODUCED'}
    elseif ($underflowClasses.Count -eq 1) {$underflowClasses[0]}
    else {'MIXED'}
$summary = [ordered]@{
    schema = 'mvm-p5-e4-attr-q3-a1-1'
    authority = 'DIAGNOSTIC_ONLY'; formal_pass_authority = $false; formal_verdict = 'NOT_RUN'
    purpose = 'seek ordinal 523 / target 3892 EOF-tail attribution'
    canonical_workload_unchanged = $true
    diagnostic_fixed_targets = $Targets
    attempts_per_target = $Attempts
    campaign_classification = $campaignClassification
    complete = $complete
    provenance_unchanged = ($endCommit -eq $commit -and $endStatus.Count -eq 0 -and $endHash -eq $provenance.executable_sha256)
    start_provenance = $provenance
    runs = @($runs)
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "ATTR-Q3-A1 diagnostic summary: $summaryPath"
if (-not $summary.complete -or -not $summary.provenance_unchanged) { exit 4 }
exit 0
