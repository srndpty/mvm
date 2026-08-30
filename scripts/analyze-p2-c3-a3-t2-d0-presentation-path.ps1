[CmdletBinding()]
param(
    # "label|group|canonicalDirectory" を1行ずつ並べたmanifest。
    # 新規取得は行わず既存artifactのみを再解析する。run setもartifactとして残す。
    [Parameter(Mandatory=$true)][string]$RunSpecFile,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
# PresentMonがindependent flipと判定したmode。DWM parentを介さずdisplayへ出る。
$independentModes=@('Hardware_Independent_Flip','Hardware_Composed_Independent_Flip')
if(-not(Test-Path -LiteralPath $RunSpecFile)){Fail "run spec manifestがありません: $RunSpecFile"}
$RunSpec=@(Get-Content -LiteralPath $RunSpecFile -Encoding utf8|
    Where-Object{-not[string]::IsNullOrWhiteSpace($_)-and-not$_.StartsWith('#')})
if($RunSpec.Count-eq0){Fail "run spec manifestが空です: $RunSpecFile"}
$rows=@()
foreach($spec in $RunSpec){
    $parts=$spec -split '\|'
    if($parts.Count-ne3){Fail "RunSpecは label|group|path 形式です: $spec"}
    $label=$parts[0];$group=$parts[1];$directory=$parts[2]
    $rawPath=Join-Path $directory 'present-history-raw.json'
    $appPath=Join-Path $directory 'traced-app.json'
    foreach($path in @($rawPath,$appPath)){if(-not(Test-Path -LiteralPath $path)){Fail "D0必須artifactがありません: $path"}}
    $raw=Get-Content -LiteralPath $rawPath -Raw -Encoding utf8|ConvertFrom-Json
    $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$raw.schema-ne'mvm-p2-etw-present-history-1'){Fail "raw schemaが不正です: $rawPath"}
    foreach($field in @('etw_events_lost','etw_buffers_lost','present_event_overflow_count')){
        if([long]$raw.$field-ne0){Fail "raw $field が0ではありません: $rawPath"}
    }
    $start=[long]$app.presentation_opportunity.measurement_start_qpc
    $end=[long]$app.presentation_opportunity.measurement_end_qpc_exclusive
    if($start-le0-or$end-le$start){Fail "measurement windowが不正です: $appPath"}
    $targetPid=[long]$raw.target_process_id
    $events=@($raw.events|Where-Object{[long]$_.process_id-eq$targetPid-and
        [long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end}|
        Sort-Object {[long]$_.present_start_qpc})
    if($events.Count-eq0){Fail "measurement window内にtarget Presentがありません: $rawPath"}
    # 旧acquisition schemaはDWM parent fieldを持たない。無いものを0とみなさない。
    $parentFieldAvailable=$events[0].PSObject.Properties.Name-contains'attached_dwm_parent_present_start_qpc'
    $counts=[ordered]@{DWM_COMPOSED=0;INDEPENDENT_DISPLAY=0;OTHER_DIRECT_DISPLAY=0
        DISPLAYED_PATH_UNRESOLVED=0;DISCARDED=0;UNKNOWN=0}
    $modeCounts=@{}
    $evidenceCounts=@{}
    foreach($ev in $events){
        $mode=[string]$ev.present_mode
        if(-not$modeCounts.ContainsKey($mode)){$modeCounts[$mode]=0}
        $modeCounts[$mode]++
        # displayedはJSON上 [] が null になることがある。欠損を要素として扱わない。
        $displayed=@($ev.displayed|Where-Object{$null-ne$_-and
            ($_.PSObject.Properties.Name-contains'qpc')-and[long]$_.qpc-gt0})
        $hasDisplayed=$displayed.Count-gt0
        $hasParent=$parentFieldAvailable-and[long]$ev.attached_dwm_parent_present_start_qpc-gt0
        $presented=[string]$ev.final_state-eq'Presented'
        # 分類は後付け推測ではなくraw stateのみから行う。
        # parent fieldが無いrunでは composed と independent を区別できないため、
        # 0とみなさず DISPLAYED_PATH_UNRESOLVED とする。
        $class=if(-not$presented){'DISCARDED'}
               elseif(-not$hasDisplayed){'UNKNOWN'}
               elseif(-not$parentFieldAvailable){'DISPLAYED_PATH_UNRESOLVED'}
               elseif($hasParent){'DWM_COMPOSED'}
               elseif($independentModes-contains$mode){'INDEPENDENT_DISPLAY'}
               else{'OTHER_DIRECT_DISPLAY'}
        $counts[$class]++
        if($hasDisplayed){
            # DisplayedQPCを確定させたevent経路をprovenanceとして残す。
            $evidence=@()
            if([bool]$ev.seen_in_frame_event){$evidence+='InFrame'}
            if([bool]$ev.wait_for_flip_event){$evidence+='WaitForFlip'}
            if([bool]$ev.wait_for_mpo_flip_event){$evidence+='WaitForMPOFlip'}
            if([bool]$ev.seen_win32k_events){$evidence+='Win32k'}
            if([bool]$ev.seen_dxgk_present){$evidence+='DxgkPresent'}
            if($parentFieldAvailable-and[long]$ev.dwm_parent_displayed_qpc-gt0){$evidence+='DwmParentDisplayed'}
            $key=if($evidence.Count-eq0){'NONE'}else{$evidence -join '+'}
            $key=$key+'/'+[string]$displayed[0].frame_type
            if(-not$evidenceCounts.ContainsKey($key)){$evidenceCounts[$key]=0}
            $evidenceCounts[$key]++
        }
    }
    $total=$events.Count
    $discardedFraction=[double]$counts.DISCARDED/$total
    $independentFraction=[double]$counts.INDEPENDENT_DISPLAY/$total
    $composedFraction=[double]$counts.DWM_COMPOSED/$total
    # regimeはfail-closed。UNKNOWNが1件でもあればregime判定しない。
    $regime=if($counts.UNKNOWN-gt0){'UNKNOWN_PRESENT_PATH'}
            elseif($discardedFraction-ge0.05){'SPARSE_COMPOSED_DISCARD'}
            elseif(-not$parentFieldAvailable){'PATH_UNRESOLVED_LEGACY_SCHEMA'}
            elseif($independentFraction-ge0.99){'INDEPENDENT_FLIP_DISPLAY'}
            elseif($composedFraction-ge0.99){'DWM_COMPOSED_DISPLAY'}
            else{'MIXED_DISPLAY_PATH'}
    $dwmPid=$null
    $nonTarget=@($raw.events|Where-Object{[long]$_.process_id-ne$targetPid}|Select-Object -ExpandProperty process_id -Unique)
    if($nonTarget.Count-eq1){$dwmPid=[long]$nonTarget[0]}
    $dwmWide=if($null-ne$dwmPid){@($raw.events|Where-Object{[long]$_.process_id-eq$dwmPid-and
        [long]$_.present_start_qpc-ge$start-and[long]$_.present_start_qpc-lt$end}).Count}else{-1}
    $rows+=[pscustomobject][ordered]@{
        label=$label;group=$group;regime=$regime
        dwm_parent_evidence_available=$parentFieldAvailable
        target_present_count=$total
        presented_count=$total-$counts.DISCARDED
        displayed_fraction=[double]($total-$counts.DISCARDED)/$total
        dwm_wide_present_start_count=$dwmWide
        classification=[ordered]@{
            DWM_COMPOSED=$counts.DWM_COMPOSED;INDEPENDENT_DISPLAY=$counts.INDEPENDENT_DISPLAY
            OTHER_DIRECT_DISPLAY=$counts.OTHER_DIRECT_DISPLAY
            DISPLAYED_PATH_UNRESOLVED=$counts.DISPLAYED_PATH_UNRESOLVED
            DISCARDED=$counts.DISCARDED;UNKNOWN=$counts.UNKNOWN}
        present_mode_histogram=[ordered]@{}
        displayed_qpc_evidence=[ordered]@{}
    }
    foreach($key in ($modeCounts.Keys|Sort-Object)){$rows[-1].present_mode_histogram[$key]=$modeCounts[$key]}
    foreach($key in ($evidenceCounts.Keys|Sort-Object)){$rows[-1].displayed_qpc_evidence[$key]=$evidenceCounts[$key]}
}
$unknownRuns=@($rows|Where-Object{$_.classification.UNKNOWN-gt0}).Count
$groups=@($rows|Select-Object -ExpandProperty group -Unique)
# 一次判定: DWM PresentStartの不在がdisplay suppressionを意味するか。
$quiet=@($rows|Where-Object{$_.group-eq'T2_QUIET'})
$external=@($rows|Where-Object{$_.group-eq'T2_EXTERNAL_DIRTY'})
$historical=@($rows|Where-Object{$_.group-eq'HISTORICAL_DISCARD'})
$verdict='INSUFFICIENT_GROUPS'
$nextAction='EXTEND_D0_RUN_SET'
if($unknownRuns-gt0){
    $verdict='UNKNOWN_PRESENT_PATH_PRESENT';$nextAction='RECORD_EXACT_DISPLAYED_QPC_EVENT_PATH'
}elseif($quiet.Count-gt0-and$external.Count-gt0-and$historical.Count-gt0){
    $quietIndependent=@($quiet|Where-Object regime -ne 'INDEPENDENT_FLIP_DISPLAY').Count-eq0
    $quietFullyDisplayed=@($quiet|Where-Object{$_.displayed_fraction-lt1.0}).Count-eq0
    $externalIndependent=@($external|Where-Object regime -ne 'INDEPENDENT_FLIP_DISPLAY').Count-eq0
    $externalComposed=@($external|Where-Object regime -ne 'DWM_COMPOSED_DISPLAY').Count-eq0
    $externalFullyDisplayed=@($external|Where-Object{$_.displayed_fraction-lt1.0}).Count-eq0
    $historicalDegraded=@($historical|Where-Object{$_.displayed_fraction-ge0.95}).Count-eq0
    # historicalの少なくとも1件はparent evidenceを持つ必要がある。
    $historicalResolved=@($historical|Where-Object dwm_parent_evidence_available).Count-gt0
    if($quietIndependent-and$quietFullyDisplayed-and$externalFullyDisplayed-and$historicalDegraded-and$historicalResolved){
        if($externalIndependent){
            # externalでもtargetがindependentのままなら、DWM PresentStartは
            # targetのdisplay pathを一切表現していない。
            $verdict='TARGET_DISPLAY_INDEPENDENT_OF_DWM_PRESENTSTART'
            $nextAction='T2_D1_PRESENTATION_PATH_REGIME_ATTRIBUTION'
        }elseif($externalComposed){
            $verdict='EXTERNAL_DIRTY_TRIGGERS_PRESENTATION_PATH_TRANSITION'
            $nextAction='T2_D1_PRESENTATION_PATH_REGIME_ATTRIBUTION'
        }else{
            $verdict='EXTERNAL_DIRTY_PATH_MIXED'
            $nextAction='T2_D1_PRESENTATION_PATH_REGIME_ATTRIBUTION'
        }
    }else{
        $verdict='PRESENTATION_PATH_NOT_SEPARATED';$nextAction='EXTEND_D0_RUN_SET'
    }
}
[ordered]@{
    schema='mvm-p2-c3-a3-t2-d0-presentation-path-1';status='PASS';authority='diagnostic_only'
    analysis_mode='OFFLINE_REANALYSIS_NO_NEW_ACQUISITION'
    verdict=$verdict;next_action=$nextAction
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
    later_attribution='DWM PresentStart absence is not equivalent to display suppression. Independent/direct presentation path must be excluded first.'
    run_spec_file=(Resolve-Path -LiteralPath $RunSpecFile).Path
    run_spec_sha256=(Get-FileHash -LiteralPath $RunSpecFile -Algorithm SHA256).Hash.ToLowerInvariant()
    unknown_present_path_run_count=$unknownRuns
    legacy_schema_run_count=@($rows|Where-Object{-not$_.dwm_parent_evidence_available}).Count
    groups=$groups;runs=$rows
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2-D0 presentation path: PASS verdict=$verdict runs=$($rows.Count)"
