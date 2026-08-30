param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$header=Read-Source 'src/app/preview/compositor_rhi_item.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$publication=Read-Source 'src/app/preview/composition_token_publication.h'
$admission=Read-Source 'src/app/preview/formal_present_join_admission.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$queue=Read-Source 'src/media/gpu_preview/required_intent_queue.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

# --- token publicationはpre-Present validation成功後だけ ------------------------
# gate本体はproduction componentにあり、rendererとintegration testが同じ実装を使う。
Require $publication 'bool publicationAllowed\(\) const \{[\s\S]{0,200}!sink_\.protocolFatalLatched\(\)' 'token publicationがfatal latchでgateされていません'
Require $publication '~CompositionTokenPublication\(\) \{ publishOnScopeExit\(\); \}' 'publicationがscope exitで行われていません'
Require $publication 'if \(!publicationAllowed\(\)\) \{(?:(?!return;)[\s\S])*sink_\.noteSuppressedBeforePresent\(\);' '抑止したpublicationを記録していません'
Reject $publication 'if \(!publicationAllowed\(\)\) \{(?:(?!return;)[\s\S])*notePublicationFailure' 'publication抑止をtransport failureとして数えています'
Require $publication 'const bool succeeded = sink_\.publishCompositionToken\(token_\);' 'publication siteがgate後のscope exitではありません'
Require $renderer 'CompositionTokenPublication publication_;' 'rendererがproduction publication componentを保持していません'
Require $renderer 'void noteSuppressedBeforePresent\(\) override \{\s*\r?\n\s*state_->nativePresentTokenSuppressedBeforePresentCount\.fetch_add' '抑止counterがstateへ接続されていません'
Require $renderer 'void notePublicationFailure\(\) override \{\s*\r?\n\s*state_->nativePresentTokenSetFailureCount\.fetch_add' 'token set failure counterの接続がありません'
if(([regex]::Matches($renderer,'hook->setCompositionToken\(')).Count-ne1){throw 'composition token publication siteがexactly 1箇所ではありません'}
Require $header 'nativePresentTokenSuppressedBeforePresentCount\{0\}' '抑止counterがstateにありません'
Require $controller '"token_publication_suppressed_before_present_count",' '抑止counterをartifactへ出していません'

# --- post-fatal Presentはformal join candidateにしない -------------------------
Require $renderer 'const bool protocolFatalLatched = state_->fatal\.load\(std::memory_order_acquire\);' 'frameSwappedでfirst protocol fatalを観測していません'
Require $renderer 'const auto joinAdmission = formalPresentJoinAdmission\(' 'admission判定componentを使っていません'
Require $renderer 'const bool formalEnvelopeActive = joinAdmission\.formalEnvelopeActive;' 'formal envelope判定が共有componentから来ていません'
Require $renderer 'if \(joinAdmission\.joinCommitAllowed\) \{' 'post-fatal frameSwappedがformal join/commitへ入り得ます'
Require $admission '!input\.protocolFatalLatched && !input\.domainReached && input\.formalCaptureActive;' 'commit gateがfatal latchを見ていません'
Require $admission 'input\.formalSchedulerEnabled && input\.nativeCaptureActive && !input\.protocolFatalLatched;' 'formal envelope判定がfatal latchを見ていません'

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
