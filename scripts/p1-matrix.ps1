<#
.SYNOPSIS
    Phase 1 / P1 の正式な計測。**P1 の合否はこの出力だけで判定する。**

.DESCRIPTION
    docs/phase1-plan.md §11 のプロトコルで実行する。

      - 素材ごとに warm-up 5 秒 -> 測定 60 秒
      - 3 run。**run ごとに別プロセス**
      - seek は seed 固定で 1000 点
      - marker 代表点 0 / 1 / 137 / 299 / 600 / 1799 / 3599

    判定対象は 1080p60 H.264 と 1080p60 HEVC。
    4K60 H.264 は診断のみで、判定には使わない。

    **計測値を文書へ手で転記しない。** 集計はこのスクリプトが
    生 JSON から再計算し、自己整合も機械で検査する。

.PARAMETER Runs
    素材あたりの run 数。既定 3。

.PARAMETER Quick
    短縮版 (warm-up 2 秒 / 測定 10 秒 / seek 100 / 1 run)。
    **判定には使えない。** 経路が壊れていないかを見るだけ。

.EXAMPLE
    pwsh scripts/p1-matrix.ps1
    pwsh scripts/p1-matrix.ps1 -Quick
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release', 'ucrt64-debug')]
    [string]$Preset = 'ucrt64-release',
    [int]$Runs = 3,
    [switch]$Quick,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $RepoRoot "build\$Preset\bin\mvm_preview_spike.exe"
$BenchDir = Join-Path $RepoRoot 'tests\assets\benchmark'
$OutDir = Join-Path $RepoRoot "build\$Preset\p1-matrix"

if (-not (Test-Path $Exe)) {
    throw "preview_spike がありません: $Exe`n先に pwsh scripts/build.ps1 -Preset $Preset を実行してください。"
}
if (-not (Test-Path $BenchDir)) {
    throw "benchmark 素材がありません: $BenchDir`npwsh scripts/make-testmedia.ps1 -Mode Benchmark を実行してください。"
}

# **性能値は release で測る。** debug の数値を判定に使わない (AGENTS.md)。
if ($Preset -ne 'ucrt64-release') {
    Write-Host "警告: $Preset の数値は P1 の判定に使えません。" -ForegroundColor Yellow
}

$env:PATH = "$Ucrt64\bin;$env:PATH"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$warmup = 5000; $measure = 60000; $seeks = 1000
if ($Quick) { $warmup = 2000; $measure = 10000; $seeks = 100; $Runs = 1 }

# 判定対象と診断対象を **名前で** 分けておく。
# 「4K の数値で判定していた」を起こさないため。
$targets = @(
    @{ id = 'v1080p60_h264'; gate = $true;  note = '判定対象' }
    @{ id = 'v1080p60_hevc'; gate = $true;  note = '判定対象' }
    @{ id = 'v4k60_h264';    gate = $false; note = '診断のみ (判定に使わない)' }
)

$markerFrames = '0,1,137,299,600,1799,3599'

Write-Host "=== P1 matrix ($Preset) ===" -ForegroundColor Cyan
Write-Host "warm-up=${warmup}ms  measure=${measure}ms  seeks=$seeks  runs=$Runs"
if ($Quick) { Write-Host "**Quick モードの数値は P1 の判定に使えません**" -ForegroundColor Yellow }

$rows = New-Object System.Collections.Generic.List[object]

