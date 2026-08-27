[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Read-Script([string]$Relative){
    $path=Join-Path $SourceRoot $Relative
    if(-not(Test-Path -LiteralPath $path)){throw "W3対象scriptがありません: $path"}
    return Get-Content -LiteralPath $path -Raw -Encoding utf8
}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Deny([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$core=Read-Script 'scripts/p2-d5-2-w3-canonical-performance-core.ps1'
$shared=Read-Script 'scripts/p2-d5-2-w3-shared-replay.ps1'
$runner=Read-Script 'scripts/build-p2-d5-2-w3-canonical-performance.ps1'
$checker=Read-Script 'scripts/check-p2-d5-2-w3-canonical-performance.ps1'
$acquire=Read-Script 'scripts/acquire-p2-d5-2-w3-fresh.ps1'

# --- 段の分離 ---
foreach($stage in @('STAGE 1: acquisition / protocol validity',
                    'STAGE 2: formal-v2 canonical authority validity',
                    'STAGE 3: accounting validity',
                    'STAGE 4: canonical performance metric construction',
                    'STAGE 5: frozen threshold evaluation',
                    'STAGE 6: canonical verdict')){
    Require $core ([regex]::Escape($stage)) "評価段が分離されていません: $stage"
}
# 1〜3がINVALIDなら4〜6へ進まず、performance FAILへ変換しない。
Require $core "verdict='AUTHORITY_OR_PROTOCOL_INVALID'" 'authority/protocol INVALIDの三値目がありません'
Require $core '\$authorityValid=\$stage1Valid-and\$stage2Valid-and\$stage3Valid' `
    '1〜3の成立をまとめて後段のgateにしていません'
Require $core '(?s)if\(-not\$authorityValid\)\{[\s\S]{0,1200}stage4_metric_constructed=\$false' `
    'INVALID時にstage4以降を評価しない経路がありません'
Require $core '(?s)if\(-not\$authorityValid\)\{[\s\S]{0,1600}canonical_performance_pass=\$null' `
    'INVALID時にperformance値をnullにしていません'
Require $checker 'authority / protocolがINVALIDなのに後段を評価しています' `
    'checkerが段の順序違反をrejectしていません'
Require $checker 'authority / protocol INVALIDをperformance verdictへ変換しています' `
    'checkerがINVALID->FAIL変換をrejectしていません'

# --- canonical chain だけから metric を作ること ---
Require $core 'legacy_presentation_authority_used=\$false' 'legacy authority不使用が固定されていません'
Require $checker 'canonical verdictがlegacy presentation authorityを使っています' `
    'checkerがlegacy authority混入をrejectしていません'
# fps の分母は W2-A physical window。legacy の measurement_elapsed_seconds は使わない。
Require $shared 'measurement_start_qpc' 'fps分母をW2-A physical windowから取っていません'
Require $shared 'measurement_end_qpc_exclusive' 'fps分母をW2-A physical windowから取っていません'
Deny $core 'measurement_elapsed_seconds' 'coreがlegacyのelapsed秒数を使っています'
Deny $shared 'measurement_elapsed_seconds' 'shared replayがlegacyのelapsed秒数を使っています'
Deny $shared '\$app\..*effective_fps' 'shared replayがlegacy effective_fpsを読んでいます'
Deny $shared '\$app\..*drop_rate' 'shared replayがlegacy drop_rateを読んでいます'

# --- Layer 1A / Layer 1B の count 差を verdict にしないこと ---
Require $core 'layer1a_layer1b_count_difference_is_not_a_verdict=\$true' `
    'Layer 1A / Layer 1B差の非verdict化が固定されていません'
Deny $core 'canonical_required_intent_count-ne.*physical_vblank_opportunity_count' `
    'Layer 1A / Layer 1Bのcount差をblockerにしています'
Require $checker 'Layer 1A / Layer 1Bのcount差をverdictへ接続しています' `
    'checkerがLayer 1A/1B差のverdict化をrejectしていません'

# --- frozen threshold を変更しないこと ---
Require $core 'MvmW3FrozenMinimumFps = 55\.0' 'fps thresholdがfrozen値ではありません'
Require $core 'MvmW3FrozenMaximumDropRate = 0\.02' 'drop thresholdがfrozen値ではありません'
Require $core 'thresholds_frozen_unchanged=\$true' 'threshold不変が宣言されていません'
Require $checker 'thresholdがfrozen値から変更されています' 'checkerがthreshold変更をrejectしていません'

# --- upstream authority の再実行と provenance ---
Require $shared '-File \$W2EChecker' 'W3がW2-E canonical authority checkerを再実行していません'
foreach($consumer in @(@($runner,'runner'),@($checker,'checker'))){
    Require $consumer[0] 'p2-d5-2-w3-shared-replay\.ps1' "$($consumer[1])が評価手順を共有していません"
    Require $consumer[0] 'Invoke-MvmW3CanonicalPerformanceFromCanonical' "$($consumer[1])が評価を呼んでいません"
    Require $consumer[0] 'check-p2-d5-2-w2-e-canonical-authority\.ps1' "$($consumer[1])がW2-E checkerをconsumeしていません"
}
Require $checker '別のupstream artifactを参照しています' 'checkerがupstream spliceをrejectしていません'

# --- acquisition provenance ---
Require $acquire 'checkpoint_sha' 'acquisitionがcheckpoint shaを記録していません'
Require $acquire 'worktree_clean' 'acquisitionがworktree状態を記録していません'
foreach($binary in @('compositor_spike','decoder','qt_gui','qt_quick')){
    Require $acquire ([regex]::Escape($binary)) "acquisitionが$binary のprovenanceを記録していません"
}
Require $acquire 'acquisition中にHEADが変化しました' 'acquisition中のHEAD変化をfail-closeしていません'
Require $acquire 'clean worktreeから取得してください' 'dirty worktreeでのacquisitionをfail-closeしていません'
Require $core 'ACQUISITION_CHECKPOINT_MISMATCH' 'checkpoint不一致のcaptureをfail-closeしていません'
Write-Output 'W3 canonical performance architecture: PASS'
