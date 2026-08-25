[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodRetired','GoodLegacyDiagnosticsRemainPresent',
        'NegativeCanonicalPerformanceAnnotated','NegativeUnclassifiedSite',
        'NegativeThresholdReintroduced','NegativeDispositionMissing','NegativeDispositionWrong',
        'NegativeLegacyDiagnosticDeleted','NegativeUnregisteredCheckerWithLegacyThreshold',
        'NegativeUnregisteredCheckerWithMultilineLegacyThreshold',
        'NegativeDeclaredCheckerWithMultilineLegacyThreshold',
        'NegativeDeclaredCheckerWithAliasedLegacyThreshold')][string]$Case,
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

# 走査対象を列挙で固定していれば、登録されていない checker に threshold を足すだけで
# false-PASS できてしまう。discovery でその穴が閉じていることを固定する。
# さらに metric 参照と FAIL が同一行に無い形 (複数行 / alias 経由) も、
# AST の taint 伝播で追えていることを固定する。
$extraChecker=$null
switch($Case){
    'NegativeUnregisteredCheckerWithLegacyThreshold'{
        $extraChecker=@'
# disposition宣言を持たない未登録checker
if ($effective_fps -lt 55) { Fail 'effective_fpsが55未満です' }
'@
    }
    'NegativeUnregisteredCheckerWithMultilineLegacyThreshold'{
        # metric参照とFAILが別行。同一行regexではすり抜ける形。
        $extraChecker=@'
# disposition宣言を持たない未登録checker
$fps = Require-Property $raw 'effective_fps'
$minimumFps = 55
if ($fps -lt $minimumFps) {
    Fail 'legacy fps failure'
}
'@
    }
    'NegativeDeclaredCheckerWithMultilineLegacyThreshold'{
        # disposition は宣言済み。file-level fallbackでは止まらず、
        # site 検出そのものが効いていなければ通り抜ける形。
        $extraChecker=$disposition+@'

$fps = Require-Property $raw 'effective_fps'
$minimumFps = 55
if ($fps -lt $minimumFps) {
    Fail 'legacy fps failure'
}
'@
    }
    'NegativeDeclaredCheckerWithAliasedLegacyThreshold'{
        # alias 経由。$value -> $bad と2段taintしないと追えない形。
        $extraChecker=$disposition+@'

$value = Require-Property $raw 'drop_rate'
$limit = 0.02
$bad = $value -gt $limit
if ($bad) {
    Fail 'drop'
}
'@
    }
}
if($null-ne$extraChecker){
    $extraChecker|Set-Content -LiteralPath (Join-Path $root 'scripts/check-extra.ps1') -Encoding utf8
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
    # disposition宣言済みの変異は file-level fallback では止まらない。
    # site検出そのものが効いていることを blocker で確認する。
    if($Case -like 'NegativeDeclaredChecker*'){
        if(-not(Test-Path -LiteralPath $outputPath)){throw "$Case のretirement artifactがありません"}
        $rejected=Get-Content -LiteralPath $outputPath -Raw -Encoding utf8|ConvertFrom-Json
        $blockers=@($rejected.blockers)
        if('LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED'-notin$blockers){
            throw "$Case をsite検出で捕まえていません (blockers=$($blockers-join', '))"
        }
        if('LEGACY_METRIC_CHECKER_AUTHORITY_UNDECLARED'-in$blockers){
            throw "$Case がfile-level fallbackで止まっており、site検出の証明になっていません"
        }
    }
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
