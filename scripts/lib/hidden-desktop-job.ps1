# 見えない desktop の上で、process の木を Job Object で所有して動かす (製品 GUI の smoke など)。
#
# 見えない desktop: CreateDesktop で作った desktop は利用者の画面に出ない。その上の window・dialog は
# 前面を奪わない。UI Automation と Windows の message は、同じ desktop の中でだけ届くので、操作する側
# (driver) も同じ desktop に置く (子孫は親の desktop を受け継ぐ)。
#
# 所有と停止は scripts/lib/build-watchdog.ps1 と同じ規則で、Job Object で厳密に行う。
#   - 根の process は一時停止の状態で作り、job に入れてから動かす。後から作られる子・孫 (親が先に
#     終わった孤児を含む) はすべて同じ job に入る。
#   - 時間切れの停止は TerminateJobObject で job ごと行う。PID の列挙や親子関係の辿りに頼らないので、
#     job の外の process (利用者の process) を止めることはない。
#   - 根が終わった後に job に残った process は、記録してから job ごと止める。
#   - job は KILL_ON_JOB_CLOSE。この runner の process が異常終了しても (例外・Ctrl+C・強制終了)、
#     OS が job の handle を閉じ、中の process は止まる。
#
# 使い方: . scripts/lib/hidden-desktop-job.ps1
#         $result = Invoke-HiddenDesktopJob -File pwsh.exe -Arguments @(...) -WorkingDirectory ... -TimeoutSeconds 1200
#         $result: exit_code, timed_out, desktop, root_process_id, at_timeout (時間切れの時点の job の中),
#                  left_after_exit (根の終了の後に残っていたもの), all_ended (最後に job が空になったか), seconds

if (-not ('Mvm.HiddenDesktopJob' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Mvm {
public sealed class HiddenDesktopJob : IDisposable {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_LIMIT {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize; public uint ActiveProcessLimit;
        public UIntPtr Affinity; public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS { public ulong a, b, c, d, e, f; }
    [StructLayout(LayoutKind.Sequential)]
    struct EXTENDED_LIMIT {
        public BASIC_LIMIT Basic; public IO_COUNTERS Io;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct BASIC_ACCOUNTING {
        public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime;
        public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr mode, int flags, uint access, IntPtr security);
    [DllImport("user32.dll", SetLastError = true)] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref EXTENDED_LIMIT info, int size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out BASIC_ACCOUNTING info, int size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder commandLine, IntPtr pa, IntPtr ta, bool inherit,
                                      uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr handle);

    const uint CREATE_SUSPENDED = 0x4, CREATE_NEW_CONSOLE = 0x10, KILL_ON_JOB_CLOSE = 0x2000;
    const uint GENERIC_ALL = 0x10000000;
    const uint WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 0x102;

    IntPtr desktop, job, process;
    public string DesktopName { get; private set; }
    public int ProcessId { get; private set; }

    HiddenDesktopJob() {}

    static Win32Exception Failure(string api, int error) {
        return new Win32Exception(error, api + " が失敗しました (Win32 エラー " + error + ": " +
                                         new Win32Exception(error).Message + ")");
    }

    // 引数を CommandLineToArgvW の規則で 1 本の command line にする (build-watchdog.ps1 と同じ)。
    static string Quote(string argument) {
        if (argument.Length > 0 && argument.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0)
            return argument;
        var result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in argument) {
            if (c == '\\') { slashes++; continue; }
            if (c == '"') result.Append('\\', slashes * 2 + 1).Append('"');
            else result.Append('\\', slashes).Append(c);
            slashes = 0;
        }
        return result.Append('\\', slashes * 2).Append('"').ToString();
    }

    // desktopName が null なら、呼び出し側と同じ desktop で動かす (試験用)。
    public static HiddenDesktopJob Start(string desktopName, string file, string[] arguments, string workingDirectory) {
        var self = new HiddenDesktopJob();
        try {
            if (desktopName != null) {
                self.desktop = CreateDesktopW(desktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
                if (self.desktop == IntPtr.Zero) throw Failure("CreateDesktop", Marshal.GetLastWin32Error());
                self.DesktopName = desktopName;
            }
            self.job = CreateJobObjectW(IntPtr.Zero, null);
            if (self.job == IntPtr.Zero) throw Failure("CreateJobObject", Marshal.GetLastWin32Error());
            var limit = new EXTENDED_LIMIT();
            limit.Basic.LimitFlags = KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(self.job, 9, ref limit, Marshal.SizeOf(limit)))
                throw Failure("SetInformationJobObject", Marshal.GetLastWin32Error());
            var line = new StringBuilder(Quote(file));
            foreach (var argument in arguments) line.Append(' ').Append(Quote(argument));
            var si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(si);
            if (desktopName != null) si.lpDesktop = "WinSta0\\" + desktopName;
            PROCESS_INFORMATION pi;
            // 標準入出力は受け継がない (見えない desktop の新しい console に出る)。記録は子が file に書く。
            if (!CreateProcessW(file, line, IntPtr.Zero, IntPtr.Zero, false, CREATE_SUSPENDED | CREATE_NEW_CONSOLE,
                                IntPtr.Zero, workingDirectory, ref si, out pi))
                throw Failure("CreateProcess (" + file + ")", Marshal.GetLastWin32Error());
            if (!AssignProcessToJobObject(self.job, pi.hProcess)) {
                int error = Marshal.GetLastWin32Error();
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                throw Failure("AssignProcessToJobObject", error);
            }
            self.process = pi.hProcess;
            self.ProcessId = pi.dwProcessId;
            uint resumed = ResumeThread(pi.hThread);
            int resumeError = Marshal.GetLastWin32Error();
            CloseHandle(pi.hThread);
            if (resumed == uint.MaxValue) throw Failure("ResumeThread", resumeError);
            return self;
        } catch {
            self.Terminate(1);
            self.Dispose();
            throw;
        }
    }

    public bool WaitForExit(int milliseconds) {
        uint result = WaitForSingleObject(process, (uint)milliseconds);
        if (result == WAIT_OBJECT_0) return true;
        if (result == WAIT_TIMEOUT) return false;
        throw Failure("WaitForSingleObject", Marshal.GetLastWin32Error());
    }

    public int GetExitCode() {
        uint code;
        if (!GetExitCodeProcess(process, out code)) throw Failure("GetExitCodeProcess", Marshal.GetLastWin32Error());
        return unchecked((int)code);
    }

    public int ActiveProcesses() {
        BASIC_ACCOUNTING info;
        if (!QueryInformationJobObject(job, 1, out info, Marshal.SizeOf(typeof(BASIC_ACCOUNTING)), IntPtr.Zero))
            throw Failure("QueryInformationJobObject", Marshal.GetLastWin32Error());
        return (int)info.ActiveProcesses;
    }

    // job の中で生きている process を "PID 名前" で返す (記録用)。
    public string[] Describe() {
        const int capacity = 4096;
        int size = 8 + capacity * IntPtr.Size;
        IntPtr buffer = Marshal.AllocHGlobal(size);
        var rows = new List<string>();
        try {
            if (!QueryInformationJobObject(job, 3, buffer, size, IntPtr.Zero))
                throw Failure("QueryInformationJobObject", Marshal.GetLastWin32Error());
            int count = Marshal.ReadInt32(buffer, 4);
            for (int i = 0; i < count; ++i) {
                long id = Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size).ToInt64();
                string name = "?";
                try { using (var p = Process.GetProcessById((int)id)) name = p.ProcessName; } catch (Exception) { }
                rows.Add(id + " " + name);
            }
        } finally { Marshal.FreeHGlobal(buffer); }
        return rows.ToArray();
    }

    // job の中を全部止め、空になるまで待つ (最大 waitMs)。空になれば true。
    public bool Terminate(uint exitCode, int waitMs = 10000) {
        if (job == IntPtr.Zero) return true;
        TerminateJobObject(job, exitCode);
        var clock = Stopwatch.StartNew();
        while (clock.ElapsedMilliseconds < waitMs) {
            try { if (ActiveProcesses() == 0) return true; } catch (Win32Exception) { return false; }
            System.Threading.Thread.Sleep(50);
        }
        return false;
    }

    public void Dispose() {
        // job を閉じると KILL_ON_JOB_CLOSE で中の process も止まる。desktop は、上の process が全部
        // 終わった後に OS が片付ける。
        if (process != IntPtr.Zero) { CloseHandle(process); process = IntPtr.Zero; }
        if (job != IntPtr.Zero) { CloseHandle(job); job = IntPtr.Zero; }
        if (desktop != IntPtr.Zero) { CloseDesktop(desktop); desktop = IntPtr.Zero; }
    }
}
}
'@
}

