[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [ValidateSet('Good','NegativePostWorktree','NegativePostSource','NegativePostQtGui','NegativePostQtQuick','NegativeWarmupMismatch','NegativeExit6Teardown','NegativeExit6Metrics','NegativeExit6Mapping','NegativeTeardownStageState','NegativeTeardownStageReport')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Read-Source([string]$Relative){Get-Content -LiteralPath (Join-Path $SourceRoot $Relative) -Raw -Encoding utf8}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}
$header=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$rendererHeader=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'
$runner=Read-Source 'scripts/acquire-p2-d5-2-w4-c2-diagnostic.ps1'
Require $header 'OutsideSourceDomainDecision' 'source-domain result enumがありません'
Deny $header 'OutsideRequiredDomainDecision' 'scheduler resultにrequired-domain enumが混入しています'
Require $header 'requiredIntentMembership[\s\S]+transportDisposition' 'required membershipとtransport dispositionが別fieldではありません'
Require $scheduler 'return finishInvocation' '全returnがinvocation ledgerを通りません'
Require $scheduler 'CompletedOrdinalUnavailable[\s\S]+CompletedOrdinalOverflow[\s\S]+TargetArithmeticOverflow' 'fatal return reasonがbranch-exactではありません'
Require $renderer 'noteInvocationTransportDisposition' 'renderer transport dispositionをinvocationへexact joinしていません'
Require $controller 'mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1' 'C2 schemaがemitされません'
Require $controller 'diagnostic_root_cause_capture[\s\S]+canonical_performance_authority' 'diagnostic/performance authority分離がありません'
function Assert-ProvenancePostChecks([string]$Text){
    Require $Text '\[int\]\$WarmupSeconds=5' 'sealed W3と同じwarmupが固定されていません'
    Require $Text '\$postStatus=& git -C \$repo status --porcelain' '終了時worktree検査がありません'
    Require $Text 'foreach\(\$path in \$sourceHashes\.Keys\)[\s\S]+\$postSourceHash-ne\$sourceHashes\[\$path\]' '終了時source hash検査がありません'
    Require $Text 'Get-FileHash -LiteralPath \$qtGui[\s\S]+-ne\$qtGuiHash' '終了時Qt6Gui hash検査がありません'
    Require $Text 'Get-FileHash -LiteralPath \$qtQuick[\s\S]+-ne\$qtQuickHash' '終了時Qt6Quick hash検査がありません'
}
function Assert-ExitSixDiagnostics([string]$Text){
    Require $Text 'W4-C2_DIAGNOSTIC_EXIT6_TEARDOWN_TIMEOUT' 'teardown timeoutのexit 6識別子がありません'
    Require $Text 'W4-C2_DIAGNOSTIC_EXIT6_METRICS_WRITE_FAILURE' 'metrics failureのexit 6識別子がありません'
    Require $Text 'W4-C2_DIAGNOSTIC_EXIT6_CLOSE_MAPPING_FAILURE' 'mapping close failureのexit 6識別子がありません'
}
function Assert-TeardownStageDiagnostics([string]$Header,[string]$Renderer,[string]$Controller){
    Require $Header 'enum class RenderTeardownDiagnosticStage' 'render teardown stage enumがありません'
    Require $Renderer 'teardownDiagnosticStage\.compare_exchange_strong' 'teardown要求をrender callbackへexact joinしていません'
    Require $Renderer 'teardownDiagnosticStage\.store\(RenderTeardownDiagnosticStage::CompositorDrain' 'compositor drain stageを記録していません'
    Require $Controller 'teardownDiagnosticStageName\(diagnosticStage\)' 'teardown timeoutが停止stageを出力しません'
}
$mutatedRunner=$runner
$mutatedHeader=$rendererHeader
$mutatedRenderer=$renderer
$mutatedController=$controller
switch($Case){
    'NegativePostWorktree' {$mutatedRunner=$mutatedRunner.Replace('$postStatus=& git -C $repo status --porcelain',"`$postStatus=''")}
    'NegativePostSource' {$mutatedRunner=$mutatedRunner.Replace('foreach($path in $sourceHashes.Keys){','foreach($path in @()){')}
    'NegativePostQtGui' {$mutatedRunner=$mutatedRunner.Replace('-ne$qtGuiHash){','-ne(Get-FileHash -LiteralPath $qtGui -Algorithm SHA256).Hash.ToLowerInvariant()){')}
    'NegativePostQtQuick' {$mutatedRunner=$mutatedRunner.Replace('-ne$qtQuickHash){','-ne(Get-FileHash -LiteralPath $qtQuick -Algorithm SHA256).Hash.ToLowerInvariant()){')}
    'NegativeWarmupMismatch' {$mutatedRunner=$mutatedRunner.Replace('[int]$WarmupSeconds=5','[int]$WarmupSeconds=2')}
    'NegativeExit6Teardown' {$mutatedController=$mutatedController.Replace('W4-C2_DIAGNOSTIC_EXIT6_TEARDOWN_TIMEOUT','W4-C2_DIAGNOSTIC_EXIT6_UNKNOWN')}
    'NegativeExit6Metrics' {$mutatedController=$mutatedController.Replace('W4-C2_DIAGNOSTIC_EXIT6_METRICS_WRITE_FAILURE','W4-C2_DIAGNOSTIC_EXIT6_UNKNOWN')}
    'NegativeExit6Mapping' {$mutatedController=$mutatedController.Replace('W4-C2_DIAGNOSTIC_EXIT6_CLOSE_MAPPING_FAILURE','W4-C2_DIAGNOSTIC_EXIT6_UNKNOWN')}
    'NegativeTeardownStageState' {$mutatedRenderer=$mutatedRenderer.Replace('RenderTeardownDiagnosticStage::CompositorDrain','RenderTeardownDiagnosticStage::RenderCallbackObserved')}
    'NegativeTeardownStageReport' {$mutatedController=$mutatedController.Replace('teardownDiagnosticStageName(diagnosticStage)','"UNOBSERVED"')}
}
if($Case-eq'Good'){
    Assert-ProvenancePostChecks $mutatedRunner
    Assert-ExitSixDiagnostics $mutatedController
    Assert-TeardownStageDiagnostics $mutatedHeader $mutatedRenderer $mutatedController
}else{
    try{
        Assert-ProvenancePostChecks $mutatedRunner
        Assert-ExitSixDiagnostics $mutatedController
        Assert-TeardownStageDiagnostics $mutatedHeader $mutatedRenderer $mutatedController
        throw "mutationが検出されませんでした: $Case"
    }
    catch{if($_.Exception.Message-like'mutationが検出されませんでした:*'){throw}}
}
Write-Output "W4-C2 invocation ledger architecture: PASS ($Case)"
