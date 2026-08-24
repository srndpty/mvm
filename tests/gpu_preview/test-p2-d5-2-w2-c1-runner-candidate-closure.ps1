[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'NegativePresentedBeforePredecessorNotDropped',
        'NegativePresentedAfterSuccessorNotDropped')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Runner,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
$runDirectory=Join-Path $Directory 'run-1';if(-not(Test-Path -LiteralPath $runDirectory)){New-Item -ItemType Directory -Path $runDirectory|Out-Null}
$displayed=if($Case-like'*Before*'){50}else{450}
$relation=if($Case-like'*Before*'){'BEFORE_PREDECESSOR'}else{'AFTER_SUCCESSOR'}
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
$inventory|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $Directory 'stub-inventory.json') -Encoding utf8
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $runDirectory 'traced-app.json') -Encoding utf8
$etw|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'present-history-raw.json') -Encoding utf8
$inventoryStub=Join-Path $Directory 'inventory-stub.ps1'
@'
param([string]$B2LiveDirectory,[string]$Output,[switch]$RequireCoverageComplete)
Copy-Item -LiteralPath (Join-Path $B2LiveDirectory 'stub-inventory.json') -Destination $Output -Force
'@|Set-Content -LiteralPath $inventoryStub -Encoding utf8
$physicalStub=Join-Path $Directory 'physical-stub.ps1'
@'
param([string]$Json,[string]$Output)
[ordered]@{shadow_authority_valid=$true}|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
'@|Set-Content -LiteralPath $physicalStub -Encoding utf8
$envelopeStub=Join-Path $Directory 'envelope-stub.ps1'
@'
param([string]$InputJson,[string]$Output)
[ordered]@{pass=$true}|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
'@|Set-Content -LiteralPath $envelopeStub -Encoding utf8
$output=Join-Path $Directory 'mapping.json'
& pwsh -NoProfile -File $Runner -C011Directory $Directory -Output $output `
    -Inventory $inventoryStub -PhysicalChecker $physicalStub -EnvelopeChecker $envelopeStub *> $null
if($LASTEXITCODE-eq0){throw "$Case がsubset PASSしました"}
if(-not(Test-Path -LiteralPath $output)){throw "$Case mapping artifactがありません"}
$result=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($result.presented_candidate_count-ne1-or$result.runs[0].presented_candidate_count-ne1-or
   -not[bool]$result.runs[0].candidate_count_identity-or$result.missing_mapping_count-ne1-or
   'PHYSICAL_MAPPING_MISSING'-notin@($result.blockers)){
    throw "$Case candidateが保持されたままmissing mappingとしてrejectされません"
}
$hash=$result.runs[0].sealed_input_sha256
foreach($field in @('traced_app','present_history_raw','upstream_inventory_proof')){
    if([string]::IsNullOrWhiteSpace([string]$hash.$field)-or[string]$hash.$field-notmatch'^[0-9a-f]{64}$'){
        throw "$Case sealed input hashが不正です: $field"
    }
}
Write-Output "W2-C1 $Case runner closure: PASS"
