[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C1Proof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
foreach($path in @($C1Proof,$C1Checker)){if(-not(Test-Path -LiteralPath $path)){Fail "C2.1必須pathがありません: $path"}}
if(Test-Path -LiteralPath $Output){Fail "既存C2.1 artifactを上書きしません: $Output"}
& pwsh -NoProfile -File $C1Checker -Proof $C1Proof *> $null
if($LASTEXITCODE-ne0){Fail 'C2.1がconsumeするC1 sealed authorityが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c21-required-intent-domain-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c21-from-c1-core.ps1')
$c1=Get-Content -LiteralPath $C1Proof -Raw -Encoding utf8|ConvertFrom-Json
$result=Invoke-MvmC21ProofFromSealedC1 -C1ProofObject $c1 -C1ProofPath $C1Proof `
    -C1CheckpointSha '5034bfcd41dd9f5c860827a9594b604be5db7446'
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$result|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.authority_exact){Fail "W2-C2.1 required intent authorityが未確定です: $(@($result.blockers)-join', ')"}
Write-Host 'P2-D5-2 W2-C2.1 required intent domain authority: PASS'
