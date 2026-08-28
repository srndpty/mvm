param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$header=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$queue=Read-Source 'src/media/gpu_preview/required_intent_queue.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

# --- token publicationはpre-Present validation成功後だけ ------------------------
Require $renderer 'bool publicationAllowed\(\) const \{[\s\S]{0,200}!state_->fatal\.load\(std::memory_order_acquire\)' 'token publicationがfatal latchでgateされていません'
Require $renderer '~NativePresentTokenCapture\(\)[\s\S]{0,900}if \(!publicationAllowed\(\)\) \{(?:(?!return;)[\s\S])*return;' 'destructorがpublication gateを通っていません'
Require $renderer 'if \(!publicationAllowed\(\)\) \{(?:(?!return;)[\s\S])*nativePresentTokenSuppressedBeforePresentCount\.fetch_add' '抑止したpublicationを記録していません'
# 抑止はtransport failureではない。token set failure counterへ混ぜない。
Reject $renderer 'if \(!publicationAllowed\(\)\) \{(?:(?!return;)[\s\S])*nativePresentTokenSetFailureCount' 'publication抑止をtoken set failureとして数えています'
if(([regex]::Matches($renderer,'hook->setCompositionToken\(')).Count-ne1){throw 'composition token publication siteがexactly 1箇所ではありません'}
Require $renderer 'const bool succeeded = hook && hook->setCompositionToken\(token_\);' 'publication siteがgate後のdestructorではありません'
Require $header 'nativePresentTokenSuppressedBeforePresentCount\{0\}' '抑止counterがstateにありません'
Require $controller '"token_publication_suppressed_before_present_count",' '抑止counterをartifactへ出していません'

# --- post-fatal Presentはformal join candidateにしない -------------------------
Require $renderer 'const bool protocolFatalLatched = state_->fatal\.load\(std::memory_order_acquire\);' 'frameSwappedでfirst protocol fatalを観測していません'
Require $renderer 'const bool formalEnvelopeActive =[\s\S]{0,300}!protocolFatalLatched;' 'post-fatal Presentをformal envelopeから除外していません'
Require $renderer 'if \(!protocolFatalLatched &&[\s\S]{0,300}formalOpportunityCaptureActive\.load' 'post-fatal frameSwappedがformal join/commitへ入り得ます'

# --- first protocol fatalはimmutable -------------------------------------------
if(([regex]::Matches($renderer,'state_->fatalReason = ')).Count-ne3){throw 'fatal reason writerの数が想定と一致しません'}
if(([regex]::Matches($renderer,'if \(state_->fatalReason\.empty\(\)\)')).Count-ne3){throw 'fatal reason writerがfirst-writer-winsではありません'}

# --- queue historyはrollbackしない ---------------------------------------------
Require $queue 'active reservationとunissued tailは意図的に保持' 'non-normal closeがactive/tailを保持する契約がありません'
Reject $queue '(?i)(rollback|undo|revert)' 'queueがreservation historyをrollbackしています'
Reject $scheduler '(?i)(rollback|undo)[A-Za-z]*Reservation' 'schedulerがreservationをrollbackしています'
Require $scheduler 'fail\(PresentationOpportunityError::SourceCoverageInsufficient\)' 'source coverage fail-close siteがありません'

# --- 推定によるcancel / recoveryの禁止 ------------------------------------------
Reject $renderer '(?i)(cancelToken|revokeToken|recoverTransaction|nearestToken|latestToken)' 'nearest/latest/推定によるtoken cancelまたはtransaction recoveryがあります'
Reject $renderer '(?i)token[A-Za-z]*(FromQpc|ByQpc|Estimate|Inference)' 'QPC/serial推定からtoken identityを作っています'

Write-Output 'P2-D5-2 B3-I6B fatal-before-Present publication atomicity architecture: PASS'
