[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('GoodExactSet','GoodDistinctDecisionsSameOrdinal',
        'GoodRequiredIntentWithoutDecision',
        'NegativeMissingRequiredSet','NegativeDuplicateRequiredSet','NegativeCountSetMismatch',
        'NegativeRequiredSetMissingMembership','NegativeMembershipExtraOutsideRequiredSet',
        'NegativeMissingDecisionQpc','NegativeBoundaryRelationMutation',
        'NegativeMissingMeasurementStartQpc','NegativeMissingMeasurementEndQpc',
        'NegativeAmbiguousFormalReverseJoin')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
function Decision([string]$Token,[string]$Ordinal){
    [pscustomobject][ordered]@{token_serial=$Token;intent_ordinal=$Ordinal;intent_scope='CURRENT_MEASUREMENT'
        decision_qpc=[int64](1000+[int]$Token);decision_qpc_exact=$true
        required_current_membership=$true;required_current_membership_exact=$true
        measurement_boundary_relation='WITHIN_CURRENT_MEASUREMENT'}
}
function Native([string]$Token,[string]$Ordinal){
    [pscustomobject][ordered]@{composition_token=[pscustomobject]@{token_serial=$Token}
        intent_ordinal=$Ordinal;intent_ordinal_valid=$true;present_serial=$Token;present_enter_qpc=[int64](2000+[int]$Token)}
}
function Formal([string]$Token,[string]$Ordinal,[long]$Sequence){
    [pscustomobject][ordered]@{composition_token_serial=$Token;intent_ordinal=$Ordinal
        etw_sequence=$Sequence;displayed_qpc=[int64](3000+$Sequence)}
}
$scope=[pscustomobject][ordered]@{authority_pass=$true;required_intent_set_exact=$true
    required_intent_ordinals=@('10','11');records=@((Decision '1' '10'),(Decision '2' '11'))}
$native=@((Native '1' '10'),(Native '2' '11'))
$formal=@((Formal '1' '10' 1),(Formal '2' '11' 2))
$required=2
$envelope=[pscustomobject]@{measurement_arm_qpc=900;measurement_start_qpc=1000;frozen_measurement_end_qpc=2000}
$baselineInputs=[pscustomobject]@{scope=$scope;native=$native;formal=$formal
    required=$required;envelope=$envelope}|ConvertTo-Json -Depth 10 -Compress
switch($Case){
    'GoodDistinctDecisionsSameOrdinal'{
        $scope.records=@((Decision '1' '10'),(Decision '2' '10'),(Decision '3' '11'))
        $native=@((Native '1' '10'),(Native '2' '10'),(Native '3' '11'))
        $formal=@((Formal '1' '10' 1),(Formal '2' '10' 2),(Formal '3' '11' 3))
    }
    'GoodRequiredIntentWithoutDecision'{
        $scope.required_intent_ordinals=@('10','11','12')
        $scope.records=@((Decision '1' '10'),(Decision '3' '12'))
        $native=@((Native '1' '10'),(Native '3' '12'))
        $formal=@((Formal '1' '10' 1),(Formal '3' '12' 3))
        $required=3
    }
    'NegativeMissingRequiredSet'{$scope.PSObject.Properties.Remove('required_intent_ordinals');$scope.required_intent_set_exact=$false}
    'NegativeDuplicateRequiredSet'{$scope.required_intent_ordinals=@('10','10')}
    'NegativeCountSetMismatch'{$required=3}
    'NegativeRequiredSetMissingMembership'{$scope.records[1].required_current_membership=$false}
    'NegativeMembershipExtraOutsideRequiredSet'{
        $scope.records+=,(Decision '3' '12')
        $native+=,(Native '3' '12')
        $formal+=,(Formal '3' '12' 3)
    }
    'NegativeMissingDecisionQpc'{$scope.records[0].decision_qpc=$null;$scope.records[0].decision_qpc_exact=$false}
    'NegativeBoundaryRelationMutation'{$scope.records[0].measurement_boundary_relation='POST_MEASUREMENT'}
    'NegativeMissingMeasurementStartQpc'{$envelope.PSObject.Properties.Remove('measurement_start_qpc')}
    'NegativeMissingMeasurementEndQpc'{$envelope.PSObject.Properties.Remove('frozen_measurement_end_qpc')}
    'NegativeAmbiguousFormalReverseJoin'{$formal+=,(Formal '1' '10' 3)}
}
$good=$Case.StartsWith('Good')
if(-not$good){
    $mutatedInputs=[pscustomobject]@{scope=$scope;native=$native;formal=$formal
        required=$required;envelope=$envelope}|ConvertTo-Json -Depth 10 -Compress
    if($mutatedInputs-ceq$baselineInputs){throw "negative input mutationが適用されませんでした: $Case"}
}
$result=Invoke-MvmC21RunInventory -IntentScopeAuthority $scope -NativePresentRecords $native `
    -FormalPresentedCandidates $formal -RequiredIntentCount $required -CaptureEnvelope $envelope
if($good){
    if(-not[bool]$result.authority_exact){throw "正当なC2.1 authorityを拒否しました: $(@($result.blockers)-join', ')"}
    if($Case-eq'GoodDistinctDecisionsSameOrdinal'-and
       [int]$result.producer_current_duplicate_ordinal_count_diagnostic-ne1){
        throw 'distinct scheduler decisionsの同一ordinalをinventoryしていません'
    }
}else{
    if([bool]$result.authority_exact){throw "不完全なC2.1 authorityを受理しました: $Case"}
    $expected=switch($Case){
        'NegativeMissingRequiredSet'{'REQUIRED_INTENT_MEMBERSHIP_PROVENANCE_MISSING'}
        'NegativeDuplicateRequiredSet'{'REQUIRED_SCHEDULER_INTENT_SET_DUPLICATE'}
        'NegativeCountSetMismatch'{'REQUIRED_INTENT_COUNT_SET_CARDINALITY_MISMATCH'}
        'NegativeRequiredSetMissingMembership'{'DECISION_REQUIRED_MEMBERSHIP_MISMATCH'}
        'NegativeMembershipExtraOutsideRequiredSet'{'DECISION_REQUIRED_MEMBERSHIP_MISMATCH'}
        'NegativeMissingDecisionQpc'{'SCHEDULER_DECISION_QPC_PROVENANCE_MISSING'}
        'NegativeBoundaryRelationMutation'{'MEASUREMENT_BOUNDARY_RELATION_MISMATCH'}
        'NegativeMissingMeasurementStartQpc'{'MEASUREMENT_START_QPC_MISSING'}
        'NegativeMissingMeasurementEndQpc'{'MEASUREMENT_END_QPC_MISSING'}
        'NegativeAmbiguousFormalReverseJoin'{'FORMAL_PRESENTED_REVERSE_ATTRIBUTION_INVALID'}
    }
    if($expected-notin@($result.blockers)){throw "期待blockerがありません: $expected"}
    Write-Output "expected blocker: $expected"
}
Write-Output "W2-C2.1 contract $Case`: PASS"
