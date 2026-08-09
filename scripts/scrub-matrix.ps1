<#
.SYNOPSIS
    M6 (scrub) の正式 matrix を実測し、生 JSON から集計する (S6 / S7)。

.DESCRIPTION
    正式 matrix は「realistic な入力」だけで構成する。

        pattern  : linear / random / fine / jump
        interval : 8333us (高頻度入力) / 16667us (標準入力)
        requests : 1000
        runs     : 3
        seed     : 全 run で固定 (既定 20260804)

    **同一条件の 3 回は同じ要求系列に対する実行揺らぎを測る。**
    run ごとに seed を変えない。変えると「実行揺らぎ」と
    「要求系列の違い」が混ざり、どちらを見ているのか分からなくなる。

    別 seed での頑健性は -SeedSweep で別途測り、
    正式 matrix とは別ファイル (seed-sweep.json) へ出す。

    interval=0 は burst stress であり、実際のスクラブ操作ではない。
    -Burst で別途測り、**M6 の合格条件数にも分母にも含めない。**

    8333us / 16667us を「人間のスライダの上限」とは言わない。
    実測していないので、単に高頻度入力 / 標準入力として扱う。

    native と proxy は同じ pattern / interval / seed / requests で測る。
    経路は -Scenario で切り替える。

.EXAMPLE
    pwsh scripts/scrub-matrix.ps1
    pwsh scripts/scrub-matrix.ps1 -Scenario bench/scenarios/s7-proxy-gop12.json -Tag gop12
    pwsh scripts/scrub-matrix.ps1 -Burst
    pwsh scripts/scrub-matrix.ps1 -SeedSweep
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$Scenario,
    [string]$Tag = 'native',
    [int]$Runs = 3,
    [int]$Requests = 1000,
    [int]$Seed = 20260804,
    [switch]$Burst,
    [switch]$SeedSweep,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'lib\metrics.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
