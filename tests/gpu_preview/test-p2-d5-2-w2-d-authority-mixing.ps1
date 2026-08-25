[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C13Fixture,
    [Parameter(Mandatory=$true)][string]$C13Core,
    [Parameter(Mandatory=$true)][string]$C1Checker,
    [Parameter(Mandatory=$true)][string]$C2Builder,
    [Parameter(Mandatory=$true)][string]$C2Checker,
    [Parameter(Mandatory=$true)][string]$Builder,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$caseDirectory=Join-Path $Directory "process-$PID"
if(Test-Path -LiteralPath $caseDirectory){Remove-Item -LiteralPath $caseDirectory -Recurse -Force}
New-Item -ItemType Directory -Path $caseDirectory|Out-Null

function Assert-Rejected([scriptblock]$Action,[string]$Message){
    $failed=$false
    try{& $Action}catch{$failed=$true}
    if(-not$failed){throw $Message}
}

# --- sealed fixture cohort ---
& $C13Fixture -Case Good -Core $C13Core -Checker $C1Checker -Directory $caseDirectory *> $null
$c1Path=Join-Path $caseDirectory 'c13-good.json'
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$runDirectory=Join-Path $c1.source_c011_directory 'run-1'
$appPath=Join-Path $runDirectory 'traced-app.json'
$terminalPath=Join-Path $runDirectory 'terminal-shadow.json'

# W2-A physical domain (origin=1, last=3 -> cardinality 3) と Layer 1A required=1 を与える。
# required(1) != physical opportunity(3) は W2-D では INVALID にしない。
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$app|Add-Member -NotePropertyName required_measurement_frame_count -NotePropertyValue 1
$shadow=$app.presentation_opportunity.physical_vblank_domain_shadow
$shadow|Add-Member -NotePropertyName required_intent_count -NotePropertyValue 1
$shadow|Add-Member -NotePropertyName physical_opportunity_count -NotePropertyValue 3
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $appPath -Encoding utf8

# B2 terminal 側の transport identity。C1 candidate 側と一致していなければならない。
@{records=@(@{final_state='Presented';etw_sequence=2;displayed_qpc=@(210)
    formal_transport_eligible=$true;embedded_token_serial='102';native_present_serial='12'})}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath $terminalPath -Encoding utf8
$c1.runs[0].sealed_input_sha256.traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
$c1.runs[0].sealed_input_sha256.b2_terminal_shadow=(Get-FileHash -LiteralPath $terminalPath -Algorithm SHA256).Hash.ToLowerInvariant()
$c1|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $c1Path -Encoding utf8
$c1Hash=(Get-FileHash -LiteralPath $c1Path -Algorithm SHA256).Hash.ToLowerInvariant()
$checkpoint='5034bfcd41dd9f5c860827a9594b604be5db7446'

# --- stub authority checker ---
# C1 / C2 は本物を再実行する。C2.1 / C2.4 / W2-A / B2 は fixture cohort には
# 実artifactが無いため stub にする。stub でも W2-D の provenance binding と
# record 再構築は本物のまま動く。
function New-StubChecker([string]$Name,[string]$Body){
    $path=Join-Path $caseDirectory $Name
    $Body|Set-Content -LiteralPath $path -Encoding utf8
    return $path
}
$c21Checker=New-StubChecker 'stub-check-c21.ps1' @'
param([Parameter(Mandatory=$true)][string]$Proof)
if(-not(Test-Path -LiteralPath $Proof)){exit 1}
exit 0
'@
$c24Checker=New-StubChecker 'stub-check-c24.ps1' @'
param([Parameter(Mandatory=$true)][string]$C011Directory,[Parameter(Mandatory=$true)][string]$Output)
@{policy_exact=$true;producer_record_count=1;checker_derived_transport_eligible_count=1}|
    ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
exit 0
'@
$b2Checker=New-StubChecker 'stub-check-b2.ps1' @'
param([Parameter(Mandatory=$true)][string]$AppJson,[Parameter(Mandatory=$true)][string]$EtwJson,
      [Parameter(Mandatory=$true)][string]$Output,[Parameter(Mandatory=$true)][string]$SourceRoot,
      [string]$CandidateLedger)
'{}'|Set-Content -LiteralPath $Output -Encoding utf8
exit 0
'@
$w2aChecker=New-StubChecker 'stub-check-w2a.ps1' @'
param([Parameter(Mandatory=$true)][string]$Json,[string]$Output)
@{status='PASS';domain_evaluated=$true;physical_opportunity_count=3;origin_ordinal=1;last_ordinal=3}|
    ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
exit 0
'@
# 別 run の physical domain を返す stub。cross-cohort splice の negative に使う。
$foreignW2aChecker=New-StubChecker 'stub-check-w2a-foreign-run.ps1' @'
param([Parameter(Mandatory=$true)][string]$Json,[string]$Output)
@{status='PASS';domain_evaluated=$true;physical_opportunity_count=4;origin_ordinal=1;last_ordinal=4}|
    ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
exit 0
'@
$c2CheckerWrapper=New-StubChecker 'wrap-check-c2.ps1' @"
param([Parameter(Mandatory=`$true)][string]`$Proof)
& pwsh -NoProfile -File '$C2Checker' -Proof `$Proof -C1Checker '$C1Checker' -C21Checker '$c21Checker' *> `$null
exit `$LASTEXITCODE
"@

# --- C2.1 exact required intent authority (stub artifact) ---
$c21Path=Join-Path $caseDirectory 'c21-good.json'
function Write-C21([string]$Path,[string[]]$Ordinals){
    [ordered]@{
        authority_exact=$true;c1_checkpoint_sha=$checkpoint
        source_c1_proof_sha256=$c1Hash
        runs=@([ordered]@{
            run=1;authority_exact=$true;required_scheduler_intent_set_exact=$true
            required_scheduler_intent_ordinals=$Ordinals
            required_scheduler_intent_set_cardinality=$Ordinals.Count
            sealed_traced_app_sha256=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Path -Encoding utf8
}
Write-C21 $c21Path @('0')

# --- C2 intent satisfaction ledger (本物の runner) ---
$c2Path=Join-Path $caseDirectory 'c2-good.json'
& $C2Builder -C1Proof $c1Path -C21Proof $c21Path -Output $c2Path -C1Checker $C1Checker -C21Checker $c21Checker *> $null

$commonArguments=@{
    C1Checker=$C1Checker;C21Checker=$c21Checker;C2Checker=$c2CheckerWrapper
    C24Checker=$c24Checker;W2AChecker=$w2aChecker;B2Checker=$b2Checker;SourceRoot=$SourceRoot
}

# --- Good: formal-v2 shadow integration ---
$dPath=Join-Path $caseDirectory 'w2-d-good.json'
& $Builder -C1Proof $c1Path -C21Proof $c21Path -C2Proof $c2Path -Output $dPath @commonArguments *> $null
& $Checker -Proof $dPath @commonArguments *> $null
$d=Get-Content -LiteralPath $dPath -Raw -Encoding utf8|ConvertFrom-Json
foreach($identity in @(
    @('required_intent_count',1),@('satisfied_intent_count',1),@('unsatisfied_intent_count',0),
    @('formal_presented_event_count',1),@('in_domain_presented_event_count',1),
    @('in_domain_presented_foreign_intent_count',0),@('filled_physical_opportunity_count',1),
    @('physical_vblank_opportunity_count',3))){
    if([int64]$d.$($identity[0])-ne[int64]$identity[1]){throw "W2-D integration identityが不正です: $($identity[0])"}
}
if(-not[bool]$d.integration_exact-or[string]$d.verdict-ne'FORMAL_V2_SHADOW_INTEGRATION_EXACT'){
    throw 'W2-D verdictが不正です'
}
# Layer 1A (1) と Layer 1B (3) の差はFAILにしない。
if([int64]$d.required_intent_count-eq[int64]$d.physical_vblank_opportunity_count){
    throw 'Layer 1A / Layer 1B差のfixtureになっていません'
}
$record=@($d.runs[0].records)[0]
foreach($field in @('intent_ordinal','intent_scope','required_intent_membership','composition_token_serial',
    'native_present_serial','etw_sequence','final_state','displayed_qpc','physical_vblank_ordinal',
    'in_measurement_physical_domain','intent_satisfied')){
    if($record.PSObject.Properties.Name-notcontains$field){throw "formal-v2 recordにfieldがありません: $field"}
}

# --- NegativeDifferentC1ForC2 ---
$badC2Path=Join-Path $caseDirectory 'c2-different-c1.json'
$badC2=Get-Content -LiteralPath $c2Path -Raw -Encoding utf8|ConvertFrom-Json
$badC2.source_c1_proof_sha256='0'*64
$badC2|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $badC2Path -Encoding utf8
Assert-Rejected {& $Builder -C1Proof $c1Path -C21Proof $c21Path -C2Proof $badC2Path `
    -Output (Join-Path $caseDirectory 'w2-d-different-c1.json') @commonArguments} `
    '別C1 cohortを参照するC2をW2-Dが受理しました'

# --- NegativeDifferentC21ForC2 ---
$badC21RefPath=Join-Path $caseDirectory 'c2-different-c21.json'
$badC21Ref=Get-Content -LiteralPath $c2Path -Raw -Encoding utf8|ConvertFrom-Json
$badC21Ref.source_c21_proof_sha256='0'*64
$badC21Ref|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $badC21RefPath -Encoding utf8
Assert-Rejected {& $Builder -C1Proof $c1Path -C21Proof $c21Path -C2Proof $badC21RefPath `
    -Output (Join-Path $caseDirectory 'w2-d-different-c21.json') @commonArguments} `
    '別C2.1 authorityを参照するC2をW2-Dが受理しました'

# --- NegativeRequiredIntentSetMutation (C2 構築後に C2.1 を差し替える) ---
$mutatedC21Path=Join-Path $caseDirectory 'c21-mutated.json'
Write-C21 $mutatedC21Path @('5')
Assert-Rejected {& $Builder -C1Proof $c1Path -C21Proof $mutatedC21Path -C2Proof $c2Path `
    -Output (Join-Path $caseDirectory 'w2-d-mutated-c21.json') @commonArguments} `
    'required intent setを差し替えたC2.1をW2-Dが受理しました'

# --- NegativeDifferentPhysicalDomainRun ---
$foreignArguments=$commonArguments.Clone();$foreignArguments.W2AChecker=$foreignW2aChecker
Assert-Rejected {& $Builder -C1Proof $c1Path -C21Proof $c21Path -C2Proof $c2Path `
    -Output (Join-Path $caseDirectory 'w2-d-foreign-domain.json') @foreignArguments} `
    '別runのphysical VBlank domainをW2-Dが受理しました'

# --- NegativeMissingUpstreamAuthority ---
Assert-Rejected {& $Builder -C1Proof $c1Path -C21Proof (Join-Path $caseDirectory 'c21-absent.json') `
    -C2Proof $c2Path -Output (Join-Path $caseDirectory 'w2-d-missing-c21.json') @commonArguments} `
    '欠落したupstream authorityをW2-Dが受理しました'

# --- NegativeSealedSourceMutation (physical domain を後から書き換える) ---
$mutatedAppDirectory=Join-Path $caseDirectory 'mutated-source'
Copy-Item -LiteralPath $c1.source_c011_directory -Destination $mutatedAppDirectory -Recurse
$mutatedC1Path=Join-Path $caseDirectory 'c13-mutated-source.json'
$mutatedC1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$mutatedC1.source_c011_directory=(Resolve-Path -LiteralPath $mutatedAppDirectory).Path
$mutatedC1|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $mutatedC1Path -Encoding utf8
$mutatedAppPath=Join-Path $mutatedAppDirectory 'run-1\traced-app.json'
$mutatedApp=Get-Content -LiteralPath $mutatedAppPath -Raw -Encoding utf8|ConvertFrom-Json
$mutatedApp.presentation_opportunity.physical_vblank_domain_shadow.physical_opportunity_count=4
$mutatedApp|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $mutatedAppPath -Encoding utf8
Assert-Rejected {& $Builder -C1Proof $mutatedC1Path -C21Proof $c21Path -C2Proof $c2Path `
    -Output (Join-Path $caseDirectory 'w2-d-mutated-source.json') @commonArguments} `
    'sealed sourceの書き換えをW2-Dが受理しました'

# --- artifact 側 negative ---
foreach($artifactCase in @(
    @('NegativeCanonicalFlagTrue',{param($Object)$Object.canonical_authority=$true}),
    @('NegativePerformanceVerdictInjected',{param($Object)$Object|Add-Member -NotePropertyName drop_rate -NotePropertyValue 0.5}),
    @('NegativeCanonicalVerdictEvaluated',{param($Object)$Object.canonical_verdict_evaluated=$true}),
    @('NegativeSatisfiedIntentMutation',{param($Object)$Object.runs[0].records[0].intent_satisfied=$false}),
    @('NegativePhysicalOrdinalMutation',{param($Object)$Object.runs[0].records[0].physical_vblank_ordinal=9}),
    @('NegativeAggregateMutation',{param($Object)$Object.satisfied_intent_count=0}))){
    $mutatedPath=Join-Path $caseDirectory "w2-d-$($artifactCase[0]).json"
    $mutated=Get-Content -LiteralPath $dPath -Raw -Encoding utf8|ConvertFrom-Json
    & $artifactCase[1] $mutated
    $mutated|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $mutatedPath -Encoding utf8
    Assert-Rejected {& $Checker -Proof $mutatedPath @commonArguments} `
        "改変W2-D artifactをcheckerが受理しました: $($artifactCase[0])"
}

Write-Output 'W2-D authority mixing: PASS'
