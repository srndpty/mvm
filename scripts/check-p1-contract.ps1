<#
.SYNOPSIS
    preview_spike が出した JSON の「契約」を検査する。

.DESCRIPTION
    終了コードだけでは足りない。preview_spike は
    device が別 adapter でも、marker を 1 件も検査しなくても、
    絵が 1 枚も出なくても exit 0 で終わりうる。
    「走った」ことと「検査した」ことは別である。

    ここで見るのは **短時間でも必ず成り立つ契約**だけであり、
    fps / seek の閾値判定は含まない。
    閾値判定は scripts/p1-matrix.ps1 が 60 秒 x 3 run の実測で行う。

.PARAMETER Json
    preview_spike --json が書いた JSON。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $Json)) {
    Write-Error "JSON がありません: $Json"
    exit 1
}

$d = Get-Content -Raw -Encoding UTF8 $Json | ConvertFrom-Json
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Assert-That([bool]$cond, [string]$what) {
    $script:checks++
    if (-not $cond) { $script:failures.Add($what) }
}

# --- 走った証拠 --------------------------------------------------------------
# 0 件で「全部通った」と報告しないための検査。
Assert-That ($d.displayed_frames -gt 0) "表示されたフレームが 0 件 (displayed_frames=$($d.displayed_frames))"
Assert-That ($d.decoded_frames -gt 0)   "decode されたフレームが 0 件"
Assert-That ($d.marker_checked -gt 0)   "marker を 1 件も検査していない"
Assert-That ($d.frame_interval_samples -gt 0) "frame interval の標本が 0 件"

# --- P1 の主要仮説 -----------------------------------------------------------
Assert-That ($d.rhi_backend -eq 'D3D11') "QRhi backend が D3D11 ではない: $($d.rhi_backend)"
Assert-That ($d.multithread_protected -eq $true) "ID3D10Multithread が有効になっていない"
Assert-That ($d.ffmpeg_adapter_known -eq $true) "FFmpeg 側の adapter を確認できていない"
Assert-That ($d.same_adapter -eq $true) "Qt と FFmpeg が同一 adapter ではない"
# same_device は same_adapter より強い条件。
# P1 は GPU copy への退避を実装していないので、device が別なら
# decode texture から SRV を作れず表示できない。よってここも必須である。
Assert-That ($d.same_device -eq $true) "Qt と FFmpeg が同一 ID3D11Device ではない"
Assert-That ($d.qt_d3d11_device -eq $d.ffmpeg_d3d11_device) `
    "device ポインタが一致しない (Qt=$($d.qt_d3d11_device) FFmpeg=$($d.ffmpeg_d3d11_device))"

# --- zero-copy ---------------------------------------------------------------
Assert-That ($d.cpu_full_frame_readback_count -eq 0) `
    "CPU full-frame readback が発生した ($($d.cpu_full_frame_readback_count) 回)"
Assert-That ($d.software_frame_rejects -eq 0) "software frame を受け取った"

