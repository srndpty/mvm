<#
.SYNOPSIS
    この repository に属する CMake/Ninja build の進行状況を読み取り専用で診断する。
.DESCRIPTION
    command timeout と build hang を区別するため、build directory を command line に
    含む process、CPU time、child compiler、.ninja_log の更新時刻だけを表示する。
    process の停止や Ninja metadata の変更は行わない。
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release','ucrt64-debug')][string]$Preset='ucrt64-release'
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$repo=(Resolve-Path (Split-Path -Parent $PSScriptRoot)).Path
$build=(Join-Path $repo "build\$Preset")
$escaped=[regex]::Escape($build.Replace('\','/'))
$commandLineAvailable=$true
try {
    $processes=@(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
        $command="$($_.CommandLine)".Replace('\','/')
        $command -match $escaped
    })
} catch {
    $commandLineAvailable=$false
    $processes=@()
}
Write-Host "repo build: $build" -ForegroundColor Cyan
if (-not $commandLineAvailable) {
    Write-Warning 'process command line を取得する権限がありません。repo scope を確認できないため停止操作は行わないでください。'
    Get-Process -Name cmake,ninja,g++,cc1plus -ErrorAction SilentlyContinue |
        Select-Object Id,ProcessName,CPU,StartTime | Format-Table -AutoSize
} elseif ($processes.Count -eq 0) { Write-Host '該当 process はありません' }
else {
    $processes | ForEach-Object {
        $process=Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        [pscustomobject]@{Pid=$_.ProcessId;ParentPid=$_.ParentProcessId;Name=$_.Name
            CpuSeconds=$(if ($process) {[math]::Round($process.CPU,3)} else {$null})
            CommandLine=$_.CommandLine}
    } | Format-Table -Wrap -AutoSize
}
$log=Join-Path $build '.ninja_log'
if (Test-Path -LiteralPath $log) {
    Get-Item -LiteralPath $log | Select-Object FullName,Length,LastWriteTime | Format-List
} else { Write-Host '.ninja_log はまだありません' }
