param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$schedulerHeader=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.h'
$scheduler=Read-Source 'src/media/gpu_preview/presentation_opportunity_scheduler.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

# --- rate authorityのsingle producer -------------------------------------------
Require $schedulerHeader 'struct RequiredIntentRate' 'required-intent rate authorityの型がありません'
Require $schedulerHeader 'inline constexpr RequiredIntentRate kFormalRequiredIntentRate\{60, 1\}' 'canonical required-intent rate authorityがありません'
Require $schedulerHeader 'inline constexpr RequiredIntentRate kFormalSourceFrameRate\{60, 1\}' 'canonical source frame rate authorityがありません'
Require $schedulerHeader 'formalRequiredIntentCountForSeconds' 'required count producerがrate authorityから作られていません'
Require $schedulerHeader 'requiredIntentRateNumerator' 'configにrequired-intent rateがありません'

# --- mappingはdisplay refreshに依存しない ---------------------------------------
$mapping=[regex]::Match($scheduler,'(?s)bool presentationTargetFrameFor\([\s\S]*?\n\}').Value
if([string]::IsNullOrWhiteSpace($mapping)){throw 'presentationTargetFrameForを抽出できません'}
Require $mapping 'config\.requiredIntentRateDenominator' 'mappingがrequired-intent rateを使っていません'
Require $mapping 'config\.requiredIntentRateNumerator' 'mappingがrequired-intent rateを使っていません'
Reject $mapping 'refreshNumerator|refreshDenominator' 'mappingがdisplay refreshへ依存しています'
Require $mapping 'checkedMultiply' 'mappingがchecked arithmeticを通っていません'

# --- targetForはmappingを複製しない --------------------------------------------
$targetFor=[regex]::Match($scheduler,'(?s)bool PresentationOpportunityScheduler::targetFor\([\s\S]*?\n\}').Value
if([string]::IsNullOrWhiteSpace($targetFor)){throw 'targetForを抽出できません'}
Require $targetFor 'return presentationTargetFrameFor\(config_, ordinal, target\);' 'targetForがmapping実装へ委譲していません'
Reject $targetFor 'checkedMultiply' 'targetForがmapping式を複製しています'

# --- rateはfail-closeで必須 -----------------------------------------------------
Require $scheduler 'config\.requiredIntentRateNumerator <= 0 \|\| config\.requiredIntentRateDenominator <= 0\)\s*\r?\n\s*return fail\(PresentationOpportunityError::InvalidConfiguration\)' 'rate未設定をstartでfail-closeしていません'

# --- coverageはrequired set全域のexact rangeで判定する ---------------------------
$coverage=[regex]::Match($scheduler,'(?s)RequiredIntentSourceCoverage\s*\r?\nrequiredIntentSourceCoverage\([\s\S]*?\n\}').Value
if([string]::IsNullOrWhiteSpace($coverage)){throw 'requiredIntentSourceCoverageを抽出できません'}
Require $coverage 'for \(long long ordinal = 0; ordinal < config\.requiredFrameCount; \+\+ordinal\)' 'coverageがrequired set全域を走査していません'
Require $coverage 'coverage\.monotonicNonDecreasing = false' 'coverageがmonotonicityを検査していません'
Require $scheduler 'coverage\.maxTarget < sourceFrameCount' 'coverage成立条件がmax target < source frame countではありません'
Require $scheduler 'coverage\.minTarget >= 0' 'coverage成立条件にmin targetが含まれていません'

# --- controllerのpreflightはcount比較へ戻さない ---------------------------------
Require $controller 'requiredMeasurementFrameCount_ =\s*\r?\n?\s*gpu::formalRequiredIntentCountForSeconds' 'required countがrate authority由来ではありません'
Reject $controller 'config_\.measureSeconds\) \* 60' 'required countに第2のrate producerが残っています'
Require $controller 'gpu::requiredIntentSourceCoverageSatisfied\(requiredIntentSourceCoverage_' 'preflightがcoverage authorityを使っていません'
Reject $controller 'sourceAFrameCount_ >= requiredMeasurementFrameCount_' 'preflightがcount比較へ戻っています'
foreach($fieldName in @('required_intent_source_coverage','target_mapping_uses_display_refresh',
                        'monotonic_non_decreasing','min_target','max_target','coverage_rule',
                        'count_comparison_used')){
    Require $controller ([regex]::Escape($fieldName)) "I6C artifact fieldが不足しています: $fieldName"
}
Require $controller '"target_mapping_uses_display_refresh", false' 'mappingのdisplay非依存をartifactで固定していません'
Require $controller '"count_comparison_used", false' 'count比較不使用をartifactで固定していません'

# --- renderer configはrate authorityだけを参照する ------------------------------
Require $renderer 'envelopePreroll \? 1 : gpu::kFormalSourceFrameRate\.numerator' 'renderer configがsource rate authorityを参照していません'
Require $renderer 'gpu::kFormalRequiredIntentRate\.numerator,\s*\r?\n?\s*gpu::kFormalRequiredIntentRate\.denominator\}' 'renderer configがrequired-intent rate authorityを参照していません'
if(([regex]::Matches($renderer,'kFormalRequiredIntentRate')).Count-ne2){throw 'renderer側のrate producerが単一configではありません'}

Write-Output 'P2-D5-2 B3-I6C source mapping / coverage correction architecture: PASS'
