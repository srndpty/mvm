[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('C1InvalidationWakes','C1ForcedRedrawRequired','C1RegimeInvalid','C1OrderConfound',
                 'C2PaintProcessingWakes','C2RedirectionSuspect','C2RegimeInvalid',
                 'NegativeUpdateChainVolume','NegativeRuntime','NegativeUserInput',
                 'NegativeDamageCadence')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Summarizer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# LARGE_SUPPRESSION は DWM PresentStart が 0/1 件、REGULAR は約60Hzで gap<=2。
function Dwm-Numbers([string]$Class){
    if($Class-eq'REGULAR'){return @{count=880;p95=1;max=2}}
    return @{count=0;p95=0;max=0}
}
$stage=if($Case-like'C2*'){'C2'}else{'C1'}
if($Case-in@('NegativeUpdateChainVolume','NegativeRuntime','NegativeUserInput','NegativeDamageCadence')){$stage='C1'}
$conditions=if($stage-eq'C1'){@('CONTROL','TARGET_HWND_INVALIDATE','EXTERNAL_DIRTY')}
            else{@('TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW','EXTERNAL_DIRTY')}
$classes=switch($Case){
    'C1InvalidationWakes'      {@{CONTROL='LARGE_SUPPRESSION';TARGET_HWND_INVALIDATE='REGULAR';EXTERNAL_DIRTY='REGULAR'}}
    'C1ForcedRedrawRequired'   {@{CONTROL='LARGE_SUPPRESSION';TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';EXTERNAL_DIRTY='REGULAR'}}
    'C1RegimeInvalid'          {@{CONTROL='LARGE_SUPPRESSION';TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';EXTERNAL_DIRTY='LARGE_SUPPRESSION'}}
    'C1OrderConfound'          {@{CONTROL='REGULAR';TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';EXTERNAL_DIRTY='REGULAR'}}
    'C2PaintProcessingWakes'   {@{TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';TARGET_HWND_REDRAW_NOW='REGULAR';EXTERNAL_DIRTY='REGULAR'}}
    'C2RedirectionSuspect'     {@{TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';TARGET_HWND_REDRAW_NOW='LARGE_SUPPRESSION';EXTERNAL_DIRTY='REGULAR'}}
    'C2RegimeInvalid'          {@{TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';TARGET_HWND_REDRAW_NOW='LARGE_SUPPRESSION';EXTERNAL_DIRTY='LARGE_SUPPRESSION'}}
    default                    {@{CONTROL='LARGE_SUPPRESSION';TARGET_HWND_INVALIDATE='LARGE_SUPPRESSION';EXTERNAL_DIRTY='REGULAR'}}
}
$expected=switch($Case){
    'C1InvalidationWakes'      {'TARGET_HWND_INVALIDATION_WAKES_DWM'}
    'C1ForcedRedrawRequired'   {'TARGET_INVALIDATION_INSUFFICIENT_FORCED_REDRAW_REQUIRED'}
    'C1RegimeInvalid'          {'T2_C_ACQUISITION_REGIME_INVALID'}
    'C1OrderConfound'          {'ORDER_TIME_CONFOUND_OR_MIXED'}
    'C2PaintProcessingWakes'   {'TARGET_PAINT_PROCESSING_WAKES_DWM'}
    'C2RedirectionSuspect'     {'TARGET_REDIRECTION_PATH_SUSPECT'}
    'C2RegimeInvalid'          {'T2_C_ACQUISITION_REGIME_INVALID'}
    'NegativeUpdateChainVolume'{'UPDATE_CHAIN_VOLUME_DIVERGENT'}
    default                    {$null}
}
$orders=@(
    @($conditions[0],$conditions[1],$conditions[2]),
    @($conditions[1],$conditions[2],$conditions[0]),
    @($conditions[2],$conditions[0],$conditions[1])
)
$damageConditions=@('TARGET_HWND_INVALIDATE','TARGET_HWND_REDRAW_NOW')
$runs=@()
$identityBase='sha-exe|sha-dec|qtc|sha-gui|sha-core|sha-p1|sha-p2|sha-quick|qdc'
for($setIndex=0;$setIndex-lt3;++$setIndex){for($position=0;$position-lt3;++$position){
    $condition=$orders[$setIndex][$position]
    $name=('set-{0}-position-{1}-{2}'-f($setIndex+1),($position+1),$condition.ToLowerInvariant())
    $runDirectory=Join-Path $Directory $name
    New-Item -ItemType Directory -Path (Join-Path $runDirectory 'canonical')|Out-Null
    $numbers=Dwm-Numbers $classes[$condition]
    $damage=if($damageConditions-contains$condition){900}else{0}
    if($Case-eq'NegativeDamageCadence'-and$condition-eq'TARGET_HWND_INVALIDATE'){$damage=10}
    $presentCount=900
    if($Case-eq'NegativeUpdateChainVolume'-and$condition-eq'TARGET_HWND_INVALIDATE'){$presentCount=700}
    $inputDetected=$Case-eq'NegativeUserInput'-and$condition-eq'CONTROL'
    $chain=[ordered]@{status='PASS';verdict='UPDATE_CHAIN_EXACT';target_pixel_toggle=$false
        exact_closed_count=$presentCount-2;native_present_count=$presentCount}
    $state=[ordered]@{dirty_tick_delta=$(if($condition-eq'EXTERNAL_DIRTY'){900}else{0})
        target_damage_delta=$damage;target_damage_failure_count=0
        target_update_region_fraction=$(if($damage-gt0){1.0}else{0.0})
        user_input_detected=$inputDetected}
    $proof=[ordered]@{schema='mvm-p2-c3-a3-t1-condition-proof-1';status='PASS';measurement_seconds=15.0
        window_state=$state;dwm_wide_present_start_count=$numbers.count
        dwm_wide_present_start_gap=[ordered]@{p95=$numbers.p95;max=$numbers.max}
        target_attached_parent_count=0;dependency_batch_size=[ordered]@{p95=0;max=0}
        presented_count=900;discarded_count=0}
    [ordered]@{schema='mvm-p2-c3-a3-t2-condition-run-1';status='PASS';condition=$condition
        formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
        production_scheduler_changed=$false;update_chain=$chain;dwm_condition=$proof}|
        ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $runDirectory 't2-summary.json') -Encoding utf8
    $identity=$identityBase
    if($Case-eq'NegativeRuntime'-and$setIndex-eq2-and$position-eq2){$identity='sha-other|sha-dec|qtc|sha-gui|sha-core|sha-p1|sha-p2|sha-quick|qdc'}
    $fields=$identity-split'\|'
    [ordered]@{acquisition_mode='CanonicalPresentMonLive';submission_mode='CONTROL';c0_r2_status='PASS'
        oracle_status='ORACLE_VALID';display_completion_status='CLOSED'
        incomplete_unknown_count=0;lost_count=0
        identities=[ordered]@{executable_sha256=$fields[0];decoder_sha256=$fields[1];qt_upstream_commit=$fields[2]
            qt_gui_dll_sha256=$fields[3];qt_core_dll_sha256=$fields[4];t2_qtbase_patch_sha256=$fields[5]
            t2_qtdeclarative_patch_sha256=$fields[6];t2_qt_quick_dll_sha256=$fields[7]
            qtdeclarative_upstream_commit=$fields[8]}}|
        ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $runDirectory 'canonical\summary.json') -Encoding utf8
    $runs+=[ordered]@{set=$setIndex+1;position=$position+1;condition=$condition;directory=$name}
}}
[ordered]@{schema='mvm-p2-c3-a3-t2-c-matrix-runs-1';status='PASS';stage=$stage;runs=$runs}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Directory 'matrix-runs.json') -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$failed=$false
try{
    & pwsh -NoProfile -File $Summarizer -MatrixDirectory $Directory -Output $output
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
if($expected-eq$null){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
    Write-Host "F3-C3-A3-T2-C summary contract: PASS ($Case rejected)"
    exit 0
}
if($failed){throw "T2-C summarizerが失敗しました: $Case"}
$proofJson=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if([string]$proofJson.verdict-ne$expected){throw "verdictが一致しません: expected=$expected actual=$($proofJson.verdict)"}
if([string]$proofJson.stage-ne$stage){throw "stageが一致しません: expected=$stage actual=$($proofJson.stage)"}
Write-Host "F3-C3-A3-T2-C summary contract: PASS ($Case -> $expected)"
