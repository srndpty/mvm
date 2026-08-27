param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeOracleOpportunity','NegativeDelayTouchesDisplayed','NegativeRecordOrder',
        'NegativeVBlankDeleted','NegativeCollisionClaim')][string]$Case,
    [Parameter(Mandatory=$true)][string]$CorpusChecker,
    [Parameter(Mandatory=$true)][string]$OracleChecker,
    [Parameter(Mandatory=$true)][string]$VBlankChecker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Directory|Out-Null
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$source=Join-Path $Directory 'source';$corpus=Join-Path $Directory 'corpus'
New-Item -ItemType Directory -Force -Path $source,$corpus,(Join-Path $corpus 'cases')|Out-Null
$samples=@();for($i=0;$i-lt130;++$i){$samples+=[ordered]@{ordinal=$i;qpc=1000+100*$i}}
$identity=[ordered]@{available=$true;monitor_handle='1';output_index=0;adapter_luid_low=1
    adapter_luid_high=0;gdi_device_name='DISPLAY1';output_device_name='DISPLAY1'
    refresh_numerator=10;refresh_denominator=1;desktop_left=0;desktop_top=0
    desktop_right=1920;desktop_bottom=1080}
$swaps=@();$events=@()
for($i=0;$i-lt7;++$i){
    $swaps+=[ordered]@{swap_qpc=1150+100*$i;swap_ordinal=$i;completed_render_ordinal=$i
        submitted_render_ordinal=$i;presented_output_frame=10+$i}
    $events+=[ordered]@{sequence_index=$i;present_start_qpc=1140+100*$i;process_id=42
        thread_id=7;swap_chain_address='0xabc';window_handle='0x123';sync_interval=1
        present_flags=0;displayed=@([ordered]@{frame_type='Application';qpc=1100+100*$i})
        present_ids=@([ordered]@{vidpn_layer_id='0x1';present_id=100+$i})}
}
$app=[ordered]@{schema='mvm-p2-formal-2';presentation_opportunity=[ordered]@{
    enabled=$true;measurement_start_qpc=1050;measurement_end_qpc_exclusive=1850
    qpc_frequency=1000;swap_record_count=7;swap_overflow_count=0
    physical_vblank=[ordered]@{enabled=$true;observer_started=$true;observer_error=''
        time_critical_priority=$true;window_output_start=$identity;window_output_end=$identity
        window_output_stable=$true;sample_count=130;ring_overflow_count=0;wait_failure_count=0
        sequence_status='OK';interval_report_ok=$true;interval_count=129;long_interval_count=0
        short_interval_count=0;nominal_period_qpc=100;cumulative_consistent=$true;samples=$samples}
    swap_records=$swaps}}
$etw=[ordered]@{schema='mvm-p2-etw-present-history-1';raw_displayed_qpc=$true;qpc_frequency=1000
    target_process_id=42;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    collision_evidence_mode='ACTUAL_QT';cadence_diagnostic=[ordered]@{
        traced_swaps_per_second=10.0;baseline_swaps_per_second=10.0;ratio=1.0;extreme_change=$false}
    events=$events}
