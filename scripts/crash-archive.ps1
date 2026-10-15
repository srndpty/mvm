<#
.SYNOPSIS
    crash の証拠の archive (build/ の外) の使用量の報告 (既定)・既存の dump の保全・明示した dump の削除。

.DESCRIPTION
    規則は docs/crash-evidence.md。archive は既定で %LOCALAPPDATA%\mvm\crash-archive (MVM_CRASH_ARCHIVE で
    変更)。自動では何も消さない。消すのは -Remove に SHA-256 と理由を明示し、さらに -Apply を付けたとき
    だけで、削除は index.jsonl に記録される。-Apply が無ければ、消す対象を表示するだけ。

.EXAMPLE
    pwsh scripts/crash-archive.ps1            # 使用量の報告だけ
    pwsh scripts/crash-archive.ps1 -Save
    pwsh scripts/crash-archive.ps1 -Remove -Sha256 <64 桁> -Reason '解析済みで不要になった'
    pwsh scripts/crash-archive.ps1 -Remove -Sha256 <64 桁> -Reason '解析済みで不要になった' -Apply
#>
[CmdletBinding(DefaultParameterSetName = 'Report')]
param(
    [Parameter(ParameterSetName = 'Save', Mandatory)][switch]$Save,
    [Parameter(ParameterSetName = 'Remove', Mandatory)][switch]$Remove,
    [Parameter(ParameterSetName = 'Remove', Mandatory)][string[]]$Sha256,
    [Parameter(ParameterSetName = 'Remove', Mandatory)][string]$Reason,
    [Parameter(ParameterSetName = 'Remove')][switch]$Apply,
    [string]$Root
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\crash-archive.ps1')

$archive = Open-MvmCrashArchive -Root $Root
try {
    if ($Save) {
        $saved = Save-MvmCrashDumps -Archive $archive -Directory (Join-Path $env:LOCALAPPDATA 'CrashDumps') -Context '手動の保全'
        $saved | Format-Table name, status, size -AutoSize | Out-String -Width 200 | Write-Host
    } elseif ($Remove) {
        $targets = Remove-MvmCrashArchiveObject -Archive $archive -Sha256 $Sha256 -Reason $Reason -Apply:$Apply
        $targets | Format-Table sha256, size -AutoSize | Out-String -Width 200 | Write-Host
        if (-not $Apply) { Write-Host '-Apply が無いので消していません (上が対象)。' -ForegroundColor Yellow }
    }
    $u = Get-MvmCrashArchiveUsage $archive
    Write-Host ("archive: {0}`n  使用 {1:N0} byte / 上限 {2:N0} byte、dump {3} 件、run {4} 件`n  volume の空き {5:N0} byte (下限 {6:N0} byte)" -f
        $u.root, $u.bytes, $u.max_bytes, $u.objects, $u.runs, $u.free_bytes, $u.min_free_bytes)
} finally {
    Close-MvmCrashArchive $archive
}
