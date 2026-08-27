param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','ParentMissing','ReadyZero','DisplayMismatch','DwmLost','MultipleDwmPid','LifecycleOrder')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Analyzer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$canonical=Join-Path $Directory 'canonical';New-Item -ItemType Directory -Path $canonical|Out-Null
$samples=@(1..4000|ForEach-Object{[ordered]@{ordinal=$_;qpc=$_}})
$app=[ordered]@{presentation_opportunity=[ordered]@{
    measurement_start_qpc=1;measurement_end_qpc_exclusive=4000;qpc_frequency=1000
    physical_vblank=[ordered]@{window_output_start=[ordered]@{refresh_numerator=1000;refresh_denominator=1};samples=$samples}
}}
$rawEvents=@()
foreach($parent in @(10,110,210)){
    $rawEvents+=[ordered]@{
        present_start_qpc=$parent;process_id=200;ready_qpc=$parent
        displayed=@([ordered]@{frame_type='NotSet';qpc=$parent+1})
        is_lost=$false;completion_class='PRESENTED';present_mode='Hardware_Legacy_Flip'
    }
}
$records=@();$sequence=0
foreach($spec in @(@{parent=10;count=1},@{parent=110;count=100},@{parent=210;count=100})){
    for($index=0;$index-lt$spec.count;++$index){
        $records+=[ordered]@{
            sequence_index=$sequence++;attached_dwm_parent_present_start_qpc=$spec.parent
            waiting_for_dwm_qpc=$spec.parent-1;attached_to_dwm_parent_qpc=$spec.parent
            dwm_parent_displayed_qpc=$spec.parent+1;dwm_parent_completion_qpc=$spec.parent+2
            completion_class=$(if($index-eq$spec.count-1){'PRESENTED'}else{'DISCARDED'})
        }
    }
}
switch($Case){
    'ParentMissing'{$rawEvents=@($rawEvents|Where-Object{[int64]$_.present_start_qpc-ne110})}
    'ReadyZero'{$rawEvents[1].ready_qpc=0}
    'DisplayMismatch'{$rawEvents[1].displayed[0].qpc=112}
    'DwmLost'{$rawEvents[1].is_lost=$true;$rawEvents[1].completion_class='LOST'}
    'MultipleDwmPid'{$rawEvents[1].process_id=201}
    'LifecycleOrder'{$records[1].attached_to_dwm_parent_qpc=109}
}
$raw=[ordered]@{
    schema='mvm-p2-etw-present-history-1';dependency_lifecycle_diagnostic=$true
    target_process_id=100;events=$rawEvents
}
$oracle=[ordered]@{
    oracle_status='ORACLE_VALID';display_completion_status='CLOSED'
    incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    records=$records
}
$summary=[ordered]@{c0_r2_status='PASS';submission_mode='CONTROL'}
$app|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'traced-app.json') -Encoding utf8
$raw|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'present-history-raw.json') -Encoding utf8
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'oracle.json') -Encoding utf8
$summary|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $canonical 'summary.json') -Encoding utf8
'synthetic contract fixture'|Set-Content -LiteralPath (Join-Path $canonical 'manifest.sha256') -Encoding ascii
$output=Join-Path $Directory 'output'
& pwsh -NoProfile -File $Analyzer -CanonicalDirectory $canonical -OutputDirectory $output *> (Join-Path $Directory 'analyzer.txt')
$exit=$LASTEXITCODE
if($Case-eq'Good'){
    if($exit-ne0-or-not(Test-Path -LiteralPath (Join-Path $output 'summary.json'))){throw "正のA3-T0契約が失敗しました: $exit"}
    $result=Get-Content -LiteralPath (Join-Path $output 'summary.json') -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$result.verdict-ne'DWM_WIDE_PARENT_PRESENTSTART_GAP'){throw "A3-T0分岐が不正です: $($result.verdict)"}
}elseif($exit-eq0){throw "壊したA3-T0契約が通過しました: $Case"}
Write-Host "F3-C3-A3-T0 contract: PASS ($Case)"
