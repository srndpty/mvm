[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ComposedNotFailure','BadWithoutGoodComposed','NoBadRegime','ParentFieldUnavailable',
                 'ModeClassBoundary','NegativeMissingPresentMode')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# 合成raw。ModeSequenceはPresent1件ごとのmode名。
function New-Run([string]$Name,[string[]]$ModeSequence,[int]$Presented,[bool]$Parent,
                 [bool]$OmitParentField,[bool]$OmitPresentMode){
    $runDirectory=Join-Path $Directory $Name
    New-Item -ItemType Directory -Path $runDirectory|Out-Null
    $events=@()
    for($index=0;$index-lt$ModeSequence.Count;++$index){
        $isPresented=$index-lt$Presented
        $event=[ordered]@{
            sequence_index=$index;present_start_qpc=1+$index;process_id=100;thread_id=1
            swap_chain_address='0x1';window_handle='0x0';sync_interval=1;present_flags=0
            final_state=$(if($isPresented){'Presented'}else{'Discarded'})
            discard_reason=$(if($isPresented){'NONE'}else{'DEPENDENT_PRESENT_SUPERSEDED'})
            completion_class=$(if($isPresented){'PRESENTED'}else{'DISCARDED'})
            is_completed=$true;is_lost=$false
            seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true
            wait_for_flip_event=$false;wait_for_mpo_flip_event=$false
            time_in_present_qpc=10;ready_qpc=1+$index;queue_submit_sequence=$index
            composition_surface_luid='0x0';win32k_present_count=0;win32k_bind_id=0
            dxgk_present_history_token='0x0';dxgk_present_history_token_data='0x0'
            displayed=$(if($isPresented){@(@{frame_type='NotSet';qpc=1000+$index})}else{@()})
            present_ids=@()
        }
        if(-not$OmitPresentMode){$event.present_mode=$ModeSequence[$index]}
        if(-not$OmitParentField){
            $parentQpc=if($Parent-and$isPresented){2000+$index*10}else{0}
            $event.attached_dwm_parent_present_start_qpc=$parentQpc
            $event.dwm_parent_displayed_qpc=$parentQpc
            $event.dependency_batch_present_start_qpc=$parentQpc
        }
        $events+=$event
    }
    [ordered]@{schema='mvm-p2-etw-present-history-1';presentmon_commit='deadbeef'
        acquisition_mode='CANONICAL_PRESENTMON_LIVE';qpc_frequency=1000;target_process_id=100
        etw_events_lost=0;etw_buffers_lost=0;present_event_overflow_count=0;events=$events}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $runDirectory 'present-history-raw.json') -Encoding utf8
    [ordered]@{presentation_opportunity=[ordered]@{measurement_start_qpc=1
        measurement_end_qpc_exclusive=1+$ModeSequence.Count;qpc_frequency=1000}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'traced-app.json') -Encoding utf8
    return $runDirectory
}
function Modes([string]$Mode,[int]$Count){,@(1..$Count|ForEach-Object{$Mode})}

$independent='Hardware_Composed_Independent_Flip'
$composed='Composed_Flip'
$specs=@()
switch($Case){
    'ComposedNotFailure'{
        $specs+="good-indep|CURRENT|$(New-Run 'good-indep' (Modes $independent 100) 100 $false $false $false)"
        $specs+="good-composed|HIST|$(New-Run 'good-composed' (Modes $composed 100) 100 $true $false $false)"
        $specs+="bad-composed|HIST|$(New-Run 'bad-composed' (Modes $composed 100) 5 $true $false $false)"
    }
    'BadWithoutGoodComposed'{
        $specs+="good-indep|CURRENT|$(New-Run 'good-indep' (Modes $independent 100) 100 $false $false $false)"
        $specs+="bad-composed|HIST|$(New-Run 'bad-composed' (Modes $composed 100) 5 $true $false $false)"
    }
    'NoBadRegime'{
        $specs+="good-indep|CURRENT|$(New-Run 'good-indep' (Modes $independent 100) 100 $false $false $false)"
        $specs+="good-composed|HIST|$(New-Run 'good-composed' (Modes $composed 100) 100 $true $false $false)"
    }
    'ParentFieldUnavailable'{
        $specs+="good-indep|CURRENT|$(New-Run 'good-indep' (Modes $independent 100) 100 $false $false $false)"
        $specs+="legacy-bad|HIST|$(New-Run 'legacy-bad' (Modes $composed 100) 5 $true $true $false)"
    }
    'ModeClassBoundary'{
        $sequence=@((Modes $independent 40)+(Modes $composed 60))
        $specs+="transition|HIST|$(New-Run 'transition' $sequence 50 $true $false $false)"
    }
    'NegativeMissingPresentMode'{
        $specs+="nomode|CURRENT|$(New-Run 'nomode' (Modes $independent 100) 100 $false $false $true)"
    }
}
$manifest=Join-Path $Directory 'run-set.manifest'
$specs|Set-Content -LiteralPath $manifest -Encoding utf8
$output=Join-Path $Directory 'inventory.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Inventory -RunSpecFile $manifest -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-eq'NegativeMissingPresentMode'){
    if(-not$failed){throw 'PresentModeを欠くrawを受理しました'}
    Write-Host 'F3-C3-A3-T2-D1-A contract: PASS (NegativeMissingPresentMode rejected)'
    exit 0
}
if($failed){throw "D1-A inventoryが失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
switch($Case){
    'ComposedNotFailure'{
        if([string]$proof.verdict-ne'COMPOSED_MODE_IS_NOT_THE_FAILURE_CAUSE'){throw "verdictが不正です: $($proof.verdict)"}
        if([long]$proof.regime_counts.GOOD_COMPOSED-ne1-or[long]$proof.regime_counts.BAD_SPARSE_COMPOSED-ne1){throw 'regime countsが不正です'}
    }
    'BadWithoutGoodComposed'{
        if([string]$proof.verdict-ne'BAD_REGIME_PRESENT_WITHOUT_GOOD_COMPOSED_REFERENCE'){throw "verdictが不正です: $($proof.verdict)"}
    }
    'NoBadRegime'{
        if([string]$proof.verdict-ne'NO_BAD_REGIME_IN_RUN_SET'){throw "verdictが不正です: $($proof.verdict)"}
    }
    'ParentFieldUnavailable'{
        $legacy=$proof.runs|Where-Object label -eq 'legacy-bad'
        if($null-ne$legacy.dwm_parent_cadence){throw 'parent field欠損時にcadenceをnullにしていません'}
        if([bool]$legacy.field_availability.attached_dwm_parent){throw 'availabilityがtrueになっています'}
        if([long]$proof.dwm_parent_comparable_run_count-ne1){throw 'comparable run countが不正です'}
    }
    'ModeClassBoundary'{
        $run=$proof.runs|Where-Object label -eq 'transition'
        if([string]$run.mode_history-ne'MODE_CLASSIFICATION_CHANGED'){throw 'mode historyが不正です'}
        if([long]$run.observed_mode_class_boundary.index-ne40){throw "boundary indexが不正です: $($run.observed_mode_class_boundary.index)"}
        if([bool]$run.composed_from_first_observed_present){throw 'composed_from_first_observed_presentがtrueです'}
        if([string]$run.mode_transition_provenance-ne'UNAVAILABLE_FINAL_MODE_ONLY'){throw 'transition provenanceが不正です'}
    }
}
Write-Host "F3-C3-A3-T2-D1-A contract: PASS ($Case)"
