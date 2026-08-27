[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$W3Proof,
    [Parameter(Mandatory=$true)][string]$C1Proof,
    [Parameter(Mandatory=$true)][string]$C21Proof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W3Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w3-canonical-performance.ps1'),
    [switch]$SkipW3Replay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(Test-Path -LiteralPath $Output){Fail "既存W4-A artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w4a-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-a-shared-replay.ps1')
$result=Invoke-MvmW4AttributionFromCanonical -W3ProofPath $W3Proof -C1ProofPath $C1Proof `
    -C21ProofPath $C21Proof -SourceRoot $SourceRoot -W3Checker $W3Checker `
    -WorkDirectory $WorkDirectory -ReplayW3Checker (-not $SkipW3Replay)
$result|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.attribution_exact){Fail "W4-A attributionが不成立です: $(@($result.blockers)-join', ')"}
Write-Host ("P2-D5-2 W4-A intent attribution: PASS required={0} satisfied={1} unsatisfied={2} downstream_loss={3}" -f `
    $result.required_intent_count,$result.satisfied_intent_count,$result.unsatisfied_intent_count,$result.downstream_loss_count)
