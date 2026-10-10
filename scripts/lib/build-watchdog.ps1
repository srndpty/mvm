# CMake / Ninja が「process は残っているが何も進まない」状態で止まったら、待ち続けずに失敗させる。
#
# 既知の例: Codex の sandbox 内では ninja が compiler を起動できず、CPU 時間も .ninja_log も
# 進まないまま止まる (AGENTS.md「Codex sandbox の既知制約」)。agent が tool の timeout まで待ち、
# 診断と kill を繰り返す時間を、この検知で明確な失敗メッセージ 1 回に置き換える。
#
# 所有と停止は Windows の Job Object で厳密に行う。
#   - process は一時停止の状態で作り、job に入れてから動かす。最初の命令を実行する前に job に
#     入っているので、後から作られる子・孫 (親が先に終わった孤児を含む) はすべて同じ job に入る。
#   - 停止は TerminateJobObject で job ごと行う。PID の列挙や親子関係の辿りに頼らないので、
#     PID の再利用や、列挙と停止の間の process の入れ替わりで、別の process を止めることはない。
#   - job は KILL_ON_JOB_CLOSE。この関数を抜けるとき (正常終了・例外・Ctrl+C) に残った
#     process も止まり、repo の build process が残留しない。
#
# 進行の証拠は次のどれか。StallSeconds の間どれも変わらなければ停止と判定する。
#   - job の CPU 時間の累計の増加。終了した process の分も含むので、process が終わっても減らない
#   - job が作った process の総数、または生きている process の数の変化 (compiler の起動・終了)
#   - ProgressFile (.ninja_log) の更新
# 停止中の ninja も CPU 時間をわずかに使うので、MinCpuSeconds 未満の増加は進行と見なさない。

