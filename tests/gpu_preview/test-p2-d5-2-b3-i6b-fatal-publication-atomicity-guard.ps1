[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeTokenPublishedBeforeValidation','NegativePendingTokenAfterFatal',
        'NegativeSuppressionCountedAsTransportFailure','NegativeSecondPublicationSite',
        'NegativeFormalReceiptAfterFatal','NegativePostFatalJoinReached',
        'NegativePrimaryFatalOverwrittenByJoin','NegativePrimaryFatalOverwrittenBySwap',
        'NegativeQueueRollback','NegativeSourceCoverageFailCloseRemoved',
        'NegativeSuppressionCounterHidden','NegativeNearestTokenRecovery')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$relatives=@(
    'src/app/preview/compositor_rhi_item.h',
    'src/app/preview/compositor_rhi_item.cpp',
    'src/app/preview/composition_token_publication.h',
    'src/app/preview/formal_present_join_admission.h',
    'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
    'src/media/gpu_preview/required_intent_queue.cpp',
    'apps/compositor_spike/compositor_spike_controller.cpp')
$mutationRoot=Join-Path $Directory "process-$PID"
$sources=@{}
foreach($relativePath in $relatives){
    $sources[$relativePath]=Get-Content -LiteralPath (Join-Path $SourceRoot $relativePath) -Raw -Encoding utf8
}
function Edit-Source([string]$RelativePath,[string]$From,[string]$To){
    $sourceText=$sources[$RelativePath]
    if($sourceText-notmatch[regex]::Escape($From)){throw "変異対象がありません: $RelativePath / $From"}
    $sources[$RelativePath]=$sourceText.Replace($From,$To)
}
$renderer='src/app/preview/compositor_rhi_item.cpp'
$publication='src/app/preview/composition_token_publication.h'
$admission='src/app/preview/formal_present_join_admission.h'
$header='src/app/preview/compositor_rhi_item.h'
$queue='src/media/gpu_preview/required_intent_queue.cpp'
$scheduler='src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$controller='apps/compositor_spike/compositor_spike_controller.cpp'
switch($Case){
    'Good'{}
    'NegativeTokenPublishedBeforeValidation'{
        Edit-Source $publication 'if (!publicationAllowed()) {' 'if (false) {'}
    'NegativePendingTokenAfterFatal'{
        Edit-Source $publication 'return active_ && valid_ && !sink_.protocolFatalLatched();' 'return active_ && valid_;'}
    'NegativeSuppressionCountedAsTransportFailure'{
        Edit-Source $publication '            sink_.noteSuppressedBeforePresent();' '            sink_.notePublicationFailure();'}
    'NegativeSecondPublicationSite'{
        Edit-Source $renderer 'return hook && hook->setCompositionToken(token);' "if (hook) hook->setCompositionToken(token);`n            return hook && hook->setCompositionToken(token);"}
    'NegativeFormalReceiptAfterFatal'{
        Edit-Source $admission '        input.formalSchedulerEnabled && input.nativeCaptureActive && !input.protocolFatalLatched;' '        input.formalSchedulerEnabled && input.nativeCaptureActive;'}
    'NegativePostFatalJoinReached'{
        Edit-Source $admission '        !input.protocolFatalLatched && !input.domainReached && input.formalCaptureActive;' '        !input.domainReached && input.formalCaptureActive;'}
    'NegativePrimaryFatalOverwrittenByJoin'{
        Edit-Source $renderer 'if (state_->fatalReason.empty())
                state_->fatalReason = "P2-D5-2 B3-I0 exact qualified commit失敗: " + reason;' 'state_->fatalReason = "P2-D5-2 B3-I0 exact qualified commit失敗: " + reason;'}
    'NegativePrimaryFatalOverwrittenBySwap'{
        Edit-Source $renderer 'if (state_->fatalReason.empty())
                    state_->fatalReason = std::string("P2-D5-2 render↔swap authority失敗: ") +' 'state_->fatalReason = std::string("P2-D5-2 render↔swap authority失敗: ") +'}
    'NegativeQueueRollback'{
        Edit-Source $queue '    // active reservationとunissued tailは意図的に保持する。closeはconsumeではない。' '    // rollback reservation history on non-normal close'}
    'NegativeSourceCoverageFailCloseRemoved'{
        Edit-Source $scheduler 'fail(PresentationOpportunityError::SourceCoverageInsufficient);' 'fail(PresentationOpportunityError::ArithmeticOverflow);'}
    'NegativeSuppressionCounterHidden'{
        Edit-Source $controller '{"token_publication_suppressed_before_present_count",' '{"token_publication_suppressed_before_present_count_disabled",'}
    'NegativeNearestTokenRecovery'{
        Edit-Source $renderer 'const auto hook = state_->nativePresentHook;
            return hook && hook->setCompositionToken(token);' "const auto hook = state_->nativePresentHook;`n            const auto nearestTokenCandidate = gpu::qpcTicks();`n            (void)nearestTokenCandidate;`n            return hook && hook->setCompositionToken(token);"}
}
foreach($relativePath in $relatives){
    $targetPath=Join-Path $mutationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force|Out-Null
    Set-Content -LiteralPath $targetPath -Value $sources[$relativePath] -Encoding utf8 -NoNewline
}
$guardFailed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$guardFailed=$true}
if($Case-eq'Good'){
    if($guardFailed){throw '未変異sourceをB3-I6B guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I6B atomicity guard $Case`: PASS";exit 0
}
if(-not$guardFailed){throw "$Case をB3-I6B guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I6B atomicity guard $Case`: PASS"
