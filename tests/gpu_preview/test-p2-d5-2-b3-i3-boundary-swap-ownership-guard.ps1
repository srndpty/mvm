[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeSecondProducer','NegativeConsumeRemoved','NegativePresentSerialRemoved',
        'NegativeScopeExactRemoved','NegativeNearestQpc','NegativeEventSerialAuthority',
        'NegativeProductSemanticsChanged')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$relatives=@(
    'src/app/preview/compositor_rhi_item.h',
    'src/app/preview/compositor_rhi_item.cpp',
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
switch($Case){
    'Good'{}
    'NegativeSecondProducer'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' 'state_->formalOpportunityIgnoreNextSwap.store(true, std::memory_order_release);' "state_->formalOpportunityIgnoreNextSwap.store(true, std::memory_order_release);`n                state_->formalOpportunityIgnoreNextSwap.store(true, std::memory_order_release);"}
    'NegativeConsumeRemoved'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' 'formalOpportunityIgnoreNextSwap.exchange(false, std::memory_order_acq_rel)' 'formalOpportunityIgnoreNextSwap.load(std::memory_order_acquire)'}
    'NegativePresentSerialRemoved'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' 'event.receiptPresentSerial = receipt.presentSerial;' 'event.receiptPresentSerial = 0;'}
    'NegativeScopeExactRemoved'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' 'scopeRecord.tokenSerial != receipt.tokenSerial' 'scopeRecord.decisionQpc != gpu::qpcTicks()'}
    'NegativeNearestQpc'{Edit-Source 'apps/compositor_spike/compositor_spike_controller.cpp' 'const QJsonObject boundarySwapOwnershipAttribution{' "const auto nearestBoundaryQpc = gpu::qpcTicks();`n    (void)nearestBoundaryQpc;`n    const QJsonObject boundarySwapOwnershipAttribution{"}
    'NegativeEventSerialAuthority'{Edit-Source 'apps/compositor_spike/compositor_spike_controller.cpp' '{"event_serial_is_identity_authority", false}' '{"event_serial_is_identity_authority", true}'}
    'NegativeProductSemanticsChanged'{Edit-Source 'apps/compositor_spike/compositor_spike_controller.cpp' '{"product_semantics_changed", false}' '{"product_semantics_changed", true}'}
}
foreach($relativePath in $relatives){
    $targetPath=Join-Path $mutationRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $targetPath) -Force|Out-Null
    Set-Content -LiteralPath $targetPath -Value $sources[$relativePath] -Encoding utf8 -NoNewline
}
$guardFailed=$false
try{& $Contract -RepoRoot $mutationRoot *> $null}catch{$guardFailed=$true}
if($Case-eq'Good'){
    if($guardFailed){throw '未変異sourceをB3-I3 guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I3 boundary guard $Case`: PASS";exit 0
}
if(-not$guardFailed){throw "$Case をB3-I3 guardが検出できませんでした"}
Write-Output "P2-D5-2 B3-I3 boundary guard $Case`: PASS"
