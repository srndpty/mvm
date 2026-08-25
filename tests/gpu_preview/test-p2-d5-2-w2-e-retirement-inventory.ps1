[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodRetired','GoodLegacyDiagnosticsRemainPresent',
        'NegativeCanonicalPerformanceAnnotated','NegativeUnclassifiedSite',
        'NegativeThresholdReintroduced','NegativeDispositionMissing','NegativeDispositionWrong',
        'NegativeLegacyDiagnosticDeleted')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# 実 repository ではなく合成 source root を検査対象にする。retirement contract
# そのものを固定したいので、本物の checker の中身に依存させない。
$root=Join-Path $Directory "process-$PID"
if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
foreach($relative in @('scripts','src/app/preview','src/media/gpu_preview')){
    New-Item -ItemType Directory -Path (Join-Path $root $relative) -Force|Out-Null
}
$disposition=@'
$MvmPresentationAuthorityDisposition = [ordered]@{
    presentation_authority        = 'FORMAL_V2'
    legacy_presentation_metrics   = 'DIAGNOSTIC'
    canonical_performance_verdict = 'DEFERRED_TO_W3'
}
'@
if($Case-eq'NegativeDispositionWrong'){
    $disposition=$disposition -replace "legacy_presentation_metrics   = 'DIAGNOSTIC'","legacy_presentation_metrics   = 'CANONICAL'"
}
if($Case-eq'NegativeDispositionMissing'){$disposition='# disposition宣言なし'}

# legacy metric を参照する失敗地点。既定は diagnostic integrity (記録値と再計算値の一致)。
$site=@'
    # W2-E: DIAGNOSTIC_INTEGRITY
    Add-Failure "drop_rateがledger再計算と一致しません (actual=$dropRate)"
'@
switch($Case){
    'NegativeCanonicalPerformanceAnnotated'{
        $site=@'
    # W2-E: CANONICAL_PERFORMANCE
    Add-Failure "drop_rateは0.02以下が必要です (actual=$dropRate)"
'@
    }
    'NegativeUnclassifiedSite'{
        $site=@'
    Add-Failure "drop_rateがledger再計算と一致しません (actual=$dropRate)"
'@
    }
    'NegativeThresholdReintroduced'{
        $site=@'
    # W2-E: DIAGNOSTIC_INTEGRITY
    Add-Failure "drop_rateがledger再計算と一致しません (actual=$dropRate)"
    if ($fps -lt 55) {
        Add-Failure "effective_fpsは55以上が必要です"
    }
'@
    }
}
$checkerBody=$disposition+"`n"+$site+"`nWrite-Host 'fixture canonical checker'`n"
foreach($name in @('check-p2-contract.ps1','check-p4-formal-contract.ps1')){
    $checkerBody|Set-Content -LiteralPath (Join-Path $root "scripts/$name") -Encoding utf8
}

# retirement = deletion ではない。legacy diagnostic source は残っていてよい。
$legacySources=@(
    @('src/app/preview/compositor_rhi_item.cpp','void CompositorRhiItem::recordFrameSwapped() {}'),
    @('src/media/gpu_preview/presentation_opportunity_scheduler.h','bool commitSwap();'))
foreach($source in $legacySources){
    $body=$source[1]
    if($Case-eq'NegativeLegacyDiagnosticDeleted'){$body='// legacy diagnosticを削除した'}
    $body|Set-Content -LiteralPath (Join-Path $root $source[0]) -Encoding utf8
}

$outputPath=Join-Path $root 'retirement.json'
$failed=$false
try{& $Inventory -SourceRoot $root -Output $outputPath *> $null}catch{$failed=$true}
$negative=$Case -like 'Negative*'
if($negative){
    if(-not$failed){throw "$Case をretirement inventoryが受理しました"}
    Write-Output "W2-E retirement inventory $Case`: PASS";exit 0
}
if($failed){throw "$Case をretirement inventoryが拒否しました"}
$result=Get-Content -LiteralPath $outputPath -Raw -Encoding utf8|ConvertFrom-Json
if([int64]$result.legacy_metric_canonical_decision_count-ne0){throw 'canonical decision countが0ではありません'}
if([int64]$result.legacy_metric_unclassified_count-ne0){throw 'unclassified siteが残っています'}
if([int64]$result.legacy_metric_diagnostic_integrity_count-ne2){throw 'diagnostic integrity siteを数えていません'}
if([string]$result.verdict-ne'LEGACY_PRESENTATION_AUTHORITY_RETIRED'){throw 'retirement verdictが不正です'}
if([bool]$result.retirement_means_deletion){throw 'retirementをdeletionとして扱っています'}
if([bool]$result.canonical_performance_verdict_evaluated){throw 'W2-Eでcanonical performance verdictを出しています'}
if([string]$result.canonical_performance_verdict_deferred_to-ne'W3'){throw 'performance verdictの保留先がW3ではありません'}
if($Case-eq'GoodLegacyDiagnosticsRemainPresent'){
    if(-not[bool]$result.legacy_diagnostics_retained){throw 'legacy diagnosticが残っていません'}
    foreach($source in @($result.legacy_diagnostic_sources)){
        if(-not[bool]$source.present){throw "legacy diagnostic sourceがありません: $($source.source)"}
        if([bool]$source.authoritative-or[bool]$source.used_for_canonical_verdict){
            throw "legacy diagnosticがauthoritativeです: $($source.source)"
        }
    }
}
Write-Output "W2-E retirement inventory $Case`: PASS"
