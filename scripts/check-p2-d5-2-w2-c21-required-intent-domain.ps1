[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "C2.1 proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
$c1Path=[string]$actual.source_c1_proof
& pwsh -NoProfile -File $C1Checker -Proof $c1Path *> $null
if($LASTEXITCODE-ne0){Fail 'C2.1 checkerのC1 sealed replayが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c21-required-intent-domain-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c21-from-c1-core.ps1')
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$expected=Invoke-MvmC21ProofFromSealedC1 -C1ProofObject $c1 -C1ProofPath $c1Path `
    -C1CheckpointSha '5034bfcd41dd9f5c860827a9594b604be5db7446'
Assert-MvmC21Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.authority_exact){Fail "C2.1 authorityが未確定です: $(@($expected.blockers)-join', ')"}
Write-Host 'P2-D5-2 W2-C2.1 checker: PASS'
