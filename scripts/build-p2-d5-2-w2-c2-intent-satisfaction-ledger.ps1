[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C1Proof,
    [Parameter(Mandatory=$true)][string]$C21Proof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1'),
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
foreach($path in @($C1Proof,$C21Proof,$C1Checker,$C21Checker)){if(-not(Test-Path -LiteralPath $path)){Fail "C2必須pathがありません: $path"}}
if(Test-Path -LiteralPath $Output){Fail "既存C2 artifactを上書きしません: $Output"}
& pwsh -NoProfile -File $C1Checker -Proof $C1Proof *> $null
if($LASTEXITCODE-ne0){Fail 'C1 sealed/replay authorityが不成立です'}
& pwsh -NoProfile -File $C21Checker -Proof $C21Proof *> $null
if($LASTEXITCODE-ne0){Fail 'C2.1 required intent authorityが不成立です'}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c2-intent-satisfaction-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c2-from-c1-core.ps1')
$c1=Get-Content -LiteralPath $C1Proof -Raw -Encoding utf8|ConvertFrom-Json
$c21=Get-Content -LiteralPath $C21Proof -Raw -Encoding utf8|ConvertFrom-Json
$c1CheckpointSha='5034bfcd41dd9f5c860827a9594b604be5db7446'
$result=Invoke-MvmC2ProofFromSealedC1 -C1ProofObject $c1 -C1ProofPath $C1Proof `
    -C21ProofObject $c21 -C21ProofPath $C21Proof -C1CheckpointSha $c1CheckpointSha
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.ledger_exact){Fail "W2-C2 intent satisfaction ledgerが不成立です: $(@($result.blockers)-join', ')"}
Write-Host "P2-D5-2 W2-C2 ledger: PASS ($($result.formal_presented_event_count) formal Presented)"
