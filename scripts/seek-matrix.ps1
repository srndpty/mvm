<#
.SYNOPSIS
    M5 (seek) を経路別 (native / proxy) に同一 workload で実測する (S7 / K)。

.DESCRIPTION
    native / GOP12 proxy / GOP1 proxy で **同じ seek 列**を使う。
    seed を固定するので、経路が違っても要求フレームの列は同一である。

    - random 1000 点、seed 20260804 固定
    - 3 回、run ごとに別プロセス (毎回 graph を作り直す)
    - warm-up: 各 run の最初の 1 回だけが cold。それを cold として分離する
    - 計時は frame image の取得完了まで

    M5: mismatch == 0 かつ p95 <= 150ms かつ max <= 400ms

    **観測された全 run の max を無視しない。** 中央値の max ではなく
    「3 回を通して観測した最大値」も併記し、そちらでも判定する。

    **件数を明示し、invariant を機械検査する。**
    matrix.json / matrix.csv に total / cold / warm / random / fixed を出す。
    total == cold + warm、total == random + fixed の両方を確かめ、
    破れたら fail-closed で停止する (件数の食い違いを黙って通さない)。

.EXAMPLE
    pwsh scripts/seek-matrix.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [int]$Runs = 3,
    [int]$Random = 1000,
    [int]$Seed = 20260804,
    [string[]]$Only,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$ScenDir  = Join-Path $RepoRoot 'bench\scenarios'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\seek" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

