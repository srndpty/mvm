<#
.SYNOPSIS
    preview (M7 / M8) を real_time 構成ごとに実測し、生 JSON から集計する (S7)。

.DESCRIPTION
    compose_frame の連続呼び出しは preview ではない。
    mlt_consumer に tractor を接続し、consumer が連続して frame を要求する
    経路だけを測る (mvm_bench preview-bench)。

    MLT 7.36.1 のソースで確認した事実:
      real_time > 0 : 非同期・フレームドロップあり
      real_time < 0 : 非同期・フレームドロップなし
      real_time = 0 : 同期。mlt_frame_get_image を呼ばない (計測不能)
      render thread 数 = abs(real_time)

    real_time=0 は preview-bench 側で拒否する。ここでも構成に入れない。

    計測は wall 時間で打ち切る。素材の尺で打ち切ると遅い構成ほど長く走り、
    構成間で「同じ時間あたり何枚出たか」を比較できなくなる。

.EXAMPLE
    pwsh scripts/preview-matrix.ps1
    pwsh scripts/preview-matrix.ps1 -Scenario bench/scenarios/s7-4k-proxy-gop12.json -Tag gop12
    pwsh scripts/preview-matrix.ps1 -RealTimes -8 -Runs 3
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$Scenario,
    [string]$Tag = '1080p-native',
    [int[]]$RealTimes = @(1, -1, -4, -8, -16),
    [int]$Runs = 3,
    [int]$MeasureMs = 60000,
    [int]$WarmupMs = 5000,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'lib\metrics.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
