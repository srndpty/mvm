<#
.SYNOPSIS
    メモリ増加の切り分け matrix を 1 ケース 1 プロセスで実行する (S6)。

.DESCRIPTION
    ownership soak で Working Set の増加が観測されたが、
    Working Set だけでは「常駐した物理ページ」と「確保し続けている仮想メモリ」を
    区別できない。PrivateUsage を併記し、phase ごとに記録して切り分ける。

    各ケースは必ず別プロセスで実行する。MLT の global cache を
    前のケースから持ち越さないため。

.EXAMPLE
    pwsh scripts/memory-matrix.ps1
    pwsh scripts/memory-matrix.ps1 -Cases A,B,C
#>
[CmdletBinding()]
param(
    [string]$Preset = 'ucrt64-release',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string[]]$Cases = @('A', 'B', 'C', 'D', 'E', 'F', 'G'),
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Bench    = Join-Path $RepoRoot "build\$Preset\bin\mvm_bench.exe"
$Scenario = Join-Path $RepoRoot 'bench\scenarios\s5-five-track.json'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "build\$Preset\memory" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

if (-not (Test-Path $Bench)) { throw "mvm_bench がありません: $Bench" }
$env:PATH = "$Ucrt64\bin;$env:PATH"

$desc = @{
    'A' = 'runtime-only  init/shutdown x50'
    'B' = 'open-close    x200 (frame なし)'
    'C' = 'frame         open/frame137/close x200'
    'D' = 'audio         open/WAV/close x30 (毎回)'
    'E' = 'full          open/frame/audio/close x100 (audio 10 回に 1 回)'
    'F' = 'reuse-graph   1 回 open, frame x1000, close'
    'G' = 'runtime/iter  init/open/frame/close/shutdown x50'
}

$rows = @()
foreach ($c in $Cases) {
    Write-Host "`n=== case $c : $($desc[$c]) ===" -ForegroundColor Cyan
    $json = Join-Path $OutDir "$c.json"
    $csv  = Join-Path $OutDir "$c.csv"
    $log  = Join-Path $OutDir "$c.log"

    # 必ず別プロセス。前ケースの global cache を持ち越さない。
    $p = Start-Process -FilePath $Bench -NoNewWindow -Wait -PassThru `
        -ArgumentList @('memory-probe', $Scenario, '--case', $c, '--csv', $csv,
                        '--wav-dir', $OutDir) `
        -RedirectStandardOutput $json -RedirectStandardError $log
    $code = $p.ExitCode

    $row = [ordered]@{ Case = $c; Desc = $desc[$c]; Exit = $code }
    if ($code -eq 0 -and (Test-Path $json)) {
        try {
            $o = Get-Content $json -Raw | ConvertFrom-Json
            $row['Iter']      = $o.iterations
            $row['Sec']       = [math]::Round($o.elapsed_sec, 1)
            $row['WS_first']  = [math]::Round($o.working_set_mb.first, 1)
            $row['WS_last']   = [math]::Round($o.working_set_mb.last, 1)
            $row['Priv_first']= [math]::Round($o.private_usage_mb.first, 1)
            $row['Priv_last'] = [math]::Round($o.private_usage_mb.last, 1)
            $row['Priv/iter'] = $o.growth.private_usage.per_iter_bytes
            $row['PrivShape'] = $o.growth.private_usage.classification
            $row['WsShape']   = $o.growth.working_set.classification
            $row['JumpIter']  = $o.growth.private_usage.biggest_jump_iteration
            $row['JumpMB']    = [math]::Round($o.growth.private_usage.biggest_jump_mb, 1)
            $row['H_first']   = $o.handles.first
            $row['H_last']    = $o.handles.last
            if ($o.PSObject.Properties['after_runtime_shutdown_mb']) {
                $row['ShutPriv'] = [math]::Round($o.after_runtime_shutdown_mb.priv, 1)
            }
            if ($o.PSObject.Properties['after_runtime_shutdown_idle_mb']) {
                $row['IdlePriv'] = [math]::Round($o.after_runtime_shutdown_idle_mb.priv, 1)
            }
        } catch {
            $row['PrivShape'] = '(JSON 解析失敗)'
        }
    }
    $rows += [pscustomobject]$row
    Write-Host ("  exit={0} priv {1} -> {2} MB  shape={3}" -f $code,
        $(if ($row.Contains('Priv_first')) { $row['Priv_first'] } else { '?' }),
        $(if ($row.Contains('Priv_last')) { $row['Priv_last'] } else { '?' }),
        $(if ($row.Contains('PrivShape')) { $row['PrivShape'] } else { '?' }))
}

Write-Host "`n=== メモリ切り分け matrix ===" -ForegroundColor Cyan
$rows | Format-Table Case, Iter, Sec, WS_first, WS_last, Priv_first, Priv_last, 'Priv/iter',
                     PrivShape, JumpIter, JumpMB, H_first, H_last -AutoSize
$rows | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'matrix.json') -Encoding UTF8
Write-Host "詳細 (全反復の CSV を含む): $OutDir"
