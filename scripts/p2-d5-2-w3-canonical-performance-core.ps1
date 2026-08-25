Set-StrictMode -Version Latest

# P2-D5-2-W3 canonical performance evaluation core。
#
# W2-E は canonical performance verdict を明示的に W3 へ保留した。ここで初めて解禁する。
# ただし「いきなり fps / drop の PASS/FAIL を見る」ことはしない。段を分ける。
#
#   1 acquisition / protocol validity
#   2 formal-v2 canonical authority validity
#   3 accounting validity
#   4 canonical performance metric construction
#   5 frozen threshold evaluation
#   6 canonical verdict
#
# 1〜3 のいずれかが INVALID なら 4〜6 へ進まない。結果は authority / protocol INVALID で
# あり、performance FAIL へ変換しない (W1 §5.2 の三値)。
#
#   AUTHORITY_OR_PROTOCOL_INVALID / CANONICAL_PERFORMANCE_PASS / CANONICAL_PERFORMANCE_FAIL
#
# また freeze 済みのとおり、required_intent_count と physical_vblank_opportunity_count の
# 差を performance failure の根拠にしない。両者は異なる母集団である。

function Get-MvmW3Value($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){return $null}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){return $null}
    return $Object.$Name
}

function Get-MvmW3Required($Object,[string]$Name){
    if($Object-is[System.Collections.IDictionary]){
        if(-not$Object.Contains($Name)){throw "W3必須fieldがありません: $Name"}
        return $Object[$Name]
    }
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){throw "W3必須fieldがありません: $Name"}
    return $Object.$Name
}

# frozen threshold。W3 で新しい閾値を発明しない。値の出所は legacy canonical gate と
# 同一であり、W2-E で diagnostic へ降格した際にも数値自体は変更していない。
$script:MvmW3FrozenMinimumFps = 55.0
$script:MvmW3FrozenMaximumDropRate = 0.02

