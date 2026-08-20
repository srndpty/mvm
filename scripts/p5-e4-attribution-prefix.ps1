[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [Alias('Profile')]
    [ValidateSet('SEEK-PREFIX', 'PAUSE-PREFIX')]
    [string]$AttributionProfile,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Executable,
    [string]$SourceA,
    [string]$SourceB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo 'build\ucrt64-release\bin\mvm_p3_av_sync_spike.exe' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
$checker = Join-Path $PSScriptRoot 'check-p3-c2-contract.ps1'
foreach ($path in @($Executable, $checker, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "ATTR-Q1 prefix必須fileがありません: $path"
    }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "ATTR-Q1 evidenceを上書きしません。新しいOutputDirectoryを指定してください: $OutputDirectory"
}
$status = @(& git -C $repo status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'git statusの取得に失敗しました' }
if ($status.Count -ne 0) {
    throw 'ATTR-Q1 prefixはclean worktreeのexact SHAでだけ実行します'
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

$sequence = [System.Collections.Generic.List[object]]::new()
foreach ($index in 1..3) { $sequence.Add([pscustomobject]@{mode='playback';run=$index}) }
if ($AttributionProfile -eq 'SEEK-PREFIX') {
    $sequence.Add([pscustomobject]@{mode='seek';run=1})
} else {
    foreach ($index in 1..3) { $sequence.Add([pscustomobject]@{mode='seek';run=$index}) }
    foreach ($index in 1..3) { $sequence.Add([pscustomobject]@{mode='pause-resume';run=$index}) }
}

$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'git commitの取得に失敗しました' }
$start = [ordered]@{
    git_commit = $commit
    dirty_worktree = $false
    executable_sha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_a_sha256 = (Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_b_sha256 = (Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
}
$entries = [System.Collections.Generic.List[object]]::new()
$referencePath = ''
foreach ($entry in $sequence) {
    $path = Join-Path $OutputDirectory "$($entry.mode)-run$($entry.run).json"
    $arguments = @('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$path,
        '--mode',$entry.mode,'--duration-seconds','60','--warmup-seconds','5',
        '--seek-count','1000','--seed','20260808','--display-timeout-ms','3000',
        '--formal-contract-c2')
    Write-Host "ATTR-Q1 $AttributionProfile $($entry.mode) run $($entry.run) を開始します" -ForegroundColor Cyan
    & $Executable @arguments
    $processExit = $LASTEXITCODE
    $contractExit = -1
    if (Test-Path -LiteralPath $path) {
        $checkerArguments = @('-NoProfile','-File',$checker,'-Json',$path,'-Mode',$entry.mode,
            '-ProcessExitCode',$processExit)
        if ($referencePath) { $checkerArguments += @('-ReferenceJson',$referencePath) }
        & pwsh @checkerArguments
        $contractExit = $LASTEXITCODE
        if ($processExit -eq 0 -and $contractExit -eq 0 -and -not $referencePath) {
            $referencePath = $path
        }
    }
    $raw = if (Test-Path -LiteralPath $path) {
        Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    } else { $null }
    $attributionValid = $false
    if ($raw) {
        $underflowProperty = $raw.PSObject.Properties['first_audio_underflow_snapshot']
        $clockProperty = $raw.PSObject.Properties['first_clock_regression_snapshot']
        $attributionValid = $null -ne $underflowProperty -and $null -ne $clockProperty
        if ($attributionValid -and $raw.measurement_audio_underflow_count -gt 0) {
            $attributionValid = $null -ne $underflowProperty.Value
        }
        if ($attributionValid -and $raw.measurement_clock_regression_count -gt 0) {
            $attributionValid = $null -ne $clockProperty.Value
        }
    }
    $entries.Add([pscustomobject]@{
        mode=$entry.mode; run=$entry.run; raw_path=$path
        process_exit_code=$processExit; contract_exit_code=$contractExit
        pass=($processExit -eq 0 -and $contractExit -eq 0)
        attribution_snapshot_contract_valid=$attributionValid; raw=$raw
    })
}

$endStatus = @(& git -C $repo status --porcelain)
$endCommit = (& git -C $repo rev-parse HEAD).Trim()
$endExecutableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$provenanceUnchanged = $endStatus.Count -eq 0 -and $endCommit -eq $start.git_commit -and
    $endExecutableHash -eq $start.executable_sha256
$complete = $entries.Count -eq $sequence.Count -and
    @($entries | Where-Object {$null -eq $_.raw -or -not $_.attribution_snapshot_contract_valid}).Count -eq 0
$allPass = $complete -and @($entries | Where-Object {-not $_.pass}).Count -eq 0
$summary = [ordered]@{
    schema = 'mvm-p5-e4-attribution-prefix-1'
    authority = 'DIAGNOSTIC_ONLY'
    formal_pass_authority = $false
    formal_verdict = 'NOT_RUN'
    profile = $AttributionProfile
    exact_formal_arguments = [ordered]@{
        warmup_seconds=5; measurement_seconds=60; seed=20260808; seek_count=1000
        display_timeout_ms=3000; contract='P3-C-2'
    }
    expected_processes = $sequence.Count
    completed_processes = $entries.Count
    complete = $complete
    all_process_contracts_pass = $allPass
    provenance_unchanged = $provenanceUnchanged
    start_provenance = $start
    end_provenance = [ordered]@{
        git_commit=$endCommit; dirty_worktree=($endStatus.Count -ne 0)
        executable_sha256=$endExecutableHash
    }
    runs = @($entries | ForEach-Object {
        [ordered]@{
            mode=$_.mode; run=$_.run; raw_path=$_.raw_path
            process_exit_code=$_.process_exit_code; contract_exit_code=$_.contract_exit_code
            pass=$_.pass
            attribution_snapshot_contract_valid=$_.attribution_snapshot_contract_valid
            first_audio_underflow_snapshot=$(if ($_.raw) {$_.raw.first_audio_underflow_snapshot} else {$null})
            first_clock_regression_snapshot=$(if ($_.raw) {$_.raw.first_clock_regression_snapshot} else {$null})
        }
    })
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "ATTR-Q1 diagnostic summary: $summaryPath"
if (-not $complete -or -not $provenanceUnchanged) { exit 4 }
if (-not $allPass) { exit 3 }
exit 0
