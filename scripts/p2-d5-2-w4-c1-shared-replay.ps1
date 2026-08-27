Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot 'p2-d5-2-w4-c1-causal-compatibility-core.ps1')

function Invoke-MvmW4C1FromProducerSemantics {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$W4BProofPath,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$W4BChecker,
        [Parameter(Mandatory=$true)][string]$WorkDirectory,
        [bool]$ReplayW4BChecker=$true
    )
    foreach($path in @($W4BProofPath,$W4BChecker,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W4-C1 upstream inputがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){
        New-Item -ItemType Directory -Path $WorkDirectory|Out-Null
    }
    if($ReplayW4BChecker){
        & pwsh -NoProfile -File $W4BChecker -Proof $W4BProofPath -SourceRoot $SourceRoot `
            -WorkDirectory (Join-Path $WorkDirectory 'w4b-check') *> $null
        if($LASTEXITCODE-ne0){throw 'W4-C1がconsumeするW4-B authorityが不成立です'}
    }
    $w4b=Get-Content -LiteralPath $W4BProofPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$w4b.schema-ne'mvm-p2-d5-2-w4-b-producer-semantics-1'-or
       -not[bool]$w4b.attribution_exact){throw 'W4-B proofがexactではありません'}

    $runs=@();$globalBlockers=@{}
    foreach($w4bRun in @($w4b.runs)){
        $runNumber=[int]$w4bRun.run
        $appPath=Join-Path ([string]$w4b.source_cohort_directory) "run-$runNumber\traced-app.json"
        if(-not(Test-Path -LiteralPath $appPath)){throw "run $runNumber のsealed appがありません: $appPath"}
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $shadow=$app.presentation_opportunity.physical_vblank_domain_shadow
        $result=Invoke-MvmW4C1CausalCompatibility `
            -DecisionRecords @($app.native_present_hook.intent_scope_provenance.records) `
            -SchedulerLedger @($app.formal_opportunity_ledger) `
            -W4BEvents @($w4bRun.events) `
            -OriginRefreshCount ([uint64]$app.formal_opportunity_origin_refresh_count) `
            -MeasurementStartQpc ([int64]$shadow.measurement_start_qpc) `
            -MeasurementEndQpcExclusive ([int64]$shadow.measurement_end_qpc_exclusive) `
            -QpcFrequency ([int64]$app.formal_qpc_frequency) `
            -MeasurementStopCaptured ([bool]$app.measurement_stop_captured)
        $result.run=$runNumber
        $result.sealed_traced_app_sha256=
            (Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
        foreach($blocker in @($result.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runs+=,$result
    }
    $compatible=0L;$notObservable=0L;$incompatible=0L;$transitionCount=0L
    $deltaCounts=[ordered]@{}
    foreach($runResult in $runs){
        $compatible+=[int64]$runResult.compatible_transition_count
        $notObservable+=[int64]$runResult.not_observable_transition_count
        $incompatible+=[int64]$runResult.incompatible_transition_count
        $transitionCount+=[int64]$runResult.w4b_transition_count
        foreach($name in @($runResult.delta_compatibility_counts.Keys)){
            if(-not$deltaCounts.Contains($name)){$deltaCounts[$name]=0L}
            $deltaCounts[$name]=[int64]$deltaCounts[$name]+[int64]$runResult.delta_compatibility_counts[$name]
        }
    }
    if($compatible+$notObservable+$incompatible-ne$transitionCount){
        $globalBlockers['TRANSITION_PARTITION_INVALID']=$true
    }
    $blockerList=@($globalBlockers.Keys|Sort-Object)
    return [ordered]@{
        schema='mvm-p2-d5-2-w4-c1-causal-compatibility-1';stage='P2-D5-2-W4-C1'
        source_w4b_proof=(Resolve-Path -LiteralPath $W4BProofPath).Path
        source_w4b_proof_sha256=
            (Get-FileHash -LiteralPath $W4BProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_cohort_directory=[string]$w4b.source_cohort_directory
        new_capture_performed=$false
        producer_instrumentation_changed=$false
        canonical_performance_authority=$false
        historical_w3_verdict_rewritten=$false
        root_cause_determined=$false
        attribution='EXACT_CAUSAL_COMPATIBILITY_PARTIAL_COVERAGE'
        branch_execution_exact=$false
        nearest_qpc_binding_used=$false
        missing_state_interpolated=$false
        source_domain_required_domain_conflated=$false
        alternative_stop_reason_excluded=$false
        c2_instrumentation_required=$true
        run_count=$runs.Count
        w4b_transition_count=$transitionCount
        compatible_transition_count=$compatible
        not_observable_transition_count=$notObservable
        incompatible_transition_count=$incompatible
        delta_compatibility_counts=$deltaCounts
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'EXACT_CAUSAL_COMPATIBILITY_PARTIAL_COVERAGE'}else{'CAUSAL_COMPATIBILITY_INVALID'})
    }
}
