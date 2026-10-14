<#
P3-7B.1: 試験用の「Explorer を開かない」動き (--test-reveal-without-explorer) が、普通の起動で有効に
ならないことを確かめる。

  1. 利用者の画面 (Default desktop) で --test-reveal-without-explorer を付けると、mvm.exe は window を
     作る前に終了コード 2 で起動を断り、理由を stderr に出す。
  2. mvm.exe は、以前の環境変数 (MVM_TEST_REVEAL_WITHOUT_EXPLORER) を参照しない (binary に名前が無い)。

見えない desktop の上で有効になることは、scripts/math-group-gui-smoke.ps1 の run が確かめる
(session.json の reveal_simulated)。この試験は、利用者の画面 (Default desktop) の上で動く CTest を前提にする。
#>
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$WorkDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$failures = 0
function Check([bool]$Ok, [string]$Message) {
    if (-not $Ok) { $script:failures++; Write-Host "FAIL: $Message" } else { Write-Host "ok: $Message" }
}

if (-not ('Mvm.Test.DesktopName' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace Mvm.Test {
public static class DesktopName {
    [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint threadId);
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder information, int length, out int needed);
    public static string Current() {
        var b = new StringBuilder(256); int needed;
        return GetUserObjectInformation(GetThreadDesktop(GetCurrentThreadId()), 2, b, b.Capacity * 2, out needed) ? b.ToString() : "";
    }
}
}
'@
}
$desktop = [Mvm.Test.DesktopName]::Current()
if ($desktop -ne 'Default') {
    Write-Host "FAIL: この試験は利用者の画面 (Default desktop) で動かす前提です (この desktop: '$desktop')"
    exit 1
}

New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$project = Join-Path $WorkDirectory 'gate.mvm'
$stderr = Join-Path $WorkDirectory 'stderr.txt'
$stdout = Join-Path $WorkDirectory 'stdout.txt'
# --manim-executable は存在する絶対 path なら何でもよい (起動を断る前に使わない)。
$process = Start-Process -FilePath $Executable -ArgumentList @('--project', $project, '--manim-executable', (Resolve-Path $Executable).Path,
    '--test-reveal-without-explorer') -PassThru -NoNewWindow -RedirectStandardError $stderr -RedirectStandardOutput $stdout
if (-not $process.WaitForExit(30000)) {
    $process.Kill($true)
    Check $false 'Default desktop で --test-reveal-without-explorer を付けた mvm.exe が 30 秒で終わらない (起動してしまった)'
} else {
    $message = [IO.File]::ReadAllText($stderr, [Text.UTF8Encoding]::new($false))
    Check ($process.ExitCode -eq 2) "Default desktop では起動を断る (終了コード $($process.ExitCode)、期待 2)"
    Check ($message.Contains('--test-reveal-without-explorer は試験の見えない desktop でだけ使えます') -and $message.Contains("'Default'")) (
        "理由を stderr に出す: $($message.Trim())")
    Check (-not $message.Contains('試験: Explorerを開かずに')) 'Explorer を開かない動きを有効にしていない'
    Check (-not (Test-Path -LiteralPath "$project.lock")) 'Project lock を取っていない (Project を開く前に断る)'
}

$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Executable).Path)
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$utf16 = [Text.Encoding]::Unicode.GetString($bytes)
Check (-not $ascii.Contains('MVM_TEST_REVEAL_WITHOUT_EXPLORER') -and -not $utf16.Contains('MVM_TEST_REVEAL_WITHOUT_EXPLORER')) (
    'mvm.exe は以前の環境変数 MVM_TEST_REVEAL_WITHOUT_EXPLORER を参照しない')

if ($failures -gt 0) { Write-Host "$failures 件失敗"; exit 1 }
Write-Host '全部成立'
exit 0
