[CmdletBinding()]
param(
    [switch]$DryRun,
    [ValidateRange(0, 3)][int]$DryRunFailRun = 0,
    [ValidateRange(0, 3)][int]$DryRunMissingRawRun = 0,
    [switch]$DryRunDirtyBaseline,
    [switch]$DryRunProvenanceChange,
    [switch]$DryRunDetachedHead,
    [switch]$DryRunRuntimePreflight,
    [string]$GitExe = 'git',
    [string]$OutputDir = 'tmp/p4-formal-matrix',
    [string]$Executable = 'build/ucrt64-release/bin/mvm_p4_composition_spike.exe',
    [string]$SourceA = 'tests/assets/p3_audio/p3_av_h264_aac.mp4',
    [string]$SourceB = 'tests/assets/p3_audio/p3_video_hevc_b.mp4',
    [string]$RuntimeBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\native-runtime.ps1')

if ([string]::IsNullOrWhiteSpace($RuntimeBin)) {
    $RuntimeBin = Get-MvmDefaultRuntimeBin
}

$gitCommand = $GitExe
$checker = Join-Path $PSScriptRoot 'check-p4-formal-contract.ps1'
$generator = Join-Path $PSScriptRoot 'new-p4-formal-synthetic.ps1'
$canonical = '0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1'
$scheduleHash = '5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79'
$fixtureA = 'd398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308'
$fixtureB = 'fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479'
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    [System.IO.Path]::GetFullPath($OutputDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $OutputDir))
}

function Invoke-Git([string[]]$Arguments) {
    $value = & $gitCommand @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') に失敗しました"
    }
    return $value
}

