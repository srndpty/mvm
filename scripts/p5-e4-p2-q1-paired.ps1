[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$CandidateWorktree = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\p2-q1-candidate'),
    [string]$BaselineWorktree = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\p2-q1-baseline'),
    [string]$SourceA,
    [string]$SourceB
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
$candidateSha = '31eda0d8d080dcf4b1680149d85b8293f618cd57'
$baselineSha = 'bb65ea5'
$checker = Join-Path $PSScriptRoot 'check-p2-contract.ps1'
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4' }

$cohorts = [ordered]@{
    candidate = [ordered]@{
        worktree = (Resolve-Path -LiteralPath $CandidateWorktree).Path
        expected_sha = $candidateSha
    }
    baseline = [ordered]@{
        worktree = (Resolve-Path -LiteralPath $BaselineWorktree).Path
        expected_sha = $baselineSha
    }
}
foreach ($entry in $cohorts.Values) {
    $entry.exe = Join-Path $entry.worktree 'build\ucrt64-release\bin\mvm_compositor_spike.exe'
}
foreach ($required in @($checker, $SourceA, $SourceB, $cohorts.candidate.exe,
        $cohorts.baseline.exe)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Q1必須ファイルがありません: $required" }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "既存artifactを上書きしません: $OutputDirectory"
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Git-Text([string]$Worktree, [string[]]$Arguments) {
    $value = & git -C $Worktree @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git -C $Worktree $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

foreach ($name in $cohorts.Keys) {
    $entry = $cohorts[$name]
    $actualSha = Git-Text $entry.worktree @('rev-parse', 'HEAD')
    $resolvedExpected = Git-Text $entry.worktree @('rev-parse', $entry.expected_sha)
    $status = Git-Text $entry.worktree @('status', '--porcelain')
    if ($actualSha -ne $resolvedExpected) {
        throw "$name SHAが不一致です: expected=$resolvedExpected actual=$actualSha"
    }
    if ($status) { throw "$name worktreeがcleanではありません: $status" }
    $entry.sha = $actualSha
    $entry.exe_sha256 = (Get-FileHash -LiteralPath $entry.exe -Algorithm SHA256).Hash.ToLowerInvariant()
}

$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$sequence = @(
    @{ cohort = 'candidate'; pair = 1; position = 1 },
    @{ cohort = 'baseline'; pair = 1; position = 2 },
    @{ cohort = 'baseline'; pair = 2; position = 1 },
    @{ cohort = 'candidate'; pair = 2; position = 2 },
    @{ cohort = 'candidate'; pair = 3; position = 1 },
    @{ cohort = 'baseline'; pair = 3; position = 2 },
    @{ cohort = 'baseline'; pair = 4; position = 1 },
    @{ cohort = 'candidate'; pair = 4; position = 2 },
    @{ cohort = 'candidate'; pair = 5; position = 1 },
    @{ cohort = 'baseline'; pair = 5; position = 2 }
)
$runs = [System.Collections.Generic.List[object]]::new()

foreach ($item in $sequence) {
    $name = $item.cohort
    $entry = $cohorts[$name]
    $ordinal = @($runs | Where-Object cohort -eq $name).Count + 1
    $rawPath = Join-Path $OutputDirectory "$name-run$ordinal.json"
    $arguments = @(
        '--source-a', $SourceA, '--source-b', $SourceB,
        '--metrics', $rawPath, '--warmup-seconds', 5,
        '--measure-seconds', 60, '--seed', 20260808,
        '--seek-count', 1000, '--display-timeout-ms', 2000,
        '--gpu-completion', 'fence', '--mode', 'playback',
        '--formal-preflight'
    )
    Write-Host "Q1 pair $($item.pair) position $($item.position): $name run $ordinal/5 を開始します"
    & $entry.exe @arguments
    $processExit = $LASTEXITCODE
    $contractExit = -1
    if (Test-Path -LiteralPath $rawPath) {
        & pwsh -NoProfile -File $checker -Json $rawPath -Mode Playback `
            -ProcessExitCode $processExit
        $contractExit = $LASTEXITCODE
    }
    if (-not (Test-Path -LiteralPath $rawPath)) {
        throw "$name run $ordinal はraw JSONを生成しませんでした"
    }
    $raw = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
    $runs.Add([ordered]@{
        cohort = $name
        run = $ordinal
        pair = $item.pair
        position = $item.position
        raw_file = Split-Path -Leaf $rawPath
        process_exit_code = $processExit
        contract_exit_code = $contractExit
        contract_pass = $processExit -eq 0 -and $contractExit -eq 0
        deadline_drop_count = [long]$raw.measurement_drop_scheduler_deadline
        drop_rate = [double]$raw.drop_rate
        effective_fps = [double]$raw.effective_fps
        present_callback_count = [long]$raw.measurement_present_callback_count
        repeated_present_count = [long]$raw.measurement_repeated_present_count
        decoded_a_count = [long]$raw.measurement_decoded_a_count
        decoded_b_count = [long]$raw.measurement_decoded_b_count
        scheduled_output_count = [long]$raw.measurement_scheduled_output_count
        displayed_composition_count = [long]$raw.measurement_displayed_composition_count
    })
}

function Measure-Cohort([string]$Name) {
    $items = @($runs | Where-Object cohort -eq $Name)
    $drops = @($items.deadline_drop_count | Sort-Object)
    return [ordered]@{
        run_count = $items.Count
        contract_pass_count = @($items | Where-Object contract_pass).Count
        deadline_drop_min = [long](($drops | Measure-Object -Minimum).Minimum)
        deadline_drop_median = [double]$drops[[math]::Floor($drops.Count / 2)]
        deadline_drop_max = [long](($drops | Measure-Object -Maximum).Maximum)
        deadline_drop_mean = [double](($drops | Measure-Object -Average).Average)
        effective_fps_mean = [double](($items.effective_fps | Measure-Object -Average).Average)
        repeated_present_mean = [double](($items.repeated_present_count | Measure-Object -Average).Average)
        present_callback_mean = [double](($items.present_callback_count | Measure-Object -Average).Average)
        decoded_a_mean = [double](($items.decoded_a_count | Measure-Object -Average).Average)
        decoded_b_mean = [double](($items.decoded_b_count | Measure-Object -Average).Average)
    }
}

$summary = [ordered]@{
    schema = 'mvm.p5-e4-p2-q1-paired.v1'
    authority = 'diagnostic_only_not_closure_evidence'
    generated_at = (Get-Date).ToUniversalTime().ToString('o')
    conditions = [ordered]@{
        order = 'C1-B1-B2-C2-C3-B3-B4-C4-C5-B5'
        mode = 'playback'
        warmup_seconds = 5
        measure_seconds = 60
        seed = 20260808
        seek_count = 1000
        display_timeout_ms = 2000
        gpu_completion = 'fence'
        formal_preflight = $true
    }
    provenance = [ordered]@{
        runner_sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
        checker_sha256 = (Get-FileHash -LiteralPath $checker -Algorithm SHA256).Hash.ToLowerInvariant()
        source_a = (Resolve-Path -LiteralPath $SourceA).Path
        source_a_sha256 = (Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
        source_b = (Resolve-Path -LiteralPath $SourceB).Path
        source_b_sha256 = (Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
        candidate = $cohorts.candidate
        baseline = $cohorts.baseline
    }
    runs = $runs
    candidate = Measure-Cohort 'candidate'
    baseline = Measure-Cohort 'baseline'
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
Write-Host "Q1 paired diagnostic完了: $OutputDirectory"
