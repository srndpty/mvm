[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('IndependentOfDwm','ExternalTransition','UnknownPresentPath','LegacySchemaUnresolved',
                 'InsufficientGroups','NegativeEtwLoss')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Analyzer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# 合成rawを作る。qpcは1始まりでmeasurement windowは[1, count+1)。
function New-Run([string]$Name,[int]$Count,[int]$Presented,[string]$Mode,[bool]$Parent,
                 [bool]$LegacySchema,[bool]$DropDisplayed,[int]$DwmWide){
    $directory=Join-Path $Directory $Name
    New-Item -ItemType Directory -Path $directory|Out-Null
    $events=@()
    for($index=0;$index-lt$Count;++$index){
        $isPresented=$index-lt$Presented
        $displayed=if($isPresented-and-not$DropDisplayed){@(@{frame_type='NotSet';qpc=1000+$index})}else{@()}
        $event=[ordered]@{
            sequence_index=$index;present_start_qpc=1+$index;process_id=100;thread_id=1
            swap_chain_address='0x1';window_handle='0x0';sync_interval=1;present_flags=0
            final_state=$(if($isPresented){'Presented'}else{'Discarded'})
            discard_reason=$(if($isPresented){'NONE'}else{'DEPENDENT_PRESENT_SUPERSEDED'})
            completion_class=$(if($isPresented){'PRESENTED'}else{'DISCARDED'})
            is_completed=$true;is_lost=$false;present_mode=$Mode
            seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true
            wait_for_flip_event=$false;wait_for_mpo_flip_event=$false
            time_in_present_qpc=10;ready_qpc=1+$index;queue_submit_sequence=$index
            composition_surface_luid='0x0';win32k_present_count=0;win32k_bind_id=0
            dxgk_present_history_token='0x0';dxgk_present_history_token_data='0x0'
            displayed=$displayed;present_ids=@()
        }
        if(-not$LegacySchema){
            $parentQpc=if($Parent-and$isPresented){2000+$index}else{0}
            $event.attached_dwm_parent_present_start_qpc=$parentQpc
            $event.dwm_parent_displayed_qpc=$parentQpc
            $event.attached_to_dwm_parent_qpc=$parentQpc
            $event.waiting_for_dwm_qpc=$parentQpc
            $event.dwm_parent_completion_qpc=$parentQpc
            $event.dependency_batch_present_start_qpc=$parentQpc
            $event.dependent_finalized_qpc=$parentQpc
            $event.earlier_superseded_by_present_start_qpc=0
            $event.earlier_superseded_qpc=0
        }
        $events+=$event
    }
    for($index=0;$index-lt$DwmWide;++$index){
        $dwm=[ordered]@{
            sequence_index=1000+$index;present_start_qpc=1+$index;process_id=200;thread_id=2
            swap_chain_address='0x0';window_handle='0x0';sync_interval=1;present_flags=0
            final_state='Presented';discard_reason='NONE';completion_class='PRESENTED'
            is_completed=$true;is_lost=$false;present_mode='Hardware_Legacy_Flip'
            seen_dxgk_present=$true;seen_win32k_events=$true;seen_in_frame_event=$true
            wait_for_flip_event=$false;wait_for_mpo_flip_event=$false
            time_in_present_qpc=10;ready_qpc=1+$index;queue_submit_sequence=$index
            composition_surface_luid='0x0';win32k_present_count=0;win32k_bind_id=0
            dxgk_present_history_token='0x0';dxgk_present_history_token_data='0x0'
            displayed=@(@{frame_type='NotSet';qpc=1000+$index});present_ids=@()
        }
        if(-not$LegacySchema){
            $dwm.attached_dwm_parent_present_start_qpc=0;$dwm.dwm_parent_displayed_qpc=0
            $dwm.attached_to_dwm_parent_qpc=0;$dwm.waiting_for_dwm_qpc=0
            $dwm.dwm_parent_completion_qpc=0;$dwm.dependency_batch_present_start_qpc=0
            $dwm.dependent_finalized_qpc=0;$dwm.earlier_superseded_by_present_start_qpc=0
            $dwm.earlier_superseded_qpc=0
        }
        $events+=$dwm
    }
    $lost=if($Case-eq'NegativeEtwLoss'){1}else{0}
    [ordered]@{schema='mvm-p2-etw-present-history-1';qpc_frequency=1000;target_process_id=100
        etw_events_lost=$lost;etw_buffers_lost=0;present_event_overflow_count=0;events=$events}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'present-history-raw.json') -Encoding utf8
    [ordered]@{presentation_opportunity=[ordered]@{measurement_start_qpc=1
        measurement_end_qpc_exclusive=1+$Count;qpc_frequency=1000}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $directory 'traced-app.json') -Encoding utf8
    return $directory
}