function Invoke-MvmW3CanonicalPerformance {
    [CmdletBinding()]
    param(
        # W3 fresh acquisition の provenance record。
        [Parameter(Mandatory=$true)]$AcquisitionProvenance,
        # W2-E canonical authority artifact (再構築検証済みのもの)。
        [Parameter(Mandatory=$true)]$CanonicalAuthority,
        # canonical measurement window。run ごとの [start, end) と QPC frequency。
        [Parameter(Mandatory=$true)][AllowEmptyCollection()][array]$MeasurementWindows,
        # このacquisitionが対応するべきcheckpoint (HEAD sha)。
        [Parameter(Mandatory=$true)][string]$ExpectedCheckpointSha,
        [double]$MinimumFps=$script:MvmW3FrozenMinimumFps,
        [double]$MaximumDropRate=$script:MvmW3FrozenMaximumDropRate
    )

    # ---- STAGE 1: acquisition / protocol validity ----
    $stage1=@{}
    $acquisitionSha=[string](Get-MvmW3Value $AcquisitionProvenance 'checkpoint_sha')
    if([string]::IsNullOrWhiteSpace($acquisitionSha)){
        $stage1['ACQUISITION_CHECKPOINT_MISSING']=$true
    }elseif($acquisitionSha-ne$ExpectedCheckpointSha){
        # cutover 後 HEAD 以外の binary で取った capture を canonical にしない。
        $stage1['ACQUISITION_CHECKPOINT_MISMATCH']=$true
    }
    if(-not[bool](Get-MvmW3Value $AcquisitionProvenance 'worktree_clean')){
        $stage1['ACQUISITION_WORKTREE_DIRTY']=$true
    }
    if(-not[bool](Get-MvmW3Value $AcquisitionProvenance 'fresh_acquisition')){
        $stage1['ACQUISITION_NOT_FRESH']=$true
    }
    foreach($binaryField in @('compositor_spike_sha256','decoder_sha256','qt_gui_sha256','qt_quick_sha256')){
        if([string]::IsNullOrWhiteSpace([string](Get-MvmW3Value $AcquisitionProvenance $binaryField))){
            $stage1['ACQUISITION_BINARY_PROVENANCE_MISSING']=$true
        }
    }
    if(-not[bool](Get-MvmW3Value $AcquisitionProvenance 'coverage_complete')){
        $stage1['ACQUISITION_DISPLAY_COVERAGE_INCOMPLETE']=$true
    }
    if(-not[bool](Get-MvmW3Value $AcquisitionProvenance 'intent_scope_exact')){
        $stage1['ACQUISITION_INTENT_SCOPE_NOT_EXACT']=$true
    }
    $runCount=[int64](Get-MvmW3Value $AcquisitionProvenance 'run_count')
    if($runCount-le0){$stage1['ACQUISITION_RUN_COUNT_INVALID']=$true}
    $stage1Blockers=@($stage1.Keys|Sort-Object)
    $stage1Valid=$stage1Blockers.Count-eq0

    # ---- STAGE 2: formal-v2 canonical authority validity ----
    $stage2=@{}
    if([string](Get-MvmW3Value $CanonicalAuthority 'presentation_authority_schema')-ne'FORMAL_V2'){
        $stage2['CANONICAL_AUTHORITY_NOT_FORMAL_V2']=$true
    }
    if(-not[bool](Get-MvmW3Value $CanonicalAuthority 'canonical_authority')){
        $stage2['CANONICAL_AUTHORITY_NOT_ENABLED']=$true
    }
    if(-not[bool](Get-MvmW3Value $CanonicalAuthority 'cutover_exact')){
        $stage2['CANONICAL_CUTOVER_NOT_EXACT']=$true
    }
    if([bool](Get-MvmW3Value $CanonicalAuthority 'frame_swapped_authority')-or
       [bool](Get-MvmW3Value $CanonicalAuthority 'dwm_frame_statistics_authority')){
        $stage2['LEGACY_AUTHORITY_STILL_CANONICAL']=$true
    }
    if([int64](Get-MvmW3Value $CanonicalAuthority 'legacy_metric_canonical_decision_count')-ne0){
        $stage2['LEGACY_METRIC_FEEDS_CANONICAL_VERDICT']=$true
    }
    if([bool](Get-MvmW3Value $CanonicalAuthority 'source_frame_identity_used')-or
       [bool](Get-MvmW3Value $CanonicalAuthority 'nearest_qpc_or_tolerance_used')){
        $stage2['CANONICAL_CHAIN_IDENTITY_INVALID']=$true
    }
    # canonical artifact が指す W2-D shadow が W3 と同じ cohort であること。
    if([string](Get-MvmW3Value $CanonicalAuthority 'source_w2d_verdict')-ne'FORMAL_V2_SHADOW_INTEGRATION_EXACT'){
        $stage2['FORMAL_V2_SHADOW_NOT_EXACT']=$true
    }
    $stage2Blockers=@($stage2.Keys|Sort-Object)
    $stage2Valid=$stage2Blockers.Count-eq0

    # ---- STAGE 3: accounting validity ----
    $stage3=@{}
    foreach($identity in @('layer1a_required_accounting_identity_exact',
                           'presented_accounting_identity_exact',
                           'filled_physical_opportunity_identity_exact',
                           'physical_vblank_domain_cardinality_exact',
                           'c2_ledger_agreement_exact')){
        if(-not[bool](Get-MvmW3Value $CanonicalAuthority $identity)){
            $stage3['CANONICAL_ACCOUNTING_IDENTITY_VIOLATION']=$true
        }
    }
    $required=[int64](Get-MvmW3Value $CanonicalAuthority 'canonical_required_intent_count')
    $satisfied=[int64](Get-MvmW3Value $CanonicalAuthority 'canonical_satisfied_intent_count')
    $unsatisfied=[int64](Get-MvmW3Value $CanonicalAuthority 'canonical_unsatisfied_intent_count')
    if($required-le0){$stage3['CANONICAL_REQUIRED_INTENT_COUNT_INVALID']=$true}
    if($satisfied-lt0-or$satisfied-gt$required){$stage3['CANONICAL_SATISFIED_INTENT_OUT_OF_DOMAIN']=$true}
    if($required-ne($satisfied+$unsatisfied)){$stage3['CANONICAL_ACCOUNTING_IDENTITY_VIOLATION']=$true}
    # measurement window は run ごとに閉じていること。fps を作る母数になる。
    $windowQpcTotal=0L;$qpcFrequency=0L
    foreach($window in $MeasurementWindows){
        $start=[int64](Get-MvmW3Value $window 'measurement_start_qpc')
        $end=[int64](Get-MvmW3Value $window 'measurement_end_qpc_exclusive')
        $frequency=[int64](Get-MvmW3Value $window 'qpc_frequency')
        if($start-le0-or$end-le$start-or$frequency-le0){
            $stage3['CANONICAL_MEASUREMENT_WINDOW_INVALID']=$true;continue
        }
        if($qpcFrequency-eq0){$qpcFrequency=$frequency}
        elseif($qpcFrequency-ne$frequency){$stage3['CANONICAL_QPC_FREQUENCY_INCONSISTENT']=$true}
        $windowQpcTotal+=($end-$start)
    }
    if($MeasurementWindows.Count-ne$runCount){$stage3['CANONICAL_MEASUREMENT_WINDOW_COUNT_MISMATCH']=$true}
    if($windowQpcTotal-le0-or$qpcFrequency-le0){$stage3['CANONICAL_MEASUREMENT_WINDOW_INVALID']=$true}
    $stage3Blockers=@($stage3.Keys|Sort-Object)
    $stage3Valid=$stage3Blockers.Count-eq0

    $authorityValid=$stage1Valid-and$stage2Valid-and$stage3Valid
    $result=[ordered]@{
        schema='mvm-p2-d5-2-w3-canonical-performance-1';stage='P2-D5-2-W3'
        presentation_authority_schema='FORMAL_V2'
        canonical_authority=$true
        evaluation_order='ACQUISITION -> AUTHORITY -> ACCOUNTING -> METRIC -> THRESHOLD -> VERDICT'
        expected_checkpoint_sha=$ExpectedCheckpointSha
        acquisition_checkpoint_sha=$acquisitionSha
        stage1_acquisition_protocol_valid=$stage1Valid
        stage1_blockers=$stage1Blockers
        stage2_canonical_authority_valid=$stage2Valid
        stage2_blockers=$stage2Blockers
        stage3_accounting_valid=$stage3Valid
        stage3_blockers=$stage3Blockers
        # Layer 1A と Layer 1B の count 差は performance failure の根拠にしない。
        layer1a_layer1b_count_difference_is_not_a_verdict=$true
        legacy_presentation_authority_used=$false
        frozen_minimum_fps=$MinimumFps
        frozen_maximum_drop_rate=$MaximumDropRate
        thresholds_frozen_unchanged=$true
    }

    if(-not$authorityValid){
        # 1〜3 が INVALID なら 4〜6 を評価しない。performance FAIL へ変換しない。
        $result.stage4_metric_constructed=$false
        $result.stage5_threshold_evaluated=$false
        $result.stage6_canonical_verdict_evaluated=$false
        $result.performance_evaluated=$false
        $result.canonical_required_intent_count=$null
        $result.canonical_satisfied_intent_count=$null
        $result.canonical_unsatisfied_intent_count=$null
        $result.canonical_true_drop_count=$null
        $result.canonical_drop_rate=$null
        $result.canonical_effective_fps=$null
        $result.canonical_measurement_seconds=$null
        $result.fps_threshold_met=$null
        $result.drop_rate_threshold_met=$null
        $result.canonical_performance_pass=$null
        $result.verdict='AUTHORITY_OR_PROTOCOL_INVALID'
        return $result
    }

    # ---- STAGE 4: canonical performance metric construction ----
    # canonical chain の値だけから作る。legacy ledger には触れない。
    $trueDrop=$unsatisfied
    $measurementSeconds=[double]$windowQpcTotal/[double]$qpcFrequency
    $dropRate=[double]$trueDrop/[double]$required
    $effectiveFps=[double]$satisfied/$measurementSeconds
    $result.stage4_metric_constructed=$true
    $result.canonical_required_intent_count=$required
    $result.canonical_satisfied_intent_count=$satisfied
    $result.canonical_unsatisfied_intent_count=$unsatisfied
    $result.canonical_true_drop_count=$trueDrop
    $result.canonical_drop_rate=$dropRate
    $result.canonical_effective_fps=$effectiveFps
    $result.canonical_measurement_seconds=$measurementSeconds

    # ---- STAGE 5: frozen threshold evaluation ----
    $fpsMet=$effectiveFps-ge$MinimumFps
    $dropMet=$dropRate-le$MaximumDropRate
    $result.stage5_threshold_evaluated=$true
    $result.fps_threshold_met=$fpsMet
    $result.drop_rate_threshold_met=$dropMet

    # ---- STAGE 6: canonical verdict ----
    $pass=$fpsMet-and$dropMet
    $result.stage6_canonical_verdict_evaluated=$true
    $result.performance_evaluated=$true
    $result.canonical_performance_pass=$pass
    $result.verdict=$(if($pass){'CANONICAL_PERFORMANCE_PASS'}else{'CANONICAL_PERFORMANCE_FAIL'})
    return $result
}
