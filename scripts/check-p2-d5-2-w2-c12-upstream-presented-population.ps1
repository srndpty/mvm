[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Proof)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W2-C1.2 proofがありません: $Proof"}
$proofObject=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string](Need $proofObject 'schema')-ne'mvm-p2-d5-2-w2-c12-upstream-presented-population-1'){Fail 'W2-C1.2 schemaが不正です'}
if(-not[bool](Need $proofObject 'diagnostic_only')-or[bool](Need $proofObject 'candidate_filter_added')-or
   [bool](Need $proofObject 'performance_accounting_connected')-or[bool](Need $proofObject 'intent_satisfaction_connected')-or
   [bool](Need $proofObject 'c1_verdict_changed')-or-not[bool](Need $proofObject 'c1_remains_invalid')){Fail 'W2-C1.2 isolationが不正です'}
$all=0L;$exact=0L;$invalid=0L;$before=0L;$within=0L;$after=0L;$coverage=0L;$b2=0L
foreach($run in @(Need $proofObject 'runs')){
    $records=@(Need $run 'records');$runAll=[int64](Need $run 'all_etw_presented_count')
    $recordKeys=@{}
    foreach($record in $records){
        $recordKey="$([int64](Need $record 'etw_sequence'))|$([int64](Need $record 'displayed_qpc'))"
        if($recordKeys.ContainsKey($recordKey)){Fail "run $($run.run) record keyが重複しています: $recordKey"}
        $recordKeys[$recordKey]=$true
        $nativeCount=[int](Need $record 'native_candidate_count')
        $expectedNativeStatus=$(if($nativeCount-eq0){'MISSING'}elseif($nativeCount-eq1-and[bool](Need $record 'native_exact')){'MATCHED'}else{'AMBIGUOUS'})
        if([string](Need $record 'native_join_status')-ne$expectedNativeStatus){Fail "run $($run.run) native join statusが不正です: $recordKey"}
        $tokenPresent=[bool](Need $record 'composition_token_present')
        if($tokenPresent-ne(-not[string]::IsNullOrWhiteSpace([string](Need $record 'composition_token_serial')))){
            Fail "run $($run.run) composition token presenceが不正です: $recordKey"
        }
        $intentValid=[bool](Need $record 'intent_ordinal_valid')
        if($intentValid-ne(-not[string]::IsNullOrWhiteSpace([string](Need $record 'intent_ordinal')))){
            Fail "run $($run.run) intent ordinal validityが不正です: $recordKey"
        }
        $expectedExact=$expectedNativeStatus-eq'MATCHED'-and[bool](Need $record 'composition_token_present')-and
            [bool](Need $record 'intent_ordinal_valid')-and[bool](Need $record 'intent_scope_exact')
        if([bool](Need $record 'upstream_exact')-ne$expectedExact){Fail "run $($run.run) upstream exact判定が不正です: $recordKey"}
        if(-not[bool](Need $record 'physical_mapping_exact')-or$null-eq(Need $record 'physical_vblank_ordinal')){
            Fail "run $($run.run) physical mappingがexactではありません: $recordKey"
        }
        if([string](Need $record 'native_capture_envelope_relation')-notin@('BEFORE_CAPTURE_BEGIN','WITHIN_CAPTURE_ENVELOPE','AFTER_CAPTURE_CLOSE')){
            Fail "run $($run.run) capture relationが不正です: $recordKey"
        }
        foreach($relationField in @('present_start_measurement_relation','displayed_measurement_relation')){
            if([string](Need $record $relationField)-notin@('BEFORE_MEASUREMENT','WITHIN_MEASUREMENT','AFTER_MEASUREMENT')){
                Fail "run $($run.run) measurement relationが不正です: $recordKey / $relationField"
            }
        }
        if(-not[bool](Need $record 'in_c1_input_population')){Fail "run $($run.run) C1 input membershipが不正です: $recordKey"}
    }
    $runExact=@($records|Where-Object{[bool]$_.upstream_exact}).Count;$runInvalid=$records.Count-$runExact
    $runBefore=@($records|Where-Object{-not[bool]$_.upstream_exact-and[string]$_.native_capture_envelope_relation-eq'BEFORE_CAPTURE_BEGIN'}).Count
    $runWithin=@($records|Where-Object{-not[bool]$_.upstream_exact-and[string]$_.native_capture_envelope_relation-eq'WITHIN_CAPTURE_ENVELOPE'}).Count
    $runAfter=@($records|Where-Object{-not[bool]$_.upstream_exact-and[string]$_.native_capture_envelope_relation-eq'AFTER_CAPTURE_CLOSE'}).Count
    if($records.Count-ne$runAll-or$runAll-ne[int64](Need $run 'c011_candidate_population_count')-or
       $runAll-ne[int64](Need $run 'c1_input_population_count')-or$runAll-ne($runExact+$runInvalid)-or
       $runInvalid-ne($runBefore+$runWithin+$runAfter)-or
       $runExact-ne[int64](Need $run 'upstream_exact_count')-or$runInvalid-ne[int64](Need $run 'upstream_invalid_count')-or
       $runBefore-ne[int64](Need $run 'invalid_before_capture_begin_count')-or
       $runWithin-ne[int64](Need $run 'invalid_within_capture_envelope_count')-or
       $runAfter-ne[int64](Need $run 'invalid_after_capture_close_count')-or
       -not[bool](Need $run 'c0_candidate_equals_c1_input')-or-not[bool](Need $run 'b2_formal_equals_layer2_presented')-or
       -not[bool](Need $run 'b2_formal_subset_of_c1_input')-or-not[bool](Need $run 'c011_coverage_subset_of_c1_input')-or
       -not[bool](Need $run 'physical_mapping_all_exact')-or-not[bool](Need $run 'population_identity_exact')){
        Fail "run $($run.run) population identityが不成立です"
    }
    $all+=$runAll;$exact+=$runExact;$invalid+=$runInvalid;$before+=$runBefore;$within+=$runWithin;$after+=$runAfter
    $coverage+=[int64](Need $run 'c011_coverage_evaluated_population_count')
    $b2+=[int64](Need $run 'b2_formal_presented_population_count')
}
foreach($populationIdentity in @(
    @('c011_candidate_population','ETW_OBSERVED_TARGET_PRESENTED'),
    @('b2_formal_presented_population','LAYER2_EXACT_JOINED_PRESENTED'),
    @('c1_input_population','C011_CANDIDATES_ALL'),
    @('c011_coverage_pass_scope','DISPLAY_RELATION_WITHIN_MEASUREMENT_PREDECESSOR_SUCCESSOR_ONLY'))){
    if([string](Need $proofObject $populationIdentity[0])-ne[string]$populationIdentity[1]){
        Fail "population定義が不正です: $($populationIdentity[0])"
    }
}
foreach($pair in @(
    @('all_presented_count',$all),@('upstream_exact_count',$exact),@('upstream_invalid_count',$invalid),
    @('invalid_before_capture_begin_count',$before),@('invalid_within_capture_envelope_count',$within),
    @('invalid_after_capture_close_count',$after),@('c011_coverage_evaluated_population_count',$coverage),
    @('b2_formal_presented_population_count',$b2),@('c1_input_population_count',$all))){
    if([int64](Need $proofObject $pair[0])-ne[int64]$pair[1]){Fail "aggregateが一致しません: $($pair[0])"}
}
if(-not[bool](Need $proofObject 'all_equals_exact_plus_invalid')-or
   -not[bool](Need $proofObject 'invalid_equals_capture_relations')-or
   -not[bool](Need $proofObject 'population_identity_exact')-or
   [string](Need $proofObject 'verdict')-ne'UPSTREAM_PRESENTED_POPULATION_ATTRIBUTED_EXACT'){Fail 'W2-C1.2 verdict identityが不成立です'}
$expectedAttribution=$(if($within-ne0){'UPSTREAM_PROVENANCE_REGRESSION_WITHIN_CAPTURE_ENVELOPE'}else{'UPSTREAM_INVALID_OUTSIDE_NATIVE_CAPTURE_ENVELOPE'})
if([string](Need $proofObject 'attribution')-ne$expectedAttribution){Fail 'W2-C1.2 attributionがrecordsと一致しません'}
Write-Output "P2-D5-2 W2-C1.2 population checker: PASS ($all = $exact + $invalid)"