$specs=@()
switch($Case){
    'IndependentOfDwm'{
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 0)"
        $specs+="ext|T2_EXTERNAL_DIRTY|$(New-Run 'ext' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 98)"
        $specs+="hist|HISTORICAL_DISCARD|$(New-Run 'hist' 100 10 'Composed_Flip' $true $false $false 12)"
    }
    'ExternalTransition'{
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 0)"
        $specs+="ext|T2_EXTERNAL_DIRTY|$(New-Run 'ext' 100 100 'Composed_Flip' $true $false $false 98)"
        $specs+="hist|HISTORICAL_DISCARD|$(New-Run 'hist' 100 10 'Composed_Flip' $true $false $false 12)"
    }
    'UnknownPresentPath'{
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $true 0)"
        $specs+="ext|T2_EXTERNAL_DIRTY|$(New-Run 'ext' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 98)"
        $specs+="hist|HISTORICAL_DISCARD|$(New-Run 'hist' 100 10 'Composed_Flip' $true $false $false 12)"
    }
    'LegacySchemaUnresolved'{
        # historicalがlegacy schemaのみだとparent evidenceを持つrunが無い。
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 0)"
        $specs+="ext|T2_EXTERNAL_DIRTY|$(New-Run 'ext' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 98)"
        $specs+="hist|HISTORICAL_DISCARD|$(New-Run 'hist' 100 10 'Composed_Flip' $true $true $false 12)"
    }
    'InsufficientGroups'{
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 0)"
    }
    'NegativeEtwLoss'{
        $specs+="quiet|T2_QUIET|$(New-Run 'quiet' 100 100 'Hardware_Composed_Independent_Flip' $false $false $false 0)"
    }
}
$manifest=Join-Path $Directory 'run-set.manifest'
$specs|Set-Content -LiteralPath $manifest -Encoding utf8
$output=Join-Path $Directory 'proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Analyzer -RunSpecFile $manifest -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-eq'NegativeEtwLoss'){
    if(-not$failed){throw 'ETW loss ありのrawを受理しました'}
    Write-Host 'F3-C3-A3-T2-D0 contract: PASS (NegativeEtwLoss rejected)'
    exit 0
}
if($failed){throw "D0 analyzerが失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
$expected=switch($Case){
    'IndependentOfDwm'      {'TARGET_DISPLAY_INDEPENDENT_OF_DWM_PRESENTSTART'}
    'ExternalTransition'    {'EXTERNAL_DIRTY_TRIGGERS_PRESENTATION_PATH_TRANSITION'}
    'UnknownPresentPath'    {'UNKNOWN_PRESENT_PATH_PRESENT'}
    'LegacySchemaUnresolved'{'PRESENTATION_PATH_NOT_SEPARATED'}
    'InsufficientGroups'    {'INSUFFICIENT_GROUPS'}
}
if([string]$proof.verdict-ne$expected){throw "verdictが一致しません: expected=$expected actual=$($proof.verdict)"}
if([string]$proof.analysis_mode-ne'OFFLINE_REANALYSIS_NO_NEW_ACQUISITION'){throw 'analysis modeが不正です'}
Write-Host "F3-C3-A3-T2-D0 contract: PASS ($Case -> $expected)"
