param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$publication=Read-Source 'src/app/preview/composition_token_publication.h'
$admission=Read-Source 'src/app/preview/formal_present_join_admission.h'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$test=Read-Source 'tests/gpu_preview/test_i6b_publication_atomicity.cpp'

# --- production componentをtestが直接実行する ----------------------------------
Require $test 'app/preview/composition_token_publication\.h' 'testがproduction publication gateをincludeしていません'
Require $test 'app/preview/formal_present_join_admission\.h' 'testがproduction admission判定をincludeしていません'
Require $test 'mvm::app::CompositionTokenPublication publication\(' 'testがproduction RAII componentを実行していません'
Require $test 'mvm::app::formalPresentJoinAdmission\(' 'testがproduction admission判定を実行していません'
Require $test 'mvm::gpu::PresentationOpportunityScheduler scheduler;' 'testがproduction schedulerを使っていません'
# logicの複製を禁止する。testが自前のpublication classを持ってはならない。
Reject $test 'class\s+\w*(Fake|Stub|Copy)\w*TokenCapture' 'testがpublication componentを複製しています'
Reject $test 'publicationAllowed\(\)\s*(const)?\s*\{' 'testがpublication gateを再実装しています'
Reject $test 'formalEnvelopeActive\s*=' 'testがadmission判定を再実装しています'

# --- rendererとtestが同一componentを使う ----------------------------------------
Require $renderer 'app/preview/composition_token_publication\.h' 'rendererがproduction publication gateを使っていません'
Require $renderer 'CompositionTokenPublication publication_;' 'rendererがpublication componentを保持していません'
Require $renderer 'formalPresentJoinAdmission\(' 'rendererがadmission判定componentを使っていません'
if(([regex]::Matches($renderer,'hook->setCompositionToken\(')).Count-ne1){throw 'publication siteがexactly 1箇所ではありません'}
Require $publication 'bool publicationAllowed\(\) const \{[\s\S]{0,200}!sink_\.protocolFatalLatched\(\)' 'publication gateがfatal latchを見ていません'
Require $publication '~CompositionTokenPublication\(\) \{ publishOnScopeExit\(\); \}' 'publicationがscope exitで行われていません'
Require $publication 'sink_\.noteSuppressedBeforePresent\(\);' '抑止counterの記録がありません'
Require $admission '!input\.protocolFatalLatched' 'admission判定がfatal latchを見ていません'

# --- injection authorityはfatal latchだけ ---------------------------------------
Require $test 'TEST ONLY' 'test seamが明示されていません'
Require $test 'void injectProtocolFatal\(' 'fatal injection seamがありません'
if(([regex]::Matches($test,'TEST ONLY')).Count-ne1){throw 'test seamがexactly 1箇所ではありません'}
Reject $test '(?i)(fakePresentSerial|fakeTokenSerial|fakeQpc|fakeVblank|fabricat)' 'testがidentityを偽造しています'
Reject $test '(?i)(presentSerial|swapchainIdentity|physicalVblank)\s*=' 'testがPresent/VBlank identityを注入しています'
Reject $test '(?i)qpcTicks\(\)' 'testがQPC authorityを注入しています'

# --- 9段acceptanceがfield/counterとして観測されている ---------------------------
foreach($assertion in @(
    'publicationAllowed\(\)',
    'sink\.publishCallCount == 0',
    'sink\.suppressedBeforePresentCount == 1',
    'reach\.takeReceiptCalls == 0',
    'reach\.bindCalls == 0',
    'reach\.commitCalls == 0',
    'std::strcmp\(sink\.fatalReason',
    'afterQueue\.dequeuedCount == beforeQueue\.dequeuedCount',
    'afterQueue\.unissuedTailCount == beforeQueue\.unissuedTailCount',
    'afterQueue\.conservationValid',
    '!scheduler\.hasPendingQualifiedEvidence\(\)')){
    Require $test $assertion "9段acceptanceのassertionが不足しています: $assertion"
}
Require $test 'publishCallsAtInjection == 0' 'fatal injectionがpublicationより前であることを固定していません'

Write-Output 'P2-D5-2 B3-I6B publication atomicity injection architecture: PASS'
