# scripts/lib/build-watchdog.ps1 の契約。
# job に入った process (後から作られた子・孫・孤児を含む) だけを止めること、CPU・process の入れ替わり・
# ファイル更新のある build は止めないこと、process の終了・入れ替わりで進行の判定が狂わないことを確かめる。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$pwshPath = (Get-Process -Id $PID).Path
$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}
function Encode([string]$Script) { [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script)) }
# 子の pwsh を同期で起動する式。引用符の入れ子を避けるため EncodedCommand で渡す。
function ChildCall([string]$Script) { "& '$pwshPath' -NoProfile -EncodedCommand $(Encode $Script)" }
function Busy([double]$Seconds) {
    "`$s = [Diagnostics.Stopwatch]::StartNew(); while (`$s.Elapsed.TotalSeconds -lt $Seconds) { `$null = 1 + 1 }"
}
function Alive([int]$Id) { [bool](Get-Process -Id $Id -ErrorAction SilentlyContinue) }
$watch = @{ StallSeconds = 4; PollMilliseconds = 250 }
function Run([string]$Script, [hashtable]$Extra = @{}) {
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    $result = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch @Extra -ArgumentList @(
        '-NoProfile', '-EncodedCommand', (Encode $Script))
    $result | Add-Member -NotePropertyName Seconds -NotePropertyValue $elapsed.Elapsed.TotalSeconds
    return $result
}

# 1. 進行の判定 (決定的な検査)。job の CPU 時間は終了した process の分を含む累計なので減らない。
#    process の終了 (Active の減少) と入れ替わり (Total の増加) は CPU が増えなくても進行とする。
$base = [pscustomobject]@{ CpuSeconds = 10.0; TotalProcesses = 3; ActiveProcesses = 2; File = 'a' }
function Sample($Cpu, $Total, $Active, $File = 'a') {
    [pscustomobject]@{ CpuSeconds = $Cpu; TotalProcesses = $Total; ActiveProcesses = $Active; File = $File }
}
Check (-not (Test-MvmBuildProgress $base (Sample 10.4 3 2) 0.5)) '閾値未満の CPU 増加だけでは進行としない'
Check (Test-MvmBuildProgress $base (Sample 10.5 3 2) 0.5) '閾値以上の CPU 増加を進行とする'
Check (Test-MvmBuildProgress $base (Sample 10.0 3 1) 0.5) 'process の終了を進行とする'
Check (Test-MvmBuildProgress $base (Sample 10.0 4 2) 0.5) '同数での入れ替わり (終了と起動) を進行とする'
Check (Test-MvmBuildProgress $base (Sample 10.0 3 2 'b') 0.5) '進行ファイルの更新を進行とする'

# 2. 子を持ったまま何も進まない job は、停止として job ごと止める。
$stalled = Run (ChildCall 'Start-Sleep -Seconds 120')
Check ($stalled.Stalled -and $null -eq $stalled.ExitCode) '進まない job を停止と判定する'
Check ($stalled.Seconds -lt 30) "停止の判定が待ち時間より早い ($([int]$stalled.Seconds) 秒)"
Check (@($stalled.ProcessIds).Count -ge 2) "報告に起動した process と子が載る ($(@($stalled.ProcessIds).Count) 件)"
Check ($stalled.Terminated -and @($stalled.ProcessIds | Where-Object { Alive $_ }).Count -eq 0) 'job の process が残らない'
Check ($stalled.Report -match 'BUILD_STALLED') '報告が BUILD_STALLED を示す'

# 3. 親が先に終わった孫 (孤児) も job に残り、停止で一緒に止まる。親子関係を辿る方式では見落とす。
$orphanFile = Join-Path $OutputDir 'orphan.pid'
Remove-Item -LiteralPath $orphanFile -ErrorAction SilentlyContinue
$spawnOrphan = "(Start-Process -FilePath '$pwshPath' -NoNewWindow -PassThru -ArgumentList " +
    "'-NoProfile','-Command','Start-Sleep -Seconds 120').Id | Set-Content -LiteralPath '$orphanFile'"
