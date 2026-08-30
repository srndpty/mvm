[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good',
        'NegativePresentedBeforePredecessorNotDropped',
        'NegativePresentedAfterSuccessorNotDropped',
        'NegativeStaleArtifactNotReused')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# S2-f2: 「非0 exitだから違反を検出した」をPASS条件にしない。
#
# 2026-08-25から2026-08-29まで、本testの2 negativeはvacuous PASSだった。
# runnerがparameter binding errorで異常終了して非0 exitになり、さらに固定
# output pathに残ったstale mapping.jsonがartifact existence checkを突破していた。
# 帰属はdocs/p5-e4-s2-f-w2-c1-runner-negative-false-green.mdにある。
#
# 再発を構造的に防ぐため次を守る。
#   1. runnerの実行失敗(usage / binding / exception)はRUNNER_EXECUTION_FAILUREとして
#      test FAILにし、semantic rejectionと同一視しない。
#   2. outputはinvocationごとに一意なdirectoryへ書く。stale artifactを継承する
#      経路自体を存在させない。mtime比較には依存しない。

function Fail([string]$Message){throw $Message}

# invocationごとに一意。warm build treeでも前回のartifactを継承できない。
$invocationId="inv-$PID-$([guid]::NewGuid().ToString('N').Substring(0,12))"
$invocationRoot=Join-Path $Directory $invocationId
New-Item -ItemType Directory -Path $invocationRoot -Force|Out-Null
$runDirectory=Join-Path $invocationRoot 'run-1'
New-Item -ItemType Directory -Path $runDirectory -Force|Out-Null

# Goodはsupport区間[100,400]の内側かつexact解を1つだけ持つQPC。
# negativeはsupportの外側(head / tail)へ置く。
$displayed=switch($Case){
    'Good'{250}
    'NegativePresentedAfterSuccessorNotDropped'{450}
    default{50}
}
$relation=switch($Case){
    'Good'{'INSIDE_SUPPORT'}
    'NegativePresentedAfterSuccessorNotDropped'{'AFTER_SUCCESSOR'}
    default{'BEFORE_PREDECESSOR'}
}
$candidate=[ordered]@{
    etw_sequence=7;native_present_serial='17';composition_token_serial='27'
    intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT';layer2_cohort_member=$false
    native_exact=$true;intent_exact=$true;intent_scope_exact=$true
    displayed_qpc=$displayed;display_relation=$relation
}
$inventory=[ordered]@{
    coverage_complete=$true;intent_scope_exact=$true;missing_scope_count=0
    ambiguous_scope_count=0;mutated_scope_count=0;run_count=1
    runs=@([ordered]@{coverage_complete=$true;intent_scope_exact=$true;candidates=@($candidate)})
}
$app=[ordered]@{
    presentation_opportunity=[ordered]@{
        physical_vblank=[ordered]@{samples=@(
            [ordered]@{ordinal=0;qpc=100},[ordered]@{ordinal=1;qpc=200},
            [ordered]@{ordinal=2;qpc=300},[ordered]@{ordinal=3;qpc=400})}
        physical_vblank_domain_shadow=[ordered]@{
            predecessor_ordinal=0;successor_ordinal=3;origin_ordinal=1;last_ordinal=2
        }
        physical_mapping_support_envelope_shadow=[ordered]@{
            schema='mvm-p2-d5-2-w2-c11-physical-mapping-support-envelope-1';shadow_only=$true
            performance_semantics_connected=$false;intent_satisfaction_connected=$false
            capture_begin_qpc=150;capture_close_qpc=350;producer_teardown_completed=$true;postroll_boundary_qpc=375
            predecessor_valid=$true;predecessor_ordinal=0;predecessor_qpc=100
            successor_valid=$true;successor_ordinal=3;successor_qpc=400
            postroll_wait_completed=$true;postroll_wait_timeout=$false;postroll_wait_elapsed_qpc=1
            lower_closed_before_candidate_capture=$true;upper_closed_after_candidate_capture_and_teardown=$true
            ring_overflow_count=0;wait_failure_count=0;output_stable=$true;authority_valid=$true
        }
    }
    native_present_hook=[ordered]@{intent_scope_provenance=[ordered]@{authority_pass=$true}}
}
$etw=[ordered]@{etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0}
$inventory|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $invocationRoot 'stub-inventory.json') -Encoding utf8
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $runDirectory 'traced-app.json') -Encoding utf8
$etw|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'present-history-raw.json') -Encoding utf8
# B2 terminal recordはcurrent schemaを満たす。formal_transport_eligibleが欠けると
# C13 formal populationが空になり、runnerはmapping phaseへ到達する前にparameter
# binding errorで落ちる。fieldの有無ではなく「runnerがintended phaseまで到達する
# こと」が保証対象である。
$terminal=[ordered]@{
    schema='mvm-p2-d5-2-w2-b2-terminal-shadow-1';verdict='NATIVE_PRESENT_TERMINAL_OUTCOME_EXACT'
    records=@([ordered]@{
        final_state='Presented';etw_sequence=7;displayed_qpc=@($displayed)
        formal_transport_eligible=$true
    })
}
$terminal|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'terminal-shadow.json') -Encoding utf8
$inventoryStub=Join-Path $invocationRoot 'inventory-stub.ps1'
@'
param([string]$B2LiveDirectory,[string]$Output,[switch]$RequireCoverageComplete)
Copy-Item -LiteralPath (Join-Path $B2LiveDirectory 'stub-inventory.json') -Destination $Output -Force
'@|Set-Content -LiteralPath $inventoryStub -Encoding utf8
$physicalStub=Join-Path $invocationRoot 'physical-stub.ps1'
@'
param([string]$Json,[string]$Output)
[ordered]@{shadow_authority_valid=$true}|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
'@|Set-Content -LiteralPath $physicalStub -Encoding utf8
$envelopeStub=Join-Path $invocationRoot 'envelope-stub.ps1'
@'
param([string]$InputJson,[string]$Output)
[ordered]@{pass=$true}|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
'@|Set-Content -LiteralPath $envelopeStub -Encoding utf8

