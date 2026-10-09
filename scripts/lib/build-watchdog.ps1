# CMake / Ninja が「process は残っているが何も進まない」状態で止まったら、待ち続けずに失敗させる。
#
# 既知の例: Codex の sandbox 内では ninja が compiler を起動できず、CPU 時間も .ninja_log も
# 進まないまま止まる (AGENTS.md「Codex sandbox の既知制約」)。agent が tool の timeout まで待ち、
# 診断と kill を繰り返す時間を、この検知で明確な失敗メッセージ 1 回に置き換える。
#
# 進行の証拠は次の 3 つ。StallSeconds の間どれも変わらなければ停止と判定する。
#   - 起動した process tree (子・孫を含む) の CPU 時間の合計の増加
#   - tree を構成する process の入れ替わり (compiler の起動・終了)
#   - ProgressFile (.ninja_log) の更新
# 止めるのは、この関数が起動した PID の tree だけ。名前で machine-wide に止めない。
# tree を観測できない環境では、誤って止めないよう検知を無効にして待つ。

function Get-MvmProcessTree {
    param([Parameter(Mandatory)][int]$RootId)
    $children = @{}
    foreach ($process in @(Get-Process -ErrorAction Stop)) {
        $parent = $null
        try { $parent = $process.Parent } catch { $parent = $null }
        if (-not $parent) { continue }
        if (-not $children.ContainsKey($parent.Id)) { $children[$parent.Id] = [Collections.Generic.List[object]]::new() }
        $children[$parent.Id].Add($process)
    }
    $root = Get-Process -Id $RootId -ErrorAction SilentlyContinue
    if (-not $root) { return @() }
    $tree = [Collections.Generic.List[object]]::new()
    $pending = [Collections.Generic.Queue[object]]::new()
    $pending.Enqueue($root)
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        $tree.Add($current)
        if ($children.ContainsKey($current.Id)) {
            foreach ($child in $children[$current.Id]) { $pending.Enqueue($child) }
        }
    }
    return $tree.ToArray()
}

function Get-MvmBuildProgressSample {
    param([Parameter(Mandatory)][int]$RootId, [string]$ProgressFile)
    $tree = @(Get-MvmProcessTree -RootId $RootId)
    $cpu = 0.0
    foreach ($process in $tree) {
        try { $cpu += $process.TotalProcessorTime.TotalSeconds } catch { }
    }
    $file = ''
    if ($ProgressFile -and (Test-Path -LiteralPath $ProgressFile -PathType Leaf)) {
        $item = Get-Item -LiteralPath $ProgressFile
        $file = "$($item.LastWriteTimeUtc.Ticks):$($item.Length)"
    }
    [pscustomobject]@{
        CpuSeconds = $cpu
        Members = (@($tree | ForEach-Object { "$($_.Id)" } | Sort-Object) -join ',')
        File = $file
        Tree = $tree
    }
}

# 戻り値: ExitCode (停止時は $null)、Stalled、Report (停止時の説明)。
function Invoke-MvmWatchedProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [string]$ProgressFile,
        # 0 で検知を無効にする。正常な compile・link は CPU 時間が進むので、長い 1 本でも止めない。
        [int]$StallSeconds = 180,
        [int]$PollMilliseconds = 2000,
        # 停止中の ninja も CPU 時間をわずかに使う。これ未満の増加は進行と見なさない。
        [double]$MinCpuSeconds = 0.5
    )
    $startInfo = [Diagnostics.ProcessStartInfo]::new($FilePath)
    $startInfo.UseShellExecute = $false
    $startInfo.WorkingDirectory = (Get-Location).ProviderPath
    foreach ($argument in $ArgumentList) { $startInfo.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($startInfo)

    $observable = $StallSeconds -gt 0
    $baseline = $null
    $lastProgress = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.WaitForExit($PollMilliseconds)) {
        if (-not $observable) { continue }
        try {
            $sample = Get-MvmBuildProgressSample -RootId $process.Id -ProgressFile $ProgressFile
        } catch {
            Write-Warning "build の process tree を観測できないため、停止の検知を無効にします: $($_.Exception.Message)"
            $observable = $false
            continue
        }
        if (-not $baseline -or
            $sample.CpuSeconds - $baseline.CpuSeconds -ge $MinCpuSeconds -or
            $sample.Members -ne $baseline.Members -or $sample.File -ne $baseline.File) {
            $baseline = $sample
            $lastProgress.Restart()
            continue
        }
        if ($lastProgress.Elapsed.TotalSeconds -lt $StallSeconds) { continue }

        $lines = @($sample.Tree | ForEach-Object {
            $cpu = try { [math]::Round($_.TotalProcessorTime.TotalSeconds, 2) } catch { '?' }
            "  PID $($_.Id) $($_.ProcessName) CPU ${cpu}s"
        })
        $compilers = @($sample.Tree | Where-Object { $_.ProcessName -in 'g++','gcc','cc1plus','cc1','ld' })
        $report = @(
            "BUILD_STALLED: $StallSeconds 秒間、CPU 時間・子 process・$(if ($ProgressFile) { Split-Path -Leaf $ProgressFile } else { '進行ファイル' }) のどれも進みませんでした。"
            "compiler process: $($compilers.Count) 件"
            'この関数が起動した process tree:'
        ) + $lines
        try { $process.Kill($true) } catch { }
        $process.WaitForExit()
        return [pscustomobject]@{ ExitCode = $null; Stalled = $true; Report = ($report -join "`n") }
    }
    $process.WaitForExit()
    [pscustomobject]@{ ExitCode = $process.ExitCode; Stalled = $false; Report = '' }
}