# --- 正しさ ------------------------------------------------------------------
Assert-That ($d.marker_mismatch -eq 0) "marker 不一致が $($d.marker_mismatch) 件"
Assert-That ($d.seek_failures -eq 0) "seek 失敗が $($d.seek_failures) 件"
# **表示された frame が要求と違うのを 0 にする (P1.1 §5)。**
# decode-ready で一致していても、画面に出たのが別 frame なら意味が無い。
Assert-That ($d.seek_display_mismatch -eq 0) `
    "seek の表示 frame が要求と違うものが $($d.seek_display_mismatch) 件"
Assert-That ($d.schema -eq 'mvm-p1-preview-3') "JSON schema が想定と違う: $($d.schema)"
Assert-That ($d.decode_errors -eq 0) "decode error が $($d.decode_errors) 件"
Assert-That ($d.render_errors -eq 0) "render error が $($d.render_errors) 件"
Assert-That ($d.device_lost_count -eq 0) "device lost が $($d.device_lost_count) 回"
Assert-That ($d.device_rejected -eq 0) "device mismatch で拒否したフレームがある"
Assert-That ($d.exit_code -eq 0) "exit_code が 0 ではない: $($d.exit_code)"

# marker の明細も個別に見る。集計値だけを信じない。
foreach ($m in $d.markers) {
    Assert-That ([string]::IsNullOrEmpty($m.error)) "marker frame $($m.requested): $($m.error)"
    Assert-That ($m.sync_ok -eq $true) "marker frame $($m.requested): 同期が取れていない"
    Assert-That ($m.marker -eq $m.requested) `
        "marker frame $($m.requested): 読み取り値が $($m.marker)"
    Assert-That ($m.displayed -eq $m.requested) `
        "marker frame $($m.requested): 表示されたのは $($m.displayed)"
}

# --- P1.1 §1: GPU 完了に基づく retirement ------------------------------------
Assert-That ($d.gpu_completion_backend -in @('fence', 'event_query')) `
    "GPU 完了追跡の backend が不正: $($d.gpu_completion_backend)"
# **GPU が読み終わる前に frame を手放したら不合格。** ここが retainDepth の
# 置き換えの要点である。
Assert-That ($d.payloads_released_before_completion -eq 0) `
    "GPU 完了前に手放した payload が $($d.payloads_released_before_completion) 件"
# **追跡できない submission があってはならない (P1.2 §4)。**
# 1 件でもあれば「GPU 完了を待った」とは言えない。
Assert-That ($d.untracked_submission_count -eq 0) `
    "追跡できない submission が $($d.untracked_submission_count) 件"
Assert-That ($d.completion_poll_failure_count -eq 0) `
    "GPU 完了 poll の失敗が $($d.completion_poll_failure_count) 件"
Assert-That ($d.gpu_completion_fatal -eq $false) `
    "GPU 完了追跡が壊れた: $($d.gpu_completion_fatal_reason)"
Assert-That ($d.gpu_completion_device_removed_count -eq 0) `
    "device removed が $($d.gpu_completion_device_removed_count) 回"
Assert-That ($d.retirement_timeout_count -eq 0) `
    "retirement の drain が $($d.retirement_timeout_count) 回 timeout した"
# per-frame で GPU 完了を blocking wait していないこと。
Assert-That ($d.forced_gpu_wait_count -eq 0) `
    "GPU 完了の強制待ちが $($d.forced_gpu_wait_count) 回発生した"
Assert-That ($d.gpu_completed_serial -le $d.gpu_submitted_serial) `
    "completed serial が submitted を超えている"
Assert-That ($d.gpu_submitted_serial -gt 0) "GPU submission serial が 1 度も発行されていない"
Assert-That ($d.retirement_depth_peak -ge $d.retirement_depth_current) `
    "retirement depth の peak が current を下回っている"

# --- P1.1 §4: SRV cache が epoch を跨いで増え続けない ------------------------
Assert-That ($d.resource_epoch -gt 0) "resource_epoch が 0 のまま"
Assert-That ($d.srv_cache_entries_current -le $d.srv_cache_entries_peak) `
    "SRV cache の current が peak を超えている"