if (-not ('Mvm.BuildJob' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace Mvm {
public sealed class BuildJob : IDisposable {
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

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, ref EXTENDED_LIMIT info, int size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, out BASIC_ACCOUNTING info, int size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info, int size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder commandLine, IntPtr pa, IntPtr ta, bool inherit,
                                      uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int which);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DuplicateHandle(IntPtr sp, IntPtr s, IntPtr tp, out IntPtr t, uint access, bool inherit, uint options);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    const uint CREATE_SUSPENDED = 0x4;
    const uint KILL_ON_JOB_CLOSE = 0x2000;
    const int STARTF_USESTDHANDLES = 0x100;
    const uint DUPLICATE_SAME_ACCESS = 0x2;

    const uint WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xFFFFFFFF;

    IntPtr job, process;
    public int ProcessId { get; private set; }
    // 失敗の後でも、試験が「起動した process が残っていない」ことを確かめられるように残す。
    public static int LastProcessId { get; private set; }

    // 試験専用の注入点。API 名を入れると、その API を呼ばずに InjectedError で失敗したものとして
    // 扱う (1 回で解除)。製品の経路では常に null。
    public static string InjectFailure;
    public static int InjectedError = 1117; // ERROR_IO_DEVICE

    BuildJob() {}

    static bool Injected(string api) {
        if (InjectFailure != api) return false;
        InjectFailure = null;
        return true;
    }
    static Win32Exception Failure(string api, int error) {
        return new Win32Exception(error, api + " が失敗しました (Win32 エラー " + error + ": " +
                                         new Win32Exception(error).Message + ")");
    }

    // 失敗の後始末。job を閉じると KILL_ON_JOB_CLOSE で中の process も止まる。job 単位の停止を
    // 先に試し、それが失敗しても handle を閉じることで止める (所有の外へ process を残さない)。
    void Abandon() {
        if (job != IntPtr.Zero) TerminateJobObject(job, 1);
        Dispose();
    }

    // 引数を CommandLineToArgvW の規則で 1 本の command line にする。
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

    static IntPtr Inheritable(int which, List<IntPtr> owned) {
        IntPtr handle = GetStdHandle(which);
        if (handle == IntPtr.Zero || handle == new IntPtr(-1)) return handle;
        IntPtr copy;
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), out copy, 0, true, DUPLICATE_SAME_ACCESS))
            return handle;
        owned.Add(copy);
        return copy;
    }

    public static BuildJob Start(string file, string[] arguments, string workingDirectory) {
        var self = new BuildJob();
        LastProcessId = 0;
        self.job = CreateJobObjectW(IntPtr.Zero, null);
        if (self.job == IntPtr.Zero) throw Failure("CreateJobObject", Marshal.GetLastWin32Error());
        var limit = new EXTENDED_LIMIT();
        limit.Basic.LimitFlags = KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(self.job, 9, ref limit, Marshal.SizeOf(limit))) {
            int error = Marshal.GetLastWin32Error();
            self.Dispose();
            throw Failure("SetInformationJobObject", error);
        }
        var line = new StringBuilder(Quote(file));
        foreach (var argument in arguments) line.Append(' ').Append(Quote(argument));
        // 親と同じ標準入出力 (console・pipe・file) へ書かせる。
        var owned = new List<IntPtr>();
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = Inheritable(-10, owned);
        si.hStdOutput = Inheritable(-11, owned);
        si.hStdError = Inheritable(-12, owned);
        PROCESS_INFORMATION pi;
        bool created = CreateProcessW(file, line, IntPtr.Zero, IntPtr.Zero, true, CREATE_SUSPENDED, IntPtr.Zero,
                                      workingDirectory, ref si, out pi);
        int createError = Marshal.GetLastWin32Error();
        foreach (var handle in owned) CloseHandle(handle);
        if (!created) { self.Dispose(); throw Failure("CreateProcess (" + file + ")", createError); }
        LastProcessId = pi.dwProcessId;
        if (!AssignProcessToJobObject(self.job, pi.hProcess)) {
            // job に入っていないので、job を閉じても止まらない。一時停止のまま個別に止める。
            int error = Marshal.GetLastWin32Error();
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            self.Dispose();
            throw Failure("AssignProcessToJobObject", error);
        }
        self.process = pi.hProcess;
        self.ProcessId = pi.dwProcessId;
        // 0xFFFFFFFF が失敗。process は job の中で一時停止のままなので、job ごと止める。
        bool injected = Injected("ResumeThread");
        uint resumed = injected ? uint.MaxValue : ResumeThread(pi.hThread);
        int resumeError = injected ? InjectedError : Marshal.GetLastWin32Error();
        CloseHandle(pi.hThread);
        if (resumed == uint.MaxValue) {
            self.Abandon();
            throw Failure("ResumeThread", resumeError);
        }
        return self;
    }

    // 時間内に終われば true、時間切れなら false。待機そのものの失敗は例外にする (終了扱いにしない)。
    public bool WaitForExit(int milliseconds) {
        bool injected = Injected("WaitForSingleObject");
        uint result = injected ? WAIT_FAILED : WaitForSingleObject(process, (uint)milliseconds);
        int error = injected ? InjectedError : Marshal.GetLastWin32Error();
        if (result == WAIT_OBJECT_0) return true;
        if (result == WAIT_TIMEOUT) return false;
        // WAIT_ABANDONED など、process の handle では起きないはずの値も失敗として扱う。
        throw Failure("WaitForSingleObject", result == WAIT_FAILED ? error : unchecked((int)result));
    }

    // 失敗を例外で返す値は property にしない。PowerShell は property の getter の例外を握りつぶし
    // $null を返すので (実測)、失敗が「値なし」に化ける。method の例外はそのまま伝わる。
    public int GetExitCode() {
        bool injected = Injected("GetExitCodeProcess");
        uint code = 0;
        bool ok = !injected && GetExitCodeProcess(process, out code);
        if (!ok) throw Failure("GetExitCodeProcess", injected ? InjectedError : Marshal.GetLastWin32Error());
        return unchecked((int)code);
    }

    BASIC_ACCOUNTING Accounting() {
        BASIC_ACCOUNTING info = new BASIC_ACCOUNTING();
        bool injected = Injected("QueryInformationJobObject");
        if (injected || !QueryInformationJobObject(job, 1, out info, Marshal.SizeOf(typeof(BASIC_ACCOUNTING)), IntPtr.Zero))
            throw Failure("QueryInformationJobObject", injected ? InjectedError : Marshal.GetLastWin32Error());
        return info;
    }
    // 終了した process の分も含む累計 (秒)。
    // { CPU 時間の累計 (秒), 作成した process の総数, 生きている process の数 } を同じ時点で返す。
    public double[] Sample() {
        var a = Accounting();
        return new double[] { (a.TotalUserTime + a.TotalKernelTime) / 1e7, a.TotalProcesses, a.ActiveProcesses };
    }

    public int[] ProcessIds() {
        const int capacity = 4096;
        int size = 8 + capacity * IntPtr.Size;
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try {
            if (!QueryInformationJobObject(job, 3, buffer, size, IntPtr.Zero))
                throw Failure("QueryInformationJobObject", Marshal.GetLastWin32Error());
            int count = Marshal.ReadInt32(buffer, 4);
            var ids = new int[count];
            for (int i = 0; i < count; i++) ids[i] = (int)Marshal.ReadIntPtr(buffer, 8 + i * IntPtr.Size).ToInt64();
            return ids;
        } finally { Marshal.FreeHGlobal(buffer); }
    }

    // job の全 process を止め、生きている process が 0 になるまで待つ。0 にならなければ false。
    // 停止の要求や待機そのものが失敗したら例外にする (呼び出し側の Dispose が job を閉じて止める)。
    public bool Terminate(int timeoutMilliseconds) {
        bool injected = Injected("TerminateJobObject");
        bool terminated = !injected && TerminateJobObject(job, 1);
        if (!terminated)
            throw Failure("TerminateJobObject", injected ? InjectedError : Marshal.GetLastWin32Error());
        WaitForExit(timeoutMilliseconds);
        var watch = System.Diagnostics.Stopwatch.StartNew();
        while (Accounting().ActiveProcesses != 0) {
            if (watch.ElapsedMilliseconds > timeoutMilliseconds) return false;
            System.Threading.Thread.Sleep(20);
        }
        return true;
    }

    public void Dispose() {
        if (process != IntPtr.Zero) { CloseHandle(process); process = IntPtr.Zero; }
        if (job != IntPtr.Zero) { CloseHandle(job); job = IntPtr.Zero; }
    }
}
}
'@
}

