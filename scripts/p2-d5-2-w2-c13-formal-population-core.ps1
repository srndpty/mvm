Set-StrictMode -Version Latest

function Get-MvmC13EventKey([int64]$Sequence,[int64]$DisplayedQpc){return "$Sequence|$DisplayedQpc"}

function Invoke-MvmC13FormalPresentedPopulation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][array]$ObservedCandidates,
        [Parameter(Mandatory=$true)][array]$B2TerminalRecords
    )
    $candidateGroups=@{};$observedRecords=@();$blockers=@{}
    foreach($candidate in $ObservedCandidates){
        $displayed=@($candidate.displayed_qpc)
        if($displayed.Count-ne1){$blockers['OBSERVED_DISPLAYED_QPC_CARDINALITY_INVALID']=$true;continue}
        $key=Get-MvmC13EventKey ([int64]$candidate.etw_sequence) ([int64]$displayed[0])
        if(-not$candidateGroups.ContainsKey($key)){$candidateGroups[$key]=@()}
        $candidateGroups[$key]=@($candidateGroups[$key])+@($candidate)
    }
    if(@($candidateGroups.GetEnumerator()|Where-Object{$_.Value.Count-ne1}).Count-ne0){
        $blockers['OBSERVED_EXACT_EVENT_KEY_DUPLICATE']=$true
    }
    $b2ByKey=@{};$b2PresentedCount=0
    foreach($terminalRecord in $B2TerminalRecords){
        if([string]$terminalRecord.final_state-ne'Presented'){continue}
        if($terminalRecord.PSObject.Properties.Name-notcontains'formal_transport_eligible'){
            $blockers['B2_FORMAL_ELIGIBILITY_MISSING']=$true
            continue
        }
        if(-not[bool]$terminalRecord.formal_transport_eligible){continue}
        $b2PresentedCount+=1
        $displayed=@($terminalRecord.displayed_qpc)
        if($displayed.Count-ne1){$blockers['B2_PRESENTED_DISPLAYED_QPC_CARDINALITY_INVALID']=$true;continue}
        $key=Get-MvmC13EventKey ([int64]$terminalRecord.etw_sequence) ([int64]$displayed[0])
        if($b2ByKey.ContainsKey($key)){$blockers['B2_FORMAL_KEY_DUPLICATE']=$true}else{$b2ByKey[$key]=$terminalRecord}
    }
    $formalCandidates=@();$formalRecords=@();$missing=0;$ambiguous=0;$formalInvalid=0
    foreach($key in @($b2ByKey.Keys|Sort-Object)){
        $candidateMatches=@($(if($candidateGroups.ContainsKey($key)){$candidateGroups[$key]}else{@()}))
        if($candidateMatches.Count-eq0){$missing+=1;continue}
        if($candidateMatches.Count-ne1){$ambiguous+=1;continue}
        $candidate=$candidateMatches[0]
        $tokenPresent=-not[string]::IsNullOrWhiteSpace([string]$candidate.composition_token_serial)-and
            [string]$candidate.composition_token_serial-ne'0'
        $upstreamExact=[bool]$candidate.native_exact-and[bool]$candidate.intent_exact-and
            [bool]$candidate.intent_scope_exact-and$tokenPresent
        if(-not$upstreamExact){$formalInvalid+=1}
        $formalCandidates+=,$candidate
        $formalRecords+=[ordered]@{
            exact_event_key=$key;etw_sequence=[int64]$candidate.etw_sequence
            displayed_qpc=[int64]$candidate.displayed_qpc
            native_exact=[bool]$candidate.native_exact;composition_token_present=$tokenPresent
            intent_exact=[bool]$candidate.intent_exact;intent_scope_exact=[bool]$candidate.intent_scope_exact
            upstream_exact=$upstreamExact
        }
    }
    $upstreamInvalidNonformal=0;$upstreamExactNonformal=0
    foreach($candidate in $ObservedCandidates){
        $displayed=@($candidate.displayed_qpc)
        if($displayed.Count-ne1){continue}
        $key=Get-MvmC13EventKey ([int64]$candidate.etw_sequence) ([int64]$displayed[0])
        $isFormal=$b2ByKey.ContainsKey($key)
        $tokenPresent=-not[string]::IsNullOrWhiteSpace([string]$candidate.composition_token_serial)-and
            [string]$candidate.composition_token_serial-ne'0'
        $upstreamExact=[bool]$candidate.native_exact-and[bool]$candidate.intent_exact-and
            [bool]$candidate.intent_scope_exact-and$tokenPresent
        if(-not$isFormal){if($upstreamExact){$upstreamExactNonformal+=1}else{$upstreamInvalidNonformal+=1}}
        $observedRecords+=[ordered]@{
            exact_event_key=$key;etw_sequence=[int64]$candidate.etw_sequence;displayed_qpc=[int64]$candidate.displayed_qpc
            in_b2_formal_presented_population=$isFormal;upstream_exact=$upstreamExact
        }
    }
    if($missing-ne0){$blockers['B2_FORMAL_C0_JOIN_MISSING']=$true}
    if($ambiguous-ne0){$blockers['B2_FORMAL_C0_JOIN_AMBIGUOUS']=$true}
    if($formalInvalid-ne0){$blockers['B2_FORMAL_UPSTREAM_INVALID']=$true}
    $formalKeySetExact=$b2ByKey.Count-eq$formalRecords.Count-and$missing-eq0-and$ambiguous-eq0
    if(-not$formalKeySetExact){$blockers['B2_C1_FORMAL_KEY_SET_MISMATCH']=$true}
    $nonformalCount=$ObservedCandidates.Count-$b2PresentedCount
    $observedIdentity=$ObservedCandidates.Count-eq($b2PresentedCount+$nonformalCount)-and
        $nonformalCount-eq($upstreamInvalidNonformal+$upstreamExactNonformal)
    if(-not$observedIdentity){$blockers['OBSERVED_POPULATION_IDENTITY_MISMATCH']=$true}
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c13-formal-presented-population-run-1'
        formal_membership_authority='B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'
        enrichment_authority='C011_EXACT_EVENT_IDENTITY_JOIN'
        formal_membership_uses_upstream_exact=$false
        formal_membership_uses_display_relation=$false
        formal_membership_uses_measurement_membership=$false
        formal_membership_uses_source_frame=$false
        formal_membership_uses_qpc_heuristic=$false
        observed_presented_count=$ObservedCandidates.Count
        b2_formal_presented_count=$b2PresentedCount
        c1_formal_input_count=$formalCandidates.Count
        nonformal_observed_presented_count=$nonformalCount
        upstream_invalid_nonformal_count=$upstreamInvalidNonformal
        upstream_exact_nonformal_count=$upstreamExactNonformal
        b2_formal_missing_c0_candidate_count=$missing
        b2_formal_ambiguous_c0_candidate_count=$ambiguous
        b2_formal_upstream_invalid_count=$formalInvalid
        b2_formal_equals_c1_formal_count=$b2PresentedCount-eq$formalCandidates.Count
        b2_formal_key_set_equals_c1_formal_key_set=$formalKeySetExact
        observed_population_identity_exact=$observedIdentity
        authority_valid=$blockers.Count-eq0
        blockers=@($blockers.Keys|Sort-Object)
        formal_candidates=$formalCandidates
        formal_records=$formalRecords
        observed_records=$observedRecords
    }
}
