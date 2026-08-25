[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Proof)){throw "C2.3 proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
$sourcePath=[string]$actual.source_c21_proof
& pwsh -NoProfile -File $C21Checker -Proof $sourcePath *> $null
if($LASTEXITCODE-ne0){throw 'C2.3 checkerのC2.1 sealed replayが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c23-producer-semantics-core.ps1')
$source=Get-Content -LiteralPath $sourcePath -Raw -Encoding utf8|ConvertFrom-Json
$expected=Invoke-MvmC23ProducerSemanticsAttribution -C21ProofObject $source -C21ProofPath $sourcePath
Assert-MvmC23Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.authority_exact){throw "C2.3 producer semantics authorityが不成立です: $(@($expected.blockers)-join', ')"}
Write-Output 'P2-D5-2 W2-C2.3 checker: PASS'
