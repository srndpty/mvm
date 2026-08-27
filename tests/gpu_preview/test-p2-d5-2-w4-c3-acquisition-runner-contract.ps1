[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Runner,
    [ValidateSet('Good','NegativeDirtyWorktreeAllowed','NegativeMissingCheckerProvenance',
        'NegativeMajorityAggregate','NegativePostCaptureUnchecked','NegativeMeasureSecondsShortened',
        'NegativePerformanceAuthorityPromotion','NegativeRunClosureWeakened')]
    [string]$Case='Good'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$text=(Get-Content -LiteralPath $Runner -Raw -Encoding utf8)-replace "`r`n","`n"
switch($Case){
    'NegativeDirtyWorktreeAllowed' {
        $text=$text.Replace("throw 'W4-C3 diagnostic captureはclean worktreeから取得してください'",
                            "Write-Host 'dirty worktreeを許容します'")}
    'NegativeMissingCheckerProvenance' {
        $text=$text.Replace('c3_checker_sha256=$c3CheckerHash','c3_checker_sha256=$null')}
    'NegativeMajorityAggregate' {
        $text=$text.Replace('$closed=($exactRuns-eq$Runs)','$closed=($exactRuns-ge[Math]::Ceiling($Runs/2))')}
    'NegativePostCaptureUnchecked' {
        $text=$text.Replace("if((Get-Sha256 `$c3Checker)-ne`$c3CheckerHash){throw 'W4-C3 capture中にC3 checkerが変化しました'}","")}
    'NegativeMeasureSecondsShortened' {
        $text=$text.Replace('[ValidateRange(1,60)][int]$MeasureSeconds=60','[ValidateRange(1,60)][int]$MeasureSeconds=5')}
    'NegativePerformanceAuthorityPromotion' {
        $text=$text.Replace('canonical_performance_authority=$false','canonical_performance_authority=$true')}
    'NegativeRunClosureWeakened' {
        $text=$text.Replace('[bool]$checked.terminal_required_intent_membership-and','')}
}
function Require([string]$Pattern,[string]$Message){if($text-notmatch$Pattern){throw $Message}}
function Deny([string]$Pattern,[string]$Message){if($text-match$Pattern){throw $Message}}
try{
    # workload / runtime条件はW4-C2 formalと同一
    Require '\[ValidateRange\(1,10\)\]\[int\]\$Runs=3' '3 runが既定ではありません'
    Require '\[ValidateRange\(1,60\)\]\[int\]\$WarmupSeconds=2' 'warmup 2秒が既定ではありません'
    Require '\[ValidateRange\(1,60\)\]\[int\]\$MeasureSeconds=60' 'measure 60秒が既定ではありません'
    Require '\[ValidateRange\(30,300\)\]\[int\]\$TimeoutSeconds=120' 'timeout 120秒が既定ではありません'
    Require "'--formal-preflight','--vblank-observer'" 'formal playback flagsが一致しません'
    Require "'--native-present-hook','on',\s*'--w4-c2-scheduler-invocation-ledger'" 'diagnostic ledger flagがありません'
    Require "'--seed','20260827','--seek-count','1000','--display-timeout-ms','2000'" 'workload seed/seek条件が一致しません'
    Require "'--gpu-completion','fence','--mode','playback'" 'playback/fence条件が一致しません'
    Require "QT_D3D_MAX_FRAME_LATENCY.*'2'" 'QT_D3D_MAX_FRAME_LATENCYが固定されていません'
    Require "MVM_P2_C3_SUBMISSION_MODE.*'CONTROL'" 'submission modeが固定されていません'
    Require 'Remove-Item Env:QSG_NO_VSYNC' 'QSG_NO_VSYNCの解除がありません'
    Require 'OPERATION_STOP_REQUIRED' '操作停止protocolが記録されません'

    # provenance bind
    Require "throw 'W4-C3 diagnostic captureはclean worktreeから取得してください'" 'clean worktree要求がありません'
    Require 'c3_checker_sha256=\$c3CheckerHash' 'C3 checkerのhashがbindされていません'
    Require 'c2_checker_sha256=\$c2CheckerHash' 'C2 checkerのhashがbindされていません'
    Require 'executable_sha256=\$binaryHash' 'binary hashがbindされていません'
    Require 'source_asset_sha256=\$sourceAssetHashes' 'source assetのhashがbindされていません'
    Require 'qt_gui_sha256=\$qtGuiHash[\s\S]{0,200}qt_quick_sha256=\$qtQuickHash' 'Qt hashがbindされていません'
    Require "capture中にHEADが変化しました" 'post-captureのHEAD検査がありません'
    Require "capture中にworktreeが変化しました" 'post-captureのworktree検査がありません'
    Require "capture中にbinaryが変化しました" 'post-captureのbinary検査がありません'
    Require "capture中にC3 checkerが変化しました" 'post-captureのC3 checker検査がありません'
    Require "capture中にC2 checkerが変化しました" 'post-captureのC2 checker検査がありません'

    # run closureとaggregate契約
    Require '\$runExact=\(\$checkerExit-eq0-and[\s\S]{0,600}qpc_used_for_join\)' 'runごとのclosure条件が不足しています'
    Require '\[bool\]\$checked\.terminal_required_intent_membership-and' 'terminal membershipがrun closure条件にありません'
    Require '\$closed=\(\$exactRuns-eq\$Runs\)' '3/3以外でcloseし得ます'
    Require 'aggregate_majority_used=\$false' 'aggregate多数決の禁止が記録されません'
    Require 'canonical_performance_authority=\$false' 'diagnostic/performance分離がありません'
    Deny 'canonical_performance_authority=\$true' 'canonical performance authorityへ昇格しています'
    Require "historical_w3_verdict_rewritten=\`$false" 'historical verdictの非書き換えが記録されません'
    if($Case-ne'Good'){throw "mutationが検出されませんでした: $Case"}
}catch{
    if($Case-eq'Good'-or$_.Exception.Message-like'mutationが検出されませんでした:*'){throw}
}
Write-Output "W4-C3 acquisition runner contract: PASS ($Case)"
