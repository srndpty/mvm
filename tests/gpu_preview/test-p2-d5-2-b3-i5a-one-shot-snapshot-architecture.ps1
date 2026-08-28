param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$Relative){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $Relative)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}
function Get-FunctionBody([string]$Text,[string]$Name){
    $match=[regex]::Match($Text,"$([regex]::Escape($Name))\s*\(\)\s*\{(?<body>[\s\S]+?)\r?\n\}")
    if(-not$match.Success){throw "layout signature functionを抽出できません: $Name"}
    return $match.Groups['body'].Value
}

$abi=Read-Source 'src/app/preview/native_present_hook_abi.h'
$hookHeader=Read-Source 'src/app/preview/native_present_hook.h'
$hook=Read-Source 'src/app/preview/native_present_hook.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'
$patch=Read-Source 'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'
$i4Design=Read-Source 'docs/p2-d5-2-b3-i4-preroll-transition-quiescence.json'

Require $abi 'MVM_NATIVE_PRESENT_HOOK_ABI_VERSION\s*=\s*6' 'one-shot snapshot追加後のABIがv6ではありません'
foreach($field in @('captureEpoch','captureThreadId','callerThreadId','callerThreadExact','pendingTokenValid','pendingReceiptValid','pendingToken','pendingReceipt')){
    Require $abi ([regex]::Escape($field)) "snapshot ABI fieldが不足しています: $field"
}
Require $abi 'mvmNativePresentOneShotSnapshotLayoutSignature' 'snapshot layout signatureがありません'
$sourceSignature=Get-FunctionBody $abi 'mvmNativePresentSourceIdentitySemanticLayoutSignature'
foreach($field in @('sourceId','sourceGeneration','resourceEpoch','frameNumber')){
    Require $sourceSignature "offsetof\(MvmNativePresentSourceIdentity,\s*$field\)" "source identity semantic fieldが署名されていません: $field"
}
$tokenSignature=Get-FunctionBody $abi 'mvmNativePresentCompositionTokenSemanticLayoutSignature'
Require $tokenSignature 'mvmNativePresentSourceIdentitySemanticLayoutSignature' 'token署名がnested source identity layoutを閉じていません'
foreach($field in @('tokenSerial','compositionEpoch','compositionState','outputFrameNumber','intentOrdinal','intentOrdinalValid','sourceCount','propagationSerial','sources')){
    Require $tokenSignature "offsetof\(MvmNativePresentCompositionToken,\s*$field\)" "composition token semantic fieldが署名されていません: $field"
}
$receiptSignature=Get-FunctionBody $abi 'mvmNativePresentFrameSwappedReceiptSemanticLayoutSignature'
foreach($field in @('presentSerial','swapchainIdentity','hresult','tokenPresent','tokenSerial','intentOrdinal','intentOrdinalValid')){
    Require $receiptSignature "offsetof\(MvmNativePresentFrameSwappedReceipt,\s*$field\)" "frameSwapped receipt semantic fieldが署名されていません: $field"
}
$oneShotSignature=Get-FunctionBody $abi 'mvmNativePresentOneShotSnapshotLayoutSignature'
Require $oneShotSignature 'mvmNativePresentCompositionTokenSemanticLayoutSignature' 'snapshot署名がnested token semantic layoutを閉じていません'
Require $oneShotSignature 'mvmNativePresentFrameSwappedReceiptSemanticLayoutSignature' 'snapshot署名がnested receipt semantic layoutを閉じていません'
foreach($field in @('abiVersion','snapshotSize','layoutSignature','captureEpoch','captureActive','captureThreadId','callerThreadId','callerThreadExact','pendingTokenValid','pendingReceiptValid','pendingToken','pendingReceipt')){
    Require $oneShotSignature "offsetof\(MvmNativePresentOneShotSnapshot,\s*$field\)" "snapshot semantic fieldが署名されていません: $field"
}
Require $abi 'oneShotSnapshotSize[\s\S]+oneShotSnapshotLayoutSignature[\s\S]+mvmNativePresentHookLayoutCompatible' 'ring handshakeがsnapshot size/layoutを検査しません'
Require $patch 'mvmCaptureEpochSerial[\s\S]+InterlockedIncrement64\(&mvmCaptureEpochSerial\)[\s\S]+ring->captureEpoch' 'capture epochをbegin時にmintしていません'
Require $patch 'mvm_qt_d3d11_present_hook_one_shot_snapshot' 'read-only snapshot exportがありません'

$snapshotMatch=[regex]::Match($patch,'mvm_qt_d3d11_present_hook_one_shot_snapshot[\s\S]+?\+\}\r?\n\+\r?\n QT_BEGIN_NAMESPACE')
if(-not$snapshotMatch.Success){throw 'snapshot export bodyを抽出できません'}
$snapshotBody=$snapshotMatch.Value
Require $snapshotBody 'mvmNativePresentOneShotSnapshotLayoutCompatible' 'snapshot call時のABI/layout検査がありません'
Require $snapshotBody 'if \(!ring \|\| ring->enabled == 0\)' 'capture外snapshotを拒否していません'
Require $snapshotBody 'ring->captureEpoch != expectedCaptureEpoch' 'snapshot call時のepoch mismatch検査がありません'
Require $snapshotBody 'ring->captureThreadId != callerThreadId' 'snapshot call時のthread mismatch検査がありません'
Require $snapshotBody 'exact\.pendingToken = mvmPendingToken[\s\S]+exact\.pendingReceipt = mvmFrameSwappedReceipt' 'pending raw identityをそのままcopyしていません'
Reject $snapshotBody 'mvmPendingTokenValid\s*=|mvmFrameSwappedReceiptValid\s*=' 'snapshot exportがone-shot validityをmutateしています'
Reject $snapshotBody 'mvmPendingToken\s*=\s*\{|mvmFrameSwappedReceipt\s*=\s*\{' 'snapshot exportがone-shot raw stateをresetしています'
Reject $snapshotBody '(?i)(latest|nearest|callbackIndex|qpc|presentSerial\s*[+]|tokenSerial\s*[+])' 'snapshot exportがpending stateを推定しています'

Require $patch 'mvmBeginPresentCapture[\s\S]+ring->captureThreadId != GetCurrentThreadId\(\)' 'actual Present thread contractがありません'
Require $patch 'mvm_qt_d3d11_present_hook_take_frame_swapped_receipt[\s\S]+ring->captureThreadId != GetCurrentThreadId\(\)' 'frameSwapped receipt thread contractがありません'
Require $hook 'mvm_qt_d3d11_present_hook_one_shot_snapshot' 'app loaderがsnapshot exportを必須にしていません'
Require $hookHeader 'readOneShotSnapshot' 'app read-only wrapperがありません'
Require $hook 'expectedCaptureEpoch != captureEpoch_[\s\S]+oneShotSnapshot_\(expectedCaptureEpoch[\s\S]+mvmNativePresentOneShotSnapshotExact' 'app wrapperがepoch/thread/layoutをexact検査していません'
Reject ($renderer+$controller) 'readOneShotSnapshot' 'I5A snapshotをmeasurement transitionへ接続しています'
Require $i4Design '"status": "UNRESOLVED_HISTORICAL_RUNTIME_FAILURE"' 'historical COMPOSITION_TOKEN_MISMATCHを変更しています'
Require $i4Design '"reclassified_as_i3_boundary_failure": false' 'historical mismatchをI3へ再分類しています'

Write-Output 'P2-D5-2 B3-I5A Qt one-shot exact snapshot architecture: PASS'
