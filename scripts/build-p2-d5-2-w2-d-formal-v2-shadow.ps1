[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C1Proof,
    [Parameter(Mandatory=$true)][string]$C21Proof,
    [Parameter(Mandatory=$true)][string]$C2Proof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
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
foreach($path in @($C1Proof,$C21Proof,$C2Proof)){if(-not(Test-Path -LiteralPath $path)){Fail "W2-D必須authorityがありません: $path"}}
if(Test-Path -LiteralPath $Output){Fail "既存W2-D artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w2d-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-formal-v2-shadow-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-from-authorities-core.ps1')
$c1=Get-Content -LiteralPath $C1Proof -Raw -Encoding utf8|ConvertFrom-Json
$c21=Get-Content -LiteralPath $C21Proof -Raw -Encoding utf8|ConvertFrom-Json
$c2=Get-Content -LiteralPath $C2Proof -Raw -Encoding utf8|ConvertFrom-Json
$replay=Invoke-MvmDUpstreamAuthorityReplay -C1ProofObject $c1 -C1ProofPath $C1Proof `
    -C21ProofPath $C21Proof -C2ProofPath $C2Proof -C1Checker $C1Checker -C21Checker $C21Checker `
    -C2Checker $C2Checker -C24Checker $C24Checker -W2AChecker $W2AChecker -B2Checker $B2Checker `
    -SourceRoot $SourceRoot -WorkDirectory $WorkDirectory
$result=Invoke-MvmDFormalV2ProofFromSealedAuthorities -C1ProofObject $c1 -C1ProofPath $C1Proof `
    -C21ProofObject $c21 -C21ProofPath $C21Proof -C2ProofObject $c2 -C2ProofPath $C2Proof `
    -UpstreamReplay $replay -C1CheckpointSha '5034bfcd41dd9f5c860827a9594b604be5db7446'
$result|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.integration_exact){Fail "W2-D formal-v2 shadow integrationが不成立です: $(@($result.blockers)-join', ')"}
Write-Host ("P2-D5-2 W2-D formal-v2 shadow: PASS required={0} satisfied={1} unsatisfied={2} physical={3} filled={4}" -f `
    $result.required_intent_count,$result.satisfied_intent_count,$result.unsatisfied_intent_count,`
    $result.physical_vblank_opportunity_count,$result.filled_physical_opportunity_count)
