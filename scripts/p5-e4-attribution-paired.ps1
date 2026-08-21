[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HeadWorktree,
    [Parameter(Mandatory)][string]$ParentWorktree,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$HeadBaseSha,
    [Parameter(Mandatory)][string]$ParentBaseSha,
    [Parameter(Mandatory)][string]$HeadDiagnosticSha,
    [Parameter(Mandatory)][string]$ParentDiagnosticSha,
    [Parameter(Mandatory)][string]$PatchIdentity,
    [ValidateSet('HeadParent','ParentHead')][string]$CohortOrder = 'HeadParent',
    [string]$DisplayPreflightExecutable,
    [ValidateRange(2,20)][int]$DisplayPreflightSamples = 5,
    [ValidateRange(0,10)][int]$DisplayPreflightIntervalSeconds = 2,
    [string]$SourceA,
    [string]$SourceB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "ATTR-Q2 artifactを上書きしません: $OutputDirectory"
}

$patchPaths = @('apps/p3_av_sync_spike','src/app/preview','src/media/audio_preview',
    'tests/audio_preview','scripts/p5-e4-attribution-prefix.ps1')
$cohorts = @(
    [pscustomobject]@{name='head';worktree=(Resolve-Path $HeadWorktree).Path
        base=$HeadBaseSha;diagnostic=$HeadDiagnosticSha},
    [pscustomobject]@{name='parent';worktree=(Resolve-Path $ParentWorktree).Path
        base=$ParentBaseSha;diagnostic=$ParentDiagnosticSha}
)
foreach ($cohort in $cohorts) {
    $actualHead = (& git -C $cohort.worktree rev-parse HEAD).Trim()
    $status = @(& git -C $cohort.worktree status --porcelain)
    if ($actualHead -ne $cohort.diagnostic -or $status.Count -ne 0) {
        throw "$($cohort.name) worktreeがclean exact diagnostic SHAではありません"
    }
    $actualBase = (& git -C $cohort.worktree rev-parse "$($cohort.diagnostic)^1").Trim()
    $expectedBase = (& git -C $cohort.worktree rev-parse $cohort.base).Trim()
    if ($actualBase -ne $expectedBase) {
        throw "$($cohort.name) diagnostic commitの第一親が指定baseと一致しません"
    }
    $patchLine = (& git -C $cohort.worktree diff $cohort.base $cohort.diagnostic -- $patchPaths |
        & git patch-id --stable)
    $actualPatchIdentity = ($patchLine -split '\s+')[0]
    if ($actualPatchIdentity -ne $PatchIdentity) {
        throw "$($cohort.name)のdiagnostic patch identityが一致しません"
    }
    $cohort.base = $expectedBase
    $cohort | Add-Member -NotePropertyName executable -NotePropertyValue (
        Join-Path $cohort.worktree 'build\ucrt64-release\bin\mvm_p3_av_sync_spike.exe')
    $cohort | Add-Member -NotePropertyName runner -NotePropertyValue (
        Join-Path $cohort.worktree 'scripts\p5-e4-attribution-prefix.ps1')
    foreach ($required in @($cohort.executable, $cohort.runner, $SourceA, $SourceB)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "ATTR-Q2必須fileがありません: $required" }
    }
}
if ($CohortOrder -eq 'ParentHead') {
    [array]::Reverse($cohorts)
}
$provenance = @($cohorts | ForEach-Object {
    [ordered]@{cohort=$_.name;base_sha=$_.base;diagnostic_sha=$_.diagnostic
        patch_identity=$PatchIdentity
        executable_path=$_.executable
        executable_sha256=(Get-FileHash -LiteralPath $_.executable -Algorithm SHA256).Hash.ToLowerInvariant()
        fixture_a_sha256=(Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
        fixture_b_sha256=(Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()}
})

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
function Write-ArtifactManifest {
    $manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
    $rootPath = (Resolve-Path $OutputDirectory).Path
    $manifest = Get-ChildItem -LiteralPath $rootPath -File -Recurse |
        Where-Object {$_.FullName -ne $manifestPath} | Sort-Object FullName | ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($rootPath, $_.FullName).Replace('\','/')
            "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
        }
    $manifest | Set-Content -LiteralPath $manifestPath -Encoding utf8
    return $manifestPath
}
$displayPreflight = $null
if ($DisplayPreflightExecutable) {
    $displayPreflightRunner = Join-Path $PSScriptRoot 'p5-e4-display-preflight.ps1'
    $displayPreflightDirectory = Join-Path $OutputDirectory 'display-preflight'
    & pwsh -NoProfile -File $displayPreflightRunner -Executable $DisplayPreflightExecutable `
        -OutputDirectory $displayPreflightDirectory -Samples $DisplayPreflightSamples `
        -IntervalSeconds $DisplayPreflightIntervalSeconds -TimeoutMs 3000 -RequireCleanGit
    $displayPreflightExit = $LASTEXITCODE
    $displayPreflightSummaryPath = Join-Path $displayPreflightDirectory 'summary.json'
    $displayPreflight = if (Test-Path -LiteralPath $displayPreflightSummaryPath) {
        Get-Content -Raw -LiteralPath $displayPreflightSummaryPath | ConvertFrom-Json
    } else { $null }
    if ($displayPreflightExit -ne 0 -or -not $displayPreflight -or -not $displayPreflight.passed) {
        $preflightFailureSummary = [ordered]@{
            schema='mvm-p5-e4-attribution-paired-1'
            authority='DIAGNOSTIC_ONLY'
            formal_pass_authority=$false
            formal_verdict='NOT_RUN'
            ordering=$(if ($CohortOrder -eq 'ParentHead') {
                'profile -> attempt 1..3 -> parent then head'
            } else {
                'profile -> attempt 1..3 -> head then parent'
            })
            expected_prefix_runs=12
            completed_prefix_runs=0
            workload_started=$false
            patch_identity=$PatchIdentity
            provenance=$provenance
            display_preflight=$displayPreflight
            prefix_runs=@()
            failures=@()
        }
        $preflightFailureSummary | ConvertTo-Json -Depth 20 |
            Set-Content -LiteralPath (Join-Path $OutputDirectory 'paired-summary.json') -Encoding utf8
        $null = Write-ArtifactManifest
        Write-Host 'ATTR-Q2B display provenance preflightが不合格のためworkloadを開始しません' -ForegroundColor Red
        exit 4
    }
}
$records = [System.Collections.Generic.List[object]]::new()
foreach ($prefixProfile in @('SEEK-PREFIX','PAUSE-PREFIX')) {
    foreach ($attempt in 1..3) {
        foreach ($cohort in $cohorts) {
            $attemptDirectory = Join-Path $OutputDirectory "$prefixProfile\attempt-$attempt\$($cohort.name)"
            Write-Host "ATTR-Q2 $prefixProfile attempt $attempt/3 $($cohort.name) を開始します" -ForegroundColor Cyan
            & pwsh -NoProfile -File $cohort.runner -Profile $prefixProfile `
                -OutputDirectory $attemptDirectory -Executable $cohort.executable `
                -SourceA $SourceA -SourceB $SourceB
            $runnerExit = $LASTEXITCODE
            $summaryPath = Join-Path $attemptDirectory 'summary.json'
            $summary = if (Test-Path -LiteralPath $summaryPath) {
                Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
            } else { $null }
            $records.Add([pscustomobject]@{profile=$prefixProfile;attempt=$attempt
                cohort=$cohort.name;runner_exit_code=$runnerExit;summary_path=$summaryPath
                summary=$summary})
        }
    }
}

$failures = [System.Collections.Generic.List[object]]::new()
$hardwareSignatures = [System.Collections.Generic.List[string]]::new()
$displaySignatures = [System.Collections.Generic.List[string]]::new()
foreach ($record in $records) {
    if (-not $record.summary) { continue }
    foreach ($run in $record.summary.runs) {
        $raw = Get-Content -Raw -LiteralPath $run.raw_path | ConvertFrom-Json
        $hardwareSignatures.Add("$($raw.adapter)|$($raw.audio_endpoint_sample_rate)|$($raw.audio_endpoint_channels)|$($raw.audio_endpoint_sample_format)")
        $display = $raw.display_environment_start
        $displaySignatures.Add("$($display.screen_name)|$($display.screen_orientation)|$($display.screen_geometry_width)x$($display.screen_geometry_height)|$($display.device_pixel_ratio)|$($display.rhi_target_pixel_width)x$($display.rhi_target_pixel_height)")
        if ($run.process_exit_code -ne 0 -or $run.contract_exit_code -ne 0) {
            $failures.Add([pscustomobject]@{profile=$record.profile;attempt=$record.attempt
                cohort=$record.cohort;mode=$run.mode;run=$run.run;raw_path=$run.raw_path
                process_exit_code=$run.process_exit_code;contract_exit_code=$run.contract_exit_code
                audio_underflow_count=$raw.measurement_audio_underflow_count
                clock_regression_count=$raw.measurement_clock_regression_count
                first_audio_underflow_snapshot=$raw.first_audio_underflow_snapshot
                first_clock_regression_snapshot=$raw.first_clock_regression_snapshot})
        }
    }
}
$uniqueHardware = @($hardwareSignatures | Select-Object -Unique)
$uniqueDisplay = @($displaySignatures | Select-Object -Unique)
$pairedSummary = [ordered]@{
    schema='mvm-p5-e4-attribution-paired-1'
    authority='DIAGNOSTIC_ONLY'
    formal_pass_authority=$false
    formal_verdict='NOT_RUN'
    ordering=$(if ($CohortOrder -eq 'ParentHead') {
        'profile -> attempt 1..3 -> parent then head'
    } else {
        'profile -> attempt 1..3 -> head then parent'
    })
    display_preflight=$displayPreflight
    expected_prefix_runs=12
    completed_prefix_runs=$records.Count
    workload_started=($records.Count -gt 0)
    patch_identity=$PatchIdentity
    provenance=$provenance
    hardware_provenance_signatures=$uniqueHardware
    display_provenance_signatures=$uniqueDisplay
    hardware_provenance_unchanged=($uniqueHardware.Count -eq 1)
    display_provenance_unchanged=($uniqueDisplay.Count -eq 1)
    prefix_runs=@($records | ForEach-Object {[ordered]@{profile=$_.profile;attempt=$_.attempt
        cohort=$_.cohort;runner_exit_code=$_.runner_exit_code;summary_path=$_.summary_path
        all_process_contracts_pass=$(if ($_.summary) {$_.summary.all_process_contracts_pass} else {$false})}})
    failures=@($failures)
}
$pairedSummaryPath = Join-Path $OutputDirectory 'paired-summary.json'
$pairedSummary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $pairedSummaryPath -Encoding utf8

$manifestPath = Write-ArtifactManifest
Write-Host "ATTR-Q2 paired summary: $pairedSummaryPath"
Write-Host "ATTR-Q2 SHA-256 manifest: $manifestPath"
if ($records.Count -ne 12 -or $uniqueHardware.Count -ne 1 -or $uniqueDisplay.Count -ne 1) { exit 4 }
if ($failures.Count -gt 0) { exit 3 }
exit 0
