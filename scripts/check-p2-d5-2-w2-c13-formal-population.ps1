[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Proof)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"};return $Object.$Name}
function Compare-ReplayedMapping([int]$Run,[string]$Population,$Replay,$ArtifactRecords){
    $replayRecords=@(Need $Replay 'records');$artifactRecords=@($ArtifactRecords)
    $artifactByKey=@{}
    foreach($record in $artifactRecords){
        $key="$([int64](Need $record 'etw_sequence'))|$([int64](Need $record 'displayed_qpc'))"
        if($artifactByKey.ContainsKey($key)){Fail "run $Run $Population artifact mapping keyが重複しています: $key"}
        $artifactByKey[$key]=$record
    }
    if($replayRecords.Count-ne$artifactByKey.Count){Fail "run $Run $Population replay mapping countが一致しません"}
    foreach($record in $replayRecords){
        $key="$([int64](Need $record 'etw_sequence'))|$([int64](Need $record 'displayed_qpc'))"
        if(-not$artifactByKey.ContainsKey($key)){Fail "run $Run $Population replay keyがartifactにありません: $key"}
        $artifactRecord=$artifactByKey[$key]
        foreach($field in @('physical_vblank_ordinal','physical_vblank_qpc','mapping_solution_count','mapping_exact','in_measurement_physical_domain')){
            if([string](Need $record $field)-ne[string](Need $artifactRecord $field)){
                Fail "run $Run $Population replay結果が一致しません: $key / $field"
            }
        }
    }
}
if(-not(Test-Path -LiteralPath $Proof)){Fail "W2-C1.3 proofがありません: $Proof"}
$proofObject=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string](Need $proofObject 'schema')-ne'mvm-p2-d5-2-w2-c1-displayed-physical-mapping-2'){Fail 'W2-C1.3 schemaが不正です'}
if([string](Need $proofObject 'formal_population_authority')-ne'B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'-or
   [bool](Need $proofObject 'performance_accounting_connected')-or[bool](Need $proofObject 'intent_satisfaction_connected')){
    Fail 'W2-C1.3 authority isolationが不正です'
}
$sourceDirectory=[string](Need $proofObject 'source_c011_directory')
$upstreamProofPath=[string](Need $proofObject 'source_upstream_inventory_proof')
foreach($path in @($sourceDirectory,$upstreamProofPath)){if(-not(Test-Path -LiteralPath $path)){Fail "sealed sourceがありません: $path"}}
$upstreamHash=(Get-FileHash -LiteralPath $upstreamProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
if($upstreamHash-ne[string](Need $proofObject 'upstream_inventory_proof_sha256')){Fail 'sealed C0 inventory hashが一致しません'}
$upstreamProof=Get-Content -LiteralPath $upstreamProofPath -Raw -Encoding utf8|ConvertFrom-Json
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
$observed=0L;$formal=0L;$nonformal=0L;$invalidNonformal=0L;$exactNonformal=0L
$proofRuns=@(Need $proofObject 'runs')
if($proofRuns.Count-ne@($upstreamProof.runs).Count){Fail 'C1 / C0 run populationが一致しません'}
for($runIndex=0;$runIndex-lt$proofRuns.Count;++$runIndex){
    $run=$proofRuns[$runIndex]
    $population=Need $run 'formal_population_authority'
    $observedRecords=@(Need $population 'observed_records');$formalRecords=@(Need $population 'formal_records')
    $observedKeys=@{};$formalKeys=@{}
    foreach($record in $observedRecords){
        $key=[string](Need $record 'exact_event_key')
        if($observedKeys.ContainsKey($key)){Fail "run $($run.run) observed keyが重複しています: $key"}
        $observedKeys[$key]=$record
    }
    foreach($record in $formalRecords){
        $key=[string](Need $record 'exact_event_key')
        if($formalKeys.ContainsKey($key)){Fail "run $($run.run) formal keyが重複しています: $key"}
        if(-not$observedKeys.ContainsKey($key)){Fail "run $($run.run) formal keyがobserved集合にありません: $key"}
        if(-not[bool](Need $record 'native_exact')-or-not[bool](Need $record 'composition_token_present')-or
           -not[bool](Need $record 'intent_exact')-or-not[bool](Need $record 'intent_scope_exact')-or
           -not[bool](Need $record 'upstream_exact')){Fail "run $($run.run) formal upstream provenanceが不正です: $key"}
        $formalKeys[$key]=$true
    }
    $sourceObservedCandidates=@($upstreamProof.runs[$runIndex].candidates)
    $sourceObservedKeys=@{}
    foreach($candidate in $sourceObservedCandidates){
        $displayed=@(Need $candidate 'displayed_qpc')
        if($displayed.Count-ne1){Fail "run $($run.run) sealed C0 DisplayedQPC cardinalityが不正です"}
        $key="$([int64](Need $candidate 'etw_sequence'))|$([int64]$displayed[0])"
        if($sourceObservedKeys.ContainsKey($key)){Fail "run $($run.run) sealed C0 keyが重複しています: $key"}
        $sourceObservedKeys[$key]=$true
    }
    if($sourceObservedKeys.Count-ne$observedKeys.Count-or@($sourceObservedKeys.Keys|Where-Object{-not$observedKeys.ContainsKey($_)}).Count-ne0){
        Fail "run $($run.run) sealed C0 / observed key setが一致しません"
    }
    $sourceRunDirectory=Join-Path $sourceDirectory "run-$($run.run)"
    $appPath=Join-Path $sourceRunDirectory 'traced-app.json'
    $rawPath=Join-Path $sourceRunDirectory 'present-history-raw.json'
    $terminalPath=Join-Path $sourceRunDirectory 'terminal-shadow.json'
    foreach($path in @($appPath,$rawPath,$terminalPath)){if(-not(Test-Path -LiteralPath $path)){Fail "run $($run.run) sealed sourceがありません: $path"}}
    $sealedHashes=Need $run 'sealed_input_sha256'
    foreach($sourceHash in @(
        @('traced_app',$appPath),@('present_history_raw',$rawPath),
        @('b2_terminal_shadow',$terminalPath),@('upstream_inventory_proof',$upstreamProofPath))){
        $actualHash=(Get-FileHash -LiteralPath $sourceHash[1] -Algorithm SHA256).Hash.ToLowerInvariant()
        if($actualHash-ne[string](Need $sealedHashes $sourceHash[0])){Fail "run $($run.run) sealed input hashが一致しません: $($sourceHash[0])"}
    }
    $terminalHash=(Get-FileHash -LiteralPath $terminalPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if($terminalHash-ne[string](Need $sealedHashes 'b2_terminal_shadow')){Fail "run $($run.run) sealed B2 hashが一致しません"}
    $terminal=Get-Content -LiteralPath $terminalPath -Raw -Encoding utf8|ConvertFrom-Json
    $sourceFormalKeys=@{}
    foreach($terminalRecord in @($terminal.records|Where-Object{
        [string]$_.final_state-eq'Presented'-and[bool](Need $_ 'formal_transport_eligible')
    })){
        $displayed=@(Need $terminalRecord 'displayed_qpc')
        if($displayed.Count-ne1){Fail "run $($run.run) sealed B2 DisplayedQPC cardinalityが不正です"}
        $key="$([int64](Need $terminalRecord 'etw_sequence'))|$([int64]$displayed[0])"
        if($sourceFormalKeys.ContainsKey($key)){Fail "run $($run.run) sealed B2 keyが重複しています: $key"}
        $sourceFormalKeys[$key]=$true
    }
    if($sourceFormalKeys.Count-ne$formalKeys.Count-or@($sourceFormalKeys.Keys|Where-Object{-not$formalKeys.ContainsKey($_)}).Count-ne0){
        Fail "run $($run.run) sealed B2 / formal key setが一致しません"
    }
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
    $opportunity=Need $app 'presentation_opportunity';$physical=Need $opportunity 'physical_vblank'
    $shadow=Need $opportunity 'physical_vblank_domain_shadow'
    $support=Need $opportunity 'physical_mapping_support_envelope_shadow'
    $physicalAuthorityValid=[bool](Need $shadow 'shadow_authority_valid')-and[bool](Need $support 'authority_valid')
    $replayedPopulation=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates $sourceObservedCandidates `
        -B2TerminalRecords @(Need $terminal 'records')
    if(-not[bool]$replayedPopulation.authority_valid){Fail "run $($run.run) sealed formal population replayが不成立です"}
    $replayedFormal=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates @($replayedPopulation.formal_candidates) `
        -Samples @(Need $physical 'samples') `
        -PredecessorOrdinal ([int64](Need $support 'predecessor_ordinal')) `
        -SuccessorOrdinal ([int64](Need $support 'successor_ordinal')) `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) `
        -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $physicalAuthorityValid `
        -EtwEventsLost ([int64](Need $raw 'etw_events_lost')) `
        -EtwBuffersLost ([int64](Need $raw 'etw_buffers_lost')) `
        -PresentEventOverflowCount ([int64](Need $raw 'present_event_overflow_count'))
    $replayedObserved=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates $sourceObservedCandidates `
        -Samples @(Need $physical 'samples') `
        -PredecessorOrdinal ([int64](Need $support 'predecessor_ordinal')) `
        -SuccessorOrdinal ([int64](Need $support 'successor_ordinal')) `
        -OriginOrdinal ([int64](Need $shadow 'origin_ordinal')) `
        -LastOrdinal ([int64](Need $shadow 'last_ordinal')) `
        -PhysicalAuthorityValid $physicalAuthorityValid `
        -EtwEventsLost ([int64](Need $raw 'etw_events_lost')) `
        -EtwBuffersLost ([int64](Need $raw 'etw_buffers_lost')) `
        -PresentEventOverflowCount ([int64](Need $raw 'present_event_overflow_count'))
    if(-not[bool]$replayedFormal.mapping_exact-or
       [int64]$replayedFormal.mapped_exact_count-ne@($replayedPopulation.formal_candidates).Count-or
       [int64]$replayedObserved.mapped_exact_count-ne$sourceObservedCandidates.Count-or
       [int64]$replayedObserved.missing_mapping_count-ne0-or[int64]$replayedObserved.ambiguous_mapping_count-ne0-or
       [int64]$replayedObserved.duplicate_physical_ordinal_count-ne0){Fail "run $($run.run) sealed mapping replayが不成立です"}
    $replayedFormalObject=$replayedFormal|ConvertTo-Json -Depth 10|ConvertFrom-Json
    $replayedObservedObject=$replayedObserved|ConvertTo-Json -Depth 10|ConvertFrom-Json
    Compare-ReplayedMapping -Run ([int]$run.run) -Population 'formal' -Replay $replayedFormalObject -ArtifactRecords @(Need $run 'records')
    Compare-ReplayedMapping -Run ([int]$run.run) -Population 'observed' -Replay $replayedObservedObject `
        -ArtifactRecords @(Need (Need $run 'observed_physical_mapping_diagnostic') 'records')
    $mappingKeys=@{}
    foreach($record in @(Need $run 'records')){
        $key="$([int64](Need $record 'etw_sequence'))|$([int64](Need $record 'displayed_qpc'))"
        if($mappingKeys.ContainsKey($key)){Fail "run $($run.run) C1 mapping keyが重複しています: $key"}
        $mappingKeys[$key]=$true
        if(-not[bool](Need $record 'mapping_exact')){Fail "run $($run.run) formal mappingがexactではありません: $key"}
    }
    if($mappingKeys.Count-ne$formalKeys.Count-or@($formalKeys.Keys|Where-Object{-not$mappingKeys.ContainsKey($_)}).Count-ne0){
        Fail "run $($run.run) B2 formal / C1 mapping key setが一致しません"
    }
    $observedMapping=Need $run 'observed_physical_mapping_diagnostic'
    $observedMappingKeys=@{}
    foreach($record in @(Need $observedMapping 'records')){
        $key="$([int64](Need $record 'etw_sequence'))|$([int64](Need $record 'displayed_qpc'))"
        if($observedMappingKeys.ContainsKey($key)){Fail "run $($run.run) observed mapping keyが重複しています: $key"}
        $observedMappingKeys[$key]=$true
        if(-not[bool](Need $record 'mapping_exact')){Fail "run $($run.run) observed physical mappingがexactではありません: $key"}
    }
    if($observedMappingKeys.Count-ne$observedKeys.Count-or@($observedKeys.Keys|Where-Object{-not$observedMappingKeys.ContainsKey($_)}).Count-ne0){
        Fail "run $($run.run) observed / diagnostic mapping key setが一致しません"
    }
    $runObserved=$observedRecords.Count;$runFormal=$formalRecords.Count;$runNonformal=$runObserved-$runFormal
    $runInvalidNonformal=@($observedRecords|Where-Object{-not[bool]$_.in_b2_formal_presented_population-and-not[bool]$_.upstream_exact}).Count
    $runExactNonformal=@($observedRecords|Where-Object{-not[bool]$_.in_b2_formal_presented_population-and[bool]$_.upstream_exact}).Count
    $formalMembershipCount=@($observedRecords|Where-Object{[bool]$_.in_b2_formal_presented_population}).Count
    if([string](Need $population 'formal_membership_authority')-ne'B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'-or
       [bool](Need $population 'formal_membership_uses_upstream_exact')-or
       [bool](Need $population 'formal_membership_uses_display_relation')-or
       [bool](Need $population 'formal_membership_uses_measurement_membership')-or
       [bool](Need $population 'formal_membership_uses_source_frame')-or
       [bool](Need $population 'formal_membership_uses_qpc_heuristic')-or
       $runObserved-ne[int64](Need $population 'observed_presented_count')-or
       $runFormal-ne[int64](Need $population 'b2_formal_presented_count')-or
       $runFormal-ne[int64](Need $population 'c1_formal_input_count')-or$formalMembershipCount-ne$runFormal-or
       $runFormal-ne[int64](Need $run 'presented_candidate_count')-or$runFormal-ne[int64](Need $run 'mapped_exact_count')-or
       $runObserved-ne[int64](Need $observedMapping 'presented_candidate_count')-or
       $runObserved-ne[int64](Need $observedMapping 'mapped_exact_count')-or
       [int64](Need $observedMapping 'missing_mapping_count')-ne0-or
       [int64](Need $observedMapping 'ambiguous_mapping_count')-ne0-or
       [int64](Need $observedMapping 'duplicate_physical_ordinal_count')-ne0-or
       $runNonformal-ne[int64](Need $population 'nonformal_observed_presented_count')-or
       $runInvalidNonformal-ne[int64](Need $population 'upstream_invalid_nonformal_count')-or
       $runExactNonformal-ne[int64](Need $population 'upstream_exact_nonformal_count')-or
       [int64](Need $population 'b2_formal_missing_c0_candidate_count')-ne0-or
       [int64](Need $population 'b2_formal_ambiguous_c0_candidate_count')-ne0-or
       [int64](Need $population 'b2_formal_upstream_invalid_count')-ne0-or
       -not[bool](Need $population 'b2_formal_equals_c1_formal_count')-or
       -not[bool](Need $population 'b2_formal_key_set_equals_c1_formal_key_set')-or
       -not[bool](Need $population 'observed_population_identity_exact')-or-not[bool](Need $population 'authority_valid')){
        Fail "run $($run.run) formal population identityが不成立です"
    }
    $observed+=$runObserved;$formal+=$runFormal;$nonformal+=$runNonformal
    $invalidNonformal+=$runInvalidNonformal;$exactNonformal+=$runExactNonformal
}
foreach($identity in @(
    @('observed_presented_count',$observed),@('formal_presented_count',$formal),
    @('nonformal_observed_presented_count',$nonformal),@('upstream_invalid_nonformal_count',$invalidNonformal),
    @('upstream_exact_nonformal_count',$exactNonformal),@('presented_candidate_count',$formal),
    @('mapped_exact_count',$formal),@('observed_physical_mapped_exact_count',$observed))){
    if([int64](Need $proofObject $identity[0])-ne[int64]$identity[1]){Fail "W2-C1.3 aggregateが不正です: $($identity[0])"}
}
if($observed-ne($formal+$nonformal)-or$nonformal-ne($invalidNonformal+$exactNonformal)-or
   [int64](Need $proofObject 'observed_physical_missing_count')-ne0-or
   [int64](Need $proofObject 'observed_physical_ambiguous_count')-ne0-or
   [int64](Need $proofObject 'observed_physical_duplicate_ordinal_count')-ne0-or
   -not[bool](Need $proofObject 'formal_population_authority_valid')-or
   -not[bool](Need $proofObject 'mapping_exact')-or
   [string](Need $proofObject 'verdict')-ne'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT'){
    Fail 'W2-C1.3 aggregate authorityが不成立です'
}
Write-Output "P2-D5-2 W2-C1.4 sealed mapping replay checker: PASS ($formal/$observed formal/observed)"
