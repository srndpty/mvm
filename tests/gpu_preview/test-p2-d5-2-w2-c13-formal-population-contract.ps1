[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','GoodSuppressedNonFormal','NegativeB2FormalEligibilityMissing','NegativeB2PresentedDropped','NegativeExtraFormalCandidate','NegativeB2CandidateMissingC0Join','NegativeB2CandidateAmbiguousC0Join','NegativeB2FormalUpstreamInvalid','NegativeObservedPopulationDropped','NegativeFormalMappedOrdinalMutation','NegativeObservedMappedOrdinalMutation','NegativeTracedAppHashMutation','NegativePresentHistoryRawHashMutation','NegativeMappingSupportBoundaryMutation')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
. $Core
$formalCandidate=[pscustomobject][ordered]@{
    etw_sequence=2;displayed_qpc=210;native_exact=$true;composition_token_serial='102'
    intent_exact=$true;intent_scope_exact=$true;native_present_serial='12';intent_ordinal='0'
    intent_scope='CURRENT_MEASUREMENT';layer2_cohort_member=$true
}
$nonformalCandidate=[pscustomobject][ordered]@{
    etw_sequence=1;displayed_qpc=90;native_exact=$false;composition_token_serial=$null
    intent_exact=$false;intent_scope_exact=$false;native_present_serial=$null;intent_ordinal=$null
    intent_scope=$null;layer2_cohort_member=$false
}
$terminal=[pscustomobject][ordered]@{final_state='Presented';etw_sequence=2;displayed_qpc=@(210);formal_transport_eligible=$true}
if($Case-eq'GoodSuppressedNonFormal'){
    $suppressedTerminal=[pscustomobject][ordered]@{
        final_state='Presented';etw_sequence=1;displayed_qpc=@(90);formal_transport_eligible=$false
    }
    $population=Invoke-MvmC13FormalPresentedPopulation `
        -ObservedCandidates @($formalCandidate,$nonformalCandidate) `
        -B2TerminalRecords @($terminal,$suppressedTerminal)
    if(-not[bool]$population.authority_valid-or[int]$population.b2_formal_presented_count-ne1-or
       [int]$population.nonformal_observed_presented_count-ne1){
        throw 'suppressed non-formal Presentをformal populationから除外できません'
    }
    Write-Output "W2-C1.3 contract $Case`: PASS";exit 0
}
if($Case-eq'NegativeB2FormalEligibilityMissing'){
    $terminal.PSObject.Properties.Remove('formal_transport_eligible')
    $population=Invoke-MvmC13FormalPresentedPopulation `
        -ObservedCandidates @($formalCandidate,$nonformalCandidate) -B2TerminalRecords @($terminal)
    if([bool]$population.authority_valid-or
       'B2_FORMAL_ELIGIBILITY_MISSING'-notin@($population.blockers)){
        throw 'B2 formal eligibility欠損をrejectしません'
    }
    Write-Output "W2-C1.3 contract $Case`: PASS";exit 0
}
if($Case-eq'NegativeB2CandidateMissingC0Join'){
    $population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates @($nonformalCandidate) -B2TerminalRecords @($terminal)
    if([bool]$population.authority_valid-or[int]$population.b2_formal_missing_c0_candidate_count-ne1-or
       'B2_FORMAL_C0_JOIN_MISSING'-notin@($population.blockers)){throw 'B2 missing C0 joinをrejectしません'}
    Write-Output "W2-C1.3 contract $Case`: PASS";exit 0
}
if($Case-eq'NegativeB2CandidateAmbiguousC0Join'){
    $population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates @($formalCandidate,$formalCandidate) -B2TerminalRecords @($terminal)
    if([bool]$population.authority_valid-or[int]$population.b2_formal_ambiguous_c0_candidate_count-ne1-or
       'B2_FORMAL_C0_JOIN_AMBIGUOUS'-notin@($population.blockers)){throw 'B2 ambiguous C0 joinをrejectしません'}
    Write-Output "W2-C1.3 contract $Case`: PASS";exit 0
}
if($Case-eq'NegativeB2FormalUpstreamInvalid'){
    $formalCandidate.intent_scope_exact=$false
    $population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates @($formalCandidate,$nonformalCandidate) -B2TerminalRecords @($terminal)
    if([bool]$population.authority_valid-or[int]$population.b2_formal_upstream_invalid_count-ne1-or
       'B2_FORMAL_UPSTREAM_INVALID'-notin@($population.blockers)){throw 'B2 formal upstream invalidをrejectしません'}
    if([int]$population.c1_formal_input_count-ne1){throw 'upstream invalidをformal membershipから除外しています'}
    Write-Output "W2-C1.3 contract $Case`: PASS";exit 0
}
$population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates @($formalCandidate,$nonformalCandidate) -B2TerminalRecords @($terminal)
if(-not[bool]$population.authority_valid-or[int]$population.observed_presented_count-ne2-or
   [int]$population.b2_formal_presented_count-ne1-or[int]$population.c1_formal_input_count-ne1-or
   [int]$population.nonformal_observed_presented_count-ne1-or[int]$population.upstream_invalid_nonformal_count-ne1){
    throw '正当なformal population identityが不成立です'
}

$proof=[ordered]@{
    schema='mvm-p2-d5-2-w2-c1-displayed-physical-mapping-2';mapping_exact=$true
    verdict='DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT';formal_population_authority_valid=$true
    formal_population_authority='B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'
    performance_accounting_connected=$false;intent_satisfaction_connected=$false
    observed_presented_count=2;formal_presented_count=1;nonformal_observed_presented_count=1
    upstream_invalid_nonformal_count=1;upstream_exact_nonformal_count=0
    presented_candidate_count=1;mapped_exact_count=1
    observed_physical_mapped_exact_count=2;observed_physical_missing_count=0
    observed_physical_ambiguous_count=0;observed_physical_duplicate_ordinal_count=0
    runs=@([ordered]@{
        run=1
        presented_candidate_count=1;mapped_exact_count=1
        formal_population_authority=[ordered]@{
            formal_membership_authority='B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'
            formal_membership_uses_upstream_exact=$false;formal_membership_uses_display_relation=$false
            formal_membership_uses_measurement_membership=$false;formal_membership_uses_source_frame=$false
            formal_membership_uses_qpc_heuristic=$false
            observed_presented_count=2;b2_formal_presented_count=1;c1_formal_input_count=1
            nonformal_observed_presented_count=1;upstream_invalid_nonformal_count=1;upstream_exact_nonformal_count=0
            b2_formal_missing_c0_candidate_count=0;b2_formal_ambiguous_c0_candidate_count=0
            b2_formal_upstream_invalid_count=0;b2_formal_equals_c1_formal_count=$true
            b2_formal_key_set_equals_c1_formal_key_set=$true;observed_population_identity_exact=$true
            authority_valid=$true
            observed_records=@(
                [ordered]@{exact_event_key='1|90';etw_sequence=1;displayed_qpc=90;in_b2_formal_presented_population=$false;upstream_exact=$false},
                [ordered]@{exact_event_key='2|210';etw_sequence=2;displayed_qpc=210;in_b2_formal_presented_population=$true;upstream_exact=$true})
            formal_records=@([ordered]@{
                exact_event_key='2|210';etw_sequence=2;displayed_qpc=210;native_exact=$true
                composition_token_present=$true;intent_exact=$true;intent_scope_exact=$true;upstream_exact=$true})
        }
        records=@([ordered]@{
            etw_sequence=2;displayed_qpc=210;physical_vblank_ordinal=3;physical_vblank_qpc=300
            mapping_solution_count=1;mapping_exact=$true;in_measurement_physical_domain=$true})
        observed_physical_mapping_diagnostic=[ordered]@{
            presented_candidate_count=2;mapped_exact_count=2;missing_mapping_count=0
            ambiguous_mapping_count=0;duplicate_physical_ordinal_count=0;records=@(
            [ordered]@{etw_sequence=1;displayed_qpc=90;physical_vblank_ordinal=1;physical_vblank_qpc=100;mapping_solution_count=1;mapping_exact=$true;in_measurement_physical_domain=$true},
            [ordered]@{etw_sequence=2;displayed_qpc=210;physical_vblank_ordinal=3;physical_vblank_qpc=300;mapping_solution_count=1;mapping_exact=$true;in_measurement_physical_domain=$true})}
    })
}
$sourceDirectory=Join-Path $Directory 'sealed-source';$sourceRun=Join-Path $sourceDirectory 'run-1'
if(-not(Test-Path -LiteralPath $sourceRun)){New-Item -ItemType Directory -Path $sourceRun|Out-Null}
$terminalPath=Join-Path $sourceRun 'terminal-shadow.json'
@{records=@(@{final_state='Presented';etw_sequence=2;displayed_qpc=@(210);formal_transport_eligible=$true})}|ConvertTo-Json -Depth 6|
    Set-Content -LiteralPath $terminalPath -Encoding utf8
$upstreamPath=Join-Path $Directory 'sealed-c011.json'
@{runs=@(@{candidates=@($nonformalCandidate,$formalCandidate)})}|ConvertTo-Json -Depth 8|
    Set-Content -LiteralPath $upstreamPath -Encoding utf8
$appPath=Join-Path $sourceRun 'traced-app.json'
@{presentation_opportunity=@{
    physical_vblank=@{samples=@(
        @{ordinal=0;qpc=50},@{ordinal=1;qpc=100},@{ordinal=2;qpc=200},@{ordinal=3;qpc=300})}
    physical_vblank_domain_shadow=@{shadow_authority_valid=$true;origin_ordinal=1;last_ordinal=3}
    physical_mapping_support_envelope_shadow=@{authority_valid=$true;predecessor_ordinal=0;successor_ordinal=3}
}}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $appPath -Encoding utf8
$rawPath=Join-Path $sourceRun 'present-history-raw.json'
@{etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0}|ConvertTo-Json|
    Set-Content -LiteralPath $rawPath -Encoding utf8
$proof.source_c011_directory=(Resolve-Path -LiteralPath $sourceDirectory).Path
$proof.source_upstream_inventory_proof=(Resolve-Path -LiteralPath $upstreamPath).Path
$proof.upstream_inventory_proof_sha256=(Get-FileHash -LiteralPath $upstreamPath -Algorithm SHA256).Hash.ToLowerInvariant()
$proof.runs[0].sealed_input_sha256=[ordered]@{
    traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
    present_history_raw=(Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash.ToLowerInvariant()
    b2_terminal_shadow=(Get-FileHash -LiteralPath $terminalPath -Algorithm SHA256).Hash.ToLowerInvariant()
    upstream_inventory_proof=(Get-FileHash -LiteralPath $upstreamPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
if($Case-eq'NegativeB2PresentedDropped'){$proof.runs[0].formal_population_authority.formal_records=@()}
elseif($Case-eq'NegativeExtraFormalCandidate'){
    $proof.runs[0].formal_population_authority.formal_records+=,[ordered]@{
        exact_event_key='1|90';etw_sequence=1;displayed_qpc=90;native_exact=$true
        composition_token_present=$true;intent_exact=$true;intent_scope_exact=$true;upstream_exact=$true}
}elseif($Case-eq'NegativeObservedPopulationDropped'){
    $proof.runs[0].formal_population_authority.observed_records=@($proof.runs[0].formal_population_authority.observed_records|Select-Object -Last 1)
    $proof.runs[0].formal_population_authority.observed_presented_count=1
    $proof.runs[0].formal_population_authority.nonformal_observed_presented_count=0
    $proof.runs[0].formal_population_authority.upstream_invalid_nonformal_count=0
    $proof.runs[0].observed_physical_mapping_diagnostic.records=@($proof.runs[0].observed_physical_mapping_diagnostic.records|Select-Object -Last 1)
    $proof.observed_presented_count=1;$proof.nonformal_observed_presented_count=0
    $proof.upstream_invalid_nonformal_count=0;$proof.observed_physical_mapped_exact_count=1
}elseif($Case-eq'NegativeFormalMappedOrdinalMutation'){
    $proof.runs[0].records[0].physical_vblank_ordinal=4
}elseif($Case-eq'NegativeObservedMappedOrdinalMutation'){
    $proof.runs[0].observed_physical_mapping_diagnostic.records[0].physical_vblank_ordinal=2
}elseif($Case-eq'NegativeTracedAppHashMutation'){
    $proof.runs[0].sealed_input_sha256.traced_app='0'*64
}elseif($Case-eq'NegativePresentHistoryRawHashMutation'){
    $proof.runs[0].sealed_input_sha256.present_history_raw='0'*64
}elseif($Case-eq'NegativeMappingSupportBoundaryMutation'){
    $appObject=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $appObject.presentation_opportunity.physical_mapping_support_envelope_shadow.predecessor_ordinal=1
    $appObject|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $appPath -Encoding utf8
    $proof.runs[0].sealed_input_sha256.traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$proofPath=Join-Path $Directory "c13-$($Case.ToLowerInvariant()).json"
$proof|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $proofPath -Encoding utf8
$failed=$false
try{& $Checker -Proof $proofPath}catch{$failed=$true}
$negative=$Case-ne'Good'
if($negative-and-not$failed){throw "改変proofをcheckerが受理しました: $Case"}
if(-not$negative-and$failed){throw '正当なC1.3 proofをcheckerが拒否しました'}
$stage=$(if($Case-in@('NegativeFormalMappedOrdinalMutation','NegativeObservedMappedOrdinalMutation',
    'NegativeTracedAppHashMutation','NegativePresentHistoryRawHashMutation','NegativeMappingSupportBoundaryMutation')){'W2-C1.4'}else{'W2-C1.3'})
Write-Output "$stage contract $Case`: PASS"
