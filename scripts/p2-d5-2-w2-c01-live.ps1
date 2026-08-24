[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,60)][int]$WarmupSeconds=2,
    [ValidateRange(1,60)][int]$MeasureSeconds=5,
    [ValidateRange(30,600)][int]$TimeoutSeconds=120
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$b2Runner=Join-Path $PSScriptRoot 'p2-d5-2-w2-b2-live.ps1'
$envelopeChecker=Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c01-capture-envelope.ps1'
$inventory=Join-Path $PSScriptRoot 'inventory-p2-d5-2-w2-c0-display-candidates.ps1'
foreach($path in @($b2Runner,$envelopeChecker,$inventory)){if(-not(Test-Path -LiteralPath $path)){throw "C0.1必須scriptがありません: $path"}}
& pwsh -NoProfile -File $b2Runner -OutputDirectory $OutputDirectory -Runs $Runs `
    -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
if($LASTEXITCODE-ne0){throw 'C0.1 fresh acquisitionのB1/B2 gateが失敗しました'}
$checks=@()
for($run=1;$run-le$Runs;++$run){
    $runDirectory=Join-Path $OutputDirectory "run-$run"
    $appJson=Join-Path $runDirectory 'traced-app.json'
    $proofPath=Join-Path $runDirectory 'capture-envelope-check.json'
    & pwsh -NoProfile -File $envelopeChecker -InputJson $appJson -Output $proofPath
    if($LASTEXITCODE-ne0){throw "C0.1 run $run capture envelope gateが失敗しました"}
    $checks+=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
}
$inventoryPath=Join-Path $OutputDirectory 'display-candidate-inventory.json'
& pwsh -NoProfile -File $inventory -B2LiveDirectory $OutputDirectory -Output $inventoryPath `
    -RequireCoverageComplete
if($LASTEXITCODE-ne0){throw 'C0.1 display candidate coverage gateが失敗しました'}
$coverage=Get-Content -LiteralPath $inventoryPath -Raw -Encoding utf8|ConvertFrom-Json
$summary=[ordered]@{
    schema='mvm-p2-d5-2-w2-c01-live-1';stage='P2-D5-2-W2-C0.1'
    acquisition_mode='CanonicalPresentMonLive';runs=$Runs
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    b1_b2_semantics_unchanged=$true;measurement_window_extended=$false
    capture_envelope_checks=$checks
    coverage_complete=[bool]$coverage.coverage_complete
    foreign_intent_minimum_required=0
    verdict=$(if([bool]$coverage.coverage_complete){'DISPLAY_DOMAIN_CANDIDATE_COVERAGE_COMPLETE'}else{'DISPLAY_DOMAIN_CANDIDATE_COVERAGE_INCOMPLETE'})
}
$summary|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $OutputDirectory 'w2-c01-live-summary.json') -Encoding utf8
Write-Host "P2-D5-2 W2-C0.1 live: PASS ($Runs/$Runs) $($summary.verdict)"
