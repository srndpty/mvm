# scripts/lib/build-watchdog.ps1 の契約。
# 停止した tree だけを止めること、CPU・ファイル更新のある process は止めないことを確かめる。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$pwshPath = (Get-Process -Id $PID).Path
$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}
$watch = @{ StallSeconds = 4; PollMilliseconds = 250 }

# 1. 子を持ったまま何も進まない tree は、停止として tree ごと止める。
$elapsed = [Diagnostics.Stopwatch]::StartNew()
$stalled = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch -ArgumentList @(
    '-NoProfile', '-Command', "& '$pwshPath' -NoProfile -Command 'Start-Sleep -Seconds 120'")
$elapsed.Stop()
Check ($stalled.Stalled -and $null -eq $stalled.ExitCode) '進まない tree を停止と判定する'
Check ($elapsed.Elapsed.TotalSeconds -lt 60) "停止の判定が待ち時間より早い ($([int]$elapsed.Elapsed.TotalSeconds) 秒)"
$ids = @([regex]::Matches($stalled.Report, 'PID (\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
Check ($ids.Count -ge 2) "報告に起動した process と子が載る ($($ids.Count) 件)"
Start-Sleep -Milliseconds 500
$alive = @($ids | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
Check ($alive.Count -eq 0) "起動した tree の process が残らない (残り $($alive.Count) 件)"
Check ($stalled.Report -match 'BUILD_STALLED') '報告が BUILD_STALLED を示す'

# 2. 対照: CPU を使い続ける process は、停止の秒数を越えても止めない。
$busy = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch -ArgumentList @(
    '-NoProfile', '-Command',
    '$s = [Diagnostics.Stopwatch]::StartNew(); while ($s.Elapsed.TotalSeconds -lt 9) { $null = 1 + 1 }; exit 0')
Check (-not $busy.Stalled -and $busy.ExitCode -eq 0) 'CPU が進む process を止めない'

# 3. 対照: CPU を使わなくても進行ファイルが更新される間は止めない。
$progress = Join-Path $OutputDir 'progress.log'
Set-Content -LiteralPath $progress -Value '' -Encoding utf8NoBOM
$touching = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch -ProgressFile $progress -ArgumentList @(
    '-NoProfile', '-Command',
    "for (`$i = 0; `$i -lt 9; `$i++) { Add-Content -LiteralPath '$progress' -Value `$i; Start-Sleep -Seconds 1 }; exit 0")
Check (-not $touching.Stalled -and $touching.ExitCode -eq 0) '進行ファイルが更新される process を止めない'

# 4. 終了コードはそのまま返す。
$failed = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch -ArgumentList @('-NoProfile', '-Command', 'exit 7')
Check (-not $failed.Stalled -and $failed.ExitCode -eq 7) '終了コードをそのまま返す'

if ($failures -ne 0) { Write-Host "build watchdog: $failures 件失敗"; exit 1 }
Write-Host 'build watchdog: PASS'
exit 0
