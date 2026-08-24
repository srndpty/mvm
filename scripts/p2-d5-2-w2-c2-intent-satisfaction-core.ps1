Set-StrictMode -Version Latest

function Get-MvmC2Property($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Invoke-MvmC2IntentSatisfactionLedger {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][array]$FormalPresentedRecords,
        [Parameter(Mandatory=$true)][array]$RequiredCurrentIntentOrdinals
    )
    $requiredSet=@{};$blockers=@{}
    foreach($requiredOrdinal in $RequiredCurrentIntentOrdinals){
        $key=[string]([uint64]$requiredOrdinal)
        if($requiredSet.ContainsKey($key)){$blockers['REQUIRED_CURRENT_INTENT_DUPLICATE']=$true}
        $requiredSet[$key]=$true
    }
    if($requiredSet.Count-eq0){$blockers['REQUIRED_CURRENT_INTENT_DOMAIN_EMPTY']=$true}
    $records=@();$missingIntent=0L;$ambiguousIntent=0L;$outsideDomain=0L
    foreach($formalRecord in $FormalPresentedRecords){
        $scope=[string](Get-MvmC2Property $formalRecord 'intent_scope')
        $scopeExact=[bool](Get-MvmC2Property $formalRecord 'intent_scope_exact')
        $ordinalValue=Get-MvmC2Property $formalRecord 'intent_ordinal'
        $ordinalValid=[bool](Get-MvmC2Property $formalRecord 'intent_ordinal_valid')
        $ordinalExact=[bool](Get-MvmC2Property $formalRecord 'intent_ordinal_exact')
        $ordinalPresent=$null-ne$ordinalValue-and-not[string]::IsNullOrWhiteSpace([string]$ordinalValue)
        $eventMissing=[string]::IsNullOrWhiteSpace($scope)-or-not$ordinalPresent-or-not$ordinalValid
        $eventAmbiguous=($scopeExact-eq$false-and-not[string]::IsNullOrWhiteSpace($scope))-or
            ($scope-notin@('CURRENT_MEASUREMENT','FOREIGN_PRE_MEASUREMENT')-and-not[string]::IsNullOrWhiteSpace($scope))-or
            ($ordinalPresent-and$ordinalValid-and-not$ordinalExact)
        if($eventMissing){$missingIntent+=1}
        if($eventAmbiguous){$ambiguousIntent+=1}
        $ordinal=$(if($ordinalPresent){[string]([uint64]$ordinalValue)}else{$null})
        $classification=$(if($scope-in@('CURRENT_MEASUREMENT','FOREIGN_PRE_MEASUREMENT')){$scope}else{'INVALID'})
        if($classification-eq'CURRENT_MEASUREMENT'-and$ordinalPresent-and$ordinalValid-and$ordinalExact-and
           -not$requiredSet.ContainsKey($ordinal)){$outsideDomain+=1}
        $records+=,[pscustomobject][ordered]@{
            exact_event_key=[string](Get-MvmC2Property $formalRecord 'exact_event_key')
            etw_sequence=Get-MvmC2Property $formalRecord 'etw_sequence'
            intent_scope=$(if([string]::IsNullOrWhiteSpace($scope)){$null}else{$scope})
            intent_scope_exact=$scopeExact
            intent_ordinal=$ordinal
            intent_ordinal_valid=$ordinalValid
            intent_ordinal_exact=$ordinalExact
            physical_vblank_ordinal=Get-MvmC2Property $formalRecord 'physical_vblank_ordinal'
            in_measurement_physical_domain=[bool](Get-MvmC2Property $formalRecord 'in_measurement_physical_domain')
            classification=$classification
        }
    }
    $eventKeys=@($records|Select-Object -ExpandProperty exact_event_key)
    if(@($eventKeys|Where-Object{[string]::IsNullOrWhiteSpace([string]$_)}).Count-ne0-or
       @($eventKeys|Group-Object|Where-Object{$_.Count-gt1}).Count-ne0){
        $blockers['FORMAL_PRESENTED_EXACT_EVENT_KEY_INVALID']=$true
    }
    $physicalDuplicate=0L
    foreach($group in @($records|Group-Object {[string]$_.physical_vblank_ordinal})){
        if([string]::IsNullOrWhiteSpace([string]$group.Name)){$blockers['PHYSICAL_VBLANK_ORDINAL_MISSING']=$true;continue}
        if($group.Count-gt1){$physicalDuplicate+=$group.Count-1}
    }
    $inDomain=@($records|Where-Object{[bool]$_.in_measurement_physical_domain})
    $currentInDomain=@($inDomain|Where-Object{$_.classification-eq'CURRENT_MEASUREMENT'-and
        [bool]$_.intent_scope_exact-and[bool]$_.intent_ordinal_valid-and[bool]$_.intent_ordinal_exact-and
        $requiredSet.ContainsKey([string]$_.intent_ordinal)})
    $foreignInDomain=@($inDomain|Where-Object{$_.classification-eq'FOREIGN_PRE_MEASUREMENT'-and
        [bool]$_.intent_scope_exact-and[bool]$_.intent_ordinal_valid-and[bool]$_.intent_ordinal_exact})
    $duplicateCurrent=0L
    foreach($group in @($currentInDomain|Group-Object intent_ordinal)){
        if($group.Count-gt1){$duplicateCurrent+=$group.Count-1}
    }
    $satisfied=@($currentInDomain|Select-Object -ExpandProperty intent_ordinal -Unique).Count
    $filled=@($inDomain|Select-Object -ExpandProperty physical_vblank_ordinal -Unique).Count
    $accountingExact=$satisfied+$foreignInDomain.Count-eq$inDomain.Count
    $physicalFillExact=$filled-eq$inDomain.Count
    if($duplicateCurrent-ne0){$blockers['DUPLICATE_CURRENT_INTENT_SATISFACTION']=$true}
    if($outsideDomain-ne0){$blockers['CURRENT_INTENT_OUTSIDE_REQUIRED_DOMAIN']=$true}
    if($missingIntent-ne0){$blockers['INTENT_PROVENANCE_MISSING']=$true}
    if($ambiguousIntent-ne0){$blockers['INTENT_PROVENANCE_AMBIGUOUS']=$true}
    if($physicalDuplicate-ne0){$blockers['MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL']=$true}
    if(-not$accountingExact){$blockers['INTENT_SATISFACTION_ACCOUNTING_IDENTITY_VIOLATION']=$true}
    if(-not$physicalFillExact){$blockers['C1_DERIVED_PHYSICAL_FILL_IDENTITY_VIOLATION']=$true}
    $blockerList=@($blockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c2-intent-satisfaction-run-1'
        formal_presented_population='C1_FORMAL_PRESENTED_ONLY'
        required_current_intent_authority='LAYER_1A_SCHEDULER_REQUIRED_INTENT_DOMAIN'
        required_current_intent_ordinals=@($RequiredCurrentIntentOrdinals|ForEach-Object{[string]([uint64]$_)})
        required_current_intent_count=$requiredSet.Count
        formal_presented_event_count=$records.Count
        in_domain_presented_event_count=$inDomain.Count
        satisfied_intent_count=$satisfied
        in_domain_presented_foreign_intent_count=$foreignInDomain.Count
        filled_physical_opportunity_count=$filled
        duplicate_current_intent_satisfaction_count=$duplicateCurrent
        current_intent_outside_required_domain_count=$outsideDomain
        missing_intent_provenance_count=$missingIntent
        ambiguous_intent_provenance_count=$ambiguousIntent
        multiple_formal_presented_per_physical_ordinal_count=$physicalDuplicate
        satisfaction_accounting_identity_exact=$accountingExact
        physical_fill_unique_ordinal_identity_exact=$true
        c1_one_presented_per_ordinal_derived_identity_exact=$physicalFillExact
        source_frame_identity_used=$false
        performance_threshold_evaluated=$false
        frame_swapped_retirement_changed=$false
        ledger_exact=$blockerList.Count-eq0
        blockers=$blockerList
        records=$records
    }
}

