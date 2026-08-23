[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('OverlapReproducesBad','IndependentLossNotSufficient','OverlapNoPathChange',
                 'StickyAfterRemove','RecoveredAfterRemove','NegativeUserInput','NegativeEtwLoss')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Summarizer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

$independent='Hardware_Composed_Independent_Flip'
$composed='Composed_Flip'
# qpc配置: warmup [1,100) / measure [100,200)
$measureStart=100;$measureEnd=200

function New-Probe([int]$Index,[string]$Condition,[string]$WindowMode,[string]$PhaseSequence,
                   [hashtable]$PhaseSpec,[bool]$UserInput,[int]$EtwLost){
    $runDirectory=Join-Path $Directory ('probe-{0:d2}-{1}' -f $Index,$Condition.ToLowerInvariant())
    $canonical=Join-Path $runDirectory 'canonical'
    New-Item -ItemType Directory -Path $canonical|Out-Null
    $events=@();$qpc=1
    $boundaries=@()
    foreach($phase in ($PhaseSequence -split '->')){
        $boundaries+=[ordered]@{phase=$phase;qpc=$qpc}
        $spec=$PhaseSpec[$phase]
        for($i=0;$i-lt[int]$spec.count;++$i){
            $displayedOk=$i-lt[int]$spec.displayed
            $events+=[ordered]@{sequence_index=$events.Count;present_start_qpc=$qpc;process_id=100
                final_state=$(if($displayedOk){'Presented'}else{'Discarded'})
                present_mode=[string]$spec.mode
                attached_dwm_parent_present_start_qpc=$(if([string]$spec.mode-eq$composed){5}else{0})
                displayed=$(if($displayedOk){@(@{frame_type='NotSet';qpc=1000+$events.Count})}else{@()})}
            $qpc++
        }
        if($phase-eq'POST_REMOVE'-or$PhaseSequence-eq'OVERLAP'){$qpc=[Math]::Max($qpc,$measureStart)}
    }
    $measureSpec=$PhaseSpec['MEASURE']
    $qpc=$measureStart
    for($i=0;$i-lt[int]$measureSpec.count;++$i){
        $displayedOk=$i-lt[int]$measureSpec.displayed
        $events+=[ordered]@{sequence_index=$events.Count;present_start_qpc=$qpc;process_id=100
            final_state=$(if($displayedOk){'Presented'}else{'Discarded'})
            present_mode=[string]$measureSpec.mode
            attached_dwm_parent_present_start_qpc=$(if([string]$measureSpec.mode-eq$composed){5}else{0})
            displayed=$(if($displayedOk){@(@{frame_type='NotSet';qpc=2000+$events.Count})}else{@()})}
        $qpc++
    }
    [ordered]@{schema='mvm-p2-etw-present-history-1';qpc_frequency=1000;target_process_id=100
        etw_events_lost=$EtwLost;etw_buffers_lost=0;present_event_overflow_count=0;events=$events}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'present-history-raw.json') -Encoding utf8
    [ordered]@{presentation_opportunity=[ordered]@{measurement_start_qpc=$measureStart
        measurement_end_qpc_exclusive=$measureEnd;qpc_frequency=1000}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $canonical 'traced-app.json') -Encoding utf8
    [ordered]@{phase_boundaries=$boundaries}|ConvertTo-Json -Depth 6|
        Set-Content -LiteralPath (Join-Path $runDirectory 'window-state-raw.json') -Encoding utf8
    [ordered]@{status='PASS'
        window_state=[ordered]@{user_input_detected=$UserInput;phase_sequence=$PhaseSequence
            designated_expected_area=230400;occluder_process_id=51004}
        dwm_wide_present_start_count=0;target_attached_parent_count=0
        target_parent_present_start_gap=[ordered]@{max=0};dependency_batch_size=[ordered]@{max=0}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'condition-proof.json') -Encoding utf8
    [ordered]@{swapchain=[ordered]@{width=1920;height=1080;buffer_count=2;scaling=1;swap_effect=4;flags=64}
        capability=[ordered]@{hardware_composition_support_flags=3}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'preflight-proof.json') -Encoding utf8
    return [ordered]@{index=$Index;condition=$Condition;window_mode=$WindowMode
        directory=('probe-{0:d2}-{1}' -f $Index,$Condition.ToLowerInvariant())}
}
function Spec([string]$Mode,[int]$Count,[int]$Displayed){@{mode=$Mode;count=$Count;displayed=$Displayed}}

