<#
.SYNOPSIS
    unique frame 数に対するメモリの増え方を測る診断 (S7 / M)。

.DESCRIPTION
    S6 のケース F は frame = iteration % 300 だった。
    1000 反復のうち実際にアクセスした固有フレームは 300 件しかない。
    したがって「増加が止まった」ことは
    「固有フレーム 300 件分を保持しきったら止まった」までしか意味しない。

    ここでは **同じフレームを取り直さない** ケース U を使い、
    unique frame 数に比例して増え続けるかどうかを見る。

    これは M16 の正式な 30 分試験ではなく診断である。
    「cache」「leak」と機構を断定しない。

    PrivateUsage が unique frame 数に比例して増え続ける場合は、
    S7 の重大所見として記録する。

.EXAMPLE
    pwsh scripts/memory-unique-frames.ps1
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$ScenDir  = Join-Path $RepoRoot 'bench\scenarios'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\memory-unique" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

# 経路ごとに 3600 unique frame を順に取る。
$cases = @(
    @{ Name = '1080p-native'; Scenario = 's7-five-track-60s.json';  Iterations = 3600 }
    @{ Name = '4k-original';  Scenario = 's7-4k-original.json';     Iterations = 3600 }
    @{ Name = '540p-proxy';   Scenario = 's7-4k-proxy-gop12.json';  Iterations = 3600 }
)

$rows = @()
foreach ($c in $cases) {
    $scen = Join-Path $ScenDir $c.Scenario
    if (-not (Test-Path $scen)) {
        Write-Host "scenario がありません、skip: $scen" -ForegroundColor Yellow
        continue
    }
    Write-Host "`n=== $($c.Name) : unique frame x $($c.Iterations) ===" -ForegroundColor Cyan
    $json = Join-Path $OutDir "$($c.Name).json"
    $csv  = Join-Path $OutDir "$($c.Name).csv"
    $log  = Join-Path $OutDir "$($c.Name).log"

    # 必ず別プロセス。前のケースの状態を持ち越さない。
    $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
        -ArgumentList @('memory-probe', $scen, '--case', 'U',
                        '--iterations', $c.Iterations, '--csv', $csv) `
        -RedirectStandardOutput $json -RedirectStandardError $log

    if ($p.ExitCode -ne 0) {
        Write-Host "失敗しました (exit $($p.ExitCode))。$log を参照。" -ForegroundColor Red
        Get-Content $log -TotalCount 10
        continue
    }
    $o = Get-Content $json -Raw | ConvertFrom-Json

    # 「unique frame を N 件触った」を推測ではなく実測で確認する。
    if ($o.unique_frames_accessed -lt $c.Iterations) {
        Write-Host ("注意: 要求 {0} 件に対し固有フレームは {1} 件しか触っていません (timeline 長 {2})" -f `
            $c.Iterations, $o.unique_frames_accessed, $o.timeline_length) -ForegroundColor Yellow
    }
    if ($o.marker_mismatch -ne 0) {
        throw "$($c.Name): 要求と違うフレームが $($o.marker_mismatch) 件返りました"
    }
    if ($o.invalid_samples -ne 0) {
        throw "$($c.Name): メモリ採取に失敗したサンプルが $($o.invalid_samples) 件あります"
    }

    $rows += [pscustomobject]([ordered]@{
        Case          = $c.Name
        UniqueFrames  = $o.unique_frames_accessed
        Iterations    = $o.iterations
        ElapsedSec    = [math]::Round($o.elapsed_sec, 1)
        PrivFirstMb   = [math]::Round($o.private_usage_mb.first, 1)
        PrivLastMb    = [math]::Round($o.private_usage_mb.last, 1)
        WsFirstMb     = [math]::Round($o.working_set_mb.first, 1)
        WsLastMb      = [math]::Round($o.working_set_mb.last, 1)
        PrivPerIter   = $o.growth.private_usage.per_iter_bytes
        PrivLastQtr   = $o.growth.private_usage.last_quarter_per_iter_bytes
        PrivShape     = $o.growth.private_usage.classification
        BiggestJumpIt = $o.growth.private_usage.biggest_jump_iteration
        BiggestJumpMb = [math]::Round($o.growth.private_usage.biggest_jump_mb, 1)
        AfterShutdownPrivMb = if ($o.PSObject.Properties['after_runtime_shutdown_mb']) {
            [math]::Round($o.after_runtime_shutdown_mb.priv, 1) } else { -1 }
        HandlesFirst  = $o.handles.first
        HandlesLast   = $o.handles.last
    })
    Write-Host ("  priv {0:N1} -> {1:N1} MB, 後半1/4 傾き {2} B/frame, 形 {3}" -f `
        $o.private_usage_mb.first, $o.private_usage_mb.last,
        $o.growth.private_usage.last_quarter_per_iter_bytes, $o.growth.private_usage.classification)
}

Write-Host "`n=== unique frame メモリ診断 ===" -ForegroundColor Cyan
Write-Host "M16 の正式試験ではない。機構 (cache / leak) を断定しない。"
$rows | Format-Table Case, UniqueFrames, ElapsedSec, PrivFirstMb, PrivLastMb, WsLastMb,
                     PrivPerIter, PrivLastQtr, PrivShape, BiggestJumpIt, BiggestJumpMb,
                     AfterShutdownPrivMb, HandlesFirst, HandlesLast -AutoSize

$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'unique-frames.json') -Encoding UTF8

# 後半 1/4 の傾きが正のままなら、unique frame 数に比例して増え続けている疑い。
foreach ($r in $rows) {
    if ($r.PrivLastQtr -gt 100000) {
        Write-Host ("[重大所見の候補] {0}: 後半 1/4 でも 1 frame あたり {1} B 増え続けています。" -f `
            $r.Case, $r.PrivLastQtr) -ForegroundColor Red
    }
}
Write-Host "生 JSON: $OutDir"