if (-not $Scenario) { $Scenario = Join-Path $RepoRoot 'bench\scenarios\s7-five-track-60s.json' }
if (-not [System.IO.Path]::IsPathRooted($Scenario)) { $Scenario = Join-Path $RepoRoot $Scenario }
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\scrub\$Tag" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Bench))    { throw "mvm_bench がありません: $Bench" }
if (-not (Test-Path $Scenario)) { throw "scenario がありません: $Scenario" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

# 正式 matrix = 4 pattern x 2 interval = 8 条件
$Patterns  = @('linear', 'random', 'fine', 'jump')
$Intervals = @(8333, 16667)

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

function Invoke-ScrubRun {
    param([string]$Pattern, [int]$Interval, [int]$RunSeed, [string]$JsonPath, [string]$LogPath)

    $p = Start-Process -FilePath $script:Bench -NoNewWindow -Wait -PassThru `
        -ArgumentList @('scrub-bench', $script:Scenario,
                        '--requests', $script:Requests,
                        '--pattern', $Pattern,
                        '--submit-interval-us', $Interval,
                        '--seed', $RunSeed) `
        -RedirectStandardOutput $JsonPath -RedirectStandardError $LogPath

    if ($p.ExitCode -ne 0) {
        throw "scrub-bench が失敗しました (pattern=$Pattern interval=$Interval seed=$RunSeed, exit $($p.ExitCode))。$LogPath を参照。"
    }
    $o = Get-Content $JsonPath -Raw | ConvertFrom-Json

    # 集計の自己整合を run 単位で機械検査する。
    # 文書へ載る値と生データがずれないことをここで担保する。
    $recalc = $o.displayed_total / $o.elapsed_sec
    if ([math]::Abs($o.display_updates_per_sec - $recalc) -gt 0.01) {
        throw "$Pattern/$Interval seed=$RunSeed : display_updates_per_sec が displayed_total/elapsed_sec と一致しません"
    }
    if ($o.submitted -ne ($o.superseded_pending + $o.decoded)) {
        throw "$Pattern/$Interval seed=$RunSeed : submitted != superseded_pending + decoded"
    }
    if ($o.decoded -ne ($o.display_latest + $o.display_lagging + $o.reject_regression + $o.decode_failed)) {
        throw "$Pattern/$Interval seed=$RunSeed : decoded の内訳が合いません"
    }
    if ($o.invariant_errors.Count -ne 0) {
        throw "$Pattern/$Interval seed=$RunSeed : invariant_errors = $($o.invariant_errors -join '; ')"
    }
    if (-not $o.final_frame_matches -or -not $o.final_generation_matches) {
        throw "$Pattern/$Interval seed=$RunSeed : 最終要求が表示されていません"
    }
    return $o
}

function New-SummaryRow {
    param([string]$Condition, [string]$Pattern, [int]$Interval, [object[]]$RunObjs, [int]$UsedSeed)

    function M([string]$Path) {
        $vals = @($RunObjs | ForEach-Object { [double](Get-NestedPropertyValue $_ $Path) })
        Get-Median $vals
    }

    [pscustomobject]([ordered]@{
        Tag         = $script:Tag
        Condition   = $Condition
        Pattern     = $Pattern
        IntervalUs  = $Interval
        Seed        = $UsedSeed
        Runs        = $RunObjs.Count
        Requests    = $script:Requests
        Submitted   = [int](M 'submitted')
        Superseded  = [int](M 'superseded_pending')
        Decoded     = [int](M 'decoded')
        DispLatest  = [int](M 'display_latest')
        DispLagging = [int](M 'display_lagging')
        Displayed   = [int](M 'displayed_total')
        RejectRegr  = [int](M 'reject_regression')
        DecodeFail  = [int](M 'decode_failed')
        UpdatesPerSec = [math]::Round((M 'display_updates_per_sec'), 2)
        LatP50Ms    = [math]::Round((M 'displayed_latency_ms.p50'), 1)
        LatP95Ms    = [math]::Round((M 'displayed_latency_ms.p95'), 1)
        LatMaxMs    = [math]::Round((M 'displayed_latency_ms.max'), 1)
        LagP50      = [math]::Round((M 'generation_lag.p50'), 1)
        LagP95      = [math]::Round((M 'generation_lag.p95'), 1)
        LagMax      = [math]::Round((M 'generation_lag.max'), 1)
        FrameDistP50 = [math]::Round((M 'frame_distance.p50'), 1)
        FrameDistP95 = [math]::Round((M 'frame_distance.p95'), 1)
        FrameDistMax = [math]::Round((M 'frame_distance.max'), 1)
        SubmitAgeP50 = [math]::Round((M 'submit_age_ms.p50'), 1)
        SubmitAgeP95 = [math]::Round((M 'submit_age_ms.p95'), 1)
        SubmitAgeMax = [math]::Round((M 'submit_age_ms.max'), 1)
        CatchupMs   = [math]::Round((M 'final_catchup_ms'), 1)
        ElapsedSec  = [math]::Round((M 'elapsed_sec'), 2)
        # marker 不一致は中央値ではなく合計。1 回でも出たら見逃さない。
        MarkerBad   = ($RunObjs | ForEach-Object { $_.marker_mismatch_displayed_only } | Measure-Object -Sum).Sum
        M6          = ''
    })
}

function Set-M6Verdict {
    param([object[]]$Rows)
    foreach ($r in $Rows) {
        $ok = ($r.UpdatesPerSec -ge 15) -and ($r.LatP95Ms -le 200) -and ($r.MarkerBad -eq 0)
        $r.M6 = if ($ok) { '合格' } else { '不合格' }
    }
}

# ---------------------------------------------------------------------------
# burst stress (interval=0)。M6 の分母に入れない。
# ---------------------------------------------------------------------------
if ($Burst) {
    Write-Host "`n=== burst stress (interval=0) : M6 の判定には使わない ===" -ForegroundColor Yellow
    $rows = @()
    foreach ($pat in $Patterns) {
        $runObjs = @()
        for ($r = 1; $r -le $Runs; $r++) {
            $runObjs += Invoke-ScrubRun -Pattern $pat -Interval 0 -RunSeed $Seed `
                -JsonPath (Join-Path $OutDir "burst-$pat-run$r.json") `
                -LogPath  (Join-Path $OutDir "burst-$pat-run$r.log")
        }
        $rows += New-SummaryRow -Condition "$pat-burst" -Pattern $pat -Interval 0 -RunObjs $runObjs -UsedSeed $Seed
    }
    foreach ($r in $rows) { $r.M6 = '(対象外: burst stress)' }
    $rows | Format-Table Condition, Superseded, Decoded, Displayed, UpdatesPerSec, LatP95Ms, CatchupMs, MarkerBad -AutoSize
    $rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'burst.json') -Encoding UTF8
    Write-Host "burst stress は M6 の合格条件数・分母に含めない。"
    return
}

