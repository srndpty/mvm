Set-StrictMode -Version Latest

# W2-D は closed authority を「並べ直す」段であり、性能判定を作る段ではない。
# したがってここでの仕事は 2 つだけである。
#   1. upstream checker を実際に再実行して authority を再確認する
#   2. sealed source から formal-v2 record を再構築し、cross-cohort splice を fail-close する
# aggregate をコピーしない。records から再計算する。

function Get-MvmDRequired($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){throw "W2-D必須fieldがありません: $Name"}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){throw "W2-D必須fieldがありません: $Name"}
    return $Object.$Name
}

function Get-MvmDFileSha256([string]$Path){
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-MvmDRunDirectory($C1ProofObject,[int]$Run){
    return Join-Path ([string](Get-MvmDRequired $C1ProofObject 'source_c011_directory')) "run-$Run"
}

# upstream authority の再実行。W2-D checker / runner の双方がこれを呼ぶ。
# 「hashが一致したから正しい」ではなく「checkerが今もPASSする」まで確認する。
function Invoke-MvmDUpstreamAuthorityReplay {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$C1ProofObject,
        [Parameter(Mandatory=$true)][string]$C1ProofPath,
        [Parameter(Mandatory=$true)][string]$C21ProofPath,
        [Parameter(Mandatory=$true)][string]$C2ProofPath,
        [Parameter(Mandatory=$true)][string]$C1Checker,
        [Parameter(Mandatory=$true)][string]$C21Checker,
        [Parameter(Mandatory=$true)][string]$C2Checker,
        [Parameter(Mandatory=$true)][string]$C24Checker,
        [Parameter(Mandatory=$true)][string]$W2AChecker,
        [Parameter(Mandatory=$true)][string]$B2Checker,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$WorkDirectory
    )
    foreach($path in @($C1ProofPath,$C21ProofPath,$C2ProofPath,$C1Checker,$C21Checker,$C2Checker,
                       $C24Checker,$W2AChecker,$B2Checker,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W2-D upstream authorityがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){New-Item -ItemType Directory -Path $WorkDirectory|Out-Null}
    foreach($replay in @(
        @('C1',$C1Checker,@('-Proof',$C1ProofPath)),
        @('C2.1',$C21Checker,@('-Proof',$C21ProofPath)),
        @('C2',$C2Checker,@('-Proof',$C2ProofPath)))){
        $checkerArguments=@($replay[2])
        & pwsh -NoProfile -File $replay[1] @checkerArguments *> $null
        if($LASTEXITCODE-ne0){throw "W2-Dがconsumeする$($replay[0]) authorityが不成立です"}
    }
    $c011Directory=[string](Get-MvmDRequired $C1ProofObject 'source_c011_directory')
    $c24Path=Join-Path $WorkDirectory 'w2-d-c24-formal-transport.json'
    & pwsh -NoProfile -File $C24Checker -C011Directory $c011Directory -Output $c24Path *> $null
    if($LASTEXITCODE-ne0){throw 'W2-DがconsumeするC2.4 formal transport authorityが不成立です'}
    $c24=Get-Content -LiteralPath $c24Path -Raw -Encoding utf8|ConvertFrom-Json
    if(-not[bool](Get-MvmDRequired $c24 'policy_exact')){throw 'C2.4 formal transport policyがexactではありません'}
    $runs=@()
    foreach($c1Run in @(Get-MvmDRequired $C1ProofObject 'runs')){
        $run=[int](Get-MvmDRequired $c1Run 'run')
        $runDirectory=Get-MvmDRunDirectory $C1ProofObject $run
        $appPath=Join-Path $runDirectory 'traced-app.json'
        $rawPath=Join-Path $runDirectory 'present-history-raw.json'
        $terminalPath=Join-Path $runDirectory 'terminal-shadow.json'
        foreach($path in @($appPath,$rawPath,$terminalPath)){
            if(-not(Test-Path -LiteralPath $path)){throw "run $run sealed sourceがありません: $path"}
        }
        # W2-A / W2-A.1 physical VBlank domain。AUTHORITY_INVALID も exit 0 なので
        # 出力の status まで見る。
        $w2aPath=Join-Path $WorkDirectory "w2-d-w2a-run-$run.json"
        & pwsh -NoProfile -File $W2AChecker -Json $appPath -Output $w2aPath *> $null
        if($LASTEXITCODE-ne0){throw "run $run W2-A physical domain authorityが不成立です"}
        $w2a=Get-Content -LiteralPath $w2aPath -Raw -Encoding utf8|ConvertFrom-Json
        if([string](Get-MvmDRequired $w2a 'status')-ne'PASS'-or
           -not[bool](Get-MvmDRequired $w2a 'domain_evaluated')){
            throw "run $run W2-A physical domainがPASSしていません: $([string](Get-MvmDRequired $w2a 'status'))"
        }
        # B2 は内部で B1 を再実行する。sealed terminal-shadow.json と一致することまで確認する。
        $b2Path=Join-Path $WorkDirectory "w2-d-b2-run-$run.json"
        & pwsh -NoProfile -File $B2Checker -AppJson $appPath -EtwJson $rawPath -Output $b2Path `
            -SourceRoot $SourceRoot -CandidateLedger $terminalPath *> $null
        if($LASTEXITCODE-ne0){throw "run $run W2-B1/B2 transport authorityが不成立です"}
        $runs+=,[ordered]@{
            run=$run
            physical_vblank_opportunity_count=[int64](Get-MvmDRequired $w2a 'physical_opportunity_count')
            physical_domain_origin_ordinal=[int64](Get-MvmDRequired $w2a 'origin_ordinal')
            physical_domain_last_ordinal=[int64](Get-MvmDRequired $w2a 'last_ordinal')
        }
    }
    return [ordered]@{
        c24_policy_exact=$true
        c24_producer_record_count=[int64](Get-MvmDRequired $c24 'producer_record_count')
        c24_transport_eligible_count=[int64](Get-MvmDRequired $c24 'checker_derived_transport_eligible_count')
        runs=$runs
    }
}

function Invoke-MvmDFormalV2ProofFromSealedAuthorities {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$C1ProofObject,
        [Parameter(Mandatory=$true)][string]$C1ProofPath,
        [Parameter(Mandatory=$true)]$C21ProofObject,
        [Parameter(Mandatory=$true)][string]$C21ProofPath,
        [Parameter(Mandatory=$true)]$C2ProofObject,
        [Parameter(Mandatory=$true)][string]$C2ProofPath,
        [Parameter(Mandatory=$true)]$UpstreamReplay,
        [Parameter(Mandatory=$true)][string]$C1CheckpointSha
    )
    # --- upstream verdict ---
    if(-not[bool](Get-MvmDRequired $C1ProofObject 'mapping_exact')-or
       [string](Get-MvmDRequired $C1ProofObject 'verdict')-ne'DISPLAYED_QPC_PHYSICAL_VBLANK_ORDINAL_EXACT'){
        throw 'C1 sealed mapping authorityがexactではありません'
    }
    if(-not[bool](Get-MvmDRequired $C21ProofObject 'authority_exact')){throw 'C2.1 required intent authorityがexactではありません'}
    if(-not[bool](Get-MvmDRequired $C2ProofObject 'ledger_exact')){throw 'C2 intent satisfaction ledgerがexactではありません'}

    # --- cross-cohort splice fail-close ---
    $c1Hash=Get-MvmDFileSha256 $C1ProofPath
    $c21Hash=Get-MvmDFileSha256 $C21ProofPath
    $c2Hash=Get-MvmDFileSha256 $C2ProofPath
    if([string](Get-MvmDRequired $C21ProofObject 'source_c1_proof_sha256')-ne$c1Hash){
        throw 'C2.1が別C1 cohortを参照しています'
    }
    if([string](Get-MvmDRequired $C2ProofObject 'source_c1_proof_sha256')-ne$c1Hash){
        throw 'C2が別C1 cohortを参照しています'
    }
    if([string](Get-MvmDRequired $C2ProofObject 'source_c21_proof_sha256')-ne$c21Hash){
        throw 'C2が別C2.1 required intent authorityを参照しています'
    }
    foreach($checkpoint in @(
        @('C2.1',[string](Get-MvmDRequired $C21ProofObject 'c1_checkpoint_sha')),
        @('C2',[string](Get-MvmDRequired $C2ProofObject 'c1_checkpoint_sha')))){
        if($checkpoint[1]-ne$C1CheckpointSha){throw "$($checkpoint[0])のC1 checkpointが一致しません"}
    }
    $c1Runs=@(Get-MvmDRequired $C1ProofObject 'runs')
    $c21Runs=@(Get-MvmDRequired $C21ProofObject 'runs')
    $c2Runs=@(Get-MvmDRequired $C2ProofObject 'runs')
    $replayRuns=@(Get-MvmDRequired $UpstreamReplay 'runs')
    if($c21Runs.Count-ne$c1Runs.Count-or$c2Runs.Count-ne$c1Runs.Count-or$replayRuns.Count-ne$c1Runs.Count){
        throw 'C1 / C2.1 / C2 / physical domain replayのrun countが一致しません'
    }

    $upstreamPath=[string](Get-MvmDRequired $C1ProofObject 'source_upstream_inventory_proof')
    $upstream=Get-Content -LiteralPath $upstreamPath -Raw -Encoding utf8|ConvertFrom-Json
    $upstreamHash=Get-MvmDFileSha256 $upstreamPath
    if([string](Get-MvmDRequired $C1ProofObject 'upstream_inventory_proof_sha256')-ne$upstreamHash){
        throw 'sealed C0 inventory hashが一致しません'
    }

    $runResults=@();$globalBlockers=@{}
    for($runIndex=0;$runIndex-lt$c1Runs.Count;++$runIndex){
        $c1Run=$c1Runs[$runIndex];$c21Run=$c21Runs[$runIndex];$c2Run=$c2Runs[$runIndex]
        $replayRun=$replayRuns[$runIndex]
        $run=[int](Get-MvmDRequired $c1Run 'run')
        foreach($alignment in @(
            @('C2.1',[int](Get-MvmDRequired $c21Run 'run')),
            @('C2',[int](Get-MvmDRequired $c2Run 'run')),
            @('W2-A',[int](Get-MvmDRequired $replayRun 'run')))){
            if($alignment[1]-ne$run){throw "run $run の$($alignment[0]) run identityが一致しません"}
        }
        $runDirectory=Get-MvmDRunDirectory $C1ProofObject $run
        $appPath=Join-Path $runDirectory 'traced-app.json'
        $rawPath=Join-Path $runDirectory 'present-history-raw.json'
        $terminalPath=Join-Path $runDirectory 'terminal-shadow.json'
        $sealed=[ordered]@{
            traced_app=Get-MvmDFileSha256 $appPath
            present_history_raw=Get-MvmDFileSha256 $rawPath
            b2_terminal_shadow=Get-MvmDFileSha256 $terminalPath
            upstream_inventory_proof=$upstreamHash
        }
        # 同一 fresh cohort であること。C1 / C2 / C2.1 が別 run の sealed source を
        # 見ていれば、ここで hash が割れる。
        $c1Sealed=Get-MvmDRequired $c1Run 'sealed_input_sha256'
        $c2Sealed=Get-MvmDRequired $c2Run 'sealed_source_sha256'
        foreach($field in @('traced_app','present_history_raw','b2_terminal_shadow','upstream_inventory_proof')){
            if([string](Get-MvmDRequired $c1Sealed $field)-ne[string]$sealed[$field]){
                throw "run $run C1 sealed source hashが一致しません: $field"
            }
            if([string](Get-MvmDRequired $c2Sealed $field)-ne[string]$sealed[$field]){
                throw "run $run C2 sealed source hashが一致しません: $field"
            }
        }
        if([string](Get-MvmDRequired $c21Run 'sealed_traced_app_sha256')-ne[string]$sealed['traced_app']){
            throw "run $run C2.1 sealed traced app hashが一致しません"
        }

        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
        $terminal=Get-Content -LiteralPath $terminalPath -Raw -Encoding utf8|ConvertFrom-Json
        $opportunity=Get-MvmDRequired $app 'presentation_opportunity'
        $physical=Get-MvmDRequired $opportunity 'physical_vblank'
        $shadow=Get-MvmDRequired $opportunity 'physical_vblank_domain_shadow'
        $support=Get-MvmDRequired $opportunity 'physical_mapping_support_envelope_shadow'
        $physicalCount=[int64](Get-MvmDRequired $shadow 'physical_opportunity_count')
        $originOrdinal=[int64](Get-MvmDRequired $shadow 'origin_ordinal')
        $lastOrdinal=[int64](Get-MvmDRequired $shadow 'last_ordinal')
        # W2-A checker が同じ run artifact から読んだ Layer 1B domain と一致すること。
        foreach($domainField in @(
            @('physical_vblank_opportunity_count',$physicalCount),
            @('physical_domain_origin_ordinal',$originOrdinal),
            @('physical_domain_last_ordinal',$lastOrdinal))){
            if([int64](Get-MvmDRequired $replayRun $domainField[0])-ne[int64]$domainField[1]){
                throw "run $run W2-A physical domainが別runのものです: $($domainField[0])"
            }
        }

        # freeze 済み formal Presented 母集団と physical mapping を sealed source から再生する。
        $population=Invoke-MvmC13FormalPresentedPopulation -ObservedCandidates @($upstream.runs[$runIndex].candidates) `
            -B2TerminalRecords @(Get-MvmDRequired $terminal 'records')
        if(-not[bool]$population.authority_valid){throw "run $run sealed C1 formal populationを再生できません"}
        $replayed=Invoke-MvmDisplayedQpcPhysicalMapping -Candidates @($population.formal_candidates) `
            -Samples @(Get-MvmDRequired $physical 'samples') `
            -PredecessorOrdinal ([int64](Get-MvmDRequired $support 'predecessor_ordinal')) `
            -SuccessorOrdinal ([int64](Get-MvmDRequired $support 'successor_ordinal')) `
            -OriginOrdinal $originOrdinal -LastOrdinal $lastOrdinal `
            -PhysicalAuthorityValid ([bool](Get-MvmDRequired $shadow 'shadow_authority_valid')-and
                [bool](Get-MvmDRequired $support 'authority_valid')) `
            -EtwEventsLost ([int64](Get-MvmDRequired $raw 'etw_events_lost')) `
            -EtwBuffersLost ([int64](Get-MvmDRequired $raw 'etw_buffers_lost')) `
            -PresentEventOverflowCount ([int64](Get-MvmDRequired $raw 'present_event_overflow_count')) `
            -RequireAllCandidatesInsideSupport $true
        if(-not[bool]$replayed.mapping_exact){throw "run $run sealed C1 physical mappingを再生できません"}

        # B2 terminal 側の FinalState / transport identity。identity key は
        # exact_event_key であり、source frame は使わない。
        $terminalByKey=@{}
        foreach($terminalRecord in @($terminal.records|Where-Object{
            [string]$_.final_state-eq'Presented'-and[bool](Get-MvmDRequired $_ 'formal_transport_eligible')})){
            $displayed=@(Get-MvmDRequired $terminalRecord 'displayed_qpc')
            if($displayed.Count-ne1){throw "run $run B2 DisplayedQPC cardinalityが不正です"}
            $key="$([int64](Get-MvmDRequired $terminalRecord 'etw_sequence'))|$([int64]$displayed[0])"
            if($terminalByKey.ContainsKey($key)){throw "run $run B2 terminal keyが重複しています: $key"}
            $terminalByKey[$key]=$terminalRecord
        }
        $formalByKey=@{}
        foreach($formalRecord in @($population.formal_records)){
            $key=[string](Get-MvmDRequired $formalRecord 'exact_event_key')
            if($formalByKey.ContainsKey($key)){throw "run $run formal event keyが重複しています: $key"}
            $formalByKey[$key]=$formalRecord
        }

        $integrationInput=@()
        foreach($mappingRecord in @($replayed.records)){
            $key="$([int64](Get-MvmDRequired $mappingRecord 'etw_sequence'))|$([int64](Get-MvmDRequired $mappingRecord 'displayed_qpc'))"
            if(-not$formalByKey.ContainsKey($key)){throw "run $run C1 formal populationにkeyがありません: $key"}
            if(-not$terminalByKey.ContainsKey($key)){throw "run $run B2 terminal outcomeにkeyがありません: $key"}
            $terminalRecord=$terminalByKey[$key]
            # transport chain の同一性。C1 candidate 側と B2 terminal 側で
            # token / native Present serial が割れていれば splice である。
            foreach($chain in @(
                @('composition_token_serial','embedded_token_serial'),
                @('native_present_serial','native_present_serial'))){
                $mappingValue=[string](Get-MvmDRequired $mappingRecord $chain[0])
                $terminalValue=[string](Get-MvmDRequired $terminalRecord $chain[1])
                if($mappingValue-ne$terminalValue){
                    throw "run $run transport chainが一致しません: $key / $($chain[0])"
                }
            }
            $integrationInput+=[pscustomobject][ordered]@{
                exact_event_key=$key
                intent_ordinal=Get-MvmDRequired $mappingRecord 'intent_ordinal'
                intent_scope=Get-MvmDRequired $mappingRecord 'intent_scope'
                composition_token_serial=Get-MvmDRequired $terminalRecord 'embedded_token_serial'
                native_present_serial=Get-MvmDRequired $terminalRecord 'native_present_serial'
                etw_sequence=[int64](Get-MvmDRequired $mappingRecord 'etw_sequence')
                final_state=Get-MvmDRequired $terminalRecord 'final_state'
                displayed_qpc=[int64](Get-MvmDRequired $mappingRecord 'displayed_qpc')
                physical_vblank_ordinal=Get-MvmDRequired $mappingRecord 'physical_vblank_ordinal'
                in_measurement_physical_domain=[bool](Get-MvmDRequired $mappingRecord 'in_measurement_physical_domain')
            }
        }

        $requiredOrdinals=@(Get-MvmDRequired $c21Run 'required_scheduler_intent_ordinals')
        if($requiredOrdinals.Count-ne[int](Get-MvmDRequired $c21Run 'required_scheduler_intent_set_cardinality')){
            throw "run $run C2.1 required intent set cardinalityが一致しません"
        }
        $integration=Invoke-MvmDFormalV2ShadowIntegration -FormalPresentedRecords $integrationInput `
            -RequiredIntentOrdinals $requiredOrdinals -PhysicalOpportunityCount $physicalCount `
            -PhysicalDomainOriginOrdinal $originOrdinal -PhysicalDomainLastOrdinal $lastOrdinal
        $integration.run=$run
        # C2 ledger と同じ数になること。W2-D は C2 の値をコピーせず独立に再計算し、
        # 一致しなければ integration を fail-close する。
        $ledgerAgreement=$true
        foreach($agreement in @(
            @('required_intent_count','required_current_intent_count'),
            @('satisfied_intent_count','satisfied_intent_count'),
            @('in_domain_presented_event_count','in_domain_presented_event_count'),
            @('in_domain_presented_foreign_intent_count','in_domain_presented_foreign_intent_count'),
            @('filled_physical_opportunity_count','filled_physical_opportunity_count'),
            @('formal_presented_event_count','formal_presented_event_count'))){
            if([int64]$integration[$agreement[0]]-ne[int64](Get-MvmDRequired $c2Run $agreement[1])){$ledgerAgreement=$false}
        }
        if(-not$ledgerAgreement){
            $integration.blockers=@(@($integration.blockers)+'C2_LEDGER_INTEGRATION_DIVERGENCE'|Sort-Object)
            $integration.integration_exact=$false
        }
        $integration.c2_ledger_agreement_exact=$ledgerAgreement
        $integration.sealed_source_sha256=$sealed
        foreach($blocker in @($integration.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runResults+=,$integration
    }

    $required=0L;$satisfied=0L;$unsatisfied=0L;$formal=0L;$inDomain=0L;$foreign=0L
    $physicalOpportunity=0L;$filled=0L
    foreach($runResult in $runResults){
        $required+=[int64]$runResult.required_intent_count
        $satisfied+=[int64]$runResult.satisfied_intent_count
        $unsatisfied+=[int64]$runResult.unsatisfied_intent_count
        $formal+=[int64]$runResult.formal_presented_event_count
        $inDomain+=[int64]$runResult.in_domain_presented_event_count
        $foreign+=[int64]$runResult.in_domain_presented_foreign_intent_count
        $physicalOpportunity+=[int64]$runResult.physical_vblank_opportunity_count
        $filled+=[int64]$runResult.filled_physical_opportunity_count
    }
    $blockerList=@($globalBlockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-d-formal-v2-shadow-1';stage='P2-D5-2-W2-D'
        integration_authority='W2A_B1_B2_C1_C21_C2_CLOSED_AUTHORITY_REPLAY'
        c1_checkpoint_sha=$C1CheckpointSha
        source_c011_directory=[string](Get-MvmDRequired $C1ProofObject 'source_c011_directory')
        source_c1_proof=(Resolve-Path -LiteralPath $C1ProofPath).Path
        source_c1_proof_sha256=$c1Hash
        source_c21_proof=(Resolve-Path -LiteralPath $C21ProofPath).Path
        source_c21_proof_sha256=$c21Hash
        source_c2_proof=(Resolve-Path -LiteralPath $C2ProofPath).Path
        source_c2_proof_sha256=$c2Hash
        source_upstream_inventory_proof=$upstreamPath
        source_upstream_inventory_proof_sha256=$upstreamHash
        c24_policy_exact=[bool](Get-MvmDRequired $UpstreamReplay 'c24_policy_exact')
        c24_producer_record_count=[int64](Get-MvmDRequired $UpstreamReplay 'c24_producer_record_count')
        c24_transport_eligible_count=[int64](Get-MvmDRequired $UpstreamReplay 'c24_transport_eligible_count')
        run_count=$runResults.Count
        required_intent_count=$required
        satisfied_intent_count=$satisfied
        unsatisfied_intent_count=$unsatisfied
        formal_presented_event_count=$formal
        in_domain_presented_event_count=$inDomain
        in_domain_presented_foreign_intent_count=$foreign
        physical_vblank_opportunity_count=$physicalOpportunity
        filled_physical_opportunity_count=$filled
        layer1a_required_accounting_identity_exact=@($runResults|Where-Object{-not[bool]$_.layer1a_required_accounting_identity_exact}).Count-eq0
        presented_accounting_identity_exact=@($runResults|Where-Object{-not[bool]$_.presented_accounting_identity_exact}).Count-eq0
        filled_physical_opportunity_identity_exact=@($runResults|Where-Object{-not[bool]$_.filled_physical_opportunity_identity_exact}).Count-eq0
        physical_vblank_domain_cardinality_exact=@($runResults|Where-Object{-not[bool]$_.physical_vblank_domain_cardinality_exact}).Count-eq0
        c2_ledger_agreement_exact=@($runResults|Where-Object{-not[bool]$_.c2_ledger_agreement_exact}).Count-eq0
        # Layer 1A と Layer 1B は別母集団である。差は INVALID でも performance FAIL でもない。
        layer1a_layer1b_count_difference_is_not_a_verdict=$true
        source_frame_identity_used=$false
        nearest_qpc_or_tolerance_used=$false
        shadow_only=$true
        canonical_authority=$false
        performance_threshold_evaluated=$false
        canonical_verdict_evaluated=$false
        frame_swapped_retirement_changed=$false
        integration_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runResults
        verdict=$(if($blockerList.Count-eq0){'FORMAL_V2_SHADOW_INTEGRATION_EXACT'}else{'FORMAL_V2_SHADOW_INTEGRATION_INVALID'})
    }
}

function Assert-MvmDFormalV2Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 16 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 16 -Compress
    if($expectedJson-ne$actualJson){throw 'W2-D artifactがsealed authorityからの再構築結果と一致しません'}
}
