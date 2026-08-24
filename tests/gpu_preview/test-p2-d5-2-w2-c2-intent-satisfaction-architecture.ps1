[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$runner=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/build-p2-d5-2-w2-c2-intent-satisfaction-ledger.ps1') -Raw -Encoding utf8
$checker=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/check-p2-d5-2-w2-c2-intent-satisfaction-ledger.ps1') -Raw -Encoding utf8
$fromC1=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c2-from-c1-core.ps1') -Raw -Encoding utf8
$core=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c2-intent-satisfaction-core.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require $runner 'check-p2-d5-2-w2-c14-sealed-mapping-replay\.ps1' 'runnerがC1 sealed/replay checkerをconsumeしていません'
Require $checker '& pwsh[\s\S]+C1Checker[\s\S]+C1 sealed/replay authority' 'C2 checkerがC1 authorityを単独再確認していません'
Require $fromC1 'Invoke-MvmC13FormalPresentedPopulation[\s\S]+Invoke-MvmDisplayedQpcPhysicalMapping' 'sealed C1 formal population / physical mappingを再生していません'
Require $fromC1 'required_measurement_frame_count[\s\S]+required_domain_derived_from_presented_min_max=\$false' 'Layer 1A required domainをPresented min/maxから独立に取得していません'
Require $core 'DUPLICATE_CURRENT_INTENT_SATISFACTION' 'duplicate current intent fail-closeがありません'
Require $core 'CURRENT_INTENT_OUTSIDE_REQUIRED_DOMAIN' 'current intent domain fail-closeがありません'
Require $core 'INTENT_PROVENANCE_MISSING[\s\S]+INTENT_PROVENANCE_AMBIGUOUS' 'intent provenance fail-closeがありません'
Require $core 'MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL' 'physical ordinal uniqueness fail-closeがありません'
Require $core 'source_frame_identity_used=\$false' 'source frame identity不使用が固定されていません'
Require $core 'performance_threshold_evaluated=\$false[\s\S]+frame_swapped_retirement_changed=\$false' 'C2禁止事項が固定されていません'
Write-Output 'W2-C2 intent satisfaction architecture: PASS'