$good=Spec $independent 50 50
$goodComposed=Spec $composed 50 50
$badComposed=Spec $composed 50 2
$overlapMeasure=$good;$otrOverlap=$good;$otrMeasure=$good
switch($Case){
    'OverlapReproducesBad'        {$overlapMeasure=$badComposed}
    'IndependentLossNotSufficient'{$overlapMeasure=$goodComposed}
    'OverlapNoPathChange'         {}
    'StickyAfterRemove'           {$otrOverlap=$goodComposed;$otrMeasure=$goodComposed}
    'RecoveredAfterRemove'        {$otrOverlap=$goodComposed;$otrMeasure=$good}
}
$userInput=@{CLEAN_STATIC=$false;FOREIGN_WINDOW_OVERLAP=$false;OVERLAP_THEN_REMOVE=$false}
$etwLost=@{CLEAN_STATIC=0;FOREIGN_WINDOW_OVERLAP=0;OVERLAP_THEN_REMOVE=0}
if($Case-eq'NegativeUserInput'){$userInput.FOREIGN_WINDOW_OVERLAP=$true}
if($Case-eq'NegativeEtwLoss'){$etwLost.CLEAN_STATIC=1}

$runs=@()
$runs+=New-Probe 1 'CLEAN_STATIC' 'VISIBLE_UNOCCLUDED' 'PRE_CLEAN' `
    @{PRE_CLEAN=(Spec $independent 10 10);MEASURE=$good} $userInput.CLEAN_STATIC $etwLost.CLEAN_STATIC
$runs+=New-Probe 2 'FOREIGN_WINDOW_OVERLAP' 'FOREIGN_WINDOW_OVERLAP' 'OVERLAP' `
    @{OVERLAP=(Spec $independent 10 10);MEASURE=$overlapMeasure} `
    $userInput.FOREIGN_WINDOW_OVERLAP $etwLost.FOREIGN_WINDOW_OVERLAP
$runs+=New-Probe 3 'OVERLAP_THEN_REMOVE' 'OVERLAP_THEN_REMOVE' 'PRE_CLEAN->OVERLAP->POST_REMOVE' `
    @{PRE_CLEAN=(Spec $independent 10 10);OVERLAP=$otrOverlap;POST_REMOVE=(Spec $independent 5 5)
      MEASURE=$otrMeasure} $userInput.OVERLAP_THEN_REMOVE $etwLost.OVERLAP_THEN_REMOVE

[ordered]@{schema='mvm-p2-c3-a3-t2-d1b3a-probe-runs-1';status='PASS';runs=$runs}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Directory 'probe-runs.json') -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Summarizer -ProbeDirectory $Directory -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($Case-like'Negative*'){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
    Write-Host "F3-C3-A3-T2-D1-B3a contract: PASS ($Case rejected)"
    exit 0
}
if($failed){throw "D1-B3a summarizerが失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
$expectedVerdict=switch($Case){
    'OverlapReproducesBad'        {'OVERLAP_REPRODUCES_BAD_SPARSE_COMPOSED'}
    'IndependentLossNotSufficient'{'INDEPENDENT_LOSS_NOT_SUFFICIENT_FOR_FAILURE'}
    default                       {'OVERLAP_DOES_NOT_CHANGE_PRESENTATION_PATH'}
}
if([string]$proof.verdict-ne$expectedVerdict){throw "verdictが一致しません: expected=$expectedVerdict actual=$($proof.verdict)"}
$expectedRecovery=switch($Case){
    'StickyAfterRemove'   {'STICKY_AFTER_REMOVE'}
    'RecoveredAfterRemove'{'RECOVERED_AFTER_REMOVE'}
    default               {'NOT_LOST_DURING_OVERLAP'}
}
if([string]$proof.eligibility_recovery-ne$expectedRecovery){throw "recoveryが一致しません: expected=$expectedRecovery actual=$($proof.eligibility_recovery)"}
Write-Host "F3-C3-A3-T2-D1-B3a contract: PASS ($Case -> $expectedVerdict / $expectedRecovery)"
