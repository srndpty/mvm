[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
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
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W2-D proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w2-d-formal-v2-shadow-1'){Fail 'W2-D schemaが不正です'}

# W2-D は noncanonical shadow である。canonical / performance semantics の注入を
# 先に fail-close する。ここを通してから再構築比較へ進む。
foreach($falseFlag in @('canonical_authority','performance_threshold_evaluated',
                        'canonical_verdict_evaluated','frame_swapped_retirement_changed',
                        'source_frame_identity_used','nearest_qpc_or_tolerance_used')){
    if($actual.PSObject.Properties.Name-notcontains$falseFlag){Fail "W2-D authority flagがありません: $falseFlag"}
    if([bool]$actual.$falseFlag){Fail "W2-Dで禁止されているflagがtrueです: $falseFlag"}
}
if(-not[bool]$actual.shadow_only){Fail 'W2-Dはshadow onlyでなければなりません'}
if(-not[bool]$actual.layer1a_layer1b_count_difference_is_not_a_verdict){
    Fail 'Layer 1A / Layer 1Bのcount差をverdictに接続してはいけません'
}
# threshold / fps / drop / canonical verdict に属する field を artifact へ持ち込まない。
$forbiddenFields=@('drop_rate','effective_fps','fps','performance_pass','performance_verdict',
                   'canonical_verdict','canonical_pass','threshold','true_drop_count',
                   'formal_true_drop_count','physical_unfilled_count','unfilled_physical_opportunity_count')
$scopes=@();$scopes+=,@('artifact',$actual)
foreach($run in @($actual.runs)){
    $scopes+=,@("run $([string]$run.run)",$run)
    foreach($record in @($run.records)){$scopes+=,@("run $([string]$run.run) record",$record)}
}
foreach($scope in $scopes){
    foreach($name in $forbiddenFields){
        if($scope[1].PSObject.Properties.Name-contains$name){
            Fail "W2-Dにperformance semanticsのfieldがあります: $($scope[0]) / $name"
        }
    }
}

foreach($sourceField in @('source_c1_proof','source_c21_proof','source_c2_proof')){
    if($actual.PSObject.Properties.Name-notcontains$sourceField){Fail "W2-D provenanceがありません: $sourceField"}
    if(-not(Test-Path -LiteralPath ([string]$actual.$sourceField))){
        Fail "W2-D upstream authority pathがありません: $([string]$actual.$sourceField)"
    }
}
$c1Path=[string]$actual.source_c1_proof
$c21Path=[string]$actual.source_c21_proof
$c2Path=[string]$actual.source_c2_proof
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w2d-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-formal-v2-shadow-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-from-authorities-core.ps1')
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$c21=Get-Content -LiteralPath $c21Path -Raw -Encoding utf8|ConvertFrom-Json
$c2=Get-Content -LiteralPath $c2Path -Raw -Encoding utf8|ConvertFrom-Json
$replay=Invoke-MvmDUpstreamAuthorityReplay -C1ProofObject $c1 -C1ProofPath $c1Path `
    -C21ProofPath $c21Path -C2ProofPath $c2Path -C1Checker $C1Checker -C21Checker $C21Checker `
    -C2Checker $C2Checker -C24Checker $C24Checker -W2AChecker $W2AChecker -B2Checker $B2Checker `
    -SourceRoot $SourceRoot -WorkDirectory $WorkDirectory
$expected=Invoke-MvmDFormalV2ProofFromSealedAuthorities -C1ProofObject $c1 -C1ProofPath $c1Path `
    -C21ProofObject $c21 -C21ProofPath $c21Path -C2ProofObject $c2 -C2ProofPath $c2Path `
    -UpstreamReplay $replay -C1CheckpointSha '5034bfcd41dd9f5c860827a9594b604be5db7446'
Assert-MvmDFormalV2Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.integration_exact){Fail "W2-D formal-v2 shadowがINVALIDです: $(@($expected.blockers)-join', ')"}
Write-Host ("P2-D5-2 W2-D checker: PASS required={0} satisfied={1} unsatisfied={2} physical={3} filled={4}" -f `
    $expected.required_intent_count,$expected.satisfied_intent_count,$expected.unsatisfied_intent_count,`
    $expected.physical_vblank_opportunity_count,$expected.filled_physical_opportunity_count)
