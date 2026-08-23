[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('ReproducesBadRegime','BothArmsGoodIndependent','ComposedBothArms','OrderTimeConfound',
                 'StaticConfigDiffers','NegativeUserInput','NegativeEtwLoss')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Summarizer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

function New-Probe([int]$Index,[string]$Arm,[string]$Mode,[string]$PresentMode,[int]$Displayed,
                   [int]$Flags,[bool]$UserInput,[int]$EtwLost,[int]$ParentCount){
    $name=('probe-{0:d2}-{1}-{2}' -f $Index,$Arm,$Mode.ToLowerInvariant())
    $runDirectory=Join-Path $Directory $name
    $canonical=Join-Path $runDirectory 'canonical'
    New-Item -ItemType Directory -Path $canonical|Out-Null
    $count=100
    $events=@()
    for($i=0;$i-lt$count;++$i){
        $isDisplayed=$i-lt$Displayed
        $events+=[ordered]@{
            sequence_index=$i;present_start_qpc=1+$i;process_id=100
            final_state=$(if($isDisplayed){'Presented'}else{'Discarded'})
            present_mode=$PresentMode
            displayed=$(if($isDisplayed){@(@{frame_type='NotSet';qpc=1000+$i})}else{@()})
        }
    }
    [ordered]@{schema='mvm-p2-etw-present-history-1';qpc_frequency=1000;target_process_id=100
        etw_events_lost=$EtwLost;etw_buffers_lost=0;present_event_overflow_count=0;events=$events}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $canonical 'present-history-raw.json') -Encoding utf8
    [ordered]@{presentation_opportunity=[ordered]@{measurement_start_qpc=1
        measurement_end_qpc_exclusive=1+$count;qpc_frequency=1000}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $canonical 'traced-app.json') -Encoding utf8
    [ordered]@{status='PASS';window_state=[ordered]@{user_input_detected=$UserInput}
        dwm_wide_present_start_count=$(if($ParentCount-gt0){$ParentCount}else{0})
        target_attached_parent_count=$ParentCount
        target_parent_present_start_gap=[ordered]@{max=$(if($ParentCount-gt0){40}else{0})}
        dependency_batch_size=[ordered]@{max=0};discarded_count=$count-$Displayed}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'condition-proof.json') -Encoding utf8
    [ordered]@{swapchain=[ordered]@{width=1920;height=1080;format=28;buffer_count=2;scaling=1
            swap_effect=4;alpha_mode=0;flags=$Flags}
        output=[ordered]@{device_name='\\.\DISPLAY1'}
        capability=[ordered]@{hardware_composition_support_flags=3;tearing_supported=$true}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'preflight-proof.json') -Encoding utf8
    return [ordered]@{index=$Index;arm=$Arm;dirty_propagation_mode=$Mode;directory=$name}
}

$independent='Hardware_Composed_Independent_Flip'
$composed='Composed_Flip'
$runs=@()
$sequence=@(@('A','DISABLED'),@('B','CONTROL'),@('B','CONTROL'),@('A','DISABLED'))
for($i=0;$i-lt$sequence.Count;++$i){
    $arm=$sequence[$i][0];$mode=$sequence[$i][1]
    $isA=$arm-eq'A'
    $presentMode=$independent;$displayed=100;$flags=64;$input=$false;$lost=0;$parent=0
    switch($Case){
        'ReproducesBadRegime'{
            if($isA){$presentMode=$composed;$displayed=5;$parent=10}
        }
        'ComposedBothArms'{
            $presentMode=$composed;$parent=90
            if($isA){$displayed=100}else{$displayed=100}
        }
        'OrderTimeConfound'{
            # arm A の内部でregimeがばらつく。
            if($isA-and$i-eq0){$presentMode=$composed;$displayed=5;$parent=10}
        }
        'StaticConfigDiffers'{
            if($isA){$flags=0}
        }
        'NegativeUserInput'{if($i-eq2){$input=$true}}
        'NegativeEtwLoss'{if($i-eq1){$lost=1}}
    }
    $runs+=New-Probe ($i+1) $arm $mode $presentMode $displayed $flags $input $lost $parent
}
[ordered]@{schema='mvm-p2-c3-a3-t2-d1b1-probe-runs-1';status='PASS';runs=$runs}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Directory 'probe-runs.json') -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Summarizer -ProbeDirectory $Directory -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
$expected=switch($Case){
    'ReproducesBadRegime'    {'DIRTY_PROPAGATION_MODE_REPRODUCES_BAD_REGIME'}
    'BothArmsGoodIndependent'{'BOTH_ARMS_GOOD_INDEPENDENT'}
    'ComposedBothArms'       {'BOTH_ARMS_SAME_REGIME'}
    'OrderTimeConfound'      {'ORDER_TIME_CONFOUND'}
    'StaticConfigDiffers'    {'STATIC_ELIGIBILITY_CONFIG_DIFFERS'}
    default                  {$null}
}
if($null-eq$expected){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
    Write-Host "F3-C3-A3-T2-D1-B1 contract: PASS ($Case rejected)"
    exit 0
}
if($failed){throw "D1-B1 summarizerが失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if([string]$proof.verdict-ne$expected){throw "verdictが一致しません: expected=$expected actual=$($proof.verdict)"}
Write-Host "F3-C3-A3-T2-D1-B1 contract: PASS ($Case -> $expected)"
