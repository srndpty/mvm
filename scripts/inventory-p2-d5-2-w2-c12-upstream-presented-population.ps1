[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C011Directory,
    [Parameter(Mandatory=$true)][string]$C011InventoryProof,
    [Parameter(Mandatory=$true)][string]$C1MappingProof,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
function Event-Key([int64]$Sequence,[int64]$DisplayedQpc){return "$Sequence|$DisplayedQpc"}
function Same-KeySet($Left,$Right){
    $leftKeys=@($Left.Keys|Sort-Object);$rightKeys=@($Right.Keys|Sort-Object)
    return $leftKeys.Count-eq$rightKeys.Count-and($leftKeys-join'|')-eq($rightKeys-join'|')
}
foreach($path in @($C011Directory,$C011InventoryProof,$C1MappingProof)){
    if(-not(Test-Path -LiteralPath $path)){Fail "W2-C1.2必須pathがありません: $path"}
}
$inventory=Get-Content -LiteralPath $C011InventoryProof -Raw -Encoding utf8|ConvertFrom-Json
$mapping=Get-Content -LiteralPath $C1MappingProof -Raw -Encoding utf8|ConvertFrom-Json
$runCount=[int](Need $inventory 'run_count')
if($runCount-le0-or@($inventory.runs).Count-ne$runCount-or@($mapping.runs).Count-ne$runCount){Fail 'run populationが不正です'}
$runs=@();$allCount=0L;$exactCount=0L;$invalidCount=0L;$beforeInvalid=0L;$withinInvalid=0L;$afterInvalid=0L
$b2Count=0L;$coverageCount=0L;$allIdentitiesExact=$true
for($run=1;$run-le$runCount;++$run){
    $runDirectory=Join-Path $C011Directory "run-$run"
    $appPath=Join-Path $runDirectory 'traced-app.json'
    $ledgerPath=Join-Path $runDirectory 'terminal-shadow.json'
    foreach($path in @($appPath,$ledgerPath)){if(-not(Test-Path -LiteralPath $path)){Fail "run $run artifactがありません: $path"}}
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $ledger=Get-Content -LiteralPath $ledgerPath -Raw -Encoding utf8|ConvertFrom-Json
    $opportunity=Need $app 'presentation_opportunity'
    $envelope=Need (Need $app 'native_present_hook') 'capture_envelope'
    $captureBegin=[int64](Need $envelope 'begin_qpc');$captureClose=[int64](Need $envelope 'close_qpc')
    $measurementStart=[int64](Need $opportunity 'measurement_start_qpc')
    $measurementEnd=[int64](Need $opportunity 'measurement_end_qpc_exclusive')
    $candidates=@($inventory.runs[$run-1].candidates);$mappingRecords=@($mapping.runs[$run-1].records)
    $mappingByKey=@{}
    foreach($record in $mappingRecords){
        $key=Event-Key ([int64](Need $record 'etw_sequence')) ([int64](Need $record 'displayed_qpc'))
        if($mappingByKey.ContainsKey($key)){Fail "run $run C1 mapping keyが重複しています: $key"}
        $mappingByKey[$key]=$record
    }
    $b2PresentedByKey=@{}
    foreach($record in @($ledger.records|Where-Object{[string]$_.final_state-eq'Presented'})){
        $displayed=@(Need $record 'displayed_qpc')
        if($displayed.Count-ne1){Fail "run $run B2 Presented DisplayedQPC cardinalityが不正です"}
        $key=Event-Key ([int64](Need $record 'etw_sequence')) ([int64]$displayed[0])
        if($b2PresentedByKey.ContainsKey($key)){Fail "run $run B2 Presented keyが重複しています: $key"}
        $b2PresentedByKey[$key]=$true
    }
    $candidateByKey=@{};$layer2PresentedByKey=@{};$coverageByKey=@{};$records=@()
    $runExact=0;$runInvalid=0;$runBefore=0;$runWithin=0;$runAfter=0
    foreach($candidate in $candidates){
        $sequence=[int64](Need $candidate 'etw_sequence');$presentStart=[int64](Need $candidate 'present_start_qpc')
        $displayed=[int64](Need $candidate 'displayed_qpc');$key=Event-Key $sequence $displayed
        if($candidateByKey.ContainsKey($key)){Fail "run $run C0 candidate keyが重複しています: $key"}
        $candidateByKey[$key]=$true
        if(-not$mappingByKey.ContainsKey($key)){Fail "run $run candidateに対応するC1 mapping recordがありません: $key"}
        $mappingRecord=$mappingByKey[$key]
        $nativeCount=[int](Need $candidate 'native_candidate_count')
        $nativeStatus=$(if($nativeCount-eq0){'MISSING'}elseif($nativeCount-eq1-and[bool](Need $candidate 'native_exact')){'MATCHED'}else{'AMBIGUOUS'})
        $tokenSerial=[string](Need $candidate 'composition_token_serial')
        $tokenPresent=-not[string]::IsNullOrWhiteSpace($tokenSerial)-and$tokenSerial-ne'0'
        $intentOrdinal=[string](Need $candidate 'intent_ordinal')
        $intentValid=[bool](Need $candidate 'intent_exact')-and-not[string]::IsNullOrWhiteSpace($intentOrdinal)
        $scopeExact=[bool](Need $candidate 'intent_scope_exact');$scope=[string](Need $candidate 'intent_scope')
        if(-not$scopeExact-or[string]::IsNullOrWhiteSpace($scope)){$scope='MISSING'}
        $upstreamExact=$nativeStatus-eq'MATCHED'-and$tokenPresent-and$intentValid-and$scopeExact
        $captureRelation=$(if($presentStart-lt$captureBegin){'BEFORE_CAPTURE_BEGIN'}elseif($presentStart-le$captureClose){'WITHIN_CAPTURE_ENVELOPE'}else{'AFTER_CAPTURE_CLOSE'})
        $presentMeasurementRelation=$(if($presentStart-lt$measurementStart){'BEFORE_MEASUREMENT'}elseif($presentStart-lt$measurementEnd){'WITHIN_MEASUREMENT'}else{'AFTER_MEASUREMENT'})
        $displayMeasurementRelation=$(if($displayed-lt$measurementStart){'BEFORE_MEASUREMENT'}elseif($displayed-lt$measurementEnd){'WITHIN_MEASUREMENT'}else{'AFTER_MEASUREMENT'})
        $inCoverage=[string](Need $candidate 'display_relation')-eq'WITHIN_PREDECESSOR_SUCCESSOR_ENVELOPE'
        $inLayer2=[bool](Need $candidate 'layer2_cohort_member')
        if($inCoverage){$coverageByKey[$key]=$true}
        if($inLayer2){$layer2PresentedByKey[$key]=$true}
        if($upstreamExact){$runExact+=1}else{
            $runInvalid+=1
            if($captureRelation-eq'BEFORE_CAPTURE_BEGIN'){$runBefore+=1}
            elseif($captureRelation-eq'WITHIN_CAPTURE_ENVELOPE'){$runWithin+=1}
            else{$runAfter+=1}
        }
        $records+=[ordered]@{
            etw_sequence=$sequence;present_start_qpc=$presentStart;displayed_qpc=$displayed
            native_join_status=$nativeStatus;native_candidate_count=$nativeCount;native_exact=[bool](Need $candidate 'native_exact')
            composition_token_present=$tokenPresent;composition_token_serial=$(if($tokenPresent){$tokenSerial}else{$null})
            intent_ordinal_valid=$intentValid;intent_ordinal=$(if($intentValid){$intentOrdinal}else{$null})
            intent_scope_exact=$scopeExact;intent_scope=$scope
            native_capture_envelope_relation=$captureRelation
            present_start_measurement_relation=$presentMeasurementRelation
            displayed_measurement_relation=$displayMeasurementRelation
            physical_vblank_ordinal=$mappingRecord.physical_vblank_ordinal
            physical_mapping_exact=[bool](Need $mappingRecord 'mapping_exact')
            upstream_exact=$upstreamExact
            in_c011_coverage_evaluated_population=$inCoverage
            in_b2_formal_presented_population=$b2PresentedByKey.ContainsKey($key)
            in_c1_input_population=$true
        }
    }
    $c0EqualsC1=Same-KeySet $candidateByKey $mappingByKey
    $b2EqualsLayer2=Same-KeySet $b2PresentedByKey $layer2PresentedByKey
    $b2Subset=@($b2PresentedByKey.Keys|Where-Object{-not$candidateByKey.ContainsKey($_)}).Count-eq0
    $coverageSubset=@($coverageByKey.Keys|Where-Object{-not$candidateByKey.ContainsKey($_)}).Count-eq0
    $physicalAllExact=@($records|Where-Object{-not[bool]$_.physical_mapping_exact}).Count-eq0
    $identityExact=$candidates.Count-eq($runExact+$runInvalid)-and$runInvalid-eq($runBefore+$runWithin+$runAfter)-and
        $c0EqualsC1-and$b2EqualsLayer2-and$b2Subset-and$coverageSubset-and$physicalAllExact
    if(-not$identityExact){$allIdentitiesExact=$false}
    $runs+=[ordered]@{
        run=$run;all_etw_presented_count=$candidates.Count;upstream_exact_count=$runExact;upstream_invalid_count=$runInvalid
        invalid_before_capture_begin_count=$runBefore;invalid_within_capture_envelope_count=$runWithin
        invalid_after_capture_close_count=$runAfter
        c011_candidate_population_count=$candidates.Count
        c011_coverage_evaluated_population_count=$coverageByKey.Count
        b2_formal_presented_population_count=$b2PresentedByKey.Count
        c1_input_population_count=$mappingRecords.Count
        c0_candidate_equals_c1_input=$c0EqualsC1;b2_formal_equals_layer2_presented=$b2EqualsLayer2
        b2_formal_subset_of_c1_input=$b2Subset;c011_coverage_subset_of_c1_input=$coverageSubset
        physical_mapping_all_exact=$physicalAllExact;population_identity_exact=$identityExact
        first_present_start_qpc=[int64](($candidates.present_start_qpc|Measure-Object -Minimum).Minimum)
        observer_start_qpc_available=$false;observer_start_qpc=$null
        observer_preroll_qpc=[int64](Need (Need $opportunity 'physical_mapping_support_envelope_shadow') 'predecessor_qpc')
        native_capture_begin_qpc=$captureBegin;native_capture_close_qpc=$captureClose
        window_visibility_transition_qpc_available=$false;window_visibility_transition_qpc=$null
        traced_app_sha256=((Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant())
        b2_terminal_shadow_sha256=((Get-FileHash -LiteralPath $ledgerPath -Algorithm SHA256).Hash.ToLowerInvariant())
        records=$records
    }
    $allCount+=$candidates.Count;$exactCount+=$runExact;$invalidCount+=$runInvalid
    $beforeInvalid+=$runBefore;$withinInvalid+=$runWithin;$afterInvalid+=$runAfter
    $b2Count+=$b2PresentedByKey.Count;$coverageCount+=$coverageByKey.Count
}
$attribution=$(if($withinInvalid-ne0){'UPSTREAM_PROVENANCE_REGRESSION_WITHIN_CAPTURE_ENVELOPE'}else{'UPSTREAM_INVALID_OUTSIDE_NATIVE_CAPTURE_ENVELOPE'})
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c12-upstream-presented-population-1';stage='P2-D5-2-W2-C1.2'
    diagnostic_only=$true;candidate_filter_added=$false;performance_accounting_connected=$false
    intent_satisfaction_connected=$false;c1_verdict_changed=$false;c1_remains_invalid=$true
    c011_coverage_pass_scope='DISPLAY_RELATION_WITHIN_MEASUREMENT_PREDECESSOR_SUCCESSOR_ONLY'
    c011_candidate_population='ETW_OBSERVED_TARGET_PRESENTED'
    b2_formal_presented_population='LAYER2_EXACT_JOINED_PRESENTED'
    c1_input_population='C011_CANDIDATES_ALL'
    population_sets_are_distinct=$allCount-ne$coverageCount-or$allCount-ne$b2Count
    run_count=$runCount;all_presented_count=$allCount;upstream_exact_count=$exactCount;upstream_invalid_count=$invalidCount
    invalid_before_capture_begin_count=$beforeInvalid;invalid_within_capture_envelope_count=$withinInvalid
    invalid_after_capture_close_count=$afterInvalid
    c011_coverage_evaluated_population_count=$coverageCount;b2_formal_presented_population_count=$b2Count
    c1_input_population_count=$allCount
    all_equals_exact_plus_invalid=$allCount-eq($exactCount+$invalidCount)
    invalid_equals_capture_relations=$invalidCount-eq($beforeInvalid+$withinInvalid+$afterInvalid)
    population_identity_exact=$allIdentitiesExact
    attribution=$attribution
    verdict=$(if($allIdentitiesExact){'UPSTREAM_PRESENTED_POPULATION_ATTRIBUTED_EXACT'}else{'UPSTREAM_PRESENTED_POPULATION_ATTRIBUTION_INVALID'})
    source_c011_inventory_sha256=((Get-FileHash -LiteralPath $C011InventoryProof -Algorithm SHA256).Hash.ToLowerInvariant())
    source_c1_mapping_sha256=((Get-FileHash -LiteralPath $C1MappingProof -Algorithm SHA256).Hash.ToLowerInvariant())
    runs=$runs
}
$outputDirectory=Split-Path -Parent $Output
if($outputDirectory-and-not(Test-Path -LiteralPath $outputDirectory)){New-Item -ItemType Directory -Path $outputDirectory|Out-Null}
$result|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
if(-not$allIdentitiesExact){Fail 'W2-C1.2 population attribution identityが不成立です'}
Write-Host "P2-D5-2 W2-C1.2 population: $($result.verdict) ($allCount = $exactCount + $invalidCount)"