function Get-MvmBuildProgressSample {
    param([Parameter(Mandatory)][Mvm.BuildJob]$Job, [string]$ProgressFile)
    $file = ''
    if ($ProgressFile -and (Test-Path -LiteralPath $ProgressFile -PathType Leaf)) {
        $item = Get-Item -LiteralPath $ProgressFile
        $file = "$($item.LastWriteTimeUtc.Ticks):$($item.Length)"
    }
    $counts = $Job.Sample()
    [pscustomobject]@{
        CpuSeconds = $counts[0]
        TotalProcesses = $counts[1]
        ActiveProcesses = $counts[2]
        File = $file
    }
}

# 前回の進行から何も変わっていなければ $false。
function Test-MvmBuildProgress {
    param([Parameter(Mandatory)]$Baseline, [Parameter(Mandatory)]$Sample, [double]$MinCpuSeconds)
    return ($Sample.CpuSeconds - $Baseline.CpuSeconds -ge $MinCpuSeconds -or
        $Sample.TotalProcesses -ne $Baseline.TotalProcesses -or
        $Sample.ActiveProcesses -ne $Baseline.ActiveProcesses -or
        $Sample.File -ne $Baseline.File)
}

# 戻り値: ExitCode (停止時は $null)、Stalled、Report (停止時の説明)、
# ProcessIds (停止時に job にいた PID)、Terminated (停止後に job の process が 0 になったか)。
function Invoke-MvmWatchedProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$ProgressFile,
        # 0 で検知を無効にする。正常な compile・link は CPU 時間が進むので、長い 1 本でも止めない。
        [int]$StallSeconds = 180,
        [int]$PollMilliseconds = 2000,
        [double]$MinCpuSeconds = 0.5
    )
    $resolved = (Get-Command -Name $FilePath -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
    $job = [Mvm.BuildJob]::Start($resolved, [string[]]$ArgumentList, (Get-Location).ProviderPath)
    try {
        $baseline = $null
        $lastProgress = [Diagnostics.Stopwatch]::StartNew()
        while (-not $job.WaitForExit($PollMilliseconds)) {
            if ($StallSeconds -le 0) { continue }
            $sample = Get-MvmBuildProgressSample -Job $job -ProgressFile $ProgressFile
            if (-not $baseline -or (Test-MvmBuildProgress -Baseline $baseline -Sample $sample `
                    -MinCpuSeconds $MinCpuSeconds)) {
                $baseline = $sample
                $lastProgress.Restart()
                continue
            }
            if ($lastProgress.Elapsed.TotalSeconds -lt $StallSeconds) { continue }

            # 報告用の名前は best effort (列挙後に終わった process は名前が取れない)。停止は job で行う。
            $ids = @($job.ProcessIds())
            $lines = @($ids | ForEach-Object {
                $name = (Get-Process -Id $_ -ErrorAction SilentlyContinue).ProcessName
                "  PID $_ $(if ($name) { $name } else { '(終了済み)' })"
            })
            $compilers = @($ids | Where-Object {
                (Get-Process -Id $_ -ErrorAction SilentlyContinue).ProcessName -in 'g++','gcc','cc1plus','cc1','ld'
            })
            $terminated = $job.Terminate(10000)
            $report = @(
                "BUILD_STALLED: $StallSeconds 秒間、CPU 時間・子 process・$(if ($ProgressFile) { Split-Path -Leaf $ProgressFile } else { '進行ファイル' }) のどれも進みませんでした。"
                "job の CPU 時間 $([math]::Round($sample.CpuSeconds, 2)) 秒、作成した process $($sample.TotalProcesses) 件、compiler process $($compilers.Count) 件"
                "job (この関数が起動した process とその子孫) の process:"
            ) + $lines + @($(if ($terminated) { 'job の process をすべて終了しました。' }
                             else { '警告: job の process が 10 秒以内に終了しませんでした。' }))
            return [pscustomobject]@{
                ExitCode = $null; Stalled = $true; Report = ($report -join "`n")
                ProcessIds = $ids; Terminated = $terminated
            }
        }
        [pscustomobject]@{
            ExitCode = $job.GetExitCode(); Stalled = $false; Report = ''; ProcessIds = @(); Terminated = $true
        }
    } finally {
        # KILL_ON_JOB_CLOSE: 根の process が終わった後に残った子孫もここで止まる。
        $job.Dispose()
    }
}
