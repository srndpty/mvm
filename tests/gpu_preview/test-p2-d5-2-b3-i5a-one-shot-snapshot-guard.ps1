[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeAbiNotBumped','NegativeEpochRemoved','NegativeRawTokenRemoved',
        'NegativeRawReceiptRemoved','NegativeSnapshotConsumesToken',
        'NegativeSnapshotConsumesReceipt','NegativeThreadCheckRemoved',
        'NegativePresentReceiptThreadChecksRemoved',
        'NegativeCaptureOutsideAccepted','NegativeLayoutCheckRemoved',
        'NegativeSameSizeSemanticFieldOffsetMutation',
        'NegativeNestedSemanticFieldOffsetMutation','NegativeLatestPresentInference','NegativeMeasurementDisconnected',
        'NegativeHistoricalMismatchReclassified')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$relatives=@(
    'src/app/preview/native_present_hook_abi.h',
    'src/app/preview/native_present_hook.h',
    'src/app/preview/native_present_hook.cpp',
    'src/app/preview/compositor_rhi_item.cpp',
    'apps/compositor_spike/compositor_spike_controller.cpp',
    'qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch',
    'docs/p2-d5-2-b3-i4-preroll-transition-quiescence.json')
# S2-h: PID だけでは isolation key にならない。Windows は PID を再利用するため、
# 過去 run の process-<PID> directory と衝突して「既存artifactを上書きしません」で
# 失敗する。S2-f2 と同じく invocation ごとに一意な suffix を付ける。
$mutationRoot=Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
$sources=@{}
foreach($relative in $relatives){$sources[$relative]=Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $SourceRoot $relative)}
function Edit-Source([string]$Relative,[string]$From,[string]$To){
    $sourceText=$sources[$Relative]
    if($sourceText-notmatch[regex]::Escape($From)){throw "変異対象がありません: $Relative / $From"}
    $sources[$Relative]=$sourceText.Replace($From,$To)
}

$abi='src/app/preview/native_present_hook_abi.h'
$renderer='src/app/preview/compositor_rhi_item.cpp'
$patch='qt-patches/qtbase-6.11.1/0001-mvm-native-present-hook.patch'
switch($Case){
    'Good'{}
    'NegativeAbiNotBumped'{Edit-Source $abi 'MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = 6' 'MVM_NATIVE_PRESENT_HOOK_ABI_VERSION = 5'}
    'NegativeEpochRemoved'{Edit-Source $patch 'ring->captureEpoch != expectedCaptureEpoch' 'ring->captureEpoch == 0'}
    'NegativeRawTokenRemoved'{Edit-Source $patch 'exact.pendingToken = mvmPendingToken;' 'exact.pendingToken = {};'}
    'NegativeRawReceiptRemoved'{Edit-Source $patch 'exact.pendingReceipt = mvmFrameSwappedReceipt;' 'exact.pendingReceipt = {};'}
    'NegativeSnapshotConsumesToken'{Edit-Source $patch 'exact.pendingToken = mvmPendingToken;' "exact.pendingToken = mvmPendingToken;`n+    mvmPendingTokenValid = false;"}
    'NegativeSnapshotConsumesReceipt'{Edit-Source $patch 'exact.pendingReceipt = mvmFrameSwappedReceipt;' "exact.pendingReceipt = mvmFrameSwappedReceipt;`n+    mvmFrameSwappedReceipt = {};"}
    'NegativeThreadCheckRemoved'{Edit-Source $patch 'ring->captureThreadId != callerThreadId' 'ring->captureThreadId == 0'}
    'NegativePresentReceiptThreadChecksRemoved'{Edit-Source $patch 'ring->captureThreadId != GetCurrentThreadId()' 'ring->captureThreadId != ring->captureThreadId'}
    'NegativeCaptureOutsideAccepted'{Edit-Source $patch 'if (!ring || ring->enabled == 0)' 'if (!ring)'}
    'NegativeLayoutCheckRemoved'{Edit-Source $patch 'mvmNativePresentOneShotSnapshotLayoutCompatible(' 'mvmNativePresentOneShotSnapshotLayoutIgnored('}
    'NegativeSameSizeSemanticFieldOffsetMutation'{Edit-Source $abi 'offsetof(MvmNativePresentOneShotSnapshot, captureActive)' 'offsetof(MvmNativePresentOneShotSnapshot, captureThreadId)'}
    'NegativeNestedSemanticFieldOffsetMutation'{Edit-Source $abi 'offsetof(MvmNativePresentFrameSwappedReceipt, hresult)' 'offsetof(MvmNativePresentFrameSwappedReceipt, tokenPresent)'}
    'NegativeLatestPresentInference'{Edit-Source $patch 'exact.pendingReceipt = mvmFrameSwappedReceipt;' "exact.pendingReceipt = mvmFrameSwappedReceipt;`n+    const auto latestPresent = ring->records[ring->recordCount - 1];"}
    'NegativeMeasurementDisconnected'{Edit-Source $renderer 'readOneShotSnapshot(hook->captureEpoch()' 'readOneShotSnapshot(0'}
    'NegativeHistoricalMismatchReclassified'{Edit-Source 'docs/p2-d5-2-b3-i4-preroll-transition-quiescence.json' '"reclassified_as_i3_boundary_failure": false' '"reclassified_as_i3_boundary_failure": true'}
}
foreach($relative in $relatives){
    $target=Join-Path $mutationRoot $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)|Out-Null
    Set-Content -NoNewline -Encoding utf8 -LiteralPath $target -Value $sources[$relative]
}
$failed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){throw '未変異sourceをB3-I5A guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I5A snapshot guard $Case`: PASS";exit 0
}
if(-not$failed){throw "$Case をB3-I5A guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I5A snapshot guard $Case`: PASS"