# **cache 内の (epoch, texture) group 数**であって decoder 数ではない (§6)。
# 1 本の decoder で計測している間、group は 1 つだけのはず。
Assert-That ($d.srv_cache_texture_groups -ge 1) "srv_cache_texture_groups が 0"
Assert-That ($d.srv_cache_texture_groups -le 2) `
    "srv_cache_texture_groups が $($d.srv_cache_texture_groups) 件ある (旧 epoch が retire されていない)"

# --- P1.2 §2: 3 つの世代がすべて出ていること --------------------------------
Assert-That ($null -ne $d.source_id) "source_id が無い"
Assert-That ($null -ne $d.source_generation) "source_generation が無い"
Assert-That ($null -ne $d.composition_epoch) "composition_epoch が無い"
# composition epoch は compositor (P1.2 では preview 層) が発行する。
# decoder の resource epoch と **同じ値になってはならない**わけではないが、
# 別のフィールドとして出ていることを要求する。
Assert-That ($d.composition_epoch -ge 1) "composition_epoch が発行されていない"

# --- P1.1 §8: device lifecycle ----------------------------------------------
# device が変わったのに握り潰していないこと。変化したなら
# 「処理した」か「fail-closed にした」かのどちらかが記録されていること。
#
# **P1.2 は復帰を実装していない。** handled は常に 0 で、
# 検出したものはすべて fail_closed になる。
Assert-That ($d.device_change_detected_count -eq
             ($d.device_change_handled_count + $d.device_change_fail_closed_count)) `
    "device change の検出件数と処理結果の件数が合わない"
Assert-That ($d.device_change_handled_count -eq 0) `
    "P1.2 は device 復帰を実装していないのに handled が $($d.device_change_handled_count) 件ある"
Assert-That ($d.device_recovery_support -eq 'none (P1.2 は fail-closed のみ)') `
    "device recovery の記述が実装と食い違っている: $($d.device_recovery_support)"

# --- P1.1 §7: frame accounting ----------------------------------------------
# queue に残っている frame を無条件に drop と呼ばない。
# drop は「表示期限を過ぎて意図的に捨てた」ものだけである。
Assert-That ($d.displayed_frames -le $d.submitted_frames) `
    "displayed が submitted を超えている (displayed=$($d.displayed_frames) submitted=$($d.submitted_frames))"
Assert-That ($d.pending_at_end -ge 0) "pending_at_end が負"
Assert-That ($d.dropped_frames -ge 0) "dropped_frames が負"
Assert-That (($d.displayed_frames + $d.dropped_frames + $d.pending_at_end) -le $d.decoded_frames) `
    "displayed + dropped + pending が decoded を超えている"
foreach ($k in 'stale_rejected', 'future_rejected', 'invalid_rejected', 'device_rejected',
               'generation_regression_rejected', 'decode_failed', 'render_failed',
               'retired_not_completed') {
    Assert-That ($d.$k -ge 0) "$k が負"
}
# **未来 generation と generation 逆行は起きてはならない。**
# 起きたら decode 側と表示側の世代管理が食い違っている。
Assert-That ($d.future_rejected -eq 0) "未来 generation の frame が $($d.future_rejected) 件届いた"
Assert-That ($d.generation_regression_rejected -eq 0) `
    "generation の逆行が $($d.generation_regression_rejected) 件あった"
Assert-That ($d.invalid_rejected -eq 0) "不正な frame が $($d.invalid_rejected) 件届いた"

# --- 集計の自己整合 ----------------------------------------------------------
# 「effective_fps」が displayed / elapsed と一致すること。
# 一致しないなら、どこかで別の数を fps と呼んでいる。
if ($d.measure_elapsed_ms -gt 0) {
    $expected = $d.displayed_frames / ($d.measure_elapsed_ms / 1000.0)
    $diff = [math]::Abs($expected - $d.effective_fps)
    Assert-That ($diff -lt 0.01) `
        "effective_fps が displayed/elapsed と一致しない (JSON=$($d.effective_fps) 再計算=$expected)"
}
Assert-That ($d.decoded_frames -ge $d.displayed_frames) `
    "displayed が decoded を超えている (decoded=$($d.decoded_frames) displayed=$($d.displayed_frames))"

if ($failures.Count -gt 0) {
    Write-Host "契約違反 $($failures.Count) 件 / 検査 $checks 件" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  FAIL $f" -ForegroundColor Red }
    exit 3
}

Write-Host "OK  契約 $checks 件すべて成立 ($Json)" -ForegroundColor Green
exit 0
