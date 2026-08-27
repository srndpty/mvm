param(
    [Parameter(Mandatory=$true)][ValidateSet('TargetDoesNotWake','NegativeControl','InvalidPositive','OrderConfound','NegativeRuntime')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Summarizer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$orders=@(
    @('CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE','EXTERNAL_DIRTY'),
    @('TARGET_RHIITEM_PIXEL_TOGGLE','EXTERNAL_DIRTY','CONTROL'),
    @('EXTERNAL_DIRTY','CONTROL','TARGET_RHIITEM_PIXEL_TOGGLE')
)
function Classification([string]$Condition,[int]$Position){
    if($Case-eq'TargetDoesNotWake'){return $(if($Condition-eq'EXTERNAL_DIRTY'){'REGULAR'}else{'LARGE'})}
    if($Case-eq'NegativeControl'){return $(if($Condition-eq'CONTROL'){'LARGE'}else{'REGULAR'})}
    if($Case-eq'InvalidPositive'){return 'LARGE'}
    return @('REGULAR','LARGE','MIXED')[$Position-1]
}
$runs=@()
for($set=1;$set-le3;++$set){for($position=1;$position-le3;++$position){
    $condition=$orders[$set-1][$position-1];$name="run-$set-$position";$dir=Join-Path $Directory $name;$canonicalDir=Join-Path $dir 'canonical'
    New-Item -ItemType Directory -Path $canonicalDir -Force|Out-Null
    $class=Classification $condition $position;$large=$class-eq'LARGE';$mixed=$class-eq'MIXED'
    $dwmMax=$(if($large){60}elseif($mixed){20}else{2});$dwmP95=$(if($mixed){5}else{1});$batchMax=$(if($large){60}else{1})
    $proof=[ordered]@{schema='mvm-p2-c3-a3-t1-condition-proof-1';status='PASS';measurement_seconds=15.0;window_state=[ordered]@{dirty_tick_delta=$(if($condition-eq'EXTERNAL_DIRTY'){900}else{0})};dwm_wide_present_start_count=60;dwm_wide_present_start_gap=[ordered]@{p95=$dwmP95;max=$dwmMax};target_attached_parent_count=60;dependency_batch_size=[ordered]@{p95=1;max=$batchMax};presented_count=60;discarded_count=0}
    $chain=[ordered]@{status='PASS';verdict='UPDATE_CHAIN_EXACT';target_pixel_toggle=($condition-eq'TARGET_RHIITEM_PIXEL_TOGGLE');exact_closed_count=898}
    [ordered]@{schema='mvm-p2-c3-a3-t2-condition-run-1';status='PASS';condition=$condition;formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false;update_chain=$chain;dwm_condition=$proof}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $dir 't2-summary.json') -Encoding utf8
    $runtime=$(if($Case-eq'NegativeRuntime'-and$set-eq3-and$position-eq3){'different'}else{'same'})
    $identities=[ordered]@{executable_sha256=$runtime;decoder_sha256='d';qt_upstream_commit='q';qt_gui_dll_sha256='g';qt_core_dll_sha256='c';t2_qtbase_patch_sha256='bp';t2_qtdeclarative_patch_sha256='dp';t2_qt_quick_dll_sha256='qq';qtdeclarative_upstream_commit='qc'}
    [ordered]@{acquisition_mode='CanonicalPresentMonLive';submission_mode='CONTROL';c0_r2_status='PASS';oracle_status='ORACLE_VALID';display_completion_status='CLOSED';incomplete_unknown_count=0;lost_count=0;identities=$identities}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $canonicalDir 'summary.json') -Encoding utf8
    $runs+=[ordered]@{set=$set;position=$position;condition=$condition;directory=$name;summary_sha256='synthetic'}
}}
[ordered]@{schema='mvm-p2-c3-a3-t2-matrix-runs-1';status='PASS';runs=$runs}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Directory 'matrix-runs.json') -Encoding utf8
$output=Join-Path $Directory 'proof.json'
& pwsh -NoProfile -File $Summarizer -MatrixDirectory $Directory -Output $output *> (Join-Path $Directory 'summarizer.txt')
$exit=$LASTEXITCODE
if($Case-eq'NegativeRuntime'){
    if($exit-eq0){throw 'runtime差を含むT2-B matrixが通過しました'}
}else{
    if($exit-ne0){throw "T2-B summary契約が失敗しました: $Case exit=$exit"}
    $actual=(Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json).verdict
    $expected=@{TargetDoesNotWake='TARGET_PIXEL_DOES_NOT_WAKE_DWM_EXTERNAL_DIRTY_REGULAR';NegativeControl='TARGET_PIXEL_AND_EXTERNAL_REGULAR_NEGATIVE_CONTROL_REQUIRED';InvalidPositive='T2_B_ACQUISITION_REGIME_INVALID';OrderConfound='ORDER_TIME_CONFOUND_OR_MIXED'}[$Case]
    if([string]$actual-ne$expected){throw "T2-B verdictが不正です: expected=$expected actual=$actual"}
}
Write-Host "F3-C3-A3-T2-B summary contract: PASS ($Case)"
