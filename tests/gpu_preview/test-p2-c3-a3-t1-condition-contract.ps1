param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('GoodVisible','GoodOccluded','GoodDirty','NegativeVisibility','NegativeCloak',
        'NegativeCoverage','NegativeUnexpected','NegativeRectChange','NegativeDirtyTicks','NegativeDwmIdentity',
        'NegativeEtwLoss','GoodTargetInvalidate','GoodTargetRedrawNow','NegativeUserInput','NegativeTargetDamage',
        'NegativeDamageFailure')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$canonical=Join-Path $Directory 'canonical';New-Item -ItemType Directory -Path $canonical|Out-Null
$mode=if($Case-eq'GoodOccluded'-or$Case-eq'NegativeCoverage'){'FULLY_OCCLUDED'}
      elseif($Case-eq'GoodDirty'-or$Case-eq'NegativeDirtyTicks'){'VISIBLE_UNOCCLUDED_FORCE_DIRTY'}
      elseif($Case-eq'GoodTargetInvalidate'-or$Case-eq'NegativeTargetDamage'-or$Case-eq'NegativeDamageFailure'){'VISIBLE_UNOCCLUDED_TARGET_INVALIDATE'}
      elseif($Case-eq'GoodTargetRedrawNow'){'VISIBLE_UNOCCLUDED_TARGET_REDRAW_NOW'}
      else{'VISIBLE_UNOCCLUDED'}
$vblankSamples=@(1..1200|ForEach-Object{[ordered]@{ordinal=$_; qpc=$_}})
$app=[ordered]@{presentation_opportunity=[ordered]@{
    measurement_start_qpc=100;measurement_end_qpc_exclusive=1100;qpc_frequency=1000
    physical_vblank=[ordered]@{window_output_start=[ordered]@{refresh_numerator=1000;refresh_denominator=1};samples=$vblankSamples}
}}
$rawEvents=@()
foreach($qpc in 100..1099){
    $rawEvents+=[ordered]@{present_start_qpc=$qpc;process_id=200;ready_qpc=$qpc;displayed=@([ordered]@{qpc=$qpc});is_lost=$false;completion_class='PRESENTED';present_mode='Hardware_Legacy_Flip';window_handle='0x0';swap_chain_address='0x0'}
}
$fallback=$mode-eq'FULLY_OCCLUDED'
$records=@()
if($fallback){
    $records=@([ordered]@{attached_dwm_parent_present_start_qpc=0;completion_class='PRESENTED'})
}else{
    foreach($qpc in @(100,200,300,400,500,600,700,800,900,1000)){$records+=[ordered]@{attached_dwm_parent_present_start_qpc=$qpc;completion_class='PRESENTED'}}
}
if($Case-eq'NegativeDwmIdentity'){$rawEvents[100].process_id=201}
$raw=[ordered]@{schema='mvm-p2-etw-present-history-1';target_process_id=100;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;events=$rawEvents}
if($Case-eq'NegativeEtwLoss'){$raw.etw_events_lost=1}
$oracle=[ordered]@{oracle_status='ORACLE_VALID';display_completion_status='CLOSED';incomplete_unknown_count=0;lost_count=0;etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;records=$records}
$targetDamageModes=@('VISIBLE_UNOCCLUDED_TARGET_INVALIDATE','VISIBLE_UNOCCLUDED_TARGET_REDRAW_NOW')
$samples=@()
for($index=0;$index-lt10;++$index){
    $dirtyTick=if($mode-eq'VISIBLE_UNOCCLUDED_FORCE_DIRTY'){$index*10}else{0}
    $damageTick=if($targetDamageModes-contains$mode){$index*10}else{0}
    $samples+=[ordered]@{
        qpc=100+$index*100;target_hwnd='0x1234';visible=$true;iconic=$false;topmost=$true;cloaked=0
        window_rect=[ordered]@{left=10;top=10;right=116;bottom=139};client_rect=[ordered]@{left=13;top=36;right=113;bottom=136}
        monitor='0x1';foreground_hwnd='0x1234';occluder_hwnd=$(if($mode-eq'FULLY_OCCLUDED'){'0x5678'}else{'0x0'})
        occluder_rect=[ordered]@{left=13;top=36;right=113;bottom=136};client_area=10000
        designated_intersection_area=$(if($mode-eq'FULLY_OCCLUDED'){10000}else{0});unexpected_intersection_area=0;dirty_tick_count=$dirtyTick
        target_damage_count=$damageTick;target_damage_failure_count=0
        target_update_region_present=$($damageTick-gt0);last_input_tick=4242
    }
}
switch($Case){
    'NegativeVisibility'{$samples[4].visible=$false}
    'NegativeCloak'{$samples[4].cloaked=1}
    'NegativeCoverage'{$samples[4].designated_intersection_area=9999}
    'NegativeUnexpected'{$samples[4].unexpected_intersection_area=1}
    'NegativeRectChange'{$samples[4].window_rect.left=11}
    'NegativeDirtyTicks'{foreach($sample in $samples){$sample.dirty_tick_count=0}}
    'NegativeUserInput'{$samples[4].last_input_tick=9999}
    'NegativeTargetDamage'{foreach($sample in $samples){$sample.target_damage_count=0}}
    'NegativeDamageFailure'{$samples[9].target_damage_failure_count=1}
}
$state=[ordered]@{schema='mvm-p2-c3-a3-t1-window-state-1';mode=$mode;target_process_id=100;qpc_frequency=1000;target_hwnd='0x1234';occluder_hwnd=$(if($mode-eq'FULLY_OCCLUDED'){'0x5678'}else{'0x0'});dirty_companion_hwnd=$(if($mode-eq'VISIBLE_UNOCCLUDED_FORCE_DIRTY'){'0x9abc'}else{'0x0'});dirty_tick_count=[long]$samples[-1].dirty_tick_count
    target_damage_count=[long]$samples[-1].target_damage_count;target_damage_failure_count=0
    target_update_region_observed_count=@($samples|Where-Object{[bool]$_.target_update_region_present}).Count
    last_input_tick_at_ready=4242;samples=$samples}