if (-not $Scenario) { $Scenario = Join-Path $RepoRoot 'bench\scenarios\s7-five-track-60s.json' }
if (-not [System.IO.Path]::IsPathRooted($Scenario)) { $Scenario = Join-Path $RepoRoot $Scenario }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\preview\$Tag" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Bench))    { throw "mvm_bench がありません: $Bench" }
if (-not (Test-Path $Scenario)) { throw "scenario がありません: $Scenario" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$rows = @()
foreach ($rt in $RealTimes) {
    if ($rt -eq 0) {
        Write-Host "real_time=0 は mlt_frame_get_image を呼ばないため計測しません。" -ForegroundColor Yellow
        continue
    }
    Write-Host "`n=== $Tag / real_time=$rt (render threads = $([math]::Abs($rt)), drop=$(if($rt -gt 0){'あり'}else{'なし'})) ===" -ForegroundColor Cyan
    $runObjs = @()
    for ($r = 1; $r -le $Runs; $r++) {
        $json = Join-Path $OutDir "rt$rt-run$r.json"
        $log  = Join-Path $OutDir "rt$rt-run$r.log"

        # run ごとに別プロセス。MLT の状態を持ち越さない。
        $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
            -ArgumentList @('preview-bench', $Scenario,
                            '--consumer', 'null',
                            '--real-time', $rt,
                            '--measure-ms', $MeasureMs,
                            '--warmup-ms', $WarmupMs,
                            '--timeout-ms', 120000,
                            '--json', $json) `
            -RedirectStandardOutput (Join-Path $OutDir "rt$rt-run$r.stdout") `
            -RedirectStandardError $log

        if ($p.ExitCode -ne 0) {
            throw "preview-bench が失敗しました (real_time=$rt run $r, exit $($p.ExitCode))。$log を参照。"
        }
        $o = Get-Content $json -Raw | ConvertFrom-Json

        # 自己整合。集計値が生データと一致することを機械で担保する。
        if ($o.measured_wall_sec -le 0) { throw "real_time=$rt run ${r}: wall 時間が 0 です" }
        # effective_fps は「描画された frame / wall」。配信数ではない。
        $recalc = $o.rendered_frames / $o.measured_wall_sec
        if ([math]::Abs($o.effective_fps - $recalc) -gt 0.01) {
            throw "real_time=$rt run ${r}: effective_fps が rendered/wall と一致しません"
        }
        if ($o.effective_real_time -ne $rt) {
            throw "real_time=$rt run ${r}: consumer が real_time=$($o.effective_real_time) を持っています"
        }
        # real_time<0 はドロップしない契約なので rendered=0 があってはいけない。
        # real_time>0 では rendered=0 がドロップそのものなので異常ではない。
        if ($rt -lt 0 -and $o.not_rendered_frames -ne 0) {
            throw "real_time=$rt run ${r}: ドロップなしのはずが rendered=0 の frame が $($o.not_rendered_frames) 件"
        }
        if ($o.problems.Count -ne 0) {
            throw "real_time=$rt run ${r}: $($o.problems -join '; ')"
        }
        $runObjs += $o
        Write-Host ("  run{0}: fps(rendered)={1:N2} delivered_fps={2:N2} drop={3} rendered={4}/{5} wall={6:N1}s" -f `
            $r, $o.effective_fps, $o.delivered_fps, $o.dropped_frames, $o.rendered_frames, $o.delivered_frames, $o.measured_wall_sec)
    }

    function M([string]$Path) {
        Get-Median (@($runObjs | ForEach-Object { [double](Get-NestedPropertyValue $_ $Path) }))
    }

    $rows += [pscustomobject]([ordered]@{
        Tag           = $Tag
        RealTime      = $rt
        Threads       = [math]::Abs($rt)
        DropEnabled   = ($rt -gt 0)
        Runs          = $Runs
        WallSec       = [math]::Round((M 'measured_wall_sec'), 2)
        Delivered     = [int](M 'delivered_frames')
        Rendered      = [int](M 'rendered_frames')
        DeliveredFps  = [math]::Round((M 'delivered_fps'), 2)
        Unique        = [int](M 'unique_positions')
        Duplicates    = [int](M 'duplicate_positions')
        Skipped       = [int](M 'skipped_positions')
        Backward      = [int](M 'backward_positions')
        Dropped       = [int](M 'dropped_frames')
        EffectiveFps  = [math]::Round((M 'effective_fps'), 2)
        SteadyFps     = [math]::Round((M 'steady_state_fps'), 2)
        DropRate      = [math]::Round((M 'drop_rate'), 4)
        StartupMs     = [math]::Round((M 'startup_latency_ms'), 1)
        IntervalP50   = [math]::Round((M 'inter_frame_interval_ms.p50'), 2)
        IntervalP95   = [math]::Round((M 'inter_frame_interval_ms.p95'), 2)
        IntervalMax   = [math]::Round((M 'inter_frame_interval_ms.max'), 2)
        CpuSec        = [math]::Round((M 'cpu_process_sec'), 1)
        CpuUtil       = [math]::Round((M 'cpu_utilization'), 2)
        ThreadsPeak   = [int](M 'threads.peak')
        WsPeakMb      = [math]::Round((M 'working_set_mb.peak'), 1)
        PrivPeakMb    = [math]::Round((M 'private_usage_mb.peak'), 1)
        M7            = ''
    })
}

# M7: effective_fps >= 50 かつ drop_rate <= 0.05
foreach ($r in $rows) {
    $r.M7 = if (($r.EffectiveFps -ge 50) -and ($r.DropRate -le 0.05)) { '合格' } else { '不合格' }
}

Write-Host "`n=== preview matrix ($Tag / 中央値 / $Runs runs) ===" -ForegroundColor Cyan
Write-Host "全構成を残す。最も良い行だけを報告しない。"
$rows | Format-Table RealTime, Threads, DropEnabled, WallSec, Delivered, Rendered, Unique,
                     Duplicates, Skipped, Dropped, EffectiveFps, DeliveredFps, DropRate,
                     StartupMs, IntervalP95, CpuUtil, ThreadsPeak, PrivPeakMb, M7 -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
$rows | Export-Csv -Path (Join-Path $OutDir 'matrix.csv') -NoTypeInformation -Encoding UTF8

$passed = @($rows | Where-Object { $_.M7 -eq '合格' })
Write-Host "`nM7 (effective_fps >= 50 かつ drop_rate <= 0.05): $($passed.Count) / $($rows.Count) 構成"
if ($passed.Count -gt 0) {
    $best = $passed | Sort-Object -Property EffectiveFps -Descending | Select-Object -First 1
    Write-Host "正式候補: real_time=$($best.RealTime) (fps $($best.EffectiveFps), drop_rate $($best.DropRate))"
} else {
    $best = $rows | Sort-Object -Property EffectiveFps -Descending | Select-Object -First 1
    Write-Host "M7 を満たす構成はありません。最良でも real_time=$($best.RealTime) fps $($best.EffectiveFps)" -ForegroundColor Yellow
}
Write-Host "生 JSON: $OutDir"
