[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeRequiredSetMutable','NegativeReserveAfterMapping',
        'NegativeDuplicateCreatesNew','NegativeRenderConsumes','NegativeQualifiedDequeueRemoved',
        'NegativeQualifiedWiringRemoved','NegativePlannedEndDropsActive',
        'NegativeDomainTerminalCompletion','NegativeDisplayImported',
        'NegativeObserverIdentity','NegativeConservationRemoved',
        'NegativeSourceCoverageSkipped')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Contract,
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$relatives=@(
    'src/media/gpu_preview/required_intent_queue.h',
    'src/media/gpu_preview/required_intent_queue.cpp',
    'src/media/gpu_preview/presentation_opportunity_scheduler.cpp',
    'src/app/preview/compositor_rhi_item.cpp',
    'apps/compositor_spike/compositor_spike_controller.cpp')
$root=Join-Path $Directory "process-$PID"
if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
$sources=@{}
foreach($relative in $relatives){$sources[$relative]=Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $SourceRoot $relative)}
function Edit-Source([string]$Relative,[string]$From,[string]$To){
    $text=$sources[$Relative]
    if($text-notmatch[regex]::Escape($From)){throw "変異対象がありません: $Relative / $From"}
    $sources[$Relative]=$text.Replace($From,$To)
}

switch($Case){
    'Good'{}
    'NegativeRequiredSetMutable'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' 'requiredIntentOrdinals_.push_back(ordinal);' 'requiredIntentOrdinals_.push_back(ordinal + 1);'}
    'NegativeReserveAfterMapping'{Edit-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp' 'requiredIntentQueue_.reserveHead()' 'requiredIntentQueue_.reserveAfterSourceMapping()'}
    'NegativeDuplicateCreatesNew'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' 'RequiredIntentReserveResult::Duplicate, activeReservation_' 'RequiredIntentReserveResult::Reserved, {++reservationSerial_, activeReservation_.intentOrdinal}'}
    'NegativeRenderConsumes'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' '++renderedCount_;' '++renderedCount_; ++headIndex_;'}
    'NegativeQualifiedDequeueRemoved'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' '++headIndex_;' 'headIndex_ += 0;'}
    'NegativeQualifiedWiringRemoved'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' 'commitQualifiedPresent(' 'observeQualifiedPresent('}
    'NegativePlannedEndDropsActive'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' 'plannedWindowEnded_ = plannedWindowEnded;' 'active_ = false; plannedWindowEnded_ = plannedWindowEnded;'}
    'NegativeDomainTerminalCompletion'{Edit-Source 'src/app/preview/compositor_rhi_item.cpp' '            formalDecisionObserved = true;' "            finishMeasurement(callbackBegin, StopArbitration::DomainTerminal, {});`n            formalDecisionObserved = true;"}
    'NegativeDisplayImported'{Edit-Source 'src/media/gpu_preview/required_intent_queue.h' 'bool displaySatisfactionImported = false;' 'bool displaySatisfactionImported = true;'}
    'NegativeObserverIdentity'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' 'bool RequiredIntentQueue::matches' "static long long DwmPresentQpc = 0;`n`nbool RequiredIntentQueue::matches"}
    'NegativeConservationRemoved'{Edit-Source 'src/media/gpu_preview/required_intent_queue.cpp' 'required == headIndex_ + activeCount + unissued' 'required >= headIndex_ + activeCount + unissued'}
    'NegativeSourceCoverageSkipped'{Edit-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp' 'PresentationOpportunityError::SourceCoverageInsufficient' 'PresentationOpportunityError::None'}
}

foreach($relative in $relatives){
    $target=Join-Path $root $relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)|Out-Null
    Set-Content -LiteralPath $target -Value $sources[$relative] -Encoding utf8 -NoNewline
}
$failed=$false
try{& $Contract -RepoRoot $root *> $null}catch{$failed=$true}
if($Case-eq'Good'){
    if($failed){throw '未変異のsourceをB3-I1 guardが拒否しました'}
    Write-Output "P2-D5-2 B3-I1 queue guard $Case`: PASS"
    exit 0
}
if(-not$failed){throw "$Case をB3-I1 guardが検出できませんでした (guardが空振りしています)"}
Write-Output "P2-D5-2 B3-I1 queue guard $Case`: PASS"