# ---------------------------------------------------------------------------
# seed sweep (頑健性の diagnostic)。正式 matrix とは別ファイル。
# ---------------------------------------------------------------------------
if ($SeedSweep) {
    Write-Host "`n=== seed sweep (diagnostic) : 正式 matrix ではない ===" -ForegroundColor Yellow
    $rows = @()
    foreach ($pat in $Patterns) {
        foreach ($iv in $Intervals) {
            $runObjs = @()
            for ($r = 0; $r -lt $Runs; $r++) {
                $s = $Seed + $r * 1000
                $runObjs += Invoke-ScrubRun -Pattern $pat -Interval $iv -RunSeed $s `
                    -JsonPath (Join-Path $OutDir "sweep-$pat-$iv-s$s.json") `
                    -LogPath  (Join-Path $OutDir "sweep-$pat-$iv-s$s.log")
            }
            $rows += New-SummaryRow -Condition "$pat-$iv" -Pattern $pat -Interval $iv -RunObjs $runObjs -UsedSeed -1
        }
    }
    Set-M6Verdict -Rows $rows
    $rows | Format-Table Condition, UpdatesPerSec, LatP95Ms, MarkerBad, M6 -AutoSize
    $rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'seed-sweep.json') -Encoding UTF8
    Write-Host "seed sweep は要求系列の違いを含むため、実行揺らぎの指標ではない。"
    return
}

# ---------------------------------------------------------------------------
# 正式 matrix
# ---------------------------------------------------------------------------
Write-Host "`n=== M6 正式 matrix ($Tag) ===" -ForegroundColor Cyan
Write-Host "scenario: $Scenario"
Write-Host "requests=$Requests runs=$Runs seed=$Seed (全 run 固定)"

$rows = @()
foreach ($pat in $Patterns) {
    foreach ($iv in $Intervals) {
        $cond = "$pat-$iv"
        Write-Host "`n--- $cond ---" -ForegroundColor Cyan
        $runObjs = @()
        for ($r = 1; $r -le $Runs; $r++) {
            $o = Invoke-ScrubRun -Pattern $pat -Interval $iv -RunSeed $Seed `
                -JsonPath (Join-Path $OutDir "$cond-run$r.json") `
                -LogPath  (Join-Path $OutDir "$cond-run$r.log")
            $runObjs += $o
            Write-Host ("  run{0}: displayed={1} ups={2:N1} p95={3:N1}ms" -f `
                $r, $o.displayed_total, $o.display_updates_per_sec, $o.displayed_latency_ms.p95)
        }
        $rows += New-SummaryRow -Condition $cond -Pattern $pat -Interval $iv -RunObjs $runObjs -UsedSeed $Seed
    }
}

Set-M6Verdict -Rows $rows

Write-Host "`n=== M6 正式 matrix ($Tag / 中央値 / $Runs runs / seed $Seed 固定) ===" -ForegroundColor Cyan
$rows | Format-Table Condition, Superseded, Decoded, DispLatest, DispLagging, Displayed,
                     UpdatesPerSec, LatP50Ms, LatP95Ms, LatMaxMs, LagP95,
                     FrameDistP95, SubmitAgeP95, CatchupMs, MarkerBad, M6 -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
$rows | Export-Csv -Path (Join-Path $OutDir 'matrix.csv') -NoTypeInformation -Encoding UTF8

$passed = @($rows | Where-Object { $_.M6 -eq '合格' }).Count
Write-Host "`nM6 合格: $passed / $($rows.Count) 条件 (正式 8 条件。burst stress は含まない)"
Write-Host "生 JSON: $OutDir"
