param(
    [Parameter(Mandatory=$true)][ValidateSet('Good','MissingWaiting','Order','ParentMismatch','Lost','EarlierIncomplete')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$samples=@(1..200|ForEach-Object{[ordered]@{ordinal=$_;qpc=$_}})
$app=[ordered]@{presentation_opportunity=[ordered]@{qpc_frequency=1000;physical_vblank=[ordered]@{window_output_start=[ordered]@{refresh_numerator=1000;refresh_denominator=1};samples=$samples}}}
function Record([int]$Sequence,[int]$Start,[int]$Wait,[int]$Parent,[int]$Display,[int]$Completion,[int]$Frame){
    return [ordered]@{
        sequence_index=$Sequence;present_serial="p$Sequence";present_mode='Composed_Flip'
        completion_class='PRESENTED';discard_reason='NONE';etw_present_start_qpc=$Start
        waiting_for_dwm_qpc=$Wait;attached_to_dwm_parent_qpc=$Parent
        attached_dwm_parent_present_start_qpc=$Parent;dwm_parent_displayed_qpc=$Display
        dwm_parent_completion_qpc=$Completion;dependent_finalized_qpc=$Completion
        earlier_superseded_by_present_start_qpc=0;earlier_superseded_qpc=0
        dependency_batch_present_start_qpc=$Parent;output_frame=$Frame;actual_opportunity_ordinals=@($Display)
    }
}
$oracle=[ordered]@{
    schema='mvm-p2-c0-native-etw-oracle-1';oracle_status='ORACLE_VALID';dependency_lifecycle_diagnostic=$true
    incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0
    records=@((Record 0 10 11 20 30 31 10),(Record 1 31 32 40 41 42 21))
}
switch($Case){
    'MissingWaiting'{$oracle.records[0].Remove('waiting_for_dwm_qpc')}
    'Order'{$oracle.records[0].waiting_for_dwm_qpc=21}
    'ParentMismatch'{$oracle.records[0].attached_dwm_parent_present_start_qpc=19}
    'Lost'{$oracle.lost_count=1}
    'EarlierIncomplete'{
        $record=$oracle.records[0];$record.completion_class='DISCARDED';$record.discard_reason='EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED'
        $record.dwm_parent_displayed_qpc=0;$record.dwm_parent_completion_qpc=0;$record.earlier_superseded_by_present_start_qpc=40;$record.earlier_superseded_qpc=0
    }
}
$appPath=Join-Path $Directory 'app.json';$oraclePath=Join-Path $Directory 'oracle.json';$output=Join-Path $Directory 'proof.json'
$app|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $appPath -Encoding utf8
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $oraclePath -Encoding utf8
& pwsh -NoProfile -File $Checker -AppJson $appPath -OracleJson $oraclePath -Output $output *> (Join-Path $Directory 'checker.txt')
$exit=$LASTEXITCODE
if($Case-eq'Good'){
    if($exit-ne0-or-not(Test-Path -LiteralPath $output)){throw "正のlifecycle契約が失敗しました: $exit"}
}elseif($exit-eq0){throw "壊したlifecycle契約が通過しました: $Case"}
Write-Host "F3-C3-A2 lifecycle contract: PASS ($Case)"
