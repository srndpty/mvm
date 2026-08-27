Set-StrictMode -Version Latest

# W4-B runner / checker が共有する再構築手順。2 箇所に書かない。
#
#   1. W4-A checker を再実行する (その中で W3 / W2-E / W2-D / 各 upstream も再実行される)
#   2. W4-A が確定した missing set を取得する。ここで作り直さない。
#   3. sealed producer ledger と W2-A physical window から W4-B event を独立再構築する
#
# W4-A を正式な handoff boundary とする。

. (Join-Path $PSScriptRoot 'p2-d5-2-w4-b-producer-semantics-core.ps1')

function Invoke-MvmW4BFromAttribution {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$W4AProofPath,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$W4AChecker,
        [Parameter(Mandatory=$true)][string]$WorkDirectory,
        [bool]$ReplayW4AChecker=$true
    )
    foreach($path in @($W4AProofPath,$W4AChecker,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W4-B upstream inputがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){New-Item -ItemType Directory -Path $WorkDirectory|Out-Null}

    if($ReplayW4AChecker){
        & pwsh -NoProfile -File $W4AChecker -Proof $W4AProofPath -SourceRoot $SourceRoot `
            -WorkDirectory (Join-Path $WorkDirectory 'w4a-check') *> $null
        if($LASTEXITCODE-ne0){throw 'W4-BがconsumeするW4-A attribution authorityが不成立です'}
    }
    $w4a=Get-Content -LiteralPath $W4AProofPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$w4a.schema-ne'mvm-p2-d5-2-w4-a-intent-attribution-1'){throw 'W4-A proof schemaが不正です'}
    if(-not[bool]$w4a.attribution_exact){throw 'W4-A attributionがexactではありません'}

    $cohortDirectory=[string]$w4a.source_cohort_directory
    $runs=@();$globalBlockers=@{}
    foreach($attributionRun in @($w4a.runs)){
        $run=[int]$attributionRun.run
        $appPath=Join-Path $cohortDirectory "run-$run\traced-app.json"
        if(-not(Test-Path -LiteralPath $appPath)){throw "run $run のtraced-appがありません: $appPath"}
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $shadow=$app.presentation_opportunity.physical_vblank_domain_shadow
        $result=Invoke-MvmW4BProducerSemantics `
            -RequiredIntentOrdinals @($attributionRun.required_intent_ordinals) `
            -MissingOrdinals @($attributionRun.missing_primary_decision_ordinals) `
            -DecisionRecords @($app.native_present_hook.intent_scope_provenance.records) `
            -MeasurementStartQpc ([int64]$shadow.measurement_start_qpc) `
            -MeasurementEndQpcExclusive ([int64]$shadow.measurement_end_qpc_exclusive) `
            -QpcFrequency ([int64]$app.formal_qpc_frequency) `
            -LegacyMeasurementElapsedSeconds ([double]$app.measurement_elapsed_seconds)
        $result.run=$run
        $result.sealed_traced_app_sha256=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
        foreach($blocker in @($result.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runs+=,$result
    }

    $cohortNames=@('HEAD_EDGE','TAIL_EDGE','LONGER_MISSING_RUN','DOUBLE_MISSING_BOUNDARY',
        'ISOLATED_MISSING','OTHER_PATTERN')
    $eventCounts=[ordered]@{};$intentCounts=[ordered]@{}
    foreach($name in $cohortNames){$eventCounts[$name]=0L;$intentCounts[$name]=0L}
    $missing=0L;$primary=0L;$required=0L
    foreach($runResult in $runs){
        $missing+=[int64]$runResult.missing_intent_count
        $primary+=[int64]$runResult.primary_decision_count
        $required+=[int64]$runResult.required_intent_count
        foreach($name in $cohortNames){
            $eventCounts[$name]+=[int64]$runResult.cohort_event_counts[$name]
            $intentCounts[$name]+=[int64]$runResult.cohort_intent_counts[$name]
        }
    }
    $intentSum=0L
    foreach($name in $cohortNames){$intentSum+=[int64]$intentCounts[$name]}
    # W4-A の missing count と一致すること。splice を fail-close する。
    if($missing-ne[int64]$w4a.unsatisfied_intent_count){$globalBlockers['W4A_MISSING_COUNT_MISMATCH']=$true}
    if($intentSum-ne$missing){$globalBlockers['COHORT_INTENT_SUM_MISMATCH']=$true}
    if(([int64]$eventCounts['DOUBLE_MISSING_BOUNDARY']*2)-ne[int64]$intentCounts['DOUBLE_MISSING_BOUNDARY']){
        $globalBlockers['DOUBLE_EVENT_INTENT_IDENTITY_VIOLATION']=$true
    }
    $blockerList=@($globalBlockers.Keys|Sort-Object)

    return [ordered]@{
        schema='mvm-p2-d5-2-w4-b-producer-semantics-1';stage='P2-D5-2-W4-B'
        population='EXACT_REQUIRED_CURRENT_INTENT_SET'
        cohort_classification='MAXIMAL_CONSECUTIVE_MISSING_RUN'
        semantic_interpretation='SCHEDULER_OPPORTUNITY_ORDINAL_DELTA_VIA_W2_B1_INTENT_IDENTITY'
        attribution_only=$true
        root_cause_determined=$false
        new_capture_performed=$false
        producer_instrumentation_changed=$false
        missing_ordinal_producer_field_interpolated=$false
        nearest_decision_qpc_used=$false
        cross_run_neighbor_splice_used=$false
        run_level_time_domain_diagnostic_present=$true
        legacy_measurement_elapsed_used_as_authority=$false
        decision_span_used_as_measurement_window=$false
        source_w4a_proof=(Resolve-Path -LiteralPath $W4AProofPath).Path
        source_w4a_proof_sha256=(Get-FileHash -LiteralPath $W4AProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_cohort_directory=$cohortDirectory
        run_count=$runs.Count
        required_intent_count=$required
        missing_intent_count=$missing
        primary_decision_count=$primary
        cohort_event_counts=$eventCounts
        cohort_intent_counts=$intentCounts
        cohort_intent_sum=$intentSum
        w4a_unsatisfied_intent_count=[int64]$w4a.unsatisfied_intent_count
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'PRODUCER_SEMANTICS_ATTRIBUTION_EXACT'}else{'PRODUCER_SEMANTICS_ATTRIBUTION_INVALID'})
    }
}

function Assert-MvmW4BProof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 24 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 24 -Compress
    if($expectedJson-ne$actualJson){throw 'W4-B artifactがsealed producer recordsからの再構築結果と一致しません'}
}
