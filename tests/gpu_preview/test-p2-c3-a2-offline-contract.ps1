param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','NegativeModeMissing','NegativeBatchZero','NegativeLost','NegativeOrder')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Analyzer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$orders=@(
    @('CONTROL','FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT'),
    @('FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT','CONTROL'),
    @('DWM_FLUSH_AFTER_PRESENT','CONTROL','FRAME_LATENCY_1'))
$pathByMode=@{CONTROL='control';FRAME_LATENCY_1='frame_latency_1';DWM_FLUSH_AFTER_PRESENT='dwm_flush_after_present'}
$sources=@()
for($run=0;$run-lt3;++$run){
    $root=Join-Path $Directory "source-$run";New-Item -ItemType Directory -Force -Path $root|Out-Null;$sources+=$root
    $order=@($orders[$run]);if($Case-eq'NegativeOrder'-and$run-eq0){$order=@($orders[1])}
    [ordered]@{execution_order=$order}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $root 'summary.json') -Encoding utf8
    foreach($mode in $pathByMode.Keys){
        $canonical=Join-Path $root $pathByMode[$mode];New-Item -ItemType Directory -Force -Path $canonical|Out-Null
        $records=@(
            [ordered]@{sequence_index=0;present_serial='1';etw_present_start_qpc=110;present_mode='Composed_Flip';completion_class='PRESENTED';dependency_batch_present_start_qpc=150;displayed_qpc=@(200);actual_opportunity_ordinals=@(1);output_frame=0;enter_bracket_ordinal=0},
            [ordered]@{sequence_index=1;present_serial='2';etw_present_start_qpc=210;present_mode='Composed_Flip';completion_class='DISCARDED';dependency_batch_present_start_qpc=250;displayed_qpc=@();actual_opportunity_ordinals=@();output_frame=1;enter_bracket_ordinal=1},
            [ordered]@{sequence_index=2;present_serial='3';etw_present_start_qpc=220;present_mode='Composed_Flip';completion_class='PRESENTED';dependency_batch_present_start_qpc=250;displayed_qpc=@(400);actual_opportunity_ordinals=@(3);output_frame=2;enter_bracket_ordinal=1})
        $lost=0
        if($Case-eq'NegativeModeMissing'-and$run-eq0-and$mode-eq'CONTROL'){$records[0].present_mode=''}
        if($Case-eq'NegativeBatchZero'-and$run-eq0-and$mode-eq'CONTROL'){$records[0].dependency_batch_present_start_qpc=0}
        if($Case-eq'NegativeLost'-and$run-eq0-and$mode-eq'CONTROL'){$lost=1}
        [ordered]@{oracle_status='ORACLE_VALID';display_completion_status='CLOSED';incomplete_unknown_count=0;lost_count=$lost;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;presented_count=2;discarded_count=1;records=$records}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'oracle.json') -Encoding utf8
        $samples=@(0..5|ForEach-Object{[ordered]@{ordinal=$_;qpc=100+100*$_}})
        [ordered]@{presentation_opportunity=[ordered]@{physical_vblank=[ordered]@{samples=$samples}}}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $canonical 'traced-app.json') -Encoding utf8
    }
}
$output=Join-Path $Directory 'output'
$actual=0
try{& $Analyzer -SourceDirectories $sources -OutputDirectory $output *> $null}catch{$actual=1}
$expected=if($Case-eq'Good'){0}else{1}
if($actual-ne$expected){throw "$Case F3-C3-A2 offline contract exitが不正です: expected=$expected actual=$actual"}
if($Case-eq'Good'){
    $summary=Get-Content -LiteralPath (Join-Path $output 'summary.json') -Raw|ConvertFrom-Json
    if(-not$summary.all_runs_single_present_mode-or$summary.large_parent_display_gap_comparable_count-ne9-or$summary.large_parent_display_gap_mismatch_count-ne0){throw 'F3-C3-A2 good summaryが不正です'}
}
Write-Host "F3-C3-A2 offline $Case contract test: PASS"
