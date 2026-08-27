[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W3Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w3-canonical-performance.ps1'),
    [switch]$SkipW3Replay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W4-A proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w4-a-intent-attribution-1'){Fail 'W4-A schemaが不正です'}

# W4-A は attribution の段であり、原因判定や instrumentation A/B の段ではない。
foreach($falseFlag in @('root_cause_determined','instrumentation_ab_performed')){
    if($actual.PSObject.Properties.Name-notcontains$falseFlag){Fail "W4-A flagがありません: $falseFlag"}
    if([bool]$actual.$falseFlag){Fail "W4-Aで禁止されているflagがtrueです: $falseFlag"}
}
if(-not[bool]$actual.attribution_only){Fail 'W4-Aはattribution onlyでなければなりません'}
if([string]$actual.population-ne'EXACT_REQUIRED_CURRENT_INTENT_SET'){Fail 'W4-A母集団が不正です'}

# provenance splice。別のupstream artifactを指していないこと。
foreach($binding in @(@('source_w3_proof','source_w3_proof_sha256'),
                      @('source_c1_proof','source_c1_proof_sha256'),
                      @('source_c21_proof','source_c21_proof_sha256'))){
    if($actual.PSObject.Properties.Name-notcontains$binding[0]){Fail "W4-A provenanceがありません: $($binding[0])"}
    $path=[string]$actual.$($binding[0])
    if(-not(Test-Path -LiteralPath $path)){Fail "W4-A upstream pathがありません: $path"}
    $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if([string]$actual.$($binding[1])-ne$hash){Fail "W4-Aが別のupstream artifactを参照しています: $($binding[0])"}
}

# aggregate だけを信用しない。bucket の排他性と網羅性をartifact上でも先に検査する。
$bucketNames=@('A_NO_PRIMARY_SCHEDULER_DECISION','C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY',
    'D_NO_NATIVE_PRESENT','E_NO_EXACT_FORMAL_PRESENTED','F_FORMAL_PRESENTED_OUTSIDE_DOMAIN',
    'G_SATISFIED_IN_DOMAIN')
$bucketSum=0L
foreach($name in $bucketNames){
    if($actual.buckets.PSObject.Properties.Name-notcontains$name){Fail "W4-A bucketがありません: $name"}
    $bucketSum+=[int64]$actual.buckets.$name
}
if($bucketSum-ne[int64]$actual.required_intent_count){Fail 'W4-A bucket sumがrequired countと一致しません'}
if([int64]$actual.satisfied_intent_count-ne[int64]$actual.buckets.G_SATISFIED_IN_DOMAIN){
    Fail 'W4-A satisfied bucketがsatisfied countと一致しません'
}
if(($bucketSum-[int64]$actual.buckets.G_SATISFIED_IN_DOMAIN)-ne[int64]$actual.unsatisfied_intent_count){
    Fail 'W4-A unsatisfied bucket sumが一致しません'
}
# canonical 値との一致。W3 の verdict をここで作り直さない。
foreach($identity in @(@('required_intent_count','canonical_required_intent_count'),
                       @('satisfied_intent_count','canonical_satisfied_intent_count'),
                       @('unsatisfied_intent_count','canonical_unsatisfied_intent_count'))){
    if([int64]$actual.$($identity[0])-ne[int64]$actual.$($identity[1])){
        Fail "W4-A集計がcanonical値と一致しません: $($identity[0])"
    }
}

if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w4a-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-a-shared-replay.ps1')
$expected=Invoke-MvmW4AttributionFromCanonical -W3ProofPath ([string]$actual.source_w3_proof) `
    -C1ProofPath ([string]$actual.source_c1_proof) -C21ProofPath ([string]$actual.source_c21_proof) `
    -SourceRoot $SourceRoot -W3Checker $W3Checker -WorkDirectory $WorkDirectory `
    -ReplayW3Checker (-not $SkipW3Replay)
Assert-MvmW4Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.attribution_exact){Fail "W4-A attributionがINVALIDです: $(@($expected.blockers)-join', ')"}
Write-Host ("P2-D5-2 W4-A checker: PASS required={0} satisfied={1} unsatisfied={2} downstream_loss={3}" -f `
    $expected.required_intent_count,$expected.satisfied_intent_count,`
    $expected.unsatisfied_intent_count,$expected.downstream_loss_count)