$appPath=Join-Path $source 'traced-app.json';$etwPath=Join-Path $source 'present-history-raw.json'
$app|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $appPath -Encoding utf8
$etw|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $etwPath -Encoding utf8
$sourceOraclePath=Join-Path $corpus 'source-oracle.json'
& pwsh -NoProfile -File $OracleChecker -AppJson $appPath -EtwJson $etwPath `
    -Output $sourceOraclePath -VBlankChecker $VBlankChecker *> $null
if($LASTEXITCODE-ne0){throw 'test source oracleを生成できません'}
$sourceOracle=Get-Content -LiteralPath $sourceOraclePath -Raw|ConvertFrom-Json

function Write-Case([string]$Id,[int[]]$Submissions,[int]$SampleStart,[int]$SampleCount,
                    [int64[]]$Synthetic,[string]$Expected){
    $caseDir=Join-Path (Join-Path $corpus 'cases') $Id;New-Item -ItemType Directory -Force -Path $caseDir|Out-Null
    $visibleSamples=@();for($i=0;$i-lt$SampleCount;++$i){$sourceIndex=$SampleStart+$i
        $visibleSamples+=[ordered]@{source_sample_index=$sourceIndex;ordinal=$samples[$sourceIndex].ordinal;qpc=$samples[$sourceIndex].qpc}}
    $visible=@();$hidden=@();for($i=0;$i-lt$Submissions.Count;++$i){$s=$Submissions[$i];$bracket=$null
        for($j=0;$j+1-lt$visibleSamples.Count;++$j){if($visibleSamples[$j].qpc-le$Synthetic[$i]-and$Synthetic[$i]-lt$visibleSamples[$j+1].qpc){$bracket=$visibleSamples[$j].ordinal;break}}
        $visible+=[ordered]@{submission_index=$s;original_callback_qpc=$swaps[$s].swap_qpc
            synthetic_callback_qpc=$Synthetic[$i];synthetic_delay_ticks=$Synthetic[$i]-$swaps[$s].swap_qpc
            synthetic_callback_bracket=$bracket}
        $sourceRecord=$sourceOracle.records[$s]
        $hidden+=[ordered]@{submission_index=$s;status=$sourceRecord.status
            present_ids=@($sourceRecord.present_ids);displayed=@($sourceRecord.displayed)
            actual_physical_opportunity_ordinal=$sourceRecord.first_opportunity_ordinal}
    }
    $n=$Submissions.Count;$m=$SampleCount-1;$solutions=if($n-gt$m){0}elseif($n-eq$m){1}else{3}
    $mapper=[ordered]@{schema='mvm-p2-r3-mapper-input-1';case_id=$Id;sync_interval=1
        qpc_frequency=1000;source_app_sha256=Hash $appPath;source_etw_sha256=Hash $etwPath
        construction_model='STRICT_MONOTONE_INJECTIVE_OVER_VISIBLE_OPPORTUNITIES'
        mapper_visible_opportunity_count=$m;vblank_samples=$visibleSamples;records=$visible}
    $oracleCase=[ordered]@{schema='mvm-p2-r3-hidden-oracle-1';case_id=$Id
        expected_solution_class=$Expected;expected_solution_count=$solutions;family='TEST'
        position='TEST';source_record_start=$Submissions[0];source_record_count=$n
        source_vblank_start_index=$SampleStart;source_vblank_sample_count=$SampleCount
        target_synthetic_bracket=$visible[0].synthetic_callback_bracket
        oracle_fields_excluded_from_mapper_input=$true;records=$hidden}
    $mapperPath=Join-Path $caseDir 'mapper-input.json';$hiddenPath=Join-Path $caseDir 'hidden-oracle.json'
    $mapper|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $mapperPath -Encoding utf8
    $oracleCase|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $hiddenPath -Encoding utf8
    return [ordered]@{case_id=$Id;family='TEST';position='TEST';expected_solution_class=$Expected
        record_count=$n;opportunity_count=$m;mapper_input="cases/$Id/mapper-input.json"
        hidden_oracle="cases/$Id/hidden-oracle.json";mapper_input_sha256=Hash $mapperPath
        hidden_oracle_sha256=Hash $hiddenPath}
}
$entries=@()
$entries+=Write-Case 'unique' @(0,1) 1 3 @(1260,1270) 'UNIQUE'
$entries+=Write-Case 'ambiguous' @(2,3) 3 4 @(1560,1570) 'AMBIGUOUS'
$entries+=Write-Case 'no-solution' @(4,5,6) 6 3 @(1760,1770,1780) 'NO_SOLUTION'
$index=[ordered]@{schema='mvm-p2-r3-synthetic-collision-corpus-1';authority='diagnostic_only'
    corpus_status='GENERATED_NOT_YET_CHECKED';mapper_proof_status='NOT_YET_EVALUABLE';mapper_changed=$false
    source_hashes=[ordered]@{manifest='test';app=Hash $appPath;etw=Hash $etwPath}
    case_count=3;cases=$entries}
$indexPath=Join-Path $corpus 'corpus-index.json'
$index|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $indexPath -Encoding utf8

$targetEntry=$index.cases[0]
switch($Case){
    'NegativeOracleOpportunity'{
        $path=Join-Path $corpus $targetEntry.hidden_oracle.Replace('/','\');$value=Get-Content $path -Raw|ConvertFrom-Json
        $value.records[0].actual_physical_opportunity_ordinal++;$value|ConvertTo-Json -Depth 20|Set-Content $path -Encoding utf8
        $targetEntry.hidden_oracle_sha256=Hash $path}
    'NegativeDelayTouchesDisplayed'{
        $path=Join-Path $corpus $targetEntry.hidden_oracle.Replace('/','\');$value=Get-Content $path -Raw|ConvertFrom-Json
        $value.records[0].displayed[0].qpc++;$value|ConvertTo-Json -Depth 20|Set-Content $path -Encoding utf8
        $targetEntry.hidden_oracle_sha256=Hash $path}
    'NegativeRecordOrder'{
        $path=Join-Path $corpus $targetEntry.mapper_input.Replace('/','\');$value=Get-Content $path -Raw|ConvertFrom-Json
        $tmp=$value.records[0];$value.records[0]=$value.records[1];$value.records[1]=$tmp
        $value|ConvertTo-Json -Depth 20|Set-Content $path -Encoding utf8;$targetEntry.mapper_input_sha256=Hash $path}
    'NegativeVBlankDeleted'{
        $path=Join-Path $corpus $targetEntry.mapper_input.Replace('/','\');$value=Get-Content $path -Raw|ConvertFrom-Json
        $value.vblank_samples=@($value.vblank_samples[0],$value.vblank_samples[2])
        $value|ConvertTo-Json -Depth 20|Set-Content $path -Encoding utf8;$targetEntry.mapper_input_sha256=Hash $path}
    'NegativeCollisionClaim'{
        $path=Join-Path $corpus $targetEntry.mapper_input.Replace('/','\');$value=Get-Content $path -Raw|ConvertFrom-Json
        $value.records[1].synthetic_callback_qpc=1310;$value.records[1].synthetic_delay_ticks=60
        $value.records[1].synthetic_callback_bracket=3
        $value|ConvertTo-Json -Depth 20|Set-Content $path -Encoding utf8;$targetEntry.mapper_input_sha256=Hash $path}
}
$index|ConvertTo-Json -Depth 20|Set-Content -LiteralPath $indexPath -Encoding utf8
& pwsh -NoProfile -File $CorpusChecker -CorpusDirectory $corpus -SourceArtifactDirectory $source `
    -OracleChecker $OracleChecker -VBlankChecker $VBlankChecker *> $null
$exitCode=$LASTEXITCODE
if($Case-eq'Good'){
    if($exitCode-ne0){throw "対照群が失敗しました: $exitCode"}
}elseif($exitCode-eq0){throw "mutationをcheckerが受理しました: $Case"}