foreach ($t in $targets) {
    $media = Join-Path $BenchDir "$($t.id).mp4"
    if (-not (Test-Path $media)) {
        Write-Host "素材がありません、skip: $media" -ForegroundColor Yellow
        continue
    }
    for ($r = 1; $r -le $Runs; $r++) {
        $json = Join-Path $OutDir "$($t.id)-run$r.json"
        Write-Host "`n--- $($t.id) run $r / $Runs ($($t.note)) ---" -ForegroundColor Yellow

        # run ごとに別プロセス。プロセス内の状態を持ち越さない。
        & $Exe --measure --media $media --json $json --label "$($t.id)-run$r" `
            --warmup-ms $warmup --measure-ms $measure --seeks $seeks `
            --seed 20260806 --marker-frames $markerFrames
        $code = $LASTEXITCODE

        if (-not (Test-Path $json)) {
            throw "$($t.id) run $r が JSON を出しませんでした (exit $code)"
        }
        $d = Get-Content -Raw -Encoding UTF8 $json | ConvertFrom-Json

        # 契約検査は共通スクリプトに一本化する。ここで書き直さない。
        & (Join-Path $PSScriptRoot 'check-p1-contract.ps1') -Json $json
        $contractOk = ($LASTEXITCODE -eq 0)

        $rows.Add([pscustomobject]@{
            id            = $t.id
            gate          = $t.gate
            run           = $r
            exit          = $code
            contractOk    = $contractOk
            codec         = $d.codec
            sameAdapter   = [bool]$d.same_adapter
            sameDevice    = [bool]$d.same_device
            readback      = [long]$d.cpu_full_frame_readback_count
            bandReadback  = [long]$d.marker_band_readback_count
            gpuCopy       = [long]$d.gpu_copy_count
            decoded       = [long]$d.decoded_frames
            displayed     = [long]$d.displayed_frames
            dropped       = [long]$d.dropped_frames
            repeated      = [long]$d.repeated_presents
            elapsedMs     = [double]$d.measure_elapsed_ms
            fps           = [double]$d.effective_fps
            dropRate      = [double]$d.drop_rate
            presentHz     = [double]$d.present_rate_hz
            intervalP95   = [double]$d.frame_interval_p95_ms
            intervalMax   = [double]$d.frame_interval_max_ms
            startupMs     = [double]$d.startup_latency_ms
            seekP50       = [double]$d.seek_p50_ms
            seekP95       = [double]$d.seek_p95_ms
            seekMax       = [double]$d.seek_max_ms
            seekFail      = [long]$d.seek_failures
            markerChecked = [long]$d.marker_checked
            markerMismatch= [long]$d.marker_mismatch
            deviceLost    = [long]$d.device_lost_count
            cpuUtil       = [double]$d.cpu_utilization
            workingSet    = [long]$d.working_set_bytes
            privateUsage  = [long]$d.private_usage_bytes
            json          = $json
        })
    }
}

if ($rows.Count -eq 0) {
    Write-Error "計測対象が 0 件でした。素材を生成してください。"
    exit 1
}

# --- 集計 --------------------------------------------------------------------
# 中央値の max ではなく、**全 run を通した観測 max** で判定する (AGENTS.md)。
$summary = New-Object System.Collections.Generic.List[object]
foreach ($id in ($rows | Select-Object -ExpandProperty id -Unique)) {
    $g = @($rows | Where-Object { $_.id -eq $id })
    $fpsList = @($g | ForEach-Object { $_.fps })
    $summary.Add([pscustomobject]@{
        id             = $id
        gate           = $g[0].gate
        runs           = $g.Count
        fpsMin         = ($fpsList | Measure-Object -Minimum).Minimum
        fpsMedian      = ($fpsList | Sort-Object)[[int]([math]::Floor($g.Count / 2))]
        dropRateMax    = ($g | ForEach-Object { $_.dropRate } | Measure-Object -Maximum).Maximum
        seekP95Max     = ($g | ForEach-Object { $_.seekP95 } | Measure-Object -Maximum).Maximum
        seekObservedMax= ($g | ForEach-Object { $_.seekMax } | Measure-Object -Maximum).Maximum
        markerMismatch = ($g | ForEach-Object { $_.markerMismatch } | Measure-Object -Sum).Sum
        markerChecked  = ($g | ForEach-Object { $_.markerChecked } | Measure-Object -Sum).Sum
        readback       = ($g | ForEach-Object { $_.readback } | Measure-Object -Sum).Sum
        deviceLost     = ($g | ForEach-Object { $_.deviceLost } | Measure-Object -Sum).Sum
        seekFail       = ($g | ForEach-Object { $_.seekFail } | Measure-Object -Sum).Sum
        sameAdapter    = -not ($g | Where-Object { -not $_.sameAdapter })
        sameDevice     = -not ($g | Where-Object { -not $_.sameDevice })
        contractOk     = -not ($g | Where-Object { -not $_.contractOk })
        startupMedian  = ($g | ForEach-Object { $_.startupMs } | Sort-Object)[[int]([math]::Floor($g.Count / 2))]
    })
}

