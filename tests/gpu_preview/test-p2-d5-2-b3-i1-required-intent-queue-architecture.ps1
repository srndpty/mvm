param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$Relative){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $Relative)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$queue=Read-Source 'src/media/gpu_preview/required_intent_queue.cpp'
$queueHeader=Read-Source 'src/media/gpu_preview/required_intent_queue.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'

Require $queue 'for\s*\(long long ordinal = 0; ordinal < requiredCount; \+\+ordinal\)[\s\S]+requiredIntentOrdinals_\.push_back\(ordinal\)' 'immutable required set [0,N)をstart時に生成していません'
Require $queue 'if \(active_\)[\s\S]+RequiredIntentReserveResult::Duplicate, activeReservation_' 'duplicateが同一active reservationを返しません'
Require $queue 'RequiredIntentQueue::markRenderComplete[\s\S]+rendered_ = true;[\s\S]+\+\+renderedCount_' 'render completionがstateだけを進めません'
Require $queue 'RequiredIntentQueue::commitQualified[\s\S]+\+\+headIndex_;[\s\S]+\+\+qualifiedCommitCount_' 'qualified commitだけのexactly-one dequeueがありません'
if(([regex]::Matches($queue,'\+\+headIndex_')).Count-ne1){throw 'head dequeue siteがexactly 1箇所ではありません'}
Require $scheduler 'requiredIntentQueue_\.reserveHead\(\)' 'schedulerがactual queue headをreserveしていません'
if($scheduler.IndexOf('requiredIntentQueue_.reserveHead()')-ge$scheduler.IndexOf('targetFor(ordinal, target)')){throw 'source mappingより先にqueue headをreserveしていません'}
Require $scheduler 'requiredIntentQueue_\.markRenderComplete\(pendingDecision_\.reservationId,[\s\S]+pendingDecision_\.opportunityOrdinal\)' 'matching render completionをqueue stateへ接続していません'
Require $renderer 'QualifiedCommitResult::QualifiedCommit\)[\s\S]+commitQualifiedPresent\([\s\S]+commitSwap\(' 'I0 QUALIFIED_COMMIT evidenceとswap commitを順に接続していません'
$qualifiedStart=$scheduler.IndexOf('bool PresentationOpportunityScheduler::commitQualifiedPresent')
$swapStart=$scheduler.IndexOf('bool PresentationOpportunityScheduler::commitSwap')
$finalizeStart=$scheduler.IndexOf('bool PresentationOpportunityScheduler::finalizePendingOpportunity')
if($qualifiedStart-lt0-or$swapStart-le$qualifiedStart-or$finalizeStart-le$swapStart){throw 'qualified/swap/finalize function境界を特定できません'}
$qualifiedBody=$scheduler.Substring($qualifiedStart,$swapStart-$qualifiedStart)
$swapBody=$scheduler.Substring($swapStart,$finalizeStart-$swapStart)
Reject $qualifiedBody 'requiredIntentQueue_\.commitQualified' 'I0 QUALIFIED_COMMIT時点でqueueをdequeueしています'
Require $qualifiedBody 'pendingQualifiedEvidence_\s*=\s*true' 'I0 evidenceをpending stateへ固定していません'
$dequeueIndex=$swapBody.IndexOf('requiredIntentQueue_.commitQualified')
if($dequeueIndex-lt0){throw 'swap commit pointにqueue dequeueがありません'}
foreach($marker in @('swapQpc <= 0','pendingDecision_.renderOrdinal !=','swapOrdinal !=','presentationAuthorityUsable','presentationOpportunityOrdinal','preparePendingOpportunityFinalization')){
    $markerIndex=$swapBody.IndexOf($marker)
    if($markerIndex-lt0-or$markerIndex-ge$dequeueIndex){throw "swap failure validationより前にdequeueしています: $marker"}
}
$applyIndex=$swapBody.IndexOf('if (establishesAnchor)')
if($applyIndex-le$dequeueIndex){throw 'queue dequeueとscheduler applyのcommit point順序が不正です'}
Reject $swapBody.Substring($applyIndex) 'return\s+(fail\(|false)' 'queue dequeue後にfailure pathが残っています'
Require (Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp') 'online_queue_state[\s\S]+issued_count[\s\S]+qualified_commit_count[\s\S]+dequeued_count[\s\S]+active_reservation_count[\s\S]+unissued_tail_count[\s\S]+conservation_valid' '後段accountingへonline queue conservation stateを出力していません'
Require $scheduler 'SourceCoverageInsufficient' 'source coverage不足の明示的contract failureがありません'
Require $scheduler 'error_ != PresentationOpportunityError::None[\s\S]+closeWithoutNormalCompletion\(\)[\s\S]+closed_ = true' 'source coverage fatal後のnon-normal cleanupがqueue snapshotを閉じません'
Require $renderer 'cause == StopArbitration::PlannedWindowEnd[\s\S]+closePlannedWindow\(\)[\s\S]+closeWithoutNormalCompletion\(\)' 'PLANNED_WINDOW_ENDだけがnormal completion ownerではありません'
Reject $renderer 'finishMeasurement\(callbackBegin,\s*StopArbitration::DomainTerminal' 'DOMAIN_TERMINALをsuccessful completionにしています'
Require $queue 'active reservationとunissued tailは意図的に保持する' 'planned endのinflight/tail保存契約がありません'
Reject $queue 'RequiredIntentQueue::close[\s\S]{0,500}(active_\s*=\s*false|requiredIntentOrdinals_\.clear|headIndex_\s*\+\+)' 'closeがactive reservationまたはrequired setをconsumeしています'
Require $queueHeader 'bool displaySatisfactionImported = false;' 'committedとdisplay satisfiedを分離していません'
Reject $queue '(?i)(Dwm|FinalState|DisplayedQpc|callbackIndex|sourceFrame|physicalObserver|previousOrdinal)' 'online queueへ禁止observer identityを混入しています'
Require $queue 'headIndex_ == qualifiedCommitCount_[\s\S]+issuedCount_ == headIndex_ \+ activeCount[\s\S]+renderedCount_ == qualifiedCommitCount_ \+ activeRenderedCount[\s\S]+required == headIndex_ \+ activeCount \+ unissued' 'online queue conservation invariantがありません'

Write-Output 'P2-D5-2 B3-I1 required-intent queue architecture: PASS'