# 事故の再現に最も近いnegative。旧harnessが使っていた固定output pathへstale
# artifactを置き、新harnessがそれを一切参照しないことを検査する。
$legacyOutput=Join-Path $Directory 'mapping.json'
if($Case-eq'NegativeStaleArtifactNotReused'){
    $stale=[ordered]@{
        schema='mvm-p2-d5-2-w2-c1-displayed-physical-mapping-1';stale_sentinel=$true
        mapping_exact=$true;missing_mapping_count=0;blockers=@()
        presented_candidate_count=99
        runs=@([ordered]@{presented_candidate_count=99;candidate_count_identity=$true})
    }
    $stale|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $legacyOutput -Encoding utf8
}

$output=Join-Path $invocationRoot 'mapping.json'
if(Test-Path -LiteralPath $output){Fail "$Case invocation directoryに既存artifactがあります: $output"}

$runnerLog=Join-Path $invocationRoot 'runner-output.txt'
& pwsh -NoProfile -File $Runner -C011Directory $invocationRoot -Output $output `
    -Inventory $inventoryStub -PhysicalChecker $physicalStub -EnvelopeChecker $envelopeStub `
    *> $runnerLog
$runnerExit=$LASTEXITCODE
$runnerDetail=[string](Get-Content -LiteralPath $runnerLog -Raw -Encoding utf8 -ErrorAction SilentlyContinue)