function Invoke-HiddenDesktopJob {
    param(
        [Parameter(Mandatory)][string]$File,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][int]$TimeoutSeconds,
        # 空なら、呼び出し側と同じ desktop で動かす (自己試験で、Job Object の所有だけを確かめるとき)。
        [string]$DesktopName = "mvm-hidden-$PID-$([Environment]::TickCount64)"
    )
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $job = [Mvm.HiddenDesktopJob]::Start($(if ($DesktopName) { $DesktopName } else { $null }), $File, $Arguments, $WorkingDirectory)
    try {
        $timedOut = -not $job.WaitForExit($TimeoutSeconds * 1000)
        $atTimeout = @(); $leftAfterExit = @()
        if ($timedOut) {
            $atTimeout = @($job.Describe())
            $allEnded = $job.Terminate(124)
            $code = -2
        } else {
            $code = $job.GetExitCode()
            # 根の console の conhost は、根より少し遅れて終わる。job が自然に空になるのを少し待ってから
            # (最大 5 秒)、残った process (driver・mvm.exe・helper) を記録して止める。
            $drain = [Diagnostics.Stopwatch]::StartNew()
            while ($job.ActiveProcesses() -gt 0 -and $drain.ElapsedMilliseconds -lt 5000) { Start-Sleep -Milliseconds 100 }
            $leftAfterExit = @($job.Describe())
            $allEnded = if ($leftAfterExit.Count -gt 0) { $job.Terminate(125) } else { $true }
        }
        [pscustomobject]@{
            exit_code = $code; timed_out = $timedOut; desktop = $job.DesktopName; root_process_id = $job.ProcessId
            at_timeout = $atTimeout; left_after_exit = $leftAfterExit; all_ended = [bool]$allEnded
            seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        }
    } finally {
        $job.Dispose()
    }
}
