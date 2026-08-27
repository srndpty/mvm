[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$core=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c1-mapping-core.ps1') -Raw -Encoding utf8
$runner=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/map-p2-d5-2-w2-c1-displayed-qpc.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require $core 'previousQpc-lt\$qpc-and\$qpc-lt\$sampleQpc' 'DisplayedQPC causal cell relationがありません'
Require $core "mapping_uses_nearest_qpc=\`$false[\s\S]+mapping_uses_arbitrary_tolerance=\`$false" 'nearest/tolerance禁止が固定されていません'
Require $core "mapping_filters_measurement_window_before_mapping=\`$false" 'mapping前window filter禁止が固定されていません'
foreach($field in @('present_start_qpc','present_return_qpc','layer2_membership','intent_scope','source_frame')){
    Require $core ("mapping_uses_{0}=\`$false" -f $field) "$field 非推論契約がありません"
}
Require $runner 'Inventory[\s\S]+-RequireCoverageComplete' 'C0.1.1 coverage authorityを再検証していません'
Require $runner 'Invoke-MvmC13FormalPresentedPopulation[\s\S]+-B2TerminalRecords' 'B2 Presented authorityからformal populationを定義していません'
Require $runner '\$presentedCandidates=@\(\$formalPopulation\.formal_candidates\)' 'B2 exact set join結果をformal mappingへ渡していません'
Require $runner '\$observedCandidates=@\(\$runInventory\.candidates\)' 'observed Presented populationを保持していません'
Require $runner 'observed_physical_mapping_diagnostic' 'observed physical mapping diagnosticを保持していません'
Require $runner 'candidate_count_identity' 'B2/C1 formal candidate count identityがありません'
if($runner-match "candidates\|Where-Object[\s\S]{0,200}(display_relation|upstream_exact)"){
    throw 'display relation/upstream exactでformal membershipを定義しています'
}
Require $runner 'SupportChecker[\s\S]+physical_mapping_support_envelope_shadow' 'C1.1 mapping support authorityをconsumeしていません'
Require $runner "PredecessorOrdinal \(\[int64\]\(Need \`$mappingSupport 'predecessor_ordinal'\)\)" 'C1 mapping lower supportがC1.1 provenanceではありません'
Require $runner "SuccessorOrdinal \(\[int64\]\(Need \`$mappingSupport 'successor_ordinal'\)\)" 'C1 mapping upper supportがC1.1 provenanceではありません'
Require $runner 'sealed_input_sha256' 'sealed input SHA-256 objectがありません'
foreach($hashField in @('traced_app','present_history_raw','upstream_inventory_proof')){
    Require $runner ("{0}=\(\(Get-FileHash" -f $hashField) "$hashField SHA-256がありません"
}
Require $runner 'performance_accounting_connected=\$false;intent_satisfaction_connected=\$false' 'C1がaccounting/satisfactionへ接続されています'
if($core-match'nearest|tolerance' -and
   $core-notmatch'mapping_uses_nearest_qpc=\$false[\s\S]+mapping_uses_arbitrary_tolerance=\$false'){
    throw 'nearest/tolerance heuristicがmapping coreへ混入しています'
}
Write-Output 'W2-C1 mapping architecture: PASS'
