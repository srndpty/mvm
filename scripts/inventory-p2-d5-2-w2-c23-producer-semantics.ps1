[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C21Proof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
foreach($path in @($C21Proof,$C21Checker)){if(-not(Test-Path -LiteralPath $path)){throw "C2.3必須pathがありません: $path"}}
if(Test-Path -LiteralPath $Output){throw "既存C2.3 artifactを上書きしません: $Output"}
& pwsh -NoProfile -File $C21Checker -Proof $C21Proof *> $null
if($LASTEXITCODE-ne0){throw 'C2.3がconsumeするC2.1 authorityが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c23-producer-semantics-core.ps1')
$source=Get-Content -LiteralPath $C21Proof -Raw -Encoding utf8|ConvertFrom-Json
$result=Invoke-MvmC23ProducerSemanticsAttribution -C21ProofObject $source -C21ProofPath $C21Proof
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$result|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.authority_exact){throw "C2.3 producer semantics authorityが不成立です: $(@($result.blockers)-join', ')"}
Write-Output 'P2-D5-2 W2-C2.3 producer semantics attribution: PASS'
