[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('GoodOutsideCapture','GoodWithinCaptureRegression','NegativeMappingCandidateDropped','NegativeProofRecordDropped','NegativeCaptureRelationMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
$runDirectory=Join-Path $Directory 'run-1'
New-Item -ItemType Directory -Path $runDirectory -Force|Out-Null
$appPath=Join-Path $runDirectory 'traced-app.json'
$ledgerPath=Join-Path $runDirectory 'terminal-shadow.json'
$c011Path=Join-Path $Directory 'c011.json'
$c1Path=Join-Path $Directory 'c1.json'
$proofPath=Join-Path $Directory 'proof.json'

@{
    presentation_opportunity=@{
        measurement_start_qpc=150
        measurement_end_qpc_exclusive=250
        physical_mapping_support_envelope_shadow=@{predecessor_qpc=50}
    }
    native_present_hook=@{capture_envelope=@{begin_qpc=100;close_qpc=300}}
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $appPath -Encoding utf8

$exactCandidate=[ordered]@{
    etw_sequence=2;present_start_qpc=200;displayed_qpc=210
    display_relation='WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE';layer2_cohort_member=$true
    native_candidate_count=1;native_exact=$true;composition_token_serial='102'
    intent_exact=$true;intent_ordinal='0';intent_scope_exact=$true;intent_scope='CURRENT_MEASUREMENT'
}
$beforeCandidate=[ordered]@{
    etw_sequence=1;present_start_qpc=80;displayed_qpc=90
    display_relation='BEFORE_PREDECESSOR';layer2_cohort_member=$false
    native_candidate_count=0;native_exact=$false;composition_token_serial=''
    intent_exact=$false;intent_ordinal='';intent_scope_exact=$false;intent_scope=''
}
$afterCandidate=[ordered]@{
    etw_sequence=3;present_start_qpc=320;displayed_qpc=330
    display_relation='AFTER_SUCCESSOR';layer2_cohort_member=$false
    native_candidate_count=0;native_exact=$false;composition_token_serial=''
    intent_exact=$false;intent_ordinal='';intent_scope_exact=$false;intent_scope=''
}
if($Case-eq'GoodWithinCaptureRegression'){
    $beforeCandidate.present_start_qpc=120
    $beforeCandidate.displayed_qpc=130
}
$candidates=@($beforeCandidate,$exactCandidate,$afterCandidate)
@{records=@(@{final_state='Presented';etw_sequence=2;displayed_qpc=@(210)})}|ConvertTo-Json -Depth 8|
    Set-Content -LiteralPath $ledgerPath -Encoding utf8
@{run_count=1;runs=@(@{candidates=$candidates})}|ConvertTo-Json -Depth 10|
    Set-Content -LiteralPath $c011Path -Encoding utf8
$mappingRecords=@(
    @{etw_sequence=1;displayed_qpc=([int64]$beforeCandidate.displayed_qpc);physical_vblank_ordinal=9;mapping_exact=$true},
    @{etw_sequence=2;displayed_qpc=210;physical_vblank_ordinal=10;mapping_exact=$true},
    @{etw_sequence=3;displayed_qpc=330;physical_vblank_ordinal=11;mapping_exact=$true}
)
if($Case-eq'NegativeMappingCandidateDropped'){$mappingRecords=@($mappingRecords|Select-Object -First 2)}
@{runs=@(@{records=$mappingRecords})}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $c1Path -Encoding utf8

$inventoryFailed=$false
try{
    & $Inventory -C011Directory $Directory -C011InventoryProof $c011Path -C1MappingProof $c1Path -Output $proofPath
}catch{$inventoryFailed=$true}
if($Case-eq'NegativeMappingCandidateDropped'){
    if(-not$inventoryFailed){throw 'C1からcandidateが欠落してもinventoryが成功しました'}
    Write-Output "W2-C1.2 contract $Case`: PASS"
    exit 0
}
if($inventoryFailed){throw "W2-C1.2 inventoryが失敗しました: $Case"}

$proofObject=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
if($Case-eq'GoodOutsideCapture'){
    if([string]$proofObject.attribution-ne'UPSTREAM_INVALID_OUTSIDE_NATIVE_CAPTURE_ENVELOPE'-or
       [int]$proofObject.all_presented_count-ne3-or[int]$proofObject.upstream_exact_count-ne1-or
       [int]$proofObject.upstream_invalid_count-ne2-or[int]$proofObject.invalid_before_capture_begin_count-ne1-or
       [int]$proofObject.invalid_within_capture_envelope_count-ne0-or[int]$proofObject.invalid_after_capture_close_count-ne1){
        throw 'capture外 attribution identityが不正です'
    }
}elseif($Case-eq'GoodWithinCaptureRegression'){
    if([string]$proofObject.attribution-ne'UPSTREAM_PROVENANCE_REGRESSION_WITHIN_CAPTURE_ENVELOPE'-or
       [int]$proofObject.invalid_within_capture_envelope_count-ne1){throw 'capture内 regressionを分類できません'}
}elseif($Case-eq'NegativeProofRecordDropped'){
    $proofObject.runs[0].records=@($proofObject.runs[0].records|Select-Object -Skip 1)
    $proofObject|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $proofPath -Encoding utf8
}elseif($Case-eq'NegativeCaptureRelationMutation'){
    $proofObject.runs[0].records[0].native_capture_envelope_relation='WITHIN_CAPTURE_ENVELOPE'
    $proofObject|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $proofPath -Encoding utf8
}

$checkerFailed=$false
try{& $Checker -Proof $proofPath}catch{$checkerFailed=$true}
$negative=$Case-in@('NegativeProofRecordDropped','NegativeCaptureRelationMutation')
if($negative-and-not$checkerFailed){throw "改変proofをcheckerが受理しました: $Case"}
if(-not$negative-and$checkerFailed){throw "正当なproofをcheckerが拒否しました: $Case"}
Write-Output "W2-C1.2 contract $Case`: PASS"
