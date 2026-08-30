[CmdletBinding()]
param(
    # "label|cohort|runDirectory" を1行ずつ並べたmanifest。新規取得は行わない。
    [Parameter(Mandatory=$true)][string]$RunSpecFile,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Has($Object,[string]$Name){$null-ne$Object-and($Object.PSObject.Properties.Name-contains$Name)}
function Percentile([long[]]$Values,[double]$P){
    if($Values.Count-eq0){return $null}
    $sorted=@($Values|Sort-Object);$index=[Math]::Ceiling($P*$sorted.Count)-1
    return [long]$sorted[[Math]::Max(0,$index)]
}
$independentModes=@('Hardware_Independent_Flip','Hardware_Composed_Independent_Flip')
$composedModes=@('Composed_Flip','Composed_Copy_GPU_GDI','Composed_Copy_CPU_GDI')
if(-not(Test-Path -LiteralPath $RunSpecFile)){Fail "run spec manifestがありません: $RunSpecFile"}
$specs=@(Get-Content -LiteralPath $RunSpecFile -Encoding utf8|
    Where-Object{-not[string]::IsNullOrWhiteSpace($_)-and-not$_.StartsWith('#')})
if($specs.Count-eq0){Fail "run spec manifestが空です: $RunSpecFile"}
$rows=@()
foreach($spec in $specs){
    $parts=$spec -split '\|'
    if($parts.Count-ne3){Fail "RunSpecは label|cohort|path 形式です: $spec"}
    $label=$parts[0];$cohort=$parts[1];$directory=$parts[2]
    $rawPath=Join-Path $directory 'present-history-raw.json'
    $appPath=Join-Path $directory 'traced-app.json'
    foreach($path in @($rawPath,$appPath)){if(-not(Test-Path -LiteralPath $path)){Fail "D1-A必須artifactがありません: $path"}}
    $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$raw.schema-ne'mvm-p2-etw-present-history-1'){Fail "raw schemaが不正です: $rawPath"}
    $lossFields=[ordered]@{}
    foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
        $lossFields[$field]=if(Has $raw $field){[long]$raw.$field}else{$null}
    }
    $start=[long]$app.presentation_opportunity.measurement_start_qpc
    $end=[long]$app.presentation_opportunity.measurement_end_qpc_exclusive
    $frequency=[long]$app.presentation_opportunity.qpc_frequency
    if($start-le0-or$end-le$start-or$frequency-le0){Fail "measurement windowが不正です: $appPath"}
    $elapsed=([double]$end-[double]$start)/[double]$frequency
    $targetPid=[long]$raw.target_process_id
    $events=@($raw.events|Where-Object{[long]$_.process_id-eq$targetPid-and
        [long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end}|
        Sort-Object {[long]$_.present_start_qpc})
    if($events.Count-eq0){Fail "measurement window内にtarget Presentがありません: $rawPath"}
    $sample=$events[0]
    # 欠損fieldは0へ変換しない。availabilityとして明示する。
    $availability=[ordered]@{
        attached_dwm_parent=Has $sample 'attached_dwm_parent_present_start_qpc'
        dwm_parent_displayed=Has $sample 'dwm_parent_displayed_qpc'
        dependency_batch=Has $sample 'dependency_batch_present_start_qpc'
        present_mode=Has $sample 'present_mode'
        displayed=Has $sample 'displayed'
        final_state=Has $sample 'final_state'
    }
    if(-not$availability.present_mode-or-not$availability.final_state-or-not$availability.displayed){
        Fail "PresentMode/FinalState/Displayedを欠くrawはinventory対象外です: $rawPath"
    }
    $modeCounts=@{};$displayedCount=0;$presentedCount=0;$unknownCount=0
    $independentCount=0;$composedCount=0;$otherModeCount=0
    $modeClasses=@()
    foreach($ev in $events){
        $mode=[string]$ev.present_mode
        if(-not$modeCounts.ContainsKey($mode)){$modeCounts[$mode]=0}
        $modeCounts[$mode]++
        $modeClass=if($independentModes-contains$mode){'INDEPENDENT'}
                   elseif($composedModes-contains$mode){'COMPOSED'}else{'OTHER'}
        switch($modeClass){
            'INDEPENDENT'{$independentCount++}
            'COMPOSED'{$composedCount++}
            default{$otherModeCount++}
        }
        $modeClasses+=[pscustomobject]@{qpc=[long]$ev.present_start_qpc;class=$modeClass}
        $presented=[string]$ev.final_state-eq'Presented'
        $displayed=@($ev.displayed|Where-Object{$null-ne$_-and(Has $_ 'qpc')-and[long]$_.qpc-gt0})
        if($presented){
            $presentedCount++
            if($displayed.Count-eq0){$unknownCount++}else{$displayedCount++}
        }
    }
    $total=$events.Count
    $displayedFraction=[double]$displayedCount/$total
    $independentFraction=[double]$independentCount/$total
    $composedFraction=[double]$composedCount/$total
    # mode遷移: rawは最終PresentModeしか持たないため、transition eventのprovenanceは無い。
    $firstMode=[string]$events[0].present_mode
    $firstModeClass=$modeClasses[0].class
    $boundary=$null
    for($index=1;$index-lt$modeClasses.Count;++$index){
        if($modeClasses[$index].class-ne$modeClasses[$index-1].class){
            $boundary=[ordered]@{index=$index;qpc=$modeClasses[$index].qpc
                from=$modeClasses[$index-1].class;to=$modeClasses[$index].class}
            break
        }
    }
    $modeHistory=if($null-eq$boundary){'SINGLE_MODE_CLASS_THROUGHOUT'}else{'MODE_CLASSIFICATION_CHANGED'}
    $composedFromFirst=$firstModeClass-eq'COMPOSED'-and$modeHistory-eq'SINGLE_MODE_CLASS_THROUGHOUT'
    # DWM parent cadence。fieldが無ければnull。sparsenessは推定せず観測のみ。
    $parentCadence=$null
    if($availability.attached_dwm_parent){
        $parents=@($events|ForEach-Object{[long]$_.attached_dwm_parent_present_start_qpc}|
            Where-Object{$_-gt0}|Sort-Object -Unique)
        $gaps=@();for($index=1;$index-lt$parents.Count;++$index){$gaps+=[long]($parents[$index]-$parents[$index-1])}
        $parentCadence=[ordered]@{
            attached_present_count=@($events|Where-Object{[long]$_.attached_dwm_parent_present_start_qpc-gt0}).Count
            distinct_parent_count=$parents.Count
            parents_per_second=if($elapsed-gt0){$parents.Count/$elapsed}else{$null}
            parent_start_gap_qpc=[ordered]@{p95=Percentile $gaps 0.95;max=if($gaps.Count-gt0){[long]($gaps|Measure-Object -Maximum).Maximum}else{$null}}
        }
    }
    $batchStats=$null
    if($availability.dependency_batch){
        $batches=@($events|ForEach-Object{[long]$_.dependency_batch_present_start_qpc}|Where-Object{$_-gt0})
        $sizes=@($batches|Group-Object|ForEach-Object{[long]$_.Count})
        $batchStats=[ordered]@{batch_count=@($batches|Sort-Object -Unique).Count
            size_p95=Percentile $sizes 0.95
            size_max=if($sizes.Count-gt0){[long]($sizes|Measure-Object -Maximum).Maximum}else{$null}}
    }
    # DWM-wide cadence。non-target PIDが一意に決まらなければnull。
    $dwmWide=$null
    $nonTarget=@($raw.events|Where-Object{[long]$_.process_id-ne$targetPid}|Select-Object -ExpandProperty process_id -Unique)
    if($nonTarget.Count-eq1){
        $dwmEvents=@($raw.events|Where-Object{[long]$_.process_id-eq[long]$nonTarget[0]-and
            [long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end})
        $dwmWide=[ordered]@{process_id=[long]$nonTarget[0];present_start_count=$dwmEvents.Count
            present_starts_per_second=if($elapsed-gt0){$dwmEvents.Count/$elapsed}else{$null}}
    }
    # environment provenance。無い場合にstableと推定しない。
    $stateCandidates=@((Join-Path $directory 'window-state-raw.json'),
                       (Join-Path (Split-Path -Parent $directory) 'window-state-raw.json'))
    $statePath=$stateCandidates|Where-Object{Test-Path -LiteralPath $_}|Select-Object -First 1
    $environment=[ordered]@{window_state_provenance='UNAVAILABLE';input_provenance='UNAVAILABLE'
        geometry_provenance='UNAVAILABLE';monitor_provenance='UNAVAILABLE';mode=$null}
    if($null-ne$statePath){
        $state=Get-Content -LiteralPath $statePath -Raw -Encoding utf8|ConvertFrom-Json
        $environment.window_state_provenance='AVAILABLE'
        $environment.mode=[string]$state.mode
        $stateSample=@($state.samples)[0]
        if(Has $stateSample 'last_input_tick'){$environment.input_provenance='AVAILABLE'}
        if(Has $stateSample 'window_rect'){$environment.geometry_provenance='AVAILABLE'}
        if(Has $stateSample 'monitor'){$environment.monitor_provenance='AVAILABLE'}
    }
    # swapchain descはどのartifactにも記録されていない。D1-Bで追加すべき項目。
    $preflight=if(Has $app 't2_preflight'){'AVAILABLE'}else{'UNAVAILABLE'}
    $regimeReason=$null
    $regime=if($unknownCount-gt0){$regimeReason='UNKNOWN_PRESENT_PATH';'UNRESOLVED'}
            elseif($displayedFraction-ge0.99){
                if($independentFraction-ge0.99){$regimeReason='DISPLAYED_AND_INDEPENDENT';'GOOD_INDEPENDENT'}
                elseif($composedFraction-ge0.99){$regimeReason='DISPLAYED_AND_COMPOSED';'GOOD_COMPOSED'}
                else{$regimeReason='MIXED_PRESENT_MODE';'UNRESOLVED'}}
            elseif($displayedFraction-le0.95){
                if($composedFraction-ge0.99){$regimeReason='DEGRADED_AND_COMPOSED';'BAD_SPARSE_COMPOSED'}
                elseif($independentFraction-ge0.99){$regimeReason='DEGRADED_AND_INDEPENDENT';'UNRESOLVED'}
                else{$regimeReason='MIXED_PRESENT_MODE_DEGRADED';'UNRESOLVED'}}
            else{$regimeReason='INTERMEDIATE_DISPLAY_FRACTION';'UNRESOLVED'}
    $row=[ordered]@{
        label=$label;cohort=$cohort;regime=$regime;regime_reason=$regimeReason
        runtime_provenance=[ordered]@{
            presentmon_commit=if(Has $raw 'presentmon_commit'){[string]$raw.presentmon_commit}else{$null}
            acquisition_mode=if(Has $raw 'acquisition_mode'){[string]$raw.acquisition_mode}else{$null}
            raw_schema=[string]$raw.schema
            t2_preflight_provenance=$preflight
            swapchain_desc_provenance='UNAVAILABLE'
        }
        etw_loss=$lossFields
        measurement_seconds=$elapsed
        target_present_count=$total;presented_count=$presentedCount
        displayed_count=$displayedCount;unknown_displayed_count=$unknownCount
        displayed_fraction=$displayedFraction
        present_mode_histogram=[ordered]@{}
        first_observed_present_mode=$firstMode
        mode_class_fractions=[ordered]@{INDEPENDENT=$independentFraction;COMPOSED=$composedFraction
            OTHER=[double]$otherModeCount/$total}
        mode_history=$modeHistory
        composed_from_first_observed_present=$composedFromFirst
        observed_mode_class_boundary=$boundary
        mode_transition_provenance='UNAVAILABLE_FINAL_MODE_ONLY'
        field_availability=$availability
        dwm_parent_cadence=$parentCadence
        dependency_batch=$batchStats
        dwm_wide=$dwmWide
        environment_provenance=$environment
    }
    foreach($key in ($modeCounts.Keys|Sort-Object)){$row.present_mode_histogram[$key]=$modeCounts[$key]}
    $rows+=[pscustomobject]$row
}
$regimeCounts=[ordered]@{GOOD_INDEPENDENT=0;GOOD_COMPOSED=0;BAD_SPARSE_COMPOSED=0;UNRESOLVED=0}
foreach($row in $rows){$regimeCounts[$row.regime]++}
$goodComposedExists=$regimeCounts.GOOD_COMPOSED-gt0
$badExists=$regimeCounts.BAD_SPARSE_COMPOSED-gt0
# GOOD_COMPOSEDとBAD_SPARSE_COMPOSEDの両方が存在するなら、Composed_Flipであること
# 自体は失敗原因ではない。
$verdict=if($goodComposedExists-and$badExists){'COMPOSED_MODE_IS_NOT_THE_FAILURE_CAUSE'}
         elseif($badExists){'BAD_REGIME_PRESENT_WITHOUT_GOOD_COMPOSED_REFERENCE'}
         else{'NO_BAD_REGIME_IN_RUN_SET'}
$comparable=@($rows|Where-Object{$_.field_availability.attached_dwm_parent}).Count
$environmentKnown=@($rows|Where-Object{$_.environment_provenance.input_provenance-eq'AVAILABLE'}).Count
$nextAction=if($goodComposedExists-and$badExists){'T2_D1_B_CONTROLLED_BAD_REGIME_REPRODUCTION'}
            else{'EXTEND_D1A_RUN_SET'}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d1a-regime-inventory-1';status='PASS';authority='diagnostic_only'
    analysis_mode='OFFLINE_REANALYSIS_NO_NEW_ACQUISITION'
    verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    note='Composed_Flip自体をfailureとみなさない。欠損fieldは0へ変換せずUNAVAILABLEとする。'
    run_spec_file=(Resolve-Path -LiteralPath $RunSpecFile).Path
    run_spec_sha256=(Get-FileHash -LiteralPath $RunSpecFile -Algorithm SHA256).Hash.ToLowerInvariant()
    run_count=$rows.Count;regime_counts=$regimeCounts
    dwm_parent_comparable_run_count=$comparable
    input_provenance_available_run_count=$environmentKnown
    runs=$rows
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D1-A regime inventory: PASS verdict=$verdict runs=$($rows.Count)"
