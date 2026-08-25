[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Read-Script([string]$Relative){
    $path=Join-Path $SourceRoot $Relative
    if(-not(Test-Path -LiteralPath $path)){throw "W2-D scriptがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$runner=Read-Script 'scripts/build-p2-d5-2-w2-d-formal-v2-shadow.ps1'
$checker=Read-Script 'scripts/check-p2-d5-2-w2-d-formal-v2-shadow.ps1'
$fromAuthorities=Read-Script 'scripts/p2-d5-2-w2-d-from-authorities-core.ps1'
$core=Read-Script 'scripts/p2-d5-2-w2-d-formal-v2-shadow-core.ps1'

# 各upstream checkerを再実行すること。hash一致だけをauthorityにしない。
foreach($upstream in @(
    @('check-p2-d5-2-w2a-physical-domain\.ps1','W2-A physical domain'),
    @('check-p2-d5-2-w2-b2-terminal-shadow\.ps1','W2-B1/B2 transport'),
    @('check-p2-d5-2-w2-c14-sealed-mapping-replay\.ps1','C1 sealed mapping replay'),
    @('check-p2-d5-2-w2-c21-required-intent-domain\.ps1','C2.1 required intent domain'),
    @('check-p2-d5-2-w2-c2-intent-satisfaction-ledger\.ps1','C2 intent satisfaction ledger'),
    @('check-p2-d5-2-w2-c24-formal-transport\.ps1','C2.4 formal transport'))){
    Require $runner $upstream[0] "runnerが$($upstream[1]) checkerをconsumeしていません"
    Require $checker $upstream[0] "checkerが$($upstream[1]) checkerをconsumeしていません"
}
Require $fromAuthorities 'Invoke-MvmDUpstreamAuthorityReplay' 'upstream checker再実行が一本化されていません'
Require $fromAuthorities "-File \`$W2AChecker[\s\S]+'status'\)-ne'PASS'" 'W2-A checkerのstatusまで確認していません'

# aggregateのコピーではなく sealed source から再構築すること。
Require $fromAuthorities 'Invoke-MvmC13FormalPresentedPopulation[\s\S]+Invoke-MvmDisplayedQpcPhysicalMapping' `
    'sealed sourceからformal population / physical mappingを再生していません'
Require $fromAuthorities 'Invoke-MvmDFormalV2ShadowIntegration' 'recordsからintegrationを再計算していません'

# cross-cohort splice fail-close。
foreach($splice in @(
    @('C2\.1が別C1 cohortを参照しています','C2.1 / C1'),
    @('C2が別C1 cohortを参照しています','C2 / C1'),
    @('C2が別C2\.1 required intent authorityを参照しています','C2 / C2.1'),
    @('W2-A physical domainが別runのものです','W2-A physical domain'),
    @('sealed source hashが一致しません','sealed source'))){
    Require $fromAuthorities $splice[0] "$($splice[1]) のcross-cohort spliceをfail-closeしていません"
}

# W2-D固有のfail-close。
foreach($blocker in @('CURRENT_INTENT_OUTSIDE_REQUIRED_INTENT_SET','FOREIGN_INTENT_INSIDE_REQUIRED_INTENT_SET',
    'DUPLICATE_SATISFIED_INTENT','MULTIPLE_FORMAL_PRESENTED_PER_PHYSICAL_ORDINAL',
    'FORMAL_V2_CHAIN_PROVENANCE_MISSING','FORMAL_V2_FINAL_STATE_NOT_PRESENTED',
    'LAYER1A_REQUIRED_ACCOUNTING_IDENTITY_VIOLATION','PRESENTED_ACCOUNTING_IDENTITY_VIOLATION',
    'FILLED_PHYSICAL_OPPORTUNITY_IDENTITY_VIOLATION','PHYSICAL_VBLANK_DOMAIN_CARDINALITY_INVALID')){
    Require $core $blocker "W2-D fail-closeがありません: $blocker"
}
Require $fromAuthorities 'C2_LEDGER_INTEGRATION_DIVERGENCE' 'C2 ledgerとの一致をfail-closeしていません'

# noncanonical shadowであることの固定。
foreach($flag in @('shadow_only=\$true','canonical_authority=\$false','performance_threshold_evaluated=\$false',
    'canonical_verdict_evaluated=\$false','frame_swapped_retirement_changed=\$false',
    'source_frame_identity_used=\$false','nearest_qpc_or_tolerance_used=\$false',
    'layer1a_layer1b_count_difference_is_not_a_verdict=\$true')){
    Require $core $flag "W2-D run recordのauthority flagが固定されていません: $flag"
    Require $fromAuthorities $flag "W2-D artifactのauthority flagが固定されていません: $flag"
}
Require $checker 'W2-Dで禁止されているflagがtrueです' 'checkerがcanonical / performance flag注入をrejectしていません'
Require $checker 'performance semanticsのfieldがあります' 'checkerがperformance semantics fieldの注入をrejectしていません'

# source frame identityをintent identityに使わないこと。
Deny $core 'source_frame(?!_identity_used)' 'W2-D integration coreがsource frameを参照しています'
Deny $fromAuthorities 'source_frame(?!_identity_used)' 'W2-D統合coreがsource frameを参照しています'
Require $fromAuthorities 'exact_event_key=\$key' 'identity keyがexact_event_keyではありません'

# nearest QPC / tolerance / phase interpolation、threshold判定を持ち込まないこと。
# 「使っていない」と宣言しているflag名自体は除いてから走査する。
$coreScan=(@($core-split"`n"|Where-Object{$_-notmatch'^\s*#'})-join"`n") `
    -replace 'nearest_qpc_or_tolerance_used','' -replace 'performance_threshold_evaluated',''
foreach($forbidden in @('nearest','tolerance','interpolat','drop_rate','effective_fps','threshold','fps')){
    Deny $coreScan ("(?i)"+$forbidden) "W2-D coreに禁止語があります: $forbidden"
}
# Layer 1A と Layer 1B の count差をblockerにしていないこと。
Deny $core 'required_intent_count-ne\$PhysicalOpportunityCount' 'Layer 1A / Layer 1Bのcount差をFAILにしています'
Write-Output 'W2-D formal-v2 shadow architecture: PASS'
