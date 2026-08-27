Set-StrictMode -Version Latest

# W4-A runner / checker が共有する再構築手順。2 箇所に書かない。
#
#   1. W3 canonical performance checker を再実行する
#      (その中で W2-E / W2-D / 各 upstream checker も再実行される)
#   2. sealed cohort の producer record と C1 mapping record から
#      required intent 単位の partition を再構築する
#
# aggregate をコピーしない。records から再計算する。

. (Join-Path $PSScriptRoot 'p2-d5-2-w4-a-intent-attribution-core.ps1')

function Invoke-MvmW4AttributionFromCanonical {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$W3ProofPath,
        [Parameter(Mandatory=$true)][string]$C1ProofPath,
        [Parameter(Mandatory=$true)][string]$C21ProofPath,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$W3Checker,
        [Parameter(Mandatory=$true)][string]$WorkDirectory,
        [bool]$ReplayW3Checker=$true
    )
    foreach($path in @($W3ProofPath,$C1ProofPath,$C21ProofPath,$W3Checker,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W4-A upstream inputがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){New-Item -ItemType Directory -Path $WorkDirectory|Out-Null}

    # 1. canonical performance authority の再実行。
    if($ReplayW3Checker){
        & pwsh -NoProfile -File $W3Checker -Proof $W3ProofPath -SourceRoot $SourceRoot `
            -WorkDirectory (Join-Path $WorkDirectory 'w3-check') *> $null
        if($LASTEXITCODE-ne0){throw 'W4-AがconsumeするW3 canonical performance authorityが不成立です'}
    }
    $w3=Get-Content -LiteralPath $W3ProofPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$w3.schema-ne'mvm-p2-d5-2-w3-canonical-performance-1'){throw 'W3 proof schemaが不正です'}
    if(-not[bool]$w3.stage3_accounting_valid){throw 'W3 accountingがVALIDではありません'}
    $c1=Get-Content -LiteralPath $C1ProofPath -Raw -Encoding utf8|ConvertFrom-Json
    $c21=Get-Content -LiteralPath $C21ProofPath -Raw -Encoding utf8|ConvertFrom-Json
    # required exact set の provenance が C2.1 と一致すること。
    $c1Hash=(Get-FileHash -LiteralPath $C1ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if([string]$c21.source_c1_proof_sha256-ne$c1Hash){throw 'C2.1が別C1 cohortを参照しています'}
    if(-not[bool]$c21.authority_exact){throw 'C2.1 required intent authorityがexactではありません'}

    $cohortDirectory=[string]$c1.source_c011_directory
    $c1Runs=@($c1.runs);$c21Runs=@($c21.runs)
    if($c21Runs.Count-ne$c1Runs.Count){throw 'C1 / C2.1のrun countが一致しません'}
    $runs=@();$globalBlockers=@{}
    for($index=0;$index-lt$c1Runs.Count;++$index){
        $run=[int]$c1Runs[$index].run
        if([int]$c21Runs[$index].run-ne$run){throw "run $run のC2.1 run identityが一致しません"}
        $appPath=Join-Path $cohortDirectory "run-$run\traced-app.json"
        if(-not(Test-Path -LiteralPath $appPath)){throw "run $run のtraced-appがありません: $appPath"}
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $hook=$app.native_present_hook
        $attribution=Invoke-MvmW4IntentAttribution `
            -RequiredIntentOrdinals @($c21Runs[$index].required_scheduler_intent_ordinals) `
            -DecisionRecords @($hook.intent_scope_provenance.records) `
            -NativePresentRecords @($hook.records) `
            -FormalMappingRecords @($c1Runs[$index].records)
        $attribution.run=$run
        $attribution.sealed_traced_app_sha256=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
        foreach($blocker in @($attribution.blockers)){$globalBlockers[[string]$blocker]=$true}
        $runs+=,$attribution
    }

    $required=0L;$satisfied=0L;$unsatisfied=0L;$primary=0L
    $duplicateSuppressed=0L;$outsideRequired=0L
    $bucketNames=@('A_NO_PRIMARY_SCHEDULER_DECISION','C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY',
        'D_NO_NATIVE_PRESENT','E_NO_EXACT_FORMAL_PRESENTED','F_FORMAL_PRESENTED_OUTSIDE_DOMAIN',
        'G_SATISFIED_IN_DOMAIN')
    $buckets=[ordered]@{}
    foreach($name in $bucketNames){$buckets[$name]=0L}
    foreach($runResult in $runs){
        $required+=[int64]$runResult.required_intent_count
        $satisfied+=[int64]$runResult.satisfied_intent_count
        $unsatisfied+=[int64]$runResult.unsatisfied_intent_count
        $primary+=[int64]$runResult.primary_decision_count
        $duplicateSuppressed+=[int64]$runResult.duplicate_callback_suppressed_count
        $outsideRequired+=[int64]$runResult.outside_required_decision_count
        foreach($name in $bucketNames){$buckets[$name]+=[int64]$runResult.buckets.$name}
    }
    $bucketSum=0L
    foreach($name in $bucketNames){$bucketSum+=[int64]$buckets[$name]}
    $downstreamLoss=0L
    foreach($name in @('C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY','D_NO_NATIVE_PRESENT',
                       'E_NO_EXACT_FORMAL_PRESENTED','F_FORMAL_PRESENTED_OUTSIDE_DOMAIN')){
        $downstreamLoss+=[int64]$buckets[$name]
    }
    # canonical 値との一致。W3 artifact の aggregate をコピーせず、再計算値と突き合わせる。
    if($required-ne[int64]$w3.canonical_required_intent_count){$globalBlockers['CANONICAL_REQUIRED_COUNT_MISMATCH']=$true}
    if($satisfied-ne[int64]$w3.canonical_satisfied_intent_count){$globalBlockers['CANONICAL_SATISFIED_COUNT_MISMATCH']=$true}
    if($unsatisfied-ne[int64]$w3.canonical_unsatisfied_intent_count){$globalBlockers['CANONICAL_UNSATISFIED_COUNT_MISMATCH']=$true}
    if($bucketSum-ne$required){$globalBlockers['REQUIRED_INTENT_PARTITION_NOT_EXHAUSTIVE']=$true}
    $blockerList=@($globalBlockers.Keys|Sort-Object)

    return [ordered]@{
        schema='mvm-p2-d5-2-w4-a-intent-attribution-1';stage='P2-D5-2-W4-A'
        population='EXACT_REQUIRED_CURRENT_INTENT_SET'
        attribution_only=$true
        root_cause_determined=$false
        instrumentation_ab_performed=$false
        source_w3_proof=(Resolve-Path -LiteralPath $W3ProofPath).Path
        source_w3_proof_sha256=(Get-FileHash -LiteralPath $W3ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_c1_proof=(Resolve-Path -LiteralPath $C1ProofPath).Path
        source_c1_proof_sha256=$c1Hash
        source_c21_proof=(Resolve-Path -LiteralPath $C21ProofPath).Path
        source_c21_proof_sha256=(Get-FileHash -LiteralPath $C21ProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
        source_cohort_directory=$cohortDirectory
        run_count=$runs.Count
        required_intent_count=$required
        buckets=$buckets
        bucket_sum=$bucketSum
        satisfied_intent_count=$satisfied
        unsatisfied_intent_count=$unsatisfied
        downstream_loss_count=$downstreamLoss
        primary_decision_count=$primary
        duplicate_callback_suppressed_count=$duplicateSuppressed
        outside_required_decision_count=$outsideRequired
        canonical_required_intent_count=[int64]$w3.canonical_required_intent_count
        canonical_satisfied_intent_count=[int64]$w3.canonical_satisfied_intent_count
        canonical_unsatisfied_intent_count=[int64]$w3.canonical_unsatisfied_intent_count
        attribution_exact=$blockerList.Count-eq0
        blockers=$blockerList
        runs=$runs
        verdict=$(if($blockerList.Count-eq0){'UNSATISFIED_INTENT_ATTRIBUTION_EXACT'}else{'UNSATISFIED_INTENT_ATTRIBUTION_INVALID'})
    }
}

function Assert-MvmW4Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 20 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 20 -Compress
    if($expectedJson-ne$actualJson){throw 'W4-A artifactがsealed recordsからの再構築結果と一致しません'}
}
