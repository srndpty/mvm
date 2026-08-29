[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','NegativeRelationMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
# S2-f2: invocationごとに一意なdirectoryへ書き、warm build treeでのstale artifact
# 継承を構造的に不可能にする。expectation semanticsは変更しない。
$Directory=Join-Path $Directory ("inv-$PID-"+[guid]::NewGuid().ToString('N').Substring(0,12))
New-Item -ItemType Directory -Path $Directory -Force|Out-Null
$runDirectory=Join-Path $Directory 'run-1';if(-not(Test-Path -LiteralPath $runDirectory)){New-Item -ItemType Directory -Path $runDirectory|Out-Null}
function Candidate([int]$Sequence,[int64]$Qpc,[string]$Relation){
    [ordered]@{etw_sequence=$Sequence;native_present_serial="$Sequence";composition_token_serial="$Sequence"
        intent_ordinal="$Sequence";intent_scope='CURRENT_MEASUREMENT';layer2_cohort_member=$true
        native_exact=$true;intent_exact=$true;intent_scope_exact=$true
        displayed_qpc=$Qpc;display_relation=$Relation}
}
$candidates=@(
    (Candidate -Sequence 1 -Qpc 50 -Relation 'BEFORE_PREDECESSOR'),
    (Candidate -Sequence 2 -Qpc 150 -Relation 'WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'),
    (Candidate -Sequence 3 -Qpc 450 -Relation 'AFTER_SUCCESSOR'))
if($Case-eq'NegativeRelationMutation'){$candidates[0].display_relation='WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'}
$proof=[ordered]@{run_count=1;runs=@([ordered]@{candidates=$candidates})}
$app=[ordered]@{presentation_opportunity=[ordered]@{
    physical_vblank=[ordered]@{samples=@(
        [ordered]@{ordinal=0;qpc=100},[ordered]@{ordinal=1;qpc=200},
        [ordered]@{ordinal=2;qpc=300},[ordered]@{ordinal=3;qpc=400})}
    physical_vblank_domain_shadow=[ordered]@{predecessor_ordinal=0;successor_ordinal=3;origin_ordinal=1;last_ordinal=2}
}}
$proofPath=Join-Path $Directory 'display-candidate-inventory.json'
$proof|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $proofPath -Encoding utf8
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $runDirectory 'traced-app.json') -Encoding utf8
$output=Join-Path $Directory 'support-gap.json'
& pwsh -NoProfile -File $Inventory -C011Directory $Directory -InventoryProof $proofPath -Output $output *> $null
if($Case-eq'NegativeRelationMutation'){
    if($LASTEXITCODE-eq0){throw 'display relation mutationが受理されました'}
}else{
    if($LASTEXITCODE-ne0){throw 'support gap inventoryが失敗しました'}
    $actual=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
    # C1 の support-domain contract 修正後、missing_mapping_count は support 内だけを数える。
    # support 外 (qpc 50 / 450) は head/tail として分類され missing にはならない。
    if(-not[bool]$actual.support_gap_diagnosis_exact-or$actual.missing_mapping_count-ne0-or
       $actual.before_physical_support_count-ne1-or$actual.after_physical_support_count-ne1-or
       $actual.inside_support_unmapped_count-ne0-or$actual.runs[0].within_support_mapped_exact_count-ne1-or
       $actual.runs[0].outside_mapping_support_head_count-ne1-or
       $actual.runs[0].outside_mapping_support_tail_count-ne1-or
       -not[bool]$actual.outside_support_classification_agrees-or
       -not[bool]$actual.inside_support_missing_is_zero-or
       $actual.verdict-ne'PHYSICAL_MAPPING_SUPPORT_ENVELOPE_INCOMPLETE'){
        throw 'support gap exact diagnosisが不正です'
    }
}
Write-Output "W2-C1.1 $Case support gap contract: PASS"
