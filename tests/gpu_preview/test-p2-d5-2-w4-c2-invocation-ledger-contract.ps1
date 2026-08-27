[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodExact','NegativeInvocationSequenceGap','NegativePrePostStateMutation',
        'NegativeDecisionReasonMutation','NegativeDecisionWithoutTransport',
        'NegativeSourceRequiredDomainConflation','NegativePostTerminalInvocation',
        'NegativeCompletedOrdinalMutation','NegativePerformanceAuthorityPromotion',
        'NegativePhysicalSuccessorRequired')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
function State([bool]$Anchored,[uint64]$Origin,[int64]$Last,[bool]$Pending,[bool]$Past){
    return [ordered]@{started=$true;closed=$false;anchored=$Anchored
        origin_refresh_count=[string]$Origin;last_finalized_opportunity_ordinal=$Last
        pending_render=$Pending;past_source_domain=$Past}
}
function Record([int]$Serial,[int64]$Qpc,[string]$Result,[string]$Reason,[string]$Ordinal,
                [uint64]$Refresh,[bool]$Membership,[bool]$Past,[string]$Disposition,
                [bool]$PrePending,[bool]$PostPending){
    return [ordered]@{scheduler_invocation_serial=[string]$Serial;invocation_qpc=$Qpc
        input_authority=[ordered]@{available=$true;refresh_count=[string]$Refresh;qpc_vblank=$Qpc
            refresh_numerator=60;refresh_denominator=1}
        pre=(State ($Serial-ne1) 100 ($Serial-2) $PrePending $false)
        result=$Result;reason=$Reason;decision_valid=$true
        duplicate_callback=($Result-eq'DUPLICATE_DECISION');intent_ordinal=$Ordinal
        target_frame=$Ordinal;repeat=$false;past_source_domain=$Past
        required_intent_membership=$Membership;required_intent_membership_exact=$true
        last_finalized_opportunity_ordinal=($Serial-2)
        formal_transport_disposition=$Disposition;formal_transport_disposition_exact=$true
        post=(State ($Serial-ne1) 100 ($Serial-2) $PostPending $Past)
        state_transition_exact=$true}
}
$records=@(
    (Record 1 100 'PRIMARY_DECISION' 'PRIMARY' '0' 99 $true $false 'TRANSPORT' $false $true),
    (Record 2 110 'DUPLICATE_DECISION' 'PENDING_RENDER' '0' 99 $true $false `
        'SUPPRESS_DUPLICATE_CALLBACK' $true $true),
    (Record 3 200 'PRIMARY_DECISION' 'PRIMARY' '2' 101 $true $false 'TRANSPORT' $false $true),
    (Record 4 300 'OUTSIDE_SOURCE_DOMAIN_DECISION' 'PAST_SOURCE_DOMAIN' '3' 102 $true `
        $true 'TRANSPORT' $false $false))
$app=[ordered]@{formal_scheduler_invocation_ledger=[ordered]@{
    schema='mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1'
    diagnostic_root_cause_capture=$true;canonical_performance_authority=$false
    physical_vblank_successor_required=$false;physical_mapping_support_authority=$false
    measurement_stop_qpc=301;native_envelope_close_qpc=302
    record_count=$records.Count;records=$records}}
switch($Case){
    'NegativeInvocationSequenceGap'{$records[2].scheduler_invocation_serial='4'}
    'NegativePrePostStateMutation'{$records[2].post.last_finalized_opportunity_ordinal=99}
    'NegativeDecisionReasonMutation'{$records[2].reason='PENDING_RENDER'}
    'NegativeDecisionWithoutTransport'{$records[2].formal_transport_disposition_exact=$false}
    'NegativeSourceRequiredDomainConflation'{$records[3].required_intent_membership=$false}
    'NegativePostTerminalInvocation'{$records+=,(Record 5 400 'PRIMARY_DECISION' 'PRIMARY' '4' 103 `
        $true $false 'TRANSPORT' $false $true);$app.formal_scheduler_invocation_ledger.records=$records
        $app.formal_scheduler_invocation_ledger.record_count=$records.Count}
    'NegativeCompletedOrdinalMutation'{$records[2].input_authority.refresh_count='100'}
    'NegativePerformanceAuthorityPromotion'{$app.formal_scheduler_invocation_ledger.canonical_performance_authority=$true}
    'NegativePhysicalSuccessorRequired'{$app.formal_scheduler_invocation_ledger.physical_vblank_successor_required=$true}
}
$json=Join-Path $Directory 'capture.json'
$app|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $json -Encoding utf8
$failed=$false
try{& pwsh -NoProfile -File $Checker -Json $json *> $null;if($LASTEXITCODE-ne0){$failed=$true}}
catch{$failed=$true}
if($Case-eq'GoodExact'){
    if($failed){throw 'GoodExactをcheckerが拒否しました'}
}elseif(-not$failed){throw "$Case がcheckerに受理されました"}
Write-Output "W4-C2 invocation ledger contract $Case : PASS"
