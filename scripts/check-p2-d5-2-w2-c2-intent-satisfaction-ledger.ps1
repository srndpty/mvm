[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1'),
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W2-C2 proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if($actual.PSObject.Properties.Name-notcontains'source_c1_proof'){Fail 'C2 source C1 proofがありません'}
if($actual.PSObject.Properties.Name-notcontains'source_c21_proof'){Fail 'C2 source C2.1 proofがありません'}
$c1Path=[string]$actual.source_c1_proof
$c21Path=[string]$actual.source_c21_proof
foreach($path in @($c1Path,$c21Path,$C1Checker,$C21Checker)){if(-not(Test-Path -LiteralPath $path)){Fail "C2 authority pathがありません: $path"}}
& pwsh -NoProfile -File $C1Checker -Proof $c1Path *> $null
if($LASTEXITCODE-ne0){Fail 'C2がconsumeするC1 sealed/replay authorityが不成立です'}
& pwsh -NoProfile -File $C21Checker -Proof $c21Path *> $null
if($LASTEXITCODE-ne0){Fail 'C2がconsumeするC2.1 required intent authorityが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c2-intent-satisfaction-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c2-from-c1-core.ps1')
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$c21=Get-Content -LiteralPath $c21Path -Raw -Encoding utf8|ConvertFrom-Json
$c1CheckpointSha='5034bfcd41dd9f5c860827a9594b604be5db7446'
$expected=Invoke-MvmC2ProofFromSealedC1 -C1ProofObject $c1 -C1ProofPath $c1Path `
    -C21ProofObject $c21 -C21ProofPath $c21Path -C1CheckpointSha $c1CheckpointSha
Assert-MvmC2Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.ledger_exact){Fail "W2-C2 ledgerがINVALIDです: $(@($expected.blockers)-join', ')"}
Write-Host "P2-D5-2 W2-C2 checker: PASS ($($expected.formal_presented_event_count) formal Presented)"
