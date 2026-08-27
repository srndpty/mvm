[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$ExpectedCheckpointSha,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W2EChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-e-canonical-authority.ps1'),
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W3 proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w3-canonical-performance-1'){Fail 'W3 schemaが不正です'}

# 段の順序が守られていること。1〜3が不成立のまま4〜6を評価した artifact を受理しない。
$stageValid=[bool]$actual.stage1_acquisition_protocol_valid-and
    [bool]$actual.stage2_canonical_authority_valid-and[bool]$actual.stage3_accounting_valid
foreach($laterStage in @('stage4_metric_constructed','stage5_threshold_evaluated',
                         'stage6_canonical_verdict_evaluated','performance_evaluated')){
    if(-not$stageValid-and[bool]$actual.$laterStage){
        Fail "authority / protocolがINVALIDなのに後段を評価しています: $laterStage"
    }
}
if(-not$stageValid){
    if([string]$actual.verdict-ne'AUTHORITY_OR_PROTOCOL_INVALID'){
        Fail 'authority / protocol INVALIDをperformance verdictへ変換しています'
    }
    foreach($nullField in @('canonical_performance_pass','canonical_drop_rate','canonical_effective_fps')){
        if($null-ne$actual.$nullField){
            Fail "INVALID runでperformance値を出しています: $nullField"
        }
    }
}
# legacy presentation authority を canonical verdict へ持ち込んでいないこと。
if([bool]$actual.legacy_presentation_authority_used){
    Fail 'canonical verdictがlegacy presentation authorityを使っています'
}
if(-not[bool]$actual.layer1a_layer1b_count_difference_is_not_a_verdict){
    Fail 'Layer 1A / Layer 1Bのcount差をverdictへ接続しています'
}
if(-not[bool]$actual.thresholds_frozen_unchanged){Fail 'thresholdがfrozen値から変更されています'}
foreach($sourceField in @('source_acquisition_provenance','source_canonical_proof')){
    if($actual.PSObject.Properties.Name-notcontains$sourceField){Fail "W3 provenanceがありません: $sourceField"}
    if(-not(Test-Path -LiteralPath ([string]$actual.$sourceField))){
        Fail "W3 upstream pathがありません: $([string]$actual.$sourceField)"
    }
}
foreach($binding in @(@('source_acquisition_provenance','source_acquisition_provenance_sha256'),
                      @('source_canonical_proof','source_canonical_proof_sha256'))){
    $hash=(Get-FileHash -LiteralPath ([string]$actual.$($binding[0])) -Algorithm SHA256).Hash.ToLowerInvariant()
    if([string]$actual.$($binding[1])-ne$hash){Fail "W3が別のupstream artifactを参照しています: $($binding[0])"}
}

if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w3-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w3-shared-replay.ps1')
$expected=Invoke-MvmW3CanonicalPerformanceFromCanonical `
    -AcquisitionProvenancePath ([string]$actual.source_acquisition_provenance) `
    -CanonicalProofPath ([string]$actual.source_canonical_proof) `
    -ExpectedCheckpointSha $ExpectedCheckpointSha -SourceRoot $SourceRoot `
    -W2EChecker $W2EChecker -WorkDirectory $WorkDirectory
Assert-MvmW3Proof -Expected $expected -Actual $actual
Write-Host ("P2-D5-2 W3 checker: {0} fps={1} drop={2} required={3} satisfied={4}" -f `
    $expected.verdict,$expected.canonical_effective_fps,$expected.canonical_drop_rate,`
    $expected.canonical_required_intent_count,$expected.canonical_satisfied_intent_count)
