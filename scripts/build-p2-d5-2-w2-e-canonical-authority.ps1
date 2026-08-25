[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$W2DProof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W2DChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-d-formal-v2-shadow.ps1'),
    [string]$RetirementInventory=(Join-Path $PSScriptRoot 'inventory-p2-d5-2-w2-e-legacy-authority.ps1'),
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1'),
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1'),
    [string]$C2Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c2-intent-satisfaction-ledger.ps1'),
    [string]$C24Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c24-formal-transport.ps1'),
    [string]$W2AChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2a-physical-domain.ps1'),
    [string]$B2Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-b2-terminal-shadow.ps1'),
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $W2DProof)){Fail "W2-D shadow proofがありません: $W2DProof"}
if(Test-Path -LiteralPath $Output){Fail "既存W2-D canonical artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w2e-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-e-shared-replay.ps1')
$result=Invoke-MvmECanonicalAuthorityFromW2D -W2DProofPath $W2DProof -SourceRoot $SourceRoot `
    -W2DChecker $W2DChecker -RetirementInventory $RetirementInventory `
    -C1Checker $C1Checker -C21Checker $C21Checker -C2Checker $C2Checker -C24Checker $C24Checker `
    -W2AChecker $W2AChecker -B2Checker $B2Checker -WorkDirectory $WorkDirectory
$result|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.cutover_exact){Fail "W2-E canonical cutoverが不成立です: $(@($result.blockers)-join', ')"}
Write-Host ("P2-D5-2 W2-E canonical authority: PASS schema={0} required={1} satisfied={2} unsatisfied={3} legacy_canonical={4}" -f `
    $result.presentation_authority_schema,$result.canonical_required_intent_count,`
    $result.canonical_satisfied_intent_count,$result.canonical_unsatisfied_intent_count,`
    $result.legacy_metric_canonical_decision_count)