function Assert-MvmC2IntentSatisfactionLedger {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $fields=@('formal_presented_population','required_current_intent_authority',
        'required_current_intent_count','formal_presented_event_count','in_domain_presented_event_count',
        'satisfied_intent_count','in_domain_presented_foreign_intent_count',
        'filled_physical_opportunity_count','duplicate_current_intent_satisfaction_count',
        'current_intent_outside_required_domain_count','missing_intent_provenance_count',
        'ambiguous_intent_provenance_count','multiple_formal_presented_per_physical_ordinal_count',
        'satisfaction_accounting_identity_exact','physical_fill_unique_ordinal_identity_exact',
        'c1_one_presented_per_ordinal_derived_identity_exact','source_frame_identity_used',
        'performance_threshold_evaluated','frame_swapped_retirement_changed','ledger_exact')
    foreach($field in $fields){
        if([string](Get-MvmC2Property $Expected $field)-ne[string](Get-MvmC2Property $Actual $field)){
            throw "C2 ledger fieldが再集計値と一致しません: $field"
        }
    }
    foreach($field in @('required_current_intent_ordinals','blockers')){
        if((@((Get-MvmC2Property $Expected $field))-join',')-ne(@((Get-MvmC2Property $Actual $field))-join',')){
            throw "C2 ledger集合が再集計値と一致しません: $field"
        }
    }
    $expectedRecords=@(Get-MvmC2Property $Expected 'records')
    $actualRecords=@(Get-MvmC2Property $Actual 'records')
    if($expectedRecords.Count-ne$actualRecords.Count){throw 'C2 ledger record件数が一致しません'}
    $recordFields=@('exact_event_key','etw_sequence','intent_scope','intent_scope_exact',
        'intent_ordinal','intent_ordinal_valid','intent_ordinal_exact','physical_vblank_ordinal',
        'in_measurement_physical_domain','classification')
    for($index=0;$index-lt$expectedRecords.Count;++$index){
        foreach($field in $recordFields){
            if([string](Get-MvmC2Property $expectedRecords[$index] $field)-ne
               [string](Get-MvmC2Property $actualRecords[$index] $field)){
                throw "C2 ledger recordが再生値と一致しません: record=$index field=$field"
            }
        }
    }
}
