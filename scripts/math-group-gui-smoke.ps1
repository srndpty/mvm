<#
.SYNOPSIS
    製品 GUI の smoke: 実際の mvm.exe を、P3-6C.2 の driver で操作し、書き出しの UX を確かめる。
    人の操作は要らない (mouse・keyboard は使わない)。

.DESCRIPTION
    scripts/math-group-product-p36c2-accept.ps1 (freeze・reference・session -Drive・verify) を、新しい出力先で
    順に呼ぶ。既定は短い場面だけ (A: 対象外の Project、B: 数式だけの Project。全部で約 4 分)。
    -Full を付けると、v4 全体 (6840 frame、約 3〜5 分) の場面 C も行う。全長は、最終の binary が変わった
    とき、または全長に関わる動き (書き出しの経路・数式 group) を変えたときにだけ使う。

    既定では、新しく作った見えない desktop (CreateDesktop) の上で全部を実行する。mvm.exe の window・保存の
    dialog・上書きの確認は利用者の画面に出ず、前面も奪わない (利用者は PC を使い続けてよい)。driver も同じ
    desktop で動くので、UI Automation と Windows の message はそのまま届く。
      - 見えない desktop から「Explorer で表示」を呼ぶと、Explorer (利用者の desktop の process) が
        SHOpenFolderAndSelectItems を 20 秒〜5 分以上待たせてから E_ABORT (0x80004004) で断り、その間 mvm の
        GUI thread が止まる (2026-10-15 に実測)。そこで mvm.exe を MVM_TEST_REVEAL_WITHOUT_EXPLORER 付きで
        起動する。path の正規化と実際の SHParseDisplayName (P3-7A の原因の箇所) は通り、Explorer を開く処理
        だけを成功として扱う。Explorer の window は開かない。
      - 画面の保存 (PrintWindow) は記録だけで、判定には使わない。
    -Visible を付けると、以前と同じく利用者の画面で実行する (mvm.exe が前面に出て、書き出しのたびに
    Explorer の window が実際に開く。Explorer で表示できることまで確かめたいときに使う)。

    出力先: build/math-group-gui-smoke/<日時>/ (前の結果は上書きしない)。経過は smoke.log に残す。
    終了コード: 0 = 全部の場面が成立 (session と verify の両方が 0 で終わり、この run で書いた verify.json の
    全項目と driver の全段が成立)、1 = 不成立がある、2 = 準備 (freeze・reference) の失敗。
    P3-6C.2 の検収の記録と判定 (build/math-group-product-p36c2-20261014/) には触れない。
#>
param(
    [switch]$Full,
    [string]$OutputDirectory = 'build/math-group-gui-smoke',
    # 利用者の画面で実行する (以前の動き)。
    [switch]$Visible,
    # 内部用: 見えない desktop の上で起動し直したときに、親が渡す出力先 (repo からの相対 path)。
    [string]$RunDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$accept = Join-Path $PSScriptRoot 'math-group-product-p36c2-accept.ps1'
$relative = if ($RunDirectory) { $RunDirectory } else { "$OutputDirectory/$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
$root = Join-Path $repo $relative
New-Item -ItemType Directory -Force $root | Out-Null
$pwshPath = (Get-Process -Id $PID).Path

if (-not $Visible -and -not $RunDirectory) {
    # 見えない desktop を作り、この script をその上で起動し直して、終わるのを待つ。
    if (-not ('Mvm.Smoke.HiddenDesktop' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Mvm.Smoke {
public static class HiddenDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, int flags, uint access, IntPtr security);
    [DllImport("user32.dll")] private static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(string application, StringBuilder commandLine, IntPtr processSecurity,
        IntPtr threadSecurity, bool inheritHandles, uint flags, IntPtr environment, string directory,
        ref STARTUPINFO startup, out PROCESS_INFORMATION information);
    [DllImport("kernel32.dll")] private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")] private static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll")] private static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);

    // desktop を作り、その上で commandLine を起動して待つ。終了コードを返す (時間切れは -2)。
    // desktop は、上の process が全部終わった後に OS が片付ける (handle を閉じても、使用中は残る)。
    public static int Run(string desktop, string application, string commandLine, string directory, int timeoutMs) {
        IntPtr handle = CreateDesktopW(desktop, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000u /* GENERIC_ALL */, IntPtr.Zero);
        if (handle == IntPtr.Zero) throw new Win32Exception();
        try {
            STARTUPINFO startup = new STARTUPINFO();
            startup.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            startup.lpDesktop = "WinSta0\\" + desktop;
            PROCESS_INFORMATION information;
            if (!CreateProcessW(application, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, false,
                                0x00000010u /* CREATE_NEW_CONSOLE */, IntPtr.Zero, directory, ref startup, out information))
                throw new Win32Exception();
            try {
                if (WaitForSingleObject(information.hProcess, (uint)timeoutMs) != 0) {
                    TerminateProcess(information.hProcess, 124);
                    return -2;
                }
                uint code;
                GetExitCodeProcess(information.hProcess, out code);
                return (int)code;
            } finally {
                CloseHandle(information.hThread);
                CloseHandle(information.hProcess);
            }
        } finally {
            CloseDesktop(handle);
        }
    }
}
}
'@
    }
    $arguments = @('-NoProfile', '-File', $PSCommandPath, '-RunDirectory', $relative, '-OutputDirectory', $OutputDirectory)
    if ($Full) { $arguments += '-Full' }
    $commandLine = (@($pwshPath) + $arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $limitSeconds = if ($Full) { 3600 } else { 1200 }
    Write-Host "見えない desktop で実行します (利用者の画面には出ません。最大 $limitSeconds 秒): $root"
    # 子の process (session の mvm.exe まで) は、この環境変数を受け継ぐ。
    $env:MVM_TEST_REVEAL_WITHOUT_EXPLORER = '1'
    try {
        $code = [Mvm.Smoke.HiddenDesktop]::Run("mvm-gui-smoke-$PID", $pwshPath, $commandLine, $repo, $limitSeconds * 1000)
    } finally {
        Remove-Item Env:MVM_TEST_REVEAL_WITHOUT_EXPLORER -ErrorAction SilentlyContinue
    }
    # 経過 (transcript の見出しと git の改行の警告を除く) を、この console にも出す。
    $log = Join-Path $root 'smoke.log'
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log | Where-Object { $_ -match '^(freeze|reference|short-|v4 |結果|準備|見えない)' }
    }
    if ($code -eq -2) { Write-Host "時間切れ ($limitSeconds 秒) で止めました: $root" }
    exit $code
}
if ($RunDirectory) {
    # 見えない desktop の上の console は誰にも見えないので、経過を file に残す。
    Start-Transcript -LiteralPath (Join-Path $root 'smoke.log') | Out-Null
}

