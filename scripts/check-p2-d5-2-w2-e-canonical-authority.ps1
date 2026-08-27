[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W2DChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-d-formal-v2-shadow.ps1'),
    [string]$RetirementInventory=(Join-Path $PSScriptRoot 'inventory-p2-d5-2-w2-e-legacy-authority.ps1'),
    [string]$C1Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c14-sealed-mapping-replay.ps1'),
    [string]$C21Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c21-required-intent-domain.ps1'),
    [string]$C2Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c2-intent-satisfaction-ledger.ps1'),
    [string]$C24Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-c24-formal-transport.ps1'),
    [string]$W2AChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2a-physical-domain.ps1'),
    [string]$B2Checker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w2-b2-terminal-shadow.ps1'),
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2-W2-E canonical authority disposition (machine-readable)。
# W2-E retirement inventory がこの宣言を読む。legacy presentation metric
# (effective_fps / drop_rate / effective_video_fps) を参照するが、
# canonical performance verdict は出さないことを宣言する。
$MvmPresentationAuthorityDisposition = [ordered]@{
    presentation_authority        = 'FORMAL_V2'
    legacy_presentation_metrics   = 'DIAGNOSTIC'
    canonical_performance_verdict = 'DEFERRED_TO_W3'
}
[void]$MvmPresentationAuthorityDisposition
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W2-E canonical proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w2-e-canonical-authority-1'){Fail 'W2-E schemaが不正です'}

# --- authority selector。cutover したことと、していないことの両方を先に固定する ---
foreach($trueFlag in @('canonical_authority','legacy_presentation_authority_retired',
                       'legacy_diagnostics_retained','layer1a_layer1b_count_difference_is_not_a_verdict')){
    if($actual.PSObject.Properties.Name-notcontains$trueFlag){Fail "W2-E authority flagがありません: $trueFlag"}
    if(-not[bool]$actual.$trueFlag){Fail "W2-Eで必須のflagがfalseです: $trueFlag"}
}
foreach($falseFlag in @('frame_swapped_authority','dwm_frame_statistics_authority',
                        'retirement_means_deletion','performance_threshold_evaluated',
                        'canonical_verdict_evaluated','historical_verdicts_rewritten',
                        'source_frame_identity_used','nearest_qpc_or_tolerance_used')){
    if($actual.PSObject.Properties.Name-notcontains$falseFlag){Fail "W2-E authority flagがありません: $falseFlag"}
    if([bool]$actual.$falseFlag){Fail "W2-Eで禁止されているflagがtrueです: $falseFlag"}
}
if([string]$actual.presentation_authority_schema-ne'FORMAL_V2'){
    Fail 'canonical presentation authority schemaがFORMAL_V2ではありません'
}
if([string]$actual.canonical_performance_verdict_deferred_to-ne'W3'){
    Fail 'canonical performance verdictの保留先がW3ではありません'
}
if([int64]$actual.legacy_metric_canonical_decision_count-ne0){
    Fail 'legacy metricがcanonical verdictへ到達しています'
}
# W2-E は authority selector の段であり performance verdict の段ではない。
$forbiddenFields=@('drop_rate','effective_fps','effective_video_fps','fps','performance_pass',
                   'performance_verdict','canonical_pass','canonical_fail','threshold',
                   'true_drop_count','formal_true_drop_count')
$scopes=@();$scopes+=,@('artifact',$actual)
foreach($run in @($actual.runs)){
    $scopes+=,@("run $([string]$run.run)",$run)
    foreach($record in @($run.records)){$scopes+=,@("run $([string]$run.run) record",$record)}
}
foreach($scope in $scopes){
    foreach($name in $forbiddenFields){
        if($scope[1].PSObject.Properties.Name-contains$name){
            Fail "W2-Eにperformance semanticsのfieldがあります: $($scope[0]) / $name"
        }
    }
}

# --- provenance。canonical が別のW2-D proofを指していないこと ---
if($actual.PSObject.Properties.Name-notcontains'source_w2d_proof'){Fail 'W2-E provenanceがありません: source_w2d_proof'}
$w2dPath=[string]$actual.source_w2d_proof
if(-not(Test-Path -LiteralPath $w2dPath)){Fail "W2-D shadow proofがありません: $w2dPath"}
$w2dHash=(Get-FileHash -LiteralPath $w2dPath -Algorithm SHA256).Hash.ToLowerInvariant()
if([string]$actual.source_w2d_proof_sha256-ne$w2dHash){
    Fail 'W2-E canonicalが別のW2-D shadow proofを参照しています'
}

if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w2e-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-e-shared-replay.ps1')
$expected=Invoke-MvmECanonicalAuthorityFromW2D -W2DProofPath $w2dPath -SourceRoot $SourceRoot `
    -W2DChecker $W2DChecker -RetirementInventory $RetirementInventory `
    -C1Checker $C1Checker -C21Checker $C21Checker -C2Checker $C2Checker -C24Checker $C24Checker `
    -W2AChecker $W2AChecker -B2Checker $B2Checker -WorkDirectory $WorkDirectory
Assert-MvmECanonicalProof -Expected $expected -Actual $actual
if(-not[bool]$expected.cutover_exact){Fail "W2-E canonical cutoverがINVALIDです: $(@($expected.blockers)-join', ')"}
Write-Host ("P2-D5-2 W2-E checker: PASS schema={0} required={1} satisfied={2} unsatisfied={3} legacy_canonical={4}" -f `
    $expected.presentation_authority_schema,$expected.canonical_required_intent_count,`
    $expected.canonical_satisfied_intent_count,$expected.canonical_unsatisfied_intent_count,`
    $expected.legacy_metric_canonical_decision_count)
