[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$W4AProof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W4AChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w4-a-intent-attribution.ps1'),
    [switch]$SkipW4AReplay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(Test-Path -LiteralPath $Output){Fail "既存W4-B artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w4b-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-b-shared-replay.ps1')
$result=Invoke-MvmW4BFromAttribution -W4AProofPath $W4AProof -SourceRoot $SourceRoot `
    -W4AChecker $W4AChecker -WorkDirectory $WorkDirectory -ReplayW4AChecker (-not $SkipW4AReplay)
$result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath $Output -Encoding utf8
if(-not[bool]$result.attribution_exact){Fail "W4-B attributionが不成立です: $(@($result.blockers)-join', ')"}
Write-Host ("P2-D5-2 W4-B producer semantics: PASS missing={0} isolated={1} double={2}" -f `
    $result.missing_intent_count,$result.cohort_event_counts.ISOLATED_MISSING,`
    $result.cohort_event_counts.DOUBLE_MISSING_BOUNDARY)
