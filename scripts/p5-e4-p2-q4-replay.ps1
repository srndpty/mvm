[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$Q3Root = (Join-Path (Split-Path -Parent $PSScriptRoot) 'bench\results\p5-e4-p2-q3-c800536'),
    [string]$ReplayExecutable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not $ReplayExecutable) {
    $ReplayExecutable = Join-Path $repo 'build\ucrt64-release\bin\mvm_p2_scheduler_replay.exe'
}
$unitExecutable = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_p2_render_opportunity.exe'
foreach ($required in @($Q3Root, $ReplayExecutable, $unitExecutable)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2-Q4必須pathがありません: $required" }
}
$Q3Root = (Resolve-Path -LiteralPath $Q3Root).Path
$ReplayExecutable = (Resolve-Path -LiteralPath $ReplayExecutable).Path
$unitExecutable = (Resolve-Path -LiteralPath $unitExecutable).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存artifactを上書きしません: $OutputDirectory" }

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-GitText([string[]]$Arguments) {
    $value = & git -C $repo @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

function Assert-Manifest([string]$Root) {
    $manifest = Join-Path $Root 'manifest.sha256'
    if (-not (Test-Path -LiteralPath $manifest)) { throw "Q3 manifestがありません: $manifest" }
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Q3 manifest行が不正です: $line" }
        $path = Join-Path $Root $Matches[2]
        if (-not (Test-Path -LiteralPath $path) -or (Get-Sha256 $path) -ne $Matches[1]) {
            throw "Q3 evidence hashが不一致です: $($Matches[2])"
        }
    }
}

$testedSha = Invoke-GitText @('rev-parse', 'HEAD')
if (Invoke-GitText @('status', '--porcelain')) { throw 'P2-Q4はclean worktreeでのみ実行できます' }
Assert-Manifest $Q3Root
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $OutputDirectory 'runner.ps1')

& $unitExecutable
if ($LASTEXITCODE -ne 0) { throw 'render opportunity synthetic検査に失敗しました' }

$runs = [System.Collections.Generic.List[object]]::new()
foreach ($index in 1..5) {
    if ((Invoke-GitText @('rev-parse', 'HEAD')) -ne $testedSha -or
        (Invoke-GitText @('status', '--porcelain', '--untracked-files=no'))) {
        throw 'replay中にsource provenanceが変化しました'
    }
    $rawPath = Join-Path $Q3Root "run-$index\playback.json"
    $output = Join-Path $OutputDirectory "run-$index.json"
    if (-not (Test-Path -LiteralPath $rawPath)) { throw "Q3 rawがありません: $rawPath" }
    & $ReplayExecutable --input $rawPath --output $output
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw "Q3 run $index replayが失敗しました: exit=$exitCode" }
    $result = Get-Content -LiteralPath $output -Raw -Encoding utf8 | ConvertFrom-Json
    if (-not $result.current_replay.exact -or -not $result.nearest_slot_candidate.invariants_pass) {
        throw "Q3 run $index replayのfail-closed検査に失敗しました"
    }
    $runs.Add([ordered]@{
        run = $index
        input = "run-$index/playback.json"
        input_sha256 = Get-Sha256 $rawPath
        output = "run-$index.json"
        output_sha256 = Get-Sha256 $output
        current = $result.current_replay
        candidate = $result.nearest_slot_candidate
    })
}

$currentDrops = 0L
$candidateDrops = 0L
$candidateRepeated = 0L
foreach ($run in $runs) {
    $currentDrops += [long]$run.current.deadline_drop
    $candidateDrops += [long]$run.candidate.deadline_drop
    $candidateRepeated += [long]$run.candidate.repeated
}
[ordered]@{
    schema = 'mvm.p5-e4-p2-q4-campaign.v1'
    authority = 'diagnostic_only_not_closure_evidence'
    tested_sha = $testedSha
    q3_evidence_root = $Q3Root
    identity = [ordered]@{
        replay_executable_sha256 = Get-Sha256 $ReplayExecutable
        unit_executable_sha256 = Get-Sha256 $unitExecutable
        runner_sha256 = Get-Sha256 $PSCommandPath
        q3_manifest_sha256 = Get-Sha256 (Join-Path $Q3Root 'manifest.sha256')
    }
    exit_criteria = [ordered]@{
        current_replay_exact_count = @($runs | Where-Object { $_.current.exact }).Count
        required_current_replay_exact_count = 5
        candidate_invariant_count = @($runs | Where-Object { $_.candidate.invariants_pass }).Count
        required_candidate_invariant_count = 5
        synthetic_scenarios_passed = 8
        threshold_is_exit_criterion = $false
    }
    aggregate = [ordered]@{
        current_deadline_drop = $currentDrops
        candidate_deadline_drop = $candidateDrops
        candidate_repeated = $candidateRepeated
    }
    runs = $runs
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Where-Object FullName -ne $manifestPath | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    } | Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "P2-Q4 offline replay完了: current=$currentDrops candidate=$candidateDrops root=$OutputDirectory"
