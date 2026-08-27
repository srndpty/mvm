[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$Resume,
    [ValidateRange(12,300)][int]$WarmupSeconds=12,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$conditionRunner=Join-Path $PSScriptRoot 'p2-c3-a3-t1-condition.ps1'
$finalizer=Join-Path $PSScriptRoot 'finalize-p2-c3-a3-t1-condition.ps1'
$summarizer=Join-Path $PSScriptRoot 'summarize-p2-c3-a3-t1.ps1'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(-not$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'F3-C3-A3-T1 matrix採取には管理者権限が必要です'}
foreach($path in @($conditionRunner,$finalizer,$summarizer)){if(-not(Test-Path -LiteralPath $path)){throw "T1必須pathがありません: $path"}}
if(Test-Path -LiteralPath $OutputDirectory){
    if(-not$Resume){throw "既存T1 matrix artifactを上書きしません: $OutputDirectory"}
    foreach($terminal in @('matrix-runs.json','matrix-proof.json','manifest.sha256')){
        if(Test-Path -LiteralPath (Join-Path $OutputDirectory $terminal)){throw "完了済みT1 matrixはresumeできません: $OutputDirectory"}
    }
}else{New-Item -ItemType Directory -Path $OutputDirectory|Out-Null}
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$orders=@(
    @('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY'),
    @('FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY','VISIBLE_UNOCCLUDED'),
    @('VISIBLE_UNOCCLUDED_FORCE_DIRTY','VISIBLE_UNOCCLUDED','FULLY_OCCLUDED')
)
$runs=@()
for($setIndex=0;$setIndex-lt$orders.Count;++$setIndex){
    for($position=0;$position-lt3;++$position){
        $mode=$orders[$setIndex][$position]
        $short=if($mode-eq'VISIBLE_UNOCCLUDED'){'visible'}elseif($mode-eq'FULLY_OCCLUDED'){'occluded'}else{'dirty'}
        $name=('set-{0}-position-{1}-{2}'-f($setIndex+1),($position+1),$short)
        $directory=Join-Path $OutputDirectory $name
        Write-Host "F3-C3-A3-T1 matrix: set=$($setIndex+1) position=$($position+1) mode=$mode"
        $summaryPath=Join-Path $directory 'summary.json'
        if(-not(Test-Path -LiteralPath $summaryPath)){
            if(Test-Path -LiteralPath $directory){
                & pwsh -NoProfile -File $finalizer -OutputDirectory $directory -Mode $mode `
                    -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds
            }else{
                & pwsh -NoProfile -File $conditionRunner -OutputDirectory $directory -Mode $mode `
                    -WarmupSeconds $WarmupSeconds -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
            }
            if($LASTEXITCODE-ne0){throw "T1 condition run/finalizeが失敗しました: $name"}
        }
        $summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
        if([string]$summary.status-ne'PASS'-or[string]$summary.mode-ne$mode){throw "T1 condition summaryが不正です: $name"}
        $runs+=[ordered]@{
            set=$setIndex+1;position=$position+1;mode=$mode;directory=$name
            summary_sha256=Hash $summaryPath
        }
    }
}
$indexPath=Join-Path $OutputDirectory 'matrix-runs.json'
[ordered]@{
    schema='mvm-p2-c3-a3-t1-matrix-runs-1';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    cyclic_orders=$orders;runs=$runs
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $indexPath -Encoding utf8
$resultPath=Join-Path $OutputDirectory 'matrix-proof.json'
& pwsh -NoProfile -File $summarizer -MatrixDirectory $OutputDirectory -Output $resultPath
if($LASTEXITCODE-ne0){throw 'T1 matrix summarizerが失敗しました'}
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T1 matrix: PASS ($resultPath)"