# --- 層1: runner execution ----------------------------------------------
# runnerがintended mapping phaseへ到達しなかった場合は、非0 exitであっても
# negativeの成功にしない。usage / binding / exceptionはtest FAILである。
if(-not(Test-Path -LiteralPath $output)){
    Fail "$Case RUNNER_EXECUTION_FAILURE: mapping artifactが生成されませんでした (exit=$runnerExit)`n$runnerDetail"
}
$result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($result.PSObject.Properties.Name-contains'stale_sentinel'){
    Fail "$Case stale artifactを読みました。invocation隔離が機能していません"
}
foreach($field in @('runs','blockers','missing_mapping_count','presented_candidate_count')){
    if($result.PSObject.Properties.Name-notcontains$field){
        Fail "$Case RUNNER_EXECUTION_FAILURE: mapping artifactにfieldがありません: $field (exit=$runnerExit)`n$runnerDetail"
    }
}
if(@($result.runs).Count-ne1){Fail "$Case mapping artifactのrun数が不正です"}
$run=@($result.runs)[0]
if(@($run.records).Count-ne1){Fail "$Case mapping recordが1件ではありません"}
$record=@($run.records)[0]

if($Case-eq'NegativeStaleArtifactNotReused'-and-not(Test-Path -LiteralPath $legacyOutput)){
    Fail "$Case stale artifactの前提が壊れています"
}

# --- 層2: semantic expectation ------------------------------------------
$blockers=@($result.blockers|ForEach-Object{[string]$_})
if($Case-eq'Good'){
    if($runnerExit-ne0){Fail "Good baselineがPASSしません (exit=$runnerExit)`n$runnerDetail"}
    if($blockers.Count-ne0){Fail "Good baselineにblockerがあります: $($blockers-join',')"}
    if(-not[bool]$result.mapping_exact){Fail 'Good baselineのmapping_exactがtrueではありません'}
    if([int64]$result.missing_mapping_count-ne0){Fail 'Good baselineのmissing_mapping_countが0ではありません'}
    if([string]$record.mapping_support_relation-ne'INSIDE_SUPPORT'){
        Fail "Good baselineのrelationが不正です: $($record.mapping_support_relation)"
    }
    if(-not[bool]$record.physical_vblank_mapping_required){
        Fail 'Good baselineでmapping_requiredがtrueではありません'
    }
    if(-not[bool]$record.mapping_exact){Fail 'Good baselineのrecord mapping_exactがtrueではありません'}
}else{
    if($runnerExit-eq0){Fail "$Case がsubset PASSしました"}
    # 2fda618でintended blockerはPHYSICAL_MAPPING_MISSINGから
    # FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORTへsupersededされた。旧expectationは
    # mappingRequired=falseにより構造的に到達不能である。
    $expectedBlocker='FORMAL_PRESENTED_OUTSIDE_MAPPING_SUPPORT'
    if($blockers-notcontains$expectedBlocker){
        Fail "$Case intended violationが一致しません: expected=$expectedBlocker actual=$($blockers-join',')"
    }
    $expectedRelation=if($Case-eq'NegativePresentedAfterSuccessorNotDropped'){'AFTER_SUCCESSOR'}else{'BEFORE_PREDECESSOR'}
    if([string]$record.mapping_support_relation-ne$expectedRelation){
        Fail "$Case relationが一致しません: expected=$expectedRelation actual=$($record.mapping_support_relation)"
    }
    if([bool]$record.physical_vblank_mapping_required){
        Fail "$Case support外でmapping_requiredがtrueです"
    }
    if([int64]$result.missing_mapping_count-ne0){
        Fail "$Case missing_mapping_countが0ではありません: $($result.missing_mapping_count)"
    }
    # candidate retention。blockerだけ一致してcandidateが落ちるregressionを防ぐ。
    if([int]$result.presented_candidate_count-ne1-or[int]$run.presented_candidate_count-ne1){
        Fail "$Case candidateが保持されていません: $($run.presented_candidate_count)"
    }
    if(-not[bool]$run.candidate_count_identity){
        Fail "$Case candidate_count_identityが成立しません"
    }
}

$hash=$run.sealed_input_sha256
foreach($field in @('traced_app','present_history_raw','b2_terminal_shadow','upstream_inventory_proof')){
    if([string]::IsNullOrWhiteSpace([string]$hash.$field)-or[string]$hash.$field-notmatch'^[0-9a-f]{64}$'){
        Fail "$Case sealed input hashが不正です: $field"
    }
}
Write-Output "W2-C1 $Case runner closure: PASS"
