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
Assert-That ($d.decode_errors -eq 0) "decode error が $($d.decode_errors) 件"
Assert-That ($d.render_errors -eq 0) "render error が $($d.render_errors) 件"
Assert-That ($d.device_lost_count -eq 0) "device lost が $($d.device_lost_count) 回"
Assert-That ($d.device_mismatch_rejects -eq 0) "device mismatch で拒否したフレームがある"
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

# --- 集計の自己整合 ----------------------------------------------------------
# 「effective_fps」が displayed / elapsed と一致すること。
# 一致しないなら、どこかで別の数を fps と呼んでいる。
if ($d.measure_elapsed_ms -gt 0) {
    $expected = $d.displayed_frames / ($d.measure_elapsed_ms / 1000.0)
    $diff = [math]::Abs($expected - $d.effective_fps)
    Assert-That ($diff -lt 0.01) `
        "effective_fps が displayed/elapsed と一致しない (JSON=$($d.effective_fps) 再計算=$expected)"
}
# dropped は decode したのに表示されなかった数である。
Assert-That ($d.dropped_frames -ge 0) "dropped_frames が負"
Assert-That ($d.decoded_frames -ge $d.displayed_frames) `
    "displayed が decoded を超えている (decoded=$($d.decoded_frames) displayed=$($d.displayed_frames))"

if ($failures.Count -gt 0) {
    Write-Host "契約違反 $($failures.Count) 件 / 検査 $checks 件" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  FAIL $f" -ForegroundColor Red }
    exit 3
}

Write-Host "OK  契約 $checks 件すべて成立 ($Json)" -ForegroundColor Green
exit 0
