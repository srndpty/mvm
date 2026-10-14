<#
.SYNOPSIS
    製品 GUI の smoke: 実際の mvm.exe を、P3-6C.2 の driver で操作し、書き出しの UX を確かめる。
    人の操作は要らない (mouse・keyboard は使わない)。

.DESCRIPTION
    scripts/math-group-product-p36c2-accept.ps1 (freeze・reference・session -Drive・verify) を、新しい出力先で
    順に呼ぶ。既定は短い場面だけ (A: 対象外の Project、B: 数式だけの Project。全部で約 4 分)。
    -Full を付けると、v4 全体 (6840 frame、約 3〜5 分) の場面 C も行う。全長は、最終の binary が変わった
    とき、または全長に関わる動き (書き出しの経路・数式 group) を変えたときにだけ使う。

    書き出しが成功するたびに、製品は出力の file を Explorer で表示する (Explorer の window が開く)。

    出力先: build/math-group-gui-smoke/<日時>/ (前の結果は上書きしない)。
    終了コード: 0 = 全部の verify が成立、1 = 不成立がある、2 = 準備 (freeze・reference) の失敗。
    P3-6C.2 の検収の記録と判定 (build/math-group-product-p36c2-20261014/) には触れない。
#>
param(
    [switch]$Full,
    [string]$OutputDirectory = 'build/math-group-gui-smoke'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$accept = Join-Path $PSScriptRoot 'math-group-product-p36c2-accept.ps1'
$relative = "$OutputDirectory/$(Get-Date -Format 'yyyyMMdd-HHmmss')"
$root = Join-Path $repo $relative
New-Item -ItemType Directory -Force $root | Out-Null
$pwshPath = (Get-Process -Id $PID).Path

function Invoke-Accept([string[]]$Arguments, [string]$Log) {
    & $pwshPath -NoProfile -File $accept @Arguments -OutputDirectory $relative *> (Join-Path $root $Log)
    $LASTEXITCODE
}

$rows = @()
foreach ($stage in @('freeze', 'reference')) {
    $code = Invoke-Accept @('-Stage', $stage) "$stage.log"
    Write-Host "$stage exit=$code"
    if ($code -ne 0) { Write-Host "準備に失敗しました: $(Join-Path $root "$stage.log")"; exit 2 }
}
$scenarios = @(@('short-fallback-unknown', 'smoke-a'), @('short-math-only-eligible', 'smoke-b'))
if ($Full) { $scenarios += , @('v4', 'smoke-c') }
foreach ($scenario in $scenarios) {
    $code = Invoke-Accept @('-Stage', 'session', '-Project', $scenario[0], '-Label', $scenario[1], '-Drive') "session-$($scenario[1]).log"
    $session = Get-ChildItem $root -Directory -Filter "*-session-$($scenario[0])-$($scenario[1])" | Select-Object -Last 1
    $verifyCode = if ($session) { Invoke-Accept @('-Stage', 'verify', '-Session', $session.Name) "verify-$($scenario[1]).log" } else { -1 }
    $verify = if ($session -and (Test-Path (Join-Path $session.FullName 'verify.json'))) { Get-Content -Raw (Join-Path $session.FullName 'verify.json') | ConvertFrom-Json } else { $null }
    $failed = if ($verify) { @($verify.verdicts.PSObject.Properties | Where-Object { -not $_.Value } | ForEach-Object { $_.Name }) } else { @('verify.json がありません') }
    $steps = if ($verify) { @($verify.drive_steps) } else { @() }
    $rows += [pscustomobject]@{ project = $scenario[0]; session = $(if ($session) { $session.Name } else { $null }); session_exit = $code
        verify_exit = $verifyCode; failed_verdicts = $failed; drive_steps_ok = @($steps | Where-Object { $_.ok }).Count; drive_steps = $steps.Count
    }
    Write-Host ("{0,-26} session={1} verify={2} driver {3}/{4} 不成立=[{5}]" -f $scenario[0], $code, $verifyCode,
        @($steps | Where-Object { $_.ok }).Count, $steps.Count, ($failed -join ','))
}
$failures = @($rows | Where-Object { $_.verify_exit -ne 0 }).Count
[ordered]@{ created = (Get-Date -Format 'o'); full = [bool]$Full; failures = $failures; scenarios = $rows
    note = '製品 GUI の smoke。人の操作なし。P3-6C.2 の検収の判定とは別の記録である。'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'smoke.json') -Encoding utf8
Write-Host "結果: $root"
if ($failures -gt 0) { exit 1 }
exit 0
