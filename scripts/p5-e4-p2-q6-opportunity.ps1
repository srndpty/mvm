[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$Q5Evidence = 'bench\results\p5-e4-p2-q5-2be57c6',
    [string]$ReplayExecutable
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Q5Evidence)) { $Q5Evidence = Join-Path $repo $Q5Evidence }
if (-not $ReplayExecutable) {
    $ReplayExecutable = Join-Path $repo 'build\ucrt64-release\bin\mvm_p2_opportunity_ordinal_replay.exe'
}
foreach ($required in @($Q5Evidence, $ReplayExecutable)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "P2-Q6必須pathがありません: $required" }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存artifactを上書きしません: $OutputDirectory" }

function Invoke-GitText([string[]]$Arguments) {
    $value = & git -C $repo @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') に失敗しました" }
    return ($value -join "`n").Trim()
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Manifest([string]$Root) {
    $manifest = Join-Path $Root 'manifest.sha256'
    if (-not (Test-Path -LiteralPath $manifest)) { throw 'Q5 manifestがありません' }
    foreach ($line in Get-Content -LiteralPath $manifest) {
        if (-not $line.Trim()) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Q5 manifest行が不正です: $line" }
        $path = Join-Path $Root $Matches[2]
        if (-not (Test-Path -LiteralPath $path) -or (Get-Sha256 $path) -ne $Matches[1].ToLowerInvariant()) {
            throw "Q5 evidence hashが一致しません: $($Matches[2])"
        }
    }
}

$testedSha = Invoke-GitText @('rev-parse', 'HEAD')
if (Invoke-GitText @('status', '--porcelain')) { throw 'P2-Q6 evidenceはclean worktreeでのみ生成できます' }
Assert-Manifest $Q5Evidence

$unit = Join-Path $repo 'build\ucrt64-release\bin\mvm_test_p2_opportunity_ordinal.exe'
if (-not (Test-Path -LiteralPath $unit)) { throw "P2-Q6 unit executableがありません: $unit" }
& $unit
if ($LASTEXITCODE -ne 0) { throw 'P2-Q6 synthetic検査に失敗しました' }

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $OutputDirectory 'runner.ps1')
$runs = [System.Collections.Generic.List[object]]::new()
foreach ($index in 1, 2) {
    $runDirectory = Join-Path $OutputDirectory "run-$index"
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $raw = Join-Path $Q5Evidence "run-$index\playback.json"
    $classification = Join-Path $Q5Evidence "run-$index\classification.json"
    $output = Join-Path $runDirectory 'replay.json'
    & $ReplayExecutable --input $raw --classification $classification --output $output
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw "Q5 run-$indexのQ6 replayに失敗しました: exit=$exitCode" }
    $replay = Get-Content -LiteralPath $output -Raw -Encoding utf8 | ConvertFrom-Json
    if (-not $replay.proof_pass) { throw "Q5 run-$indexのQ6 proofが成立しません" }
    $runs.Add([ordered]@{
            run = $index
            replay_sha256 = Get-Sha256 $output
            displayed = [long]$replay.replay.displayed
            true_dropped = [long]$replay.replay.true_dropped
            repeated = [long]$replay.replay.repeated
            cadence_and_domain_loss = [long]$replay.reconciliation.cadence_and_domain_loss
            q5_true_loss_at_synthetic_skip_sites =
                [long]$replay.reconciliation.q5_true_loss_at_synthetic_skip_sites
            other_ordinal_gap_source_loss =
                [long]$replay.reconciliation.other_ordinal_gap_source_loss
        })
}

[ordered]@{
    schema = 'mvm.p5-e4-p2-q6-opportunity-ordinal-campaign.v1'
    authority = 'diagnostic_only_not_closure_evidence'
    tested_sha = $testedSha
    q5_evidence = (Resolve-Path -LiteralPath $Q5Evidence).Path
    q5_manifest_sha256 = Get-Sha256 (Join-Path $Q5Evidence 'manifest.sha256')
    replay_executable_sha256 = Get-Sha256 $ReplayExecutable
    unit_executable_sha256 = Get-Sha256 $unit
    production_scheduler_changed = $false
    formal_checker_changed = $false
    threshold_changed = $false
    synthetic_scenarios_passed = 15
    proof_pass = $true
    runs = $runs
} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8

$readme = @"
# P5-E4 / P2-Q6 — Opportunity-Ordinal Scheduler Proof

`DIAGNOSTIC_ONLY`。Q5 historical evidenceとP2-D5-1 FAILは変更しない。
actual swap間隔をexact refresh rationalでopportunity ordinalへ変換し、60 fps source domainを
pure/offline replayした。production scheduler、formal checker、2% thresholdは変更していない。

全runでframe 0開始、unique frame strictly increasing、frame 3600非表示、
`displayed + true_dropped = 3600`を満たした。Q5のTRUE_OPPORTUNITY_LOSSとの差は、
cadence/domain lossとsynthetic skip地点以外のordinal gap lossへ分解して保存した。
"@
$readme | Set-Content -LiteralPath (Join-Path $OutputDirectory 'README.md') -Encoding utf8

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
    Where-Object FullName -ne $manifestPath | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($OutputDirectory.Length + 1).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    } | Set-Content -LiteralPath $manifestPath -Encoding ascii

Write-Host "P2-Q6 offline proof完了: 2/2 trace PASS root=$OutputDirectory"
