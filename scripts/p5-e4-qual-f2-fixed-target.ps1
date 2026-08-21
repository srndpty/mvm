[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$ExpectedProductionSha,
    [string]$Executable,
    [string]$SourceA,
    [string]$SourceB,
    [ValidateRange(1, 10)][int]$Attempts = 3
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $repo 'build\ucrt64-release\bin\mvm_p3_av_sync_spike.exe' }
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
foreach ($path in @($Executable, $SourceA, $SourceB)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "QUAL-F2必須fileがありません: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存evidenceを上書きしません: $OutputDirectory" }
$status = @(& git -C $repo status --porcelain)
$commit = (& git -C $repo rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { throw 'clean worktreeが必要です' }
if ($commit -ne $ExpectedProductionSha) { throw "production SHAが一致しません: $commit" }

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$executableHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$runs = [System.Collections.Generic.List[object]]::new()
foreach ($target in @(3890, 3891, 3892)) {
    foreach ($attempt in 1..$Attempts) {
        $rawPath = Join-Path $OutputDirectory "target-$target-attempt-$attempt.json"
        Write-Host "QUAL-F2 target=$target attempt=$attempt を開始します" -ForegroundColor Cyan
        & $Executable --source-a $SourceA --source-b $SourceB --metrics $rawPath `
            --mode seek --duration-seconds 60 --warmup-seconds 5 --seek-count 1 `
            --seed 20260808 --display-timeout-ms 3000 --formal-contract-c2 `
            --diagnostic-fixed-seek-target $target
        $processExit = $LASTEXITCODE
        $raw = if (Test-Path -LiteralPath $rawPath) {
            Get-Content -Raw -LiteralPath $rawPath | ConvertFrom-Json
        } else { $null }
        $identityPass = $null -ne $raw -and $raw.integrated_seek_exact -eq 1 -and
            $raw.seeks.Count -eq 1 -and $raw.seeks[0].requested_frame -eq $target -and
            $raw.seeks[0].first_displayed_video_frame -eq $target -and
            $raw.seeks[0].first_audio_sample -eq $target * 800
        $terminalPass = $target -ne 3892 -or ($null -ne $raw -and
            $raw.measurement_terminal_eof_silence_callback_count -gt 0 -and
            $raw.first_terminal_eof_requested_start -eq 3119840 -and
            $raw.first_terminal_eof_requested_count -eq 480 -and
            $raw.first_terminal_eof_audio_samples -eq 288 -and
            $raw.first_terminal_eof_silence_samples -eq 192 -and
            $raw.first_terminal_eof_end_sample_exclusive -eq 3120128)
        $pass = $processExit -eq 0 -and $null -ne $raw -and $raw.pass -eq $true -and
            $raw.measurement_audio_underflow_count -eq 0 -and $identityPass -and $terminalPass
        $runs.Add([pscustomobject]@{
            target=$target; attempt=$attempt; raw_path=$rawPath; process_exit_code=$processExit
            pass=$pass; exact_identity_pass=$identityPass; terminal_eof_contract_pass=$terminalPass
            underflow_count=$(if ($raw) {$raw.measurement_audio_underflow_count} else {-1})
            terminal_eof_silence_callbacks=$(if ($raw) {$raw.measurement_terminal_eof_silence_callback_count} else {-1})
            terminal_eof_silence_samples=$(if ($raw) {$raw.measurement_terminal_eof_silence_samples} else {-1})
        })
    }
}
$endCommit = (& git -C $repo rev-parse HEAD).Trim()
$endStatus = @(& git -C $repo status --porcelain)
$endHash = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
$allPass = $runs.Count -eq 3 * $Attempts -and @($runs | Where-Object {-not $_.pass}).Count -eq 0
$provenanceUnchanged = $endCommit -eq $commit -and $endStatus.Count -eq 0 -and
    $endHash -eq $executableHash
$summary = [ordered]@{
    schema='mvm-p5-e4-qual-f2-fixed-target-1'; authority='TARGETED_INTEGRATION_ONLY'
    formal_pass_authority=$false; formal_verdict='NOT_RUN'; production_sha=$commit
    executable_sha256=$executableHash
    fixture_a_sha256=(Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_b_sha256=(Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
    all_runs_pass=$allPass; provenance_unchanged=$provenanceUnchanged; runs=@($runs)
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $summaryPath -Encoding utf8
$root = (Resolve-Path -LiteralPath $OutputDirectory).Path
$manifest = Get-ChildItem -LiteralPath $root -File | Sort-Object Name | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
}
$manifest | Set-Content -LiteralPath (Join-Path $root 'manifest.sha256') -Encoding ascii
Write-Host "QUAL-F2 targeted summary: $summaryPath"
if (-not $allPass -or -not $provenanceUnchanged) { exit 4 }
exit 0