Write-Host "`n=== 集計 ===" -ForegroundColor Cyan
$summary | Format-Table id, gate, runs, @{n='fps(min)';e={'{0:N2}' -f $_.fpsMin}},
    @{n='drop(max)';e={'{0:N4}' -f $_.dropRateMax}},
    @{n='seek p95';e={'{0:N1}' -f $_.seekP95Max}},
    @{n='seek max';e={'{0:N1}' -f $_.seekObservedMax}},
    markerChecked, markerMismatch, readback, deviceLost, sameDevice, sameAdapter, contractOk -AutoSize

# --- exit criteria (docs/phase1-plan.md §12) ---------------------------------
# **不合格でも閾値をここで変えない。** 原因を記録して止める。
$verdict = New-Object System.Collections.Generic.List[string]
$gates = @($summary | Where-Object { $_.gate })

if ($gates.Count -lt 2) {
    $verdict.Add("判定対象が $($gates.Count) 件しかない (H.264 / HEVC の両方が必要)")
}
foreach ($s in $gates) {
    if (-not $s.contractOk)              { $verdict.Add("$($s.id): 契約検査に失敗") }
    if (-not $s.sameAdapter)             { $verdict.Add("$($s.id): Qt と FFmpeg が同一 adapter ではない") }
    if (-not $s.sameDevice)              { $verdict.Add("$($s.id): Qt と FFmpeg が同一 ID3D11Device ではない") }
    if ($s.readback -ne 0)               { $verdict.Add("$($s.id): CPU full-frame readback が $($s.readback) 回") }
    if ($s.markerChecked -le 0)          { $verdict.Add("$($s.id): marker を 1 件も検査していない") }
    if ($s.markerMismatch -ne 0)         { $verdict.Add("$($s.id): marker 不一致 $($s.markerMismatch) 件") }
    if ($s.fpsMin -lt 55)                { $verdict.Add("$($s.id): effective_fps 最小 $('{0:N2}' -f $s.fpsMin) < 55") }
    if ($s.dropRateMax -gt 0.02)         { $verdict.Add("$($s.id): drop_rate 最大 $('{0:N4}' -f $s.dropRateMax) > 0.02") }
    if ($s.seekP95Max -gt 150)           { $verdict.Add("$($s.id): seek p95 $('{0:N1}' -f $s.seekP95Max)ms > 150ms") }
    if ($s.seekObservedMax -gt 400)      { $verdict.Add("$($s.id): seek 観測 max $('{0:N1}' -f $s.seekObservedMax)ms > 400ms") }
    if ($s.seekFail -ne 0)               { $verdict.Add("$($s.id): seek 失敗 $($s.seekFail) 件") }
    if ($s.deviceLost -ne 0)             { $verdict.Add("$($s.id): device lost $($s.deviceLost) 回") }
}

$pass = ($verdict.Count -eq 0) -and -not $Quick
$summaryPath = Join-Path $OutDir 'summary.json'
[pscustomobject]@{
    schema     = 'mvm-p1-matrix-1'
    preset     = $Preset
    quick      = [bool]$Quick
    warmup_ms  = $warmup
    measure_ms = $measure
    seeks      = $seeks
    runs       = $Runs
    timestamp  = (Get-Date).ToString('o')
    runs_detail= $rows
    summary    = $summary
    violations = $verdict
    pass       = $pass
} | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $summaryPath

Write-Host "`n生 JSON と集計: $OutDir" -ForegroundColor Cyan

if ($Quick) {
    Write-Host "`nQuick モードのため合否判定は行いません。" -ForegroundColor Yellow
    exit 0
}
if ($verdict.Count -gt 0) {
    Write-Host "`nP1 不合格 ($($verdict.Count) 件)" -ForegroundColor Red
    foreach ($v in $verdict) { Write-Host "  - $v" -ForegroundColor Red }
    Write-Host "`n閾値は変更しない。原因を記録して停止すること。" -ForegroundColor Red
    exit 3
}

Write-Host "`nP1 合格 (exit criteria を全充足)" -ForegroundColor Green
exit 0
