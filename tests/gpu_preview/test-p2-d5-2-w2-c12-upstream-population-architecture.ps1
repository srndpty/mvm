[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$inventory=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/inventory-p2-d5-2-w2-c12-upstream-presented-population.ps1') -Raw -Encoding utf8
$checker=Get-Content -LiteralPath (Join-Path $SourceRoot 'scripts/check-p2-d5-2-w2-c12-upstream-presented-population.ps1') -Raw -Encoding utf8
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
Require $inventory 'foreach\(\$candidate in \$candidates\)' 'C0.1.1 candidate全件走査がありません'
Require $inventory 'Event-Key \$sequence \$displayed' 'ETW sequence / DisplayedQPC exact identityがありません'
Require $inventory "c011_candidate_population='ETW_OBSERVED_TARGET_PRESENTED'" 'C0.1.1 candidate populationが明記されていません'
Require $inventory "b2_formal_presented_population='LAYER2_EXACT_JOINED_PRESENTED'" 'B2 formal Presented populationが明記されていません'
Require $inventory "c1_input_population='C011_CANDIDATES_ALL'" 'C1 input populationが明記されていません'
Require $inventory 'candidate_filter_added=\$false' 'diagnosticでcandidate filterを追加しない契約がありません'
Require $inventory 'c1_verdict_changed=\$false;c1_remains_invalid=\$true' 'C1 INVALID維持契約がありません'
Require $inventory 'observer_start_qpc_available=\$false' 'observer start未取得を推測せず明記していません'
Require $inventory 'window_visibility_transition_qpc_available=\$false' 'visibility transition未取得を推測せず明記していません'
if($inventory-match '\$candidates\s*=\s*@\([^\r\n]*Where-Object'){throw 'C1.2がC0 candidateをfilterしています'}
foreach($forbidden in @('source_frame','nearest','tolerance')){
    if($inventory-match$forbidden){throw "禁止された推論入力があります: $forbidden"}
}
Require $checker 'foreach\(\$run in @\(Need \$proofObject ''runs''\)\)' 'checkerがrun recordsを再検算していません'
Require $checker '\$runInvalid-ne\(\$runBefore\+\$runWithin\+\$runAfter\)' 'invalid capture relation identityを再検算していません'
Write-Output 'W2-C1.2 upstream population architecture: PASS'
