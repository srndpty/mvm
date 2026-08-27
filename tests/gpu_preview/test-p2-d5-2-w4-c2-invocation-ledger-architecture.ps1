[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Read-Source([string]$Relative){Get-Content -LiteralPath (Join-Path $SourceRoot $Relative) -Raw -Encoding utf8}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}
$header=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'
Require $header 'OutsideSourceDomainDecision' 'source-domain result enumがありません'
Deny $header 'OutsideRequiredDomainDecision' 'scheduler resultにrequired-domain enumが混入しています'
Require $header 'requiredIntentMembership[\s\S]+transportDisposition' 'required membershipとtransport dispositionが別fieldではありません'
Require $scheduler 'return finishInvocation' '全returnがinvocation ledgerを通りません'
Require $scheduler 'CompletedOrdinalUnavailable[\s\S]+CompletedOrdinalOverflow[\s\S]+TargetArithmeticOverflow' 'fatal return reasonがbranch-exactではありません'
Require $renderer 'noteInvocationTransportDisposition' 'renderer transport dispositionをinvocationへexact joinしていません'
Require $controller 'mvm-p2-d5-2-w4-c2-scheduler-invocation-ledger-1' 'C2 schemaがemitされません'
Require $controller 'diagnostic_root_cause_capture[\s\S]+canonical_performance_authority' 'diagnostic/performance authority分離がありません'
Write-Output 'W4-C2 invocation ledger architecture: PASS'
