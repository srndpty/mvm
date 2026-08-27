Set-StrictMode -Version Latest

function Get-MvmC2Required($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){throw "C2必須fieldがありません: $Name"}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){throw "C2必須fieldがありません: $Name"}
    return $Object.$Name
}

function Invoke-MvmC2ProofFromSealedC1 {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$C1ProofObject,
        [Parameter(Mandatory=$true)][string]$C1ProofPath,
        [Parameter(Mandatory=$true)]$C21ProofObject,
        [Parameter(Mandatory=$true)][string]$C21ProofPath,
        [Parameter(Mandatory=$true)][string]$C1CheckpointSha
    )
    if(-not[bool](Get-MvmC2Required $C21ProofObject 'authority_exact')){
        throw 'C2.1 required intent authorityがexactではありません'
    }
    $c1Hash=(Get-FileHash -LiteralPath $C1ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if([string](Get-MvmC2Required $C21ProofObject 'source_c1_proof_sha256')-ne$c1Hash){
        throw 'C2.1が参照するC1 proofが一致しません'
    }
    $sourceDirectory=[string](Get-MvmC2Required $C1ProofObject 'source_c011_directory')
    $upstreamPath=[string](Get-MvmC2Required $C1ProofObject 'source_upstream_inventory_proof')
    $upstream=Get-Content -LiteralPath $upstreamPath -Raw -Encoding utf8|ConvertFrom-Json
    $runResults=@();$globalBlockers=@{}
    $proofRuns=@(Get-MvmC2Required $C1ProofObject 'runs')
    $c21Runs=@(Get-MvmC2Required $C21ProofObject 'runs')
    if($c21Runs.Count-ne$proofRuns.Count){throw 'C1 / C2.1 run countが一致しません'}
    for($runIndex=0;$runIndex-lt$proofRuns.Count;++$runIndex){
        $c1Run=$proofRuns[$runIndex];$run=[int](Get-MvmC2Required $c1Run 'run')
        $c21Run=$c21Runs[$runIndex]
        if([int](Get-MvmC2Required $c21Run 'run')-ne$run-or
           -not[bool](Get-MvmC2Required $c21Run 'authority_exact')-or
           -not[bool](Get-MvmC2Required $c21Run 'required_scheduler_intent_set_exact')){
            throw "run $run C2.1 required intent authorityが不成立です"
        }
        $runDirectory=Join-Path $sourceDirectory "run-$run"
        $appPath=Join-Path $runDirectory 'traced-app.json'
        $rawPath=Join-Path $runDirectory 'present-history-raw.json'
        $terminalPath=Join-Path $runDirectory 'terminal-shadow.json'
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
        $terminal=Get-Content -LiteralPath $terminalPath -Raw -Encoding utf8|ConvertFrom-Json
        $observedCandidates=@($upstream.runs[$runIndex].candidates)
        $population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates $observedCandidates `
            -B2TerminalRecords @(Get-MvmC2Required $terminal 'records')
        if(-not[bool]$population.authority_valid){throw "run $run sealed C1 formal populationを再生できません"}
        $opportunity=Get-MvmC2Required $app 'presentation_opportunity'
        $physical=Get-MvmC2Required $opportunity 'physical_vblank'
        $shadow=Get-MvmC2Required $opportunity 'physical_vblank_domain_shadow'
        $support=Get-MvmC2Required $opportunity 'physical_mapping_support_envelope_shadow'
        $replayed=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates @($population.formal_candidates) `
            -Samples @(Get-MvmC2Required $physical 'samples') `
            -PredecessorOrdinal ([int64](Get-MvmC2Required $support 'predecessor_ordinal')) `
            -SuccessorOrdinal ([int64](Get-MvmC2Required $support 'successor_ordinal')) `
            -OriginOrdinal ([int64](Get-MvmC2Required $shadow 'origin_ordinal')) `
            -LastOrdinal ([int64](Get-MvmC2Required $shadow 'last_ordinal')) `
            -PhysicalAuthorityValid ([bool](Get-MvmC2Required $shadow 'shadow_authority_valid')-and
                [bool](Get-MvmC2Required $support 'authority_valid')) `
            -EtwEventsLost ([int64](Get-MvmC2Required $raw 'etw_events_lost')) `
            -EtwBuffersLost ([int64](Get-MvmC2Required $raw 'etw_buffers_lost')) `
            -PresentEventOverflowCount ([int64](Get-MvmC2Required $raw 'present_event_overflow_count')) `
            -RequireAllCandidatesInsideSupport $true
        if(-not[bool]$replayed.mapping_exact){throw "run $run sealed C1 physical mappingを再生できません"}
        $formalByKey=@{}
        foreach($formalRecord in @($population.formal_records)){
            $key=[string](Get-MvmC2Required $formalRecord 'exact_event_key')
            if($formalByKey.ContainsKey($key)){throw "run $run formal event keyが重複しています: $key"}
            $formalByKey[$key]=$formalRecord
        }
        $c2Input=@()
        foreach($mappingRecord in @($replayed.records)){
            $key="$([int64](Get-MvmC2Required $mappingRecord 'etw_sequence'))|$([int64](Get-MvmC2Required $mappingRecord 'displayed_qpc'))"
            if(-not$formalByKey.ContainsKey($key)){throw "run $run C1 replay joinが欠損しています: $key"}
            $formalRecord=$formalByKey[$key]
            $ordinal=Get-MvmC2Required $mappingRecord 'intent_ordinal'
            $c2Input+=[pscustomobject][ordered]@{
                exact_event_key=$key
                etw_sequence=[int64](Get-MvmC2Required $mappingRecord 'etw_sequence')
                intent_scope=Get-MvmC2Required $mappingRecord 'intent_scope'
                intent_scope_exact=[bool](Get-MvmC2Required $formalRecord 'intent_scope_exact')
                intent_ordinal=$ordinal
                intent_ordinal_valid=$null-ne$ordinal-and-not[string]::IsNullOrWhiteSpace([string]$ordinal)
                intent_ordinal_exact=[bool](Get-MvmC2Required $formalRecord 'intent_exact')
                physical_vblank_ordinal=Get-MvmC2Required $mappingRecord 'physical_vblank_ordinal'
                in_measurement_physical_domain=[bool](Get-MvmC2Required $mappingRecord 'in_measurement_physical_domain')
            }
        }
        $requiredOrdinals=@(Get-MvmC2Required $c21Run 'required_scheduler_intent_ordinals')
        if($requiredOrdinals.Count-ne[int](Get-MvmC2Required $c21Run 'required_scheduler_intent_set_cardinality')){
            throw "run $run C2.1 required intent set cardinalityが一致しません"
        }
        $ledger=Invoke-MvmC2IntentSatisfactionLedger -FormalPresentedRecords $c2Input `
            -RequiredCurrentIntentOrdinals $requiredOrdinals
        $ledger.run=$run
        $ledger.layer1a_required_intent_count_source='C2.1:required_scheduler_intent_ordinals'
        $ledger.layer1a_required_intent_domain_rule='EXACT_SET_MEMBERSHIP'
        $ledger.source_c21_run=$run
        $ledger.required_domain_derived_from_presented_min_max=$false
        $ledger.sealed_source_sha256=[ordered]@{
            traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
            present_history_raw=(Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash.ToLowerInvariant()
            b2_terminal_shadow=(Get-FileHash -LiteralPath $terminalPath -Algorithm SHA256).Hash.ToLowerInvariant()
            upstream_inventory_proof=(Get-FileHash -LiteralPath $upstreamPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        foreach($blocker in @($ledger.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runResults+=,$ledger
    }
    $required=0L;$formal=0L;$inDomain=0L;$satisfied=0L;$foreign=0L;$filled=0L
    $duplicate=0L;$outside=0L;$missing=0L;$ambiguous=0L;$multiPhysical=0L
    foreach($runResult in $runResults){
        $required+=[int64]$runResult.required_current_intent_count
        $formal+=[int64]$runResult.formal_presented_event_count
        $inDomain+=[int64]$runResult.in_domain_presented_event_count
        $satisfied+=[int64]$runResult.satisfied_intent_count
        $foreign+=[int64]$runResult.in_domain_presented_foreign_intent_count
        $filled+=[int64]$runResult.filled_physical_opportunity_count
        $duplicate+=[int64]$runResult.duplicate_current_intent_satisfaction_count
        $outside+=[int64]$runResult.current_intent_outside_required_domain_count
        $missing+=[int64]$runResult.missing_intent_provenance_count
        $ambiguous+=[int64]$runResult.ambiguous_intent_provenance_count
        $multiPhysical+=[int64]$runResult.multiple_formal_presented_per_physical_ordinal_count
    }
    $blockerList=@($globalBlockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c2-intent-satisfaction-ledger-1';stage='P2-D5-2-W2-C2'
        c1_checkpoint_sha=$C1CheckpointSha
        source_c1_proof=(Resolve-Path -LiteralPath $C1ProofPath).Path
        source_c1_proof_sha256=$c1Hash
        source_c21_proof=(Resolve-Path -LiteralPath $C21ProofPath).Path
        source_c21_proof_sha256=(Get-FileHash -LiteralPath $C21ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        c1_authority_consumed='C1_SEALED_FORMAL_POPULATION_AND_PHYSICAL_MAPPING_REPLAY'
        formal_presented_population='C1_FORMAL_PRESENTED_ONLY'
        observed_diagnostic_population_consumed=$false
        required_intent_authority='C2.1_EXACT_SCHEDULER_REQUIRED_INTENT_SET'
        required_domain_derived_from_presented_min_max=$false
        source_frame_identity_used=$false
        performance_threshold_evaluated=$false
        canonical_verdict_evaluated=$false
        frame_swapped_retirement_changed=$false
        run_count=$runResults.Count;required_current_intent_count=$required
        formal_presented_event_count=$formal
        in_domain_presented_event_count=$inDomain;satisfied_intent_count=$satisfied
        in_domain_presented_foreign_intent_count=$foreign
        filled_physical_opportunity_count=$filled
        duplicate_current_intent_satisfaction_count=$duplicate
        current_intent_outside_required_domain_count=$outside
        missing_intent_provenance_count=$missing;ambiguous_intent_provenance_count=$ambiguous
        multiple_formal_presented_per_physical_ordinal_count=$multiPhysical
        satisfaction_accounting_identity_exact=@($runResults|Where-Object{-not[bool]$_.satisfaction_accounting_identity_exact}).Count-eq0
        physical_fill_unique_ordinal_identity_exact=@($runResults|Where-Object{-not[bool]$_.physical_fill_unique_ordinal_identity_exact}).Count-eq0
        c1_one_presented_per_ordinal_derived_identity_exact=@($runResults|Where-Object{-not[bool]$_.c1_one_presented_per_ordinal_derived_identity_exact}).Count-eq0
        ledger_exact=$blockerList.Count-eq0;blockers=$blockerList;runs=$runResults
        verdict=$(if($blockerList.Count-eq0){'INTENT_SATISFACTION_LEDGER_EXACT'}else{'INTENT_SATISFACTION_LEDGER_INVALID'})
    }
}

function Assert-MvmC2Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $fields=@('schema','stage','c1_checkpoint_sha','source_c1_proof_sha256','source_c21_proof_sha256','c1_authority_consumed',
        'formal_presented_population','observed_diagnostic_population_consumed','required_intent_authority',
        'required_domain_derived_from_presented_min_max','source_frame_identity_used',
        'performance_threshold_evaluated','canonical_verdict_evaluated','frame_swapped_retirement_changed',
        'run_count','required_current_intent_count','formal_presented_event_count',
        'in_domain_presented_event_count','satisfied_intent_count',
        'in_domain_presented_foreign_intent_count','filled_physical_opportunity_count',
        'duplicate_current_intent_satisfaction_count','current_intent_outside_required_domain_count',
        'missing_intent_provenance_count','ambiguous_intent_provenance_count',
        'multiple_formal_presented_per_physical_ordinal_count','satisfaction_accounting_identity_exact',
        'physical_fill_unique_ordinal_identity_exact','c1_one_presented_per_ordinal_derived_identity_exact',
        'ledger_exact','verdict')
    foreach($field in $fields){
        if([string](Get-MvmC2Property $Expected $field)-ne[string](Get-MvmC2Property $Actual $field)){
            throw "C2 proof aggregateが再集計値と一致しません: $field"
        }
    }
    if((@($Expected.blockers)-join',')-ne(@($Actual.blockers)-join',')){throw 'C2 proof blocker集合が一致しません'}
    $expectedRuns=@($Expected.runs);$actualRuns=@($Actual.runs)
    if($expectedRuns.Count-ne$actualRuns.Count){throw 'C2 proof run数が一致しません'}
    for($index=0;$index-lt$expectedRuns.Count;++$index){
        foreach($field in @('run','source_c21_run','layer1a_required_intent_count_source','layer1a_required_intent_domain_rule',
            'required_domain_derived_from_presented_min_max')){
            if([string](Get-MvmC2Property $expectedRuns[$index] $field)-ne
               [string](Get-MvmC2Property $actualRuns[$index] $field)){
                throw "C2 run authority fieldが一致しません: run=$index field=$field"
            }
        }
        $expectedHashes=Get-MvmC2Property $expectedRuns[$index] 'sealed_source_sha256'
        $actualHashes=Get-MvmC2Property $actualRuns[$index] 'sealed_source_sha256'
        foreach($field in @('traced_app','present_history_raw','b2_terminal_shadow','upstream_inventory_proof')){
            if([string](Get-MvmC2Property $expectedHashes $field)-ne[string](Get-MvmC2Property $actualHashes $field)){
                throw "C2 sealed source hashが一致しません: run=$index field=$field"
            }
        }
        Assert-MvmC2IntentSatisfactionLedger -Expected $expectedRuns[$index] -Actual $actualRuns[$index]
    }
}
