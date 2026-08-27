Set-StrictMode -Version Latest

function Get-MvmC24Value($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if($Object.Contains($Name)){return $Object[$Name]}
        return $null
    }
    if($null-ne$Object-and$Object.PSObject.Properties.Name-contains$Name){return $Object.$Name}
    return $null
}

function Invoke-MvmC24FormalTransportPolicy {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][array]$ProducerRecords,
        [Parameter(Mandatory=$true)][int64]$RecordedDuplicateSuppressedCount,
        [Parameter(Mandatory=$true)][int64]$RecordedOutsideSuppressedCount
    )
    $records=@();$blockers=@{};$duplicate=0L;$outside=0L;$eligible=0L
    for($index=0;$index-lt$ProducerRecords.Count;++$index){
        $record=$ProducerRecords[$index]
        $scope=[string](Get-MvmC24Value $record 'intent_scope')
        $membership=[bool](Get-MvmC24Value $record 'required_current_membership')
        $membershipExact=[bool](Get-MvmC24Value $record 'required_current_membership_exact')
        $duplicateCallback=[bool](Get-MvmC24Value $record 'duplicate_callback')
        $recordedDisposition=[string](Get-MvmC24Value $record 'transport_disposition')
        $recordedEligible=[bool](Get-MvmC24Value $record 'formal_transport_eligible')
        $producerExact=[bool](Get-MvmC24Value $record 'producer_semantics_exact')
        $scopeKnown=$scope-in@('CURRENT_MEASUREMENT','FOREIGN_PRE_MEASUREMENT')
        $expectedDisposition=$(if(-not$membershipExact){'INVALID_MEMBERSHIP_PROVENANCE'}
            elseif($duplicateCallback){'SUPPRESS_DUPLICATE_CALLBACK'}
            elseif($scope-ne'FOREIGN_PRE_MEASUREMENT'-and-not$membership){'SUPPRESS_OUTSIDE_REQUIRED_SET'}
            else{'TRANSPORT'})
        $expectedEligible=$expectedDisposition-eq'TRANSPORT'
        if(-not$membershipExact){$blockers['REQUIRED_MEMBERSHIP_PROVENANCE_INVALID']=$true}
        if(-not$scopeKnown-or-not$producerExact){$blockers['PRODUCER_SEMANTICS_PROVENANCE_INVALID']=$true}
        if($recordedDisposition-ne$expectedDisposition){$blockers['TRANSPORT_DISPOSITION_SEMANTIC_MISMATCH']=$true}
        if($recordedEligible-ne$expectedEligible){$blockers['FORMAL_TRANSPORT_ELIGIBILITY_MISMATCH']=$true}
        if($expectedDisposition-eq'SUPPRESS_DUPLICATE_CALLBACK'){++$duplicate}
        elseif($expectedDisposition-eq'SUPPRESS_OUTSIDE_REQUIRED_SET'){++$outside}
        if($expectedEligible){++$eligible}
        $records+=,[ordered]@{
            record_index=$index;token_serial=[string](Get-MvmC24Value $record 'token_serial')
            intent_ordinal=[string](Get-MvmC24Value $record 'intent_ordinal')
            intent_scope=$scope;required_current_membership=$membership
            required_current_membership_exact=$membershipExact;duplicate_callback=$duplicateCallback
            expected_transport_disposition=$expectedDisposition
            recorded_transport_disposition=$recordedDisposition
            expected_formal_transport_eligible=$expectedEligible
            recorded_formal_transport_eligible=$recordedEligible
            policy_exact=$membershipExact-and$scopeKnown-and$producerExact-and
                $recordedDisposition-eq$expectedDisposition-and$recordedEligible-eq$expectedEligible
        }
    }
    if($duplicate-ne$RecordedDuplicateSuppressedCount){$blockers['DUPLICATE_SUPPRESSION_COUNT_MISMATCH']=$true}
    if($outside-ne$RecordedOutsideSuppressedCount){$blockers['OUTSIDE_SUPPRESSION_COUNT_MISMATCH']=$true}
    $blockerList=@($blockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c24-formal-transport-policy-run-1'
        producer_record_count=$ProducerRecords.Count
        checker_derived_transport_eligible_count=$eligible
        checker_derived_duplicate_suppressed_count=$duplicate
        checker_derived_outside_suppressed_count=$outside
        recorded_duplicate_suppressed_count=$RecordedDuplicateSuppressedCount
        recorded_outside_suppressed_count=$RecordedOutsideSuppressedCount
        policy_exact=$blockerList.Count-eq0;blockers=$blockerList;records=$records
    }
}
