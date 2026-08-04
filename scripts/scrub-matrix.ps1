<#
.SYNOPSIS
    M6 (scrub) を 8 条件 x N 回で実測し、生 JSON から集計する (S6)。

.DESCRIPTION
    monotonic display 契約における M6 の実測。
    条件は「投入パターン」x「投入間隔」の直積。

    投入間隔 0 は「UI が出しうる最速」であり、実際のスクラブ操作ではない。
    人が触るスライダは 60Hz 前後が上限なので、16667us が基準条件である。
    0 は上限側の負荷条件として残す。

    集計値は必ずこのスクリプトが生の JSON から再計算する。
    手で転記した数値を文書へ書かない。中央値は run 数の中央 (奇数前提)。

.EXAMPLE
    pwsh scripts/scrub-matrix.ps1
    pwsh scripts/scrub-matrix.ps1 -Runs 3 -Requests 300
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [int]$Runs = 3,
    [int]$Requests = 300,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$Scenario = Join-Path $RepoRoot 'bench\scenarios\s5-five-track.json'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\scrub" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

# 8 条件 = pattern 2 種 x 投入間隔 4 種
$patterns  = @('linear', 'random')
$intervals = @(0, 8333, 16667, 33333)

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return $null }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$rows = @()
$allRuns = @()

foreach ($pat in $patterns) {
    foreach ($iv in $intervals) {
        $cond = "$pat-$iv"
        Write-Host "`n=== 条件 $cond (投入間隔 ${iv}us) ===" -ForegroundColor Cyan

        $runObjs = @()
        for ($r = 1; $r -le $Runs; $r++) {
            $json = Join-Path $OutDir "$cond-run$r.json"
            $log  = Join-Path $OutDir "$cond-run$r.log"

            # 条件ごと run ごとに別プロセス。MLT の状態を持ち越さない。
            $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
                -ArgumentList @('scrub-bench', $Scenario,
                                '--requests', $Requests,
                                '--pattern', $pat,
                                '--submit-interval-us', $iv,
                                '--seed', (20260804 + $r)) `
                -RedirectStandardOutput $json -RedirectStandardError $log

            if ($p.ExitCode -ne 0) {
                throw "scrub-bench が失敗しました (条件 $cond run $r, exit $($p.ExitCode))。$log を参照。"
            }
            $o = Get-Content $json -Raw | ConvertFrom-Json

            # 集計の自己整合を run 単位で検査する。
            # 文書に載る値と生データがずれないことを機械で担保する。
            $recalc = $o.displayed_total / $o.elapsed_sec
            if ([math]::Abs($o.display_updates_per_sec - $recalc) -gt 0.01) {
                throw "条件 $cond run ${r}: display_updates_per_sec が displayed_total/elapsed_sec と一致しません"
            }
            if ($o.submitted -ne ($o.superseded_pending + $o.decoded)) {
                throw "条件 $cond run ${r}: submitted != superseded_pending + decoded"
            }
            if ($o.decoded -ne ($o.display_latest + $o.display_lagging +
                                $o.reject_regression + $o.decode_failed)) {
                throw "条件 $cond run ${r}: decoded の内訳が合いません"
            }
            if ($o.invariant_errors.Count -ne 0) {
                throw "条件 $cond run ${r}: invariant_errors = $($o.invariant_errors -join '; ')"
            }
            if (-not $o.final_frame_matches -or -not $o.final_generation_matches) {
                throw "条件 $cond run ${r}: 最終要求が表示されていません"
            }

            $runObjs += $o
            $allRuns += [pscustomobject]@{ Condition = $cond; Run = $r; Data = $o }
            Write-Host ("  run{0}: displayed={1} ups={2:N1} p95={3:N1}ms" -f `
                $r, $o.displayed_total, $o.display_updates_per_sec, $o.displayed_latency_ms.p95)
        }

        $rows += [pscustomobject]([ordered]@{
            Condition   = $cond
            Pattern     = $pat
            IntervalUs  = $iv
            Runs        = $Runs
            Submitted   = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.submitted }))
            Superseded  = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.superseded_pending }))
            Decoded     = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.decoded }))
            DispLatest  = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.display_latest }))
            DispLagging = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.display_lagging }))
            Displayed   = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.displayed_total }))
            RejectRegr  = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.reject_regression }))
            DecodeFail  = [int](Get-Median ($runObjs | ForEach-Object { [double]$_.decode_failed }))
            UpdatesPerSec = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.display_updates_per_sec })), 2)
            LatP50Ms    = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.displayed_latency_ms.p50 })), 1)
            LatP95Ms    = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.displayed_latency_ms.p95 })), 1)
            LatMaxMs    = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.displayed_latency_ms.max })), 1)
            LagP50      = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.generation_lag.p50 })), 1)
            LagP95      = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.generation_lag.p95 })), 1)
            LagMax      = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.generation_lag.max })), 1)
            CatchupMs   = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.final_catchup_ms })), 1)
            ElapsedSec  = [math]::Round((Get-Median ($runObjs | ForEach-Object { [double]$_.elapsed_sec })), 2)
            MarkerBad   = ($runObjs | ForEach-Object { $_.marker_mismatch_displayed_only } | Measure-Object -Sum).Sum
            # M6 閾値: >= 15 updates/sec かつ request->display p95 <= 200ms
            M6          = ''
        })
    }
}

foreach ($row in $rows) {
    $ok = ($row.UpdatesPerSec -ge 15) -and ($row.LatP95Ms -le 200) -and ($row.MarkerBad -eq 0)
    $row.M6 = if ($ok) { '合格' } else { '不合格' }
}

Write-Host "`n=== M6 scrub matrix (中央値 / $Runs runs) ===" -ForegroundColor Cyan
$rows | Format-Table Condition, Submitted, Superseded, Decoded, DispLatest, DispLagging,
                     Displayed, RejectRegr, DecodeFail, UpdatesPerSec, LatP50Ms, LatP95Ms,
                     LatMaxMs, LagP95, CatchupMs, ElapsedSec, MarkerBad, M6 -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
$rows | Export-Csv -Path (Join-Path $OutDir 'matrix.csv') -NoTypeInformation -Encoding UTF8

$passed = @($rows | Where-Object { $_.M6 -eq '合格' }).Count
Write-Host "`nM6 合格条件: $passed / $($rows.Count)"
Write-Host "生 JSON: $OutDir"
