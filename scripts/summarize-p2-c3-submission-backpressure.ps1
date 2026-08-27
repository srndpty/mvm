[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string[]]$SourceDirectories,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Checker=(Join-Path $PSScriptRoot 'check-p2-c3-submission-backpressure.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
if($SourceDirectories.Count-ne3){throw 'F3-C3-A counterbalanced集計にはsource runが3件必要です'}
if(-not(Test-Path -LiteralPath $Checker)){throw "F3-C3-A checkerがありません: $Checker"}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C3-A集計を上書きしません: $OutputDirectory"}
$expectedOrders=@(
    'CONTROL,FRAME_LATENCY_1,DWM_FLUSH_AFTER_PRESENT',
    'FRAME_LATENCY_1,DWM_FLUSH_AFTER_PRESENT,CONTROL',
    'DWM_FLUSH_AFTER_PRESENT,CONTROL,FRAME_LATENCY_1')
$modes=@('CONTROL','FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT')
$pathByMode=@{CONTROL='control';FRAME_LATENCY_1='frame_latency_1';DWM_FLUSH_AFTER_PRESENT='dwm_flush_after_present'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$all=@();$sources=@()
for($runIndex=0;$runIndex-lt$SourceDirectories.Count;++$runIndex){
    $source=(Resolve-Path -LiteralPath $SourceDirectories[$runIndex]).Path
    $summaryPath=Join-Path $source 'summary.json'
    if(-not(Test-Path -LiteralPath $summaryPath)){throw "source summaryがありません: $summaryPath"}
    $sourceSummary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
    $order=@($sourceSummary.execution_order)-join','
    if($order-ne$expectedOrders[$runIndex]){throw "counterbalanced orderが不正です: run=$($runIndex+1) actual=$order"}
    $sources+=[ordered]@{run=$runIndex+1;directory=$source;summary_sha256=Hash $summaryPath;execution_order=@($sourceSummary.execution_order)}
    foreach($mode in $modes){
        $canonical=Join-Path $source $pathByMode[$mode]
        $oracle=Join-Path $canonical 'oracle.json';$app=Join-Path $canonical 'traced-app.json'
        $proof=Join-Path $OutputDirectory ("run{0}-{1}-proof.json" -f ($runIndex+1),$pathByMode[$mode])
        & pwsh -NoProfile -File $Checker -OracleJson $oracle -AppJson $app -SubmissionMode $mode -Output $proof
        if($LASTEXITCODE-ne0){throw "F3-C3-A recheckが失敗しました: run=$($runIndex+1) mode=$mode"}
        $value=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
        $all+=[ordered]@{run=$runIndex+1;position=[Array]::IndexOf(@($sourceSummary.execution_order),$mode)+1;mode=$mode;proof=$value;proof_sha256=Hash $proof}
    }
}
$aggregates=@()
foreach($mode in $modes){
    $values=@($all|Where-Object{$_.mode-eq$mode})
    $native=[int64](($values|ForEach-Object{[int64]$_.proof.native_present_count}|Measure-Object -Sum).Sum)
    $presented=[int64](($values|ForEach-Object{[int64]$_.proof.presented_count}|Measure-Object -Sum).Sum)
    $discarded=[int64](($values|ForEach-Object{[int64]$_.proof.discarded_count}|Measure-Object -Sum).Sum)
    if($native-ne$presented+$discarded){throw "aggregate closureが不正です: $mode"}
    $aggregates+=[ordered]@{
        mode=$mode;run_count=$values.Count;positions=@($values.position|Sort-Object)
        native_present_count=$native;presented_count=$presented;discarded_count=$discarded
        presented_fraction=if($native-gt0){$presented/$native}else{0.0}
        discarded_fraction=if($native-gt0){$discarded/$native}else{0.0}
        dependent_superseded_count=[int64](($values|ForEach-Object{[int64]$_.proof.discard_reason_histogram.DEPENDENT_PRESENT_SUPERSEDED}|Measure-Object -Sum).Sum)
        earlier_superseded_count=[int64](($values|ForEach-Object{[int64]$_.proof.discard_reason_histogram.EARLIER_SWAPCHAIN_PRESENT_SUPERSEDED}|Measure-Object -Sum).Sum)
        displayed_unique_source_frames=[int64](($values|ForEach-Object{[int64]$_.proof.displayed_unique_source_frame_count}|Measure-Object -Sum).Sum)
        formal_source_frame_drops=[int64](($values|ForEach-Object{[int64]$_.proof.formal_source_frame_drops}|Measure-Object -Sum).Sum)
        dependency_batch_p95_per_run=@($values|ForEach-Object{[int64]$_.proof.dependency_batch_size.p95})
        dependency_batch_observed_max=[int64](($values|ForEach-Object{[int64]$_.proof.dependency_batch_size.max}|Measure-Object -Maximum).Maximum)
        per_run=@($values|ForEach-Object{[ordered]@{
            run=$_.run;position=$_.position;native=[int64]$_.proof.native_present_count
            presented=[int64]$_.proof.presented_count;discarded=[int64]$_.proof.discarded_count
            batch_p95=[int64]$_.proof.dependency_batch_size.p95;batch_max=[int64]$_.proof.dependency_batch_size.max
            formal_source_drops=[int64]$_.proof.formal_source_frame_drops}})
    }
}
[ordered]@{
    schema='mvm-p2-c3-submission-backpressure-counterbalanced-summary-1'
    status='PASS';authority='diagnostic_only';formal_counter_authority_changed=$false
    formal_drop_threshold_changed=$false;counterbalancing='THREE_CYCLIC_ORDERS_EACH_MODE_EACH_POSITION_ONCE'
    sources=$sources;aggregates=$aggregates
    identities=[ordered]@{checker_sha256=Hash $Checker}
}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A counterbalanced summary: PASS ($OutputDirectory)"