$orphaned = Run ((ChildCall $spawnOrphan) + '; Start-Sleep -Seconds 120')
$orphanId = if (Test-Path -LiteralPath $orphanFile) { [int](Get-Content -LiteralPath $orphanFile) } else { 0 }
Check ($orphanId -gt 0 -and $orphaned.Stalled) '前提: 孤児の孫を作って停止した'
Check ($orphanId -in @($orphaned.ProcessIds)) '孤児の孫も job の process として報告する'
Check ($orphaned.Terminated -and -not (Alive $orphanId)) '孤児の孫も停止で止まる'

# 4. 正常に終わった build が残した process は、関数を抜けるときに止まる (残留させない)。
Remove-Item -LiteralPath $orphanFile -ErrorAction SilentlyContinue
$finished = Run ((ChildCall $spawnOrphan) + '; exit 0')
$leftId = if (Test-Path -LiteralPath $orphanFile) { [int](Get-Content -LiteralPath $orphanFile) } else { 0 }
Start-Sleep -Milliseconds 500
Check (-not $finished.Stalled -and $finished.ExitCode -eq 0 -and $leftId -gt 0) '前提: 子孫を残して正常終了した'
Check (-not (Alive $leftId)) '正常終了後に残った子孫を止める'

# 5. 対照: CPU を使い続ける process は、停止の秒数を越えても止めない。
$busy = Run ((Busy 9) + '; exit 0')
Check (-not $busy.Stalled -and $busy.ExitCode -eq 0) 'CPU が進む process を止めない'

# 6. 子の入れ替わり: 短い compiler が終わった後に、停止の秒数より長く動く linker が続く。
#    終了した compiler の CPU 時間が累計から消えても、後の linker を停止と誤らない。
$replaced = Run ((ChildCall (Busy 2)) + '; ' + (ChildCall (Busy 9)) + '; exit 0')
Check (-not $replaced.Stalled -and $replaced.ExitCode -eq 0) '入れ替わった後の長い linker を止めない'

# 7. 子の入れ替わり: compiler が終わった後に何も進まない子へ替わったら、停止の秒数で止める。
#    入れ替わり直後は進行とみなすが、その後の無変化を見逃さない。
$idleAfter = Run ((ChildCall (Busy 2)) + '; ' + (ChildCall 'Start-Sleep -Seconds 120'))
Check ($idleAfter.Stalled -and $idleAfter.Terminated) '入れ替わった後に進まない子を停止と判定する'
Check ($idleAfter.Seconds -lt 30) "入れ替わり後の停止の判定が待ち時間より早い ($([int]$idleAfter.Seconds) 秒)"

# 8. 対照: CPU を使わなくても進行ファイルが更新される間は止めない。
$progress = Join-Path $OutputDir 'progress.log'
Set-Content -LiteralPath $progress -Value '' -Encoding utf8NoBOM
$touching = Run ("for (`$i = 0; `$i -lt 9; `$i++) { Add-Content -LiteralPath '$progress' -Value `$i; " +
    'Start-Sleep -Seconds 1 }; exit 0') @{ ProgressFile = $progress }
Check (-not $touching.Stalled -and $touching.ExitCode -eq 0) '進行ファイルが更新される process を止めない'

# 9. 終了コードと、空白・引用符を含む引数はそのまま渡す。
$failed = Run 'exit 7'
Check (-not $failed.Stalled -and $failed.ExitCode -eq 7) '終了コードをそのまま返す'
$echoFile = Join-Path $OutputDir 'args.txt'
$echoScript = Join-Path $OutputDir 'echo-arg.ps1'
Set-Content -LiteralPath $echoScript -Encoding utf8NoBOM -Value (
    "Set-Content -LiteralPath '$echoFile' -Value `$args[0] -Encoding utf8NoBOM; exit 0")
$quoted = Invoke-MvmWatchedProcess -FilePath $pwshPath @watch -ArgumentList @(
    '-NoProfile', '-File', $echoScript, 'a b "c" d\')
Check ($quoted.ExitCode -eq 0 -and (Get-Content -LiteralPath $echoFile) -eq 'a b "c" d\') '空白・引用符・末尾の \ を含む引数を壊さない'

if ($failures -ne 0) { Write-Host "build watchdog: $failures 件失敗"; exit 1 }
Write-Host 'build watchdog: PASS'
exit 0
