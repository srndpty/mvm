param(
    [Parameter(Mandatory=$true)][ValidateSet('Visibility','Dirty','Rejected','OrderConfound','NegativeOrder')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Summarizer,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null
$orders=@(
    @('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY'),
    @('FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY','VISIBLE_UNOCCLUDED'),
    @('VISIBLE_UNOCCLUDED_FORCE_DIRTY','VISIBLE_UNOCCLUDED','FULLY_OCCLUDED')
)
function Classification([string]$Mode,[int]$Position){
    if($Case-eq'Visibility'){return $(if($Mode-eq'FULLY_OCCLUDED'){'LARGE'}else{'REGULAR'})}
    if($Case-eq'Dirty'){return $(if($Mode-eq'VISIBLE_UNOCCLUDED_FORCE_DIRTY'){'REGULAR'}else{'LARGE'})}
    if($Case-eq'Rejected'){return 'LARGE'}
    return @('REGULAR','LARGE','MIXED')[$Position-1]
}
$runs=@()
for($set=1;$set-le3;++$set){
    for($position=1;$position-le3;++$position){
        $mode=$orders[$set-1][$position-1];$name="run-$set-$position";$dir=Join-Path $Directory $name;New-Item -ItemType Directory -Path $dir|Out-Null
        $class=Classification $mode $position
        $dwmMax=if($class-eq'LARGE'){60}elseif($class-eq'REGULAR'){2}else{20}
        $dwmP95=if($class-eq'MIXED'){5}else{1};$batchMax=if($class-eq'LARGE'){60}else{1};$batchP95=1
        $sparse=$Case-eq'Dirty'-and$mode-eq'VISIBLE_UNOCCLUDED'
        if($sparse){$dwmMax=0;$dwmP95=0;$batchMax=505;$batchP95=505}
        $proof=[ordered]@{dwm_wide_present_start_count=$(if($sparse){1}else{60});dwm_wide_present_start_gap=[ordered]@{p50=$(if($sparse){0}else{1});p95=$dwmP95;max=$dwmMax};target_attached_parent_count=$(if($sparse){1}else{60});target_parent_present_start_gap=[ordered]@{p95=$dwmP95;max=$dwmMax};dependency_batch_size=[ordered]@{p50=1;p95=$batchP95;max=$batchMax};presented_count=60;discarded_count=0;dependent_superseded_count=0;earlier_superseded_count=0}
        [ordered]@{schema='mvm-p2-c3-a3-t1-condition-run-1';status='PASS';mode=$mode;formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false;proof=$proof}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $dir 'summary.json') -Encoding utf8
        $runs+=[ordered]@{set=$set;position=$position;mode=$mode;directory=$name;summary_sha256='synthetic'}
    }
}
if($Case-eq'NegativeOrder'){$runs[8].position=2}
[ordered]@{schema='mvm-p2-c3-a3-t1-matrix-runs-1';status='PASS';runs=$runs}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $Directory 'matrix-runs.json') -Encoding utf8
$output=Join-Path $Directory 'proof.json'
& pwsh -NoProfile -File $Summarizer -MatrixDirectory $Directory -Output $output *> (Join-Path $Directory 'summarizer.txt')
$exit=$LASTEXITCODE
if($Case-eq'NegativeOrder'){
    if($exit-eq0){throw '壊したT1 cyclic order契約が通過しました'}
}else{
    if($exit-ne0){throw "T1 summary契約が失敗しました: $Case exit=$exit"}
    $actual=(Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json).verdict
    $expected=@{Visibility='FORMAL_HARNESS_VISIBILITY_DEFECT';Dirty='DIRTY_WAKE_SUPPRESSION';Rejected='VISIBILITY_DIRTY_HYPOTHESIS_REJECTED';OrderConfound='ORDER_TIME_CONFOUND_OR_MIXED'}[$Case]
    if([string]$actual-ne$expected){throw "T1 verdictが不正です: expected=$expected actual=$actual"}
}
Write-Host "F3-C3-A3-T1 summary contract: PASS ($Case)"
