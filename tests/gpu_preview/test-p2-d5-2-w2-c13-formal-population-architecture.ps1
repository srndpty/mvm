[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$core=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/p2-d5-2-w2-c13-formal-population-core.ps1') -Raw -Encoding utf8
$runner=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/map-p2-d5-2-w2-c1-displayed-qpc.ps1') -Raw -Encoding utf8
$checker=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/check-p2-d5-2-w2-c13-formal-population.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require $core "formal_membership_authority='B2_TERMINAL_FINAL_STATE_PRESENTED_EXACT_EVENT_SET'" 'B2 Presented authorityが固定されていません'
Require $core 'Get-MvmC13EventKey \(\[int64\]\$terminalRecord\.etw_sequence\)' 'B2 exact event keyがありません'
Require $core 'Get-MvmC13EventKey \(\[int64\]\$candidate\.etw_sequence\)' 'C0 exact enrichment keyがありません'
foreach($field in @('upstream_exact','display_relation','measurement_membership','source_frame','qpc_heuristic')){
    Require $core ("formal_membership_uses_{0}=\`$false" -f $field) "$field がformal membershipから隔離されていません"
}
Require $runner '\$observedCandidates=@\(\$runInventory\.candidates\)' 'observed populationを保持していません'
Require $runner '\$presentedCandidates=@\(\$formalPopulation\.formal_candidates\)' 'B2 formal populationをmappingへ渡していません'
Require $runner 'observed_physical_mapping_diagnostic' 'observed physical diagnosticがありません'
Require $checker 'B2 formal / C1 mapping key setが一致しません' 'checkerがformal mapping key setを再検算していません'
Require $checker 'observed / diagnostic mapping key setが一致しません' 'checkerがobserved population保持を再検算していません'
Require $checker 'sealed C0 / observed key setが一致しません' 'checkerがsealed C0 observed setを再検証していません'
Require $checker 'sealed B2 / formal key setが一致しません' 'checkerがsealed B2 formal setを再検証していません'
if($runner-match 'Where-Object[^\r\n]*(upstream_exact|display_relation)'){throw 'runnerがheuristicでformal membershipを定義しています'}
Write-Output 'W2-C1.3 formal population architecture: PASS'