function Get-Hash([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Invoke-ChildScript([string]$File, [string[]]$Arguments) {
    $process = Start-Process -FilePath (Get-Process -Id $PID).Path `
        -ArgumentList (@('-NoProfile', '-File', $File) + $Arguments) `
        -Wait -PassThru -NoNewWindow
    return [int]$process.ExitCode
}

$runs = @()
$startHead = $null
$endHead = $null
$branch = $null
$startClean = $false
$endClean = $false
$exeHash = $null
$endExeHash = $null
$endFixtureA = $null
$endFixtureB = $null
$baselineProvenance = $null
$runtimePreflightPass = $null
$failureReason = $null

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
Push-Location $root
try {
    try {
        $actualDirty = @(Invoke-Git @('status', '--porcelain'))
        $startClean = $actualDirty.Count -eq 0
        $startHead = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
        $branchOutput = if ($DryRunDetachedHead) {
            $null
        } else {
            Invoke-Git @('branch', '--show-current')
        }
        $branch = if ($null -eq $branchOutput) { $null } else { "$branchOutput".Trim() }

        $computed = [Convert]::ToHexString(
            [Security.Cryptography.SHA256]::HashData(
                [Text.Encoding]::UTF8.GetBytes($canonical))).ToLowerInvariant()
        if ($computed -ne $scheduleHash) {
            throw 'formal canonical/hashの内部契約が不一致です'
        }
        if ($DryRunDirtyBaseline) {
            $startClean = $false
            throw 'dry-run synthetic dirty baselineをworkload開始前に拒否しました'
        }
        if (-not $DryRun -and -not $startClean) {
            throw 'REAL formal matrixはclean worktreeだけで開始できます'
        }

        if (-not $DryRun) {
            foreach ($pair in @(@($SourceA, $fixtureA), @($SourceB, $fixtureB))) {
                if (-not (Test-Path -LiteralPath $pair[0])) {
                    throw "fixtureがありません: $($pair[0])"
                }
                if ((Get-Hash $pair[0]) -ne $pair[1]) {
                    throw "fixture hashが違います: $($pair[0])"
                }
            }
        }
        if (-not $DryRun -and -not (Test-Path -LiteralPath $Executable)) {
            throw "formal executableがありません: $Executable"
        }
        $exeHash = if (Test-Path -LiteralPath $Executable) {
            Get-Hash $Executable
        } else {
            'DRY_RUN'
        }

        if (-not $DryRun -or $DryRunRuntimePreflight) {
            $runtime = Test-MvmNativeRuntime -RuntimeBin $RuntimeBin
            $RuntimeBin = $runtime.RuntimeBin
            $runtimePreflightPass = [bool]$runtime.Pass
            if (-not $runtime.Pass) {
                throw $runtime.Reason
            }
        }

        for ($i = 1; $i -le 3; $i++) {
            $raw = Join-Path $outputPath "run$i-raw.json"
            $processExit = 3
            $runFailureReason = $null
            try {
                if ($DryRun) {
                    $case = if ($i -eq $DryRunMissingRawRun) {
                        'MissingRaw'
                    } elseif ($i -eq $DryRunFailRun) {
                        'WrongState'
                    } else {
                        'GoodFormal'
                    }
                    $processExit = Invoke-ChildScript $generator @('-Output', $raw, '-Case', $case)
                } else {
                    $processExit = Invoke-MvmNativeProcess -FilePath $Executable -RuntimeBin $RuntimeBin `
                        -ArgumentList @('--source-a', $SourceA, '--source-b', $SourceB,
                            '--metrics', $raw, '--workload', 'formal')
                }
            } catch {
                $runFailureReason = "producer起動に失敗しました: $($_.Exception.Message)"
            }

            $rawExists = Test-Path -LiteralPath $raw
            $checkerRan = $false
            $checkerExit = 3
            if ($rawExists) {
                try {
                    $checkerRan = $true
                    $checkerExit = Invoke-ChildScript $checker @('-Json', $raw)
                } catch {
                    $runFailureReason = "checker起動に失敗しました: $($_.Exception.Message)"
                }
            } elseif ($null -eq $runFailureReason) {
                $runFailureReason = 'producer rawが生成されませんでした'
            }

            $data = if ($rawExists) {
                try {
                    Get-Content -Raw -LiteralPath $raw | ConvertFrom-Json
                } catch {
                    $runFailureReason = "producer rawを解析できません: $($_.Exception.Message)"
                    $null
                }
            } else {
                $null
            }
            $provenance = if ($null -ne $data) {
                "$($data.adapter)|$($data.audio_endpoint_sample_rate)|" +
                    "$($data.audio_endpoint_channels)|$($data.audio_endpoint_sample_format)"
            } else {
                $null
            }
            if ($null -eq $baselineProvenance -and $null -ne $provenance) {
                $baselineProvenance = $provenance
            }
            if ($DryRunProvenanceChange -and $i -eq 3) {
                $provenance = 'changed-provenance'
            }

            $runPass = $rawExists -and $checkerRan -and $processExit -eq 0 -and `
                $checkerExit -eq 0 -and $null -ne $provenance -and `
                $provenance -eq $baselineProvenance
            $runs += [ordered]@{
                run = $i
                raw_path = [System.IO.Path]::GetFullPath($raw)
                raw_exists = [bool]$rawExists
                process_exit_code = [int]$processExit
                checker_exit_code = [int]$checkerExit
                checker_ran = [bool]$checkerRan
                effective_video_fps = if ($null -ne $data) { $data.effective_video_fps } else { $null }
                drop_rate = if ($null -ne $data) { $data.drop_rate } else { $null }
                av_abs_p95_ms = if ($null -ne $data) { $data.application_av_delta_abs_ms.p95 } else { $null }
                av_abs_max_ms = if ($null -ne $data) { $data.application_av_delta_abs_ms.max } else { $null }
                provenance = $provenance
                failure_reason = $runFailureReason
                pass = [bool]$runPass
            }
        }
    } catch {
        $failureReason = $_.Exception.Message
    }

    try {
        $endHead = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
        $endDirty = @(Invoke-Git @('status', '--porcelain'))
        $endClean = $endDirty.Count -eq 0
        if (Test-Path -LiteralPath $Executable) {
            $endExeHash = Get-Hash $Executable
        }
        if (Test-Path -LiteralPath $SourceA) {
            $endFixtureA = Get-Hash $SourceA
        }
        if (Test-Path -LiteralPath $SourceB) {
            $endFixtureB = Get-Hash $SourceB
        }
    } catch {
        if ($null -eq $failureReason) {
            $failureReason = "終了provenance取得に失敗しました: $($_.Exception.Message)"
        }
    }

    $runProvenanceUnchanged = $runs.Count -eq 3 -and
        @($runs | Where-Object { $null -eq $_.provenance -or $_.provenance -ne $baselineProvenance }).Count -eq 0
    $executableProvenanceUnchanged = $DryRun -or $exeHash -eq $endExeHash
    $fixtureProvenanceUnchanged = $DryRun -or
        ($endFixtureA -eq $fixtureA -and $endFixtureB -eq $fixtureB)
    $provenanceUnchanged = $null -eq $failureReason -and $startHead -eq $endHead -and `
        $executableProvenanceUnchanged -and $runProvenanceUnchanged -and `
        $fixtureProvenanceUnchanged
    $allRuns = $null -eq $failureReason -and $runs.Count -eq 3 -and `
        @($runs | Where-Object { -not $_.pass }).Count -eq 0
    if (-not $DryRun) {
        $allRuns = $allRuns -and $startClean -and $endClean -and $provenanceUnchanged -and `
            $runtimePreflightPass
    }

    $summary = [ordered]@{
        schema = 'mvm-p4-formal-matrix-1'
        schema_version = 1
        dry_run = [bool]$DryRun
        formal_verdict = if ($DryRun) { 'NOT_RUN' } elseif ($allRuns) { 'PASS' } else { 'FAIL' }
        dry_run_harness_pass = if ($DryRun) { [bool]$allRuns } else { $false }
        all_runs_pass = [bool]$allRuns
        failure_reason = $failureReason
        start_head = $startHead
        end_head = $endHead
        branch = $branch
        start_worktree_clean = [bool]$startClean
        end_worktree_clean = [bool]$endClean
        provenance_unchanged = [bool]$provenanceUnchanged
        runtime_bin = [System.IO.Path]::GetFullPath($RuntimeBin)
        runtime_path_preflight_pass = $runtimePreflightPass
        executable_sha256 = $exeHash
        fixture_a_sha256 = $fixtureA
        fixture_b_sha256 = $fixtureB
        schedule_hash = $scheduleHash
        adapter = if ($null -ne $baselineProvenance) { ($baselineProvenance -split '\|')[0] } else { $null }
        audio_endpoint = $baselineProvenance
        runs = $runs
    }
    $summaryPath = Join-Path $outputPath 'summary.json'
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8

    if ($DryRun) {
        Write-Host "[p4-matrix] dry-run harness pass=$allRuns formal_verdict=NOT_RUN"
    } else {
        Write-Host "[p4-matrix] formal all_runs_pass=$allRuns"
    }
    if ($null -ne $failureReason) {
        Write-Host "[p4-matrix] FAIL $failureReason"
    }
    if (-not $allRuns) {
        exit 3
    }
} finally {
    Pop-Location
}