$submission=[ordered]@{schema='mvm-p2-c3-submission-backpressure-proof-2';proof_status='PASS';submission_mode='CONTROL';native_present_count=10;presented_count=10;discarded_count=0;discard_reason_histogram=[ordered]@{DEPENDENT_PRESENT_SUPERSEDED=0;EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED=0};dependency_batch_count=$(if($fallback){0}else{10});dependency_batch_size=[ordered]@{p50=$(if($fallback){0}else{1});p95=$(if($fallback){0}else{1});max=$(if($fallback){0}else{1});histogram=[ordered]@{}}}
$app|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'traced-app.json') -Encoding utf8
$raw|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'present-history-raw.json') -Encoding utf8
$oracle|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'oracle.json') -Encoding utf8
$statePath=Join-Path $Directory 'state.json';$state|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $statePath -Encoding utf8
$submissionPath=Join-Path $Directory 'submission.json';$submission|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $submissionPath -Encoding utf8
$output=Join-Path $Directory 'proof.json'
& pwsh -NoProfile -File $Checker -CanonicalDirectory $canonical -WindowStateJson $statePath -SubmissionProofJson $submissionPath -ExpectedMode $mode -Output $output *> (Join-Path $Directory 'checker.txt')
$exit=$LASTEXITCODE
if($Case-like'Good*'){
    if($exit-ne0-or-not(Test-Path -LiteralPath $output)){throw "正のT1 condition契約が失敗しました: $Case exit=$exit"}
}elseif($exit-eq0){throw "壊したT1 condition契約が通過しました: $Case"}
Write-Host "F3-C3-A3-T1 condition contract: PASS ($Case)"