function Invoke-Accept([string[]]$Arguments, [string]$Log) {
    & $pwshPath -NoProfile -File $accept @Arguments -OutputDirectory $relative *> (Join-Path $root $Log)
    $LASTEXITCODE
}

# 1 つの場面の判定。成立の条件: session と verify の両方が 0 で終わり、verify の開始より後に書かれた
# verify.json があり、その全項目が成立し、driver の段が 1 つ以上あって全部成功している。古い verify.json
# (前の run・前の verify) は認めない。自己試験 (math-group-product-p36c2-selftest.ps1) がこの関数を直接呼ぶ。
function Get-ScenarioVerdict([int]$SessionExit, [int]$VerifyExit, [string]$VerifyPath, [datetime]$VerifyStarted) {
    $fresh = [bool]$VerifyPath -and (Test-Path -LiteralPath $VerifyPath) -and
        (Get-Item -LiteralPath $VerifyPath).LastWriteTime -ge $VerifyStarted
    $verify = if ($fresh) { Get-Content -Raw -LiteralPath $VerifyPath | ConvertFrom-Json } else { $null }
    $failed = @(if ($verify) { $verify.verdicts.PSObject.Properties | Where-Object { -not $_.Value } | ForEach-Object { $_.Name } }
        else { 'この run の verify.json がありません' })
    $steps = @(if ($verify -and $verify.PSObject.Properties['drive_steps']) { $verify.drive_steps })
    $stepsOk = @($steps | Where-Object { $_.ok }).Count
    [pscustomobject]@{
        passed = $SessionExit -eq 0 -and $VerifyExit -eq 0 -and $fresh -and $failed.Count -eq 0 -and
            $steps.Count -gt 0 -and $stepsOk -eq $steps.Count
        fresh = $fresh; failed = $failed; steps = $steps.Count; steps_ok = $stepsOk
    }
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
    # この run のこの場面で作られた session と verify の記録だけを認める (古い記録で通さない)。
    $sessionStarted = Get-Date
    $code = Invoke-Accept @('-Stage', 'session', '-Project', $scenario[0], '-Label', $scenario[1], '-Drive') "session-$($scenario[1]).log"
    $session = @(Get-ChildItem $root -Directory -Filter "*-session-$($scenario[0])-$($scenario[1])" |
            Where-Object { $_.CreationTime -ge $sessionStarted }) | Select-Object -Last 1
    $verifyStarted = Get-Date
    $verifyCode = if ($session) { Invoke-Accept @('-Stage', 'verify', '-Session', $session.Name) "verify-$($scenario[1]).log" } else { -1 }
    $verifyPath = if ($session) { Join-Path $session.FullName 'verify.json' } else { $null }
    $verdict = Get-ScenarioVerdict $code $verifyCode $verifyPath $verifyStarted
    $rows += [pscustomobject]@{ project = $scenario[0]; session = $(if ($session) { $session.Name } else { $null }); session_exit = $code
        verify_exit = $verifyCode; verify_fresh = $verdict.fresh; failed_verdicts = $verdict.failed
        drive_steps_ok = $verdict.steps_ok; drive_steps = $verdict.steps; passed = $verdict.passed
    }
    Write-Host ("{0,-26} session={1} verify={2} driver {3}/{4} 不成立=[{5}] 判定={6}" -f $scenario[0], $code, $verifyCode,
        $verdict.steps_ok, $verdict.steps, ($verdict.failed -join ','), $(if ($verdict.passed) { '成立' } else { '不成立' }))
}
$failures = @($rows | Where-Object { -not $_.passed }).Count
[ordered]@{ created = (Get-Date -Format 'o'); full = [bool]$Full; failures = $failures
    desktop = $(if ($RunDirectory) { '見えない desktop' } else { '利用者の画面 (-Visible)' }); scenarios = $rows
    note = '製品 GUI の smoke。人の操作なし。P3-6C.2 の検収の判定とは別の記録である。'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'smoke.json') -Encoding utf8
Write-Host "結果: $root"
if ($failures -gt 0) { exit 1 }
exit 0
