Set-StrictMode -Version Latest

function Invoke-MvmC21ProofFromSealedC1 {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$C1ProofObject,
        [Parameter(Mandatory=$true)][string]$C1ProofPath,
        [Parameter(Mandatory=$true)][string]$C1CheckpointSha,
        [string[]]$DiagnosticIntentOrdinals=@('0','301')
    )
    $sourceDirectory=[string](Get-MvmC21Value $C1ProofObject 'source_c011_directory')
    $upstreamPath=[string](Get-MvmC21Value $C1ProofObject 'source_upstream_inventory_proof')
    $upstream=Get-Content -LiteralPath $upstreamPath -Raw -Encoding utf8|ConvertFrom-Json
    $runs=@();$globalBlockers=@{}
    $c1Runs=@(Get-MvmC21Value $C1ProofObject 'runs')
    for($runIndex=0;$runIndex-lt$c1Runs.Count;++$runIndex){
        $run=[int](Get-MvmC21Value $c1Runs[$runIndex] 'run')
        $runDirectory=Join-Path $sourceDirectory "run-$run"
        $appPath=Join-Path $runDirectory 'traced-app.json'
        $terminalPath=Join-Path $runDirectory 'terminal-shadow.json'
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $terminal=Get-Content -LiteralPath $terminalPath -Raw -Encoding utf8|ConvertFrom-Json
        $population=Invoke-MvmC13FormalPresentedPopulation `
            -ObservedCandidates @($upstream.runs[$runIndex].candidates) `
            -B2TerminalRecords @($terminal.records)
        if(-not[bool]$population.authority_valid){throw "run $run C1 formal Presentedを再生できません"}
        $hook=Get-MvmC21Value $app 'native_present_hook'
        $scope=Get-MvmC21Value $hook 'intent_scope_provenance'
        if(-not[bool](Get-MvmC21Value $scope 'authority_pass')){throw "run $run scheduler decision scope authorityが不成立です"}
        $inventory=Invoke-MvmC21RunInventory -IntentScopeAuthority $scope `
            -NativePresentRecords @(Get-MvmC21Value $hook 'records') `
            -FormalPresentedCandidates @($population.formal_candidates) `
            -RequiredIntentCount ([int64](Get-MvmC21Value $app 'required_measurement_frame_count')) `
            -CaptureEnvelope (Get-MvmC21Value $hook 'capture_envelope') `
            -DiagnosticIntentOrdinals $DiagnosticIntentOrdinals
        $inventory.run=$run
        $inventory.sealed_traced_app_sha256=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
        foreach($blocker in @($inventory.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runs+=,$inventory
    }
    $blockerList=@($globalBlockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w2-c21-required-intent-domain-authority-1'
        stage='P2-D5-2-W2-C2.1'
        c1_checkpoint_sha=$C1CheckpointSha
        source_c1_proof=(Resolve-Path -LiteralPath $C1ProofPath).Path
        source_c1_proof_sha256=(Get-FileHash -LiteralPath $C1ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_upstream_inventory_sha256=(Get-FileHash -LiteralPath $upstreamPath -Algorithm SHA256).Hash.ToLowerInvariant()
        input_authority='C1_SEALED_FORMAL_PRESENTED_PLUS_SCHEDULER_PRODUCER_SCOPE_LEDGER'
        run_count=$runs.Count
        required_intent_identity_authority_exact=@($runs|Where-Object{-not[bool]$_.required_scheduler_intent_set_exact}).Count-eq0
        required_count_set_identity_exact=@($runs|Where-Object{-not[bool]$_.required_count_equals_exact_set_cardinality}).Count-eq0
        scheduler_decision_qpc_exact=@($runs|Where-Object{'SCHEDULER_DECISION_QPC_PROVENANCE_MISSING'-in@($_.blockers)}).Count-eq0
        measurement_boundary_relation_exact=@($runs|Where-Object{'MEASUREMENT_BOUNDARY_RELATION_UNRESOLVED'-in@($_.blockers)}).Count-eq0
        presented_population_used_to_derive_required_set=$false
        source_frame_identity_used=$false
        nearest_qpc_or_tolerance_used=$false
        producer_changed=$false
        c2_ledger_changed=$false
        performance_integration_evaluated=$false
        branch_a_established=$false
        branch_b_established=$false
        authority_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'REQUIRED_INTENT_DOMAIN_AUTHORITY_EXACT'}else{'REQUIRED_INTENT_DOMAIN_AUTHORITY_UNRESOLVED'})
    }
}

function Assert-MvmC21Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 16 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 16 -Compress
    if($expectedJson-ne$actualJson){throw 'C2.1 artifactがsealed sourceからの再inventory結果と一致しません'}
}
