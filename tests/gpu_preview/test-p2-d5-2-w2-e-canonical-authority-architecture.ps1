[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Read-Script([string]$Relative){
    $path=Join-Path $SourceRoot $Relative
    if(-not(Test-Path -LiteralPath $path)){throw "W2-E対象scriptがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}
function Remove-PowerShellComments([string]$Text){return [regex]::Replace($Text,'(?m)#.*$','')}

$runner=Read-Script 'scripts/build-p2-d5-2-w2-e-canonical-authority.ps1'
$checker=Read-Script 'scripts/check-p2-d5-2-w2-e-canonical-authority.ps1'
$shared=Read-Script 'scripts/p2-d5-2-w2-e-shared-replay.ps1'
$core=Read-Script 'scripts/p2-d5-2-w2-e-canonical-authority-core.ps1'
$inventory=Read-Script 'scripts/inventory-p2-d5-2-w2-e-legacy-authority.ps1'

# --- W2-E.1 canonical cutover contract ---
# W2-D artifactのbooleanを反転させるのではなく、W2-D checkerを再実行したうえで
# canonical statementを独立構築すること。
Require $shared '-File \$W2DChecker' 'W2-E がW2-D checkerを再実行していません'
Require $shared '-File \$RetirementInventory' 'W2-E がretirement inventoryを再実行していません'
Require $shared 'Invoke-MvmDFormalV2ProofFromSealedAuthorities' `
    'W2-E がsealed authorityからformal-v2 integrationを独立再構築していません'
Require $shared 'Invoke-MvmDUpstreamAuthorityReplay' 'W2-E がupstream checkerを再実行していません'
# runner / checker は同じ手順を共有する。2箇所に書かない。
foreach($consumer in @(@($runner,'runner'),@($checker,'checker'))){
    Require $consumer[0] 'p2-d5-2-w2-e-shared-replay\.ps1' "$($consumer[1])がcutover手順を共有していません"
    Require $consumer[0] 'Invoke-MvmECanonicalAuthorityFromW2D' "$($consumer[1])がcanonical構築を呼んでいません"
    Require $consumer[0] 'check-p2-d5-2-w2-d-formal-v2-shadow\.ps1' "$($consumer[1])がW2-D checkerをconsumeしていません"
    Require $consumer[0] 'inventory-p2-d5-2-w2-e-legacy-authority\.ps1' "$($consumer[1])がretirement inventoryをconsumeしていません"
}

# authority selector の宣言。
Require $core "presentation_authority_schema='FORMAL_V2'" 'canonical authority schemaがFORMAL_V2ではありません'
Require $core 'canonical_authority=\$true' 'canonical_authorityがtrueで固定されていません'
Require $core 'frame_swapped_authority=\$false' 'frameSwapped authorityがfalseで固定されていません'
Require $core 'dwm_frame_statistics_authority=\$false' 'DWM authorityがfalseで固定されていません'
Require $core 'legacy_presentation_authority_retired=\$true' 'legacy authority retirementが宣言されていません'
Require $core 'canonical_source=\$script:MvmECanonicalSource' 'canonical sourceがformal-v2 chainで固定されていません'
Require $core "intent -> composition_token -> native_present -> exact_present_event -> final_state -> displayed_qpc -> physical_vblank_ordinal" `
    'canonical chainの宣言がfrozen chainと一致しません'

# W2-E は performance verdict の段ではない。
foreach($flag in @('performance_threshold_evaluated=\$false','canonical_verdict_evaluated=\$false',
    'historical_verdicts_rewritten=\$false','source_frame_identity_used=\$false',
    'nearest_qpc_or_tolerance_used=\$false',"canonical_performance_verdict_deferred_to='W3'")){
    Require $core $flag "W2-Eのnon-goalが固定されていません: $flag"
}
Require $checker 'W2-Eで禁止されているflagがtrueです' 'checkerがcanonical/performance flag注入をrejectしていません'
Require $checker 'performance semanticsのfieldがあります' 'checkerがperformance fieldの注入をrejectしていません'
Require $checker '別のW2-D shadow proofを参照しています' 'checkerが別W2-D proofへのspliceをrejectしていません'

# --- W2-E.2 retirement completeness ---
foreach($blocker in @('FORMAL_V2_SHADOW_NOT_EXACT','HISTORICAL_VERDICT_REWRITTEN',
    'LEGACY_AUTHORITY_STILL_CANONICAL','LEGACY_METRIC_FEEDS_CANONICAL_VERDICT',
    'LEGACY_METRIC_FAILURE_SITE_UNCLASSIFIED','LEGACY_DIAGNOSTIC_SOURCE_MISSING',
    'CANONICAL_SOURCE_FRAME_IDENTITY','CANONICAL_NEAREST_QPC_FALLBACK')){
    Require $core $blocker "W2-E fail-closeがありません: $blocker"
}
# retirement = deletion ではない。
Require $core 'retirement_means_deletion=\$false' 'retirementをdeletionとして扱っています'
Require $inventory 'retirement_means_deletion=\$false' 'inventoryがretirementをdeletionとして扱っています'
Require $inventory 'legacy_diagnostic_sources' 'legacy diagnosticの残存をpositiveに記録していません'
# 走査対象を列挙で固定すると、未登録checkerにthresholdを足すだけでfalse-PASSできる。
Require $inventory "Get-ChildItem[^
]*'check-\*\.ps1'" 'inventoryが走査対象をdiscoveryしていません'
Deny $inventory '\$canonicalCheckers\s*=\s*@\(' 'inventoryが走査対象をハードコードしています'
Require $inventory 'LEGACY_METRIC_CHECKER_AUTHORITY_UNDECLARED' `
    'disposition未宣言のlegacy metric consumerをfail-closeしていません'
# failure siteの検出を同一行regexに頼ると、metric参照とFAILが別行に分かれた時点で
# すり抜ける。ASTとtaint伝播で判定式まで追っていることを固定する。
Require $inventory "failure_site_analysis='POWERSHELL_AST_WITH_LEGACY_METRIC_TAINT'" `
    'failure site検出方式が宣言されていません'
Require $inventory 'Parser\]::ParseFile' 'inventoryがASTを使っていません'
Require $inventory 'AssignmentStatementAst[\s\S]+while\(\$changed\)' `
    'legacy metricのtaintを固定点まで伝播させていません'
Require $inventory 'IfStatementAst[\s\S]+Clauses' 'emitterを囲む判定式を追っていません'

# --- canonical checker 側が cutover を宣言していること ---
foreach($relative in @('scripts/check-p2-contract.ps1','scripts/check-p3-c-contract.ps1',
                       'scripts/check-p4-formal-contract.ps1')){
    $canonicalChecker=Read-Script $relative
    Require $canonicalChecker "presentation_authority\s*=\s*'FORMAL_V2'" "$relative がFORMAL_V2 authorityを宣言していません"
    Require $canonicalChecker "legacy_presentation_metrics\s*=\s*'DIAGNOSTIC'" "$relative がlegacy metricをdiagnosticと宣言していません"
    Require $canonicalChecker "canonical_performance_verdict\s*=\s*'DEFERRED_TO_W3'" "$relative がperformance verdictをW3へ保留していません"
    # legacy fps / drop threshold が canonical FAIL へ到達していないこと。
    $code=Remove-PowerShellComments $canonicalChecker
    Deny $code '(?s)\$fps[^\n]*-lt\s*55[^\n]*\{[^}]*(Add-Failure|Fail )' "$relative にlegacy fps thresholdのFAILが残っています"
    Deny $code '(?s)\$drop[Rr]?[a-zA-Z]*[^\n]*-gt\s*0\.02[^\n]*\{[^}]*(Add-Failure|Fail )' "$relative にlegacy drop thresholdのFAILが残っています"
    # legacy 値自体は diagnostic として報告し続けること (retirement = deletion ではない)。
    Require $canonicalChecker 'W2-E\] legacy presentation metrics \(diagnostic, non-authoritative\)' `
        "$relative がlegacy metricをdiagnosticとして報告していません"
}
Write-Output 'W2-E canonical authority architecture: PASS'
