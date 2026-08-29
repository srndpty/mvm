[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeMappingUsesDisplayRefresh','NegativeTargetForDuplicatesMapping',
        'NegativeRateValidationRemoved','NegativeSecondIntentRateProducer',
        'NegativeCountComparisonPreflight','NegativeCoverageUsesLastTargetOnly',
        'NegativeMonotonicityNotChecked','NegativeCoverageAllowsMaxAtSourceCount',
        'NegativeCoverageIgnoresMinTarget','NegativeRendererHardcodedSourceRate',
        'NegativeArtifactClaimsRefreshMapping','NegativeArtifactClaimsCountComparison')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$relatives=@(
    'src/media/gpu_preview/presentation_opportunity_scheduler.h',
    'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
    'src/app/preview/compositor_rhi_item.cpp',
    'apps/compositor_spike/compositor_spike_controller.cpp')
# S2-h: PID だけでは isolation key にならない。Windows は PID を再利用するため、
# 過去 run の process-<PID> directory と衝突して「既存artifactを上書きしません」で
# 失敗する。S2-f2 と同じく invocation ごとに一意な suffix を付ける。
$mutationRoot=Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
$sources=@{}
foreach($relativePath in $relatives){
    $sources[$relativePath]=Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw -Encoding utf8
}
function Edit-Source([string]$RelativePath,[string]$From,[string]$To){
    $sourceText=$sources[$RelativePath]
    if($sourceText-notmatch[regex]::Escape($From)){throw "変異対象がありません: $RelativePath / $From"}
    $sources[$RelativePath]=$sourceText.Replace($From,$To)
}
$scheduler='src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$renderer='src/app/preview/compositor_rhi_item.cpp'
$controller='apps/compositor_spike/compositor_spike_controller.cpp'
switch($Case){
    'Good'{}
    'NegativeMappingUsesDisplayRefresh'{
        Edit-Source $scheduler '!checkedMultiply(numerator, config.requiredIntentRateDenominator, numerator) ||' '!checkedMultiply(numerator, config.refreshDenominator, numerator) ||'}
    'NegativeTargetForDuplicatesMapping'{
        Edit-Source $scheduler 'return presentationTargetFrameFor(config_, ordinal, target);' 'long long numerator = 0;
    if (!checkedMultiply(ordinal, config_.sourceFpsNumerator, numerator))
        return false;
    target = numerator;
    return true;'}
    'NegativeRateValidationRemoved'{
        Edit-Source $scheduler 'config.requiredIntentRateNumerator <= 0 || config.requiredIntentRateDenominator <= 0)' 'false)'}
    'NegativeSecondIntentRateProducer'{
        Edit-Source $controller 'requiredMeasurementFrameCount_ =
        gpu::formalRequiredIntentCountForSeconds(static_cast<long long>(config_.measureSeconds));' 'requiredMeasurementFrameCount_ = static_cast<long long>(config_.measureSeconds) * 60;'}
    'NegativeCountComparisonPreflight'{
        Edit-Source $controller '    sourceCoverageOk_ =
        gpu::requiredIntentSourceCoverageSatisfied(requiredIntentSourceCoverage_,
                                                   sourceAFrameCount_) &&' '    sourceCoverageOk_ = sourceAFrameCount_ >= requiredMeasurementFrameCount_ &&
        gpu::requiredIntentSourceCoverageSatisfied(requiredIntentSourceCoverage_,
                                                   sourceAFrameCount_) &&'}
    'NegativeCoverageUsesLastTargetOnly'{
        Edit-Source $scheduler 'for (long long ordinal = 0; ordinal < config.requiredFrameCount; ++ordinal) {' 'for (long long ordinal = config.requiredFrameCount - 1; ordinal < config.requiredFrameCount; ++ordinal) {'}
    'NegativeMonotonicityNotChecked'{
        Edit-Source $scheduler '            if (target < previous)
                coverage.monotonicNonDecreasing = false;' '            (void)previous;'}
    'NegativeCoverageAllowsMaxAtSourceCount'{
        Edit-Source $scheduler 'coverage.maxTarget < sourceFrameCount;' 'coverage.maxTarget <= sourceFrameCount;'}
    'NegativeCoverageIgnoresMinTarget'{
        Edit-Source $scheduler 'return coverage.valid && coverage.minTarget >= 0 && sourceFrameCount > 0 &&' 'return coverage.valid && sourceFrameCount > 0 &&'}
    'NegativeRendererHardcodedSourceRate'{
        Edit-Source $renderer 'envelopePreroll ? 1 : gpu::kFormalSourceFrameRate.numerator,' 'envelopePreroll ? 1 : 60,'}
    'NegativeArtifactClaimsRefreshMapping'{
        Edit-Source $controller '{"target_mapping_uses_display_refresh", false},' '{"target_mapping_uses_display_refresh", true},'}
    'NegativeArtifactClaimsCountComparison'{
        Edit-Source $controller '{"count_comparison_used", false},' '{"count_comparison_used", true},'}
}
foreach($relativePath in $relatives){
    $targetPath=Join-Path $mutationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force|Out-Null
    Set-Content -LiteralPath $targetPath -Value $sources[$relativePath] -Encoding utf8 -NoNewline
}
$guardFailed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$guardFailed=$true}
if($Case-eq'Good'){
    if($guardFailed){throw '未変異sourceをB3-I6C guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I6C mapping guard $Case`: PASS";exit 0
}
if(-not$guardFailed){throw "$Case をB3-I6C guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I6C mapping guard $Case`: PASS"