$paths = @(
    @{ Tag = '4k-original';  Scenario = 's7-4k-original.json' }
    @{ Tag = 'proxy-gop12';  Scenario = 's7-4k-proxy-gop12.json' }
    @{ Tag = 'proxy-gop1';   Scenario = 's7-4k-proxy-gop1.json' }
    # S7.1: preview で使う video source を全て proxy 化した正式評価用。
    @{ Tag = 'proxy-all-gop12'; Scenario = 's7-4k-proxy-all-gop12.json' }
)
if ($Only) { $paths = @($paths | Where-Object { $Only -contains $_.Tag }) }
if ($paths.Count -eq 0) { throw "対象の経路がありません (-Only の指定を確認)" }

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return 0.0 }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$rows = @()
foreach ($p in $paths) {
    $scen = Join-Path $ScenDir $p.Scenario
    if (-not (Test-Path $scen)) {
        Write-Host "scenario がありません、skip: $scen" -ForegroundColor Yellow
        continue
    }
    Write-Host "`n=== $($p.Tag) : random $Random, seed $Seed 固定 ===" -ForegroundColor Cyan

    $runObjs = @()
    for ($r = 1; $r -le $Runs; $r++) {
        $json = Join-Path $OutDir "$($p.Tag)-run$r.json"
        $log  = Join-Path $OutDir "$($p.Tag)-run$r.log"
        $csv  = Join-Path $OutDir "$($p.Tag)-run$r.csv"

        $proc = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
            -ArgumentList @('seek-bench', $scen, '--random', $Random, '--seed', $Seed,
                            '--csv', $csv) `
            -RedirectStandardOutput $json -RedirectStandardError $log

        if ($proc.ExitCode -ne 0) {
            throw "seek-bench が失敗しました ($($p.Tag) run $r, exit $($proc.ExitCode))。$log を参照。"
        }
        $o = Get-Content $json -Raw | ConvertFrom-Json
        $runObjs += $o
        Write-Host ("  run{0}: mismatch={1} p50={2:N1} p95={3:N1} max={4:N1}ms" -f `
            $r, $o.mismatch, $o.latency_ms.all.p50, $o.latency_ms.all.p95, $o.latency_ms.all.max)
    }

    function M([string]$Path) {
        Get-Median (@($runObjs | ForEach-Object { [double](Invoke-Expression "`$_.$Path") }))
    }

    # 観測された全 run の最大値。中央値で隠さない。
    $observedMax = ($runObjs | ForEach-Object { [double]$_.latency_ms.all.max } | Measure-Object -Maximum).Maximum
    $totalMismatch = ($runObjs | ForEach-Object { [long]$_.mismatch } | Measure-Object -Sum).Sum

    # --- 件数の集計と invariant 検査 (fail-closed) --------------------------
    # total == cold + warm と total == random + fixed を確かめる。
    # 固定点数は scenario 長で決まる決定論的な値であり、run 間で一致するはず。
    # 一致しなければ「同じ workload を測れていない」ので停止する。
    $totalSamples = 0L; $coldCount = 0L; $warmCount = 0L
    $fixedPerRun = $null
    foreach ($o in $runObjs) {
        $c    = [long]$o.count
        $cold = [long]$o.latency_ms.cold.count
        $warm = [long]$o.latency_ms.warm.count
        if ($c -ne ($cold + $warm)) {
            throw "件数 invariant 違反 ($($p.Tag)): count=$c != cold=$cold + warm=$warm"
        }
        $fpr = $c - [long]$Random
        if ($fpr -lt 0) {
            throw "件数 invariant 違反 ($($p.Tag)): count=$c が --random $Random を下回っている"
        }
        if ($null -eq $fixedPerRun) { $fixedPerRun = $fpr }
        elseif ($fixedPerRun -ne $fpr) {
            throw "件数 invariant 違反 ($($p.Tag)): run 間で固定点数が食い違う ($fixedPerRun vs $fpr)"
        }
        $totalSamples += $c; $coldCount += $cold; $warmCount += $warm
    }
    $randomCount = [long]$Random * $runObjs.Count
    $fixedCount  = $totalSamples - $randomCount
    if ($totalSamples -ne ($coldCount + $warmCount)) {
        throw "件数 invariant 違反 ($($p.Tag)): total=$totalSamples != cold=$coldCount + warm=$warmCount"
    }
    if ($totalSamples -ne ($randomCount + $fixedCount)) {
        throw "件数 invariant 違反 ($($p.Tag)): total=$totalSamples != random=$randomCount + fixed=$fixedCount"
    }

    $rows += [pscustomobject]([ordered]@{
        Tag          = $p.Tag
        Runs         = $Runs
        Requests     = $Random
        Seed         = $Seed
        Mismatch     = $totalMismatch
        TotalSamples = $totalSamples
        ColdCount    = $coldCount
        WarmCount    = $warmCount
        RandomCount  = $randomCount
        FixedCount   = $fixedCount
        P50Ms        = [math]::Round((M 'latency_ms.all.p50'), 1)
        P95Ms        = [math]::Round((M 'latency_ms.all.p95'), 1)
        MaxMedianMs  = [math]::Round((M 'latency_ms.all.max'), 1)
        MaxObservedMs = [math]::Round($observedMax, 1)
        MeanMs       = [math]::Round((M 'latency_ms.all.mean'), 1)
        StddevMs     = [math]::Round((M 'latency_ms.all.stddev'), 1)
        ColdP50Ms    = [math]::Round((M 'latency_ms.cold.p50'), 1)
        WarmP50Ms    = [math]::Round((M 'latency_ms.warm.p50'), 1)
        FwdP95Ms     = [math]::Round((M 'latency_ms.forward.p95'), 1)
        BackP95Ms    = [math]::Round((M 'latency_ms.backward.p95'), 1)
        M5           = ''
    })
}

foreach ($r in $rows) {
    # 観測された最大値で判定する。中央値の max では外れ値を隠すことになる。
    $ok = ($r.Mismatch -eq 0) -and ($r.P95Ms -le 150) -and ($r.MaxObservedMs -le 400)
    $r.M5 = if ($ok) { '合格' } else { '不合格' }
}

Write-Host "`n=== M5 seek matrix (中央値 / $Runs runs / seed $Seed 固定) ===" -ForegroundColor Cyan
Write-Host "MaxObservedMs は 3 回を通して観測した最大値。中央値で隠さない。"
Write-Host "件数の invariant (total==cold+warm, total==random+fixed) は集計時に検査済み。"
$rows | Format-Table Tag, TotalSamples, ColdCount, WarmCount, RandomCount, FixedCount -AutoSize
$rows | Format-Table Tag, Mismatch, P50Ms, P95Ms, MaxMedianMs, MaxObservedMs, MeanMs, StddevMs,
                     ColdP50Ms, WarmP50Ms, FwdP95Ms, BackP95Ms, M5 -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
$rows | Export-Csv -Path (Join-Path $OutDir 'matrix.csv') -NoTypeInformation -Encoding UTF8
Write-Host "生 JSON: $OutDir"
