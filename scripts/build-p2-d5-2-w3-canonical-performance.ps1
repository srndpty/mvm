[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AcquisitionProvenance,
    [Parameter(Mandatory=$true)][string]$CanonicalProof,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$ExpectedCheckpointSha,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W2EChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-e-canonical-authority.ps1'),
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
foreach($path in @($AcquisitionProvenance,$CanonicalProof,$W2EChecker)){
    if(-not(Test-Path -LiteralPath $path)){Fail "W3必須inputがありません: $path"}
}
if(Test-Path -LiteralPath $Output){Fail "既存W3 artifactを上書きしません: $Output"}
$outputDirectory=Split-Path -Parent $Output
if([string]::IsNullOrWhiteSpace($outputDirectory)){$outputDirectory='.'}
if(-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path $outputDirectory ((Split-Path -Leaf $Output)+'.w3-replay')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w3-shared-replay.ps1')
$result=Invoke-MvmW3CanonicalPerformanceFromCanonical -AcquisitionProvenancePath $AcquisitionProvenance `
    -CanonicalProofPath $CanonicalProof -ExpectedCheckpointSha $ExpectedCheckpointSha `
    -SourceRoot $SourceRoot -W2EChecker $W2EChecker -WorkDirectory $WorkDirectory
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if([string]$result.verdict-eq'AUTHORITY_OR_PROTOCOL_INVALID'){
    $blockers=@($result.stage1_blockers)+@($result.stage2_blockers)+@($result.stage3_blockers)
    Fail "W3 authority / protocolが不成立です: $(@($blockers)-join', ')"
}
Write-Host ("P2-D5-2 W3 canonical performance: {0} fps={1:N3} drop={2:P3} required={3} satisfied={4}" -f `
    $result.verdict,$result.canonical_effective_fps,$result.canonical_drop_rate,`
    $result.canonical_required_intent_count,$result.canonical_satisfied_intent_count)
