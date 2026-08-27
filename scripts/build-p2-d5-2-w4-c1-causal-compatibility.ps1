[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$W4BProof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W4BChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w4-b-producer-semantics.ps1'),
    [switch]$SkipW4BReplay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Output){throw "既存W4-C1 artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){
    New-Item -ItemType Directory -Path $outputDirectory|Out-Null
}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w4c1-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-c1-shared-replay.ps1')
$result=Invoke-MvmW4C1FromProducerSemantics -W4BProofPath $W4BProof -SourceRoot $SourceRoot `
    -W4BChecker $W4BChecker -WorkDirectory $WorkDirectory -ReplayW4BChecker (-not$SkipW4BReplay)
$result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.attribution_exact){throw "W4-C1 replayが不成立です: $(@($result.blockers)-join', ')"}
Write-Host ("P2-D5-2 W4-C1 compatibility: PASS compatible={0} not_observable={1}" -f `
    $result.compatible_transition_count,$result.not_observable_transition_count)
