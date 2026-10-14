<#
.SYNOPSIS
    P3-6C.2: 実際の mvm.exe を人が操作して行う、数式の高速化 (数式 group) の書き出しの検収。
    この script は、対象の固定・起動と記録・事後の検査だけを行う。GUI の操作は人が行う。

.DESCRIPTION
    段階 (-Stage):
      freeze     mvm.exe、runtime の DLL (MSYS2 UCRT64 の bin・MLT の module・Qt の plugin と QML の plugin)、
                 source と QML、v4 の Project・数式の成果物・参照する素材、短い fixture、この script と
                 手順の文書の SHA-256 を freeze.json へ書く。既にあれば書き直さない。
      reference  短い fixture を、検証 harness の --product-pure (製品の要求を書き換えない書き出し) で
                 書き出し、出力の SHA-256 を reference.json へ書く。人の書き出しとの照合に使う。
                 v4 全体の参照は、P3-4・P3-5A・P3-6B の出力 (-HistoricalOutputs) である。
      session    固定した mvm.exe を、指定の Project で普通に起動し、閉じられるまで見守る。
                 GUI は一切操作しない。次を記録する (session.json):
                   - 起動した実行 file の path と SHA-256、読み込まれた DLL と freeze との照合
                   - mvm.exe が console (stderr) へ出す「書き出しを始めます」「書き出しの結果」の行を、
                     この script の単調な時計 (Stopwatch) で読んだ時刻。開始から結果までの経過
                   - 起動の前後の、アプリの設定 (registry) と Project の file
                   - mvm の window の画面 (一定の間隔で PNG に保存する。操作はしない)
                 watchdog (-WatchdogSeconds) は診断用で、超えたら記録するだけである (止めない)。検収の
                 基準 (600 秒) とは別に持つ。
      verify     session の記録と出力の MP4 を調べる (ffprobe・ffmpeg。build はしない)。
                 v4: H.264 High・1920x1080・60 fps・6840 frame・時刻・復号・一時 file・SHA-256。
                 短い fixture: reference.json の SHA-256 との一致。

    session と verify は、実行の前と後に freeze.json と照合する。source を書き換えない。build はしない。

.EXAMPLE
    pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage session -Project v4 -Label full
#>
param(
    [Parameter(Mandatory)][ValidateSet('freeze', 'reference', 'session', 'verify')][string]$Stage,
    [ValidateSet('v4', 'short-math-only-eligible', 'short-fallback-unknown')][string]$Project = 'v4',
    [string]$Label = '',
    [string]$Session = '',
    # 自動の検収 (2026-10-14。人の操作をやめ、-Drive で driver が操作する)。1 回目の人の操作の記録は
    # build/math-group-product-p36c2-20261014 の直下、2 回目の準備は attempt-2/ にある (使っていない)。
    [string]$OutputDirectory = 'build/math-group-product-p36c2-20261014/automated',
    [string]$V4ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$FixtureDirectory = 'build/math-group-product-p36c2-20261014/fixtures',
    [string[]]$HistoricalOutputs = @(
        'build/math-export-p34-20261013/20261013-011412-full/candidate/v4-full.mp4',
        'build/math-export-p35a-20261013/20261013-024935-strong/candidate-full/v4-full.mp4',
        'build/math-group-product-p36b-20261014/20261014-063420-full-authoritative-1/product/v4-full.mp4'),
    [int]$TotalFrames = 6840,
    [double]$BudgetSeconds = 600,
    [int]$WatchdogSeconds = 1500,
    [int]$ScreenshotSeconds = 2,
    [switch]$SkipEnvironmentPreflight,
    # 人の代わりに、scripts/math-group-product-p36c2-drive.ps1 が mvm.exe を操作する (mouse・keyboard は
    # 使わない)。Project ごとの場面: short-fallback-unknown = A、short-math-only-eligible = B、v4 = C。
    # 環境の検査は記録するだけで、起動を止めない (書き出しと UI Automation は画面の表示を要しない)。
    [switch]$Drive,
    # script の自己試験だけが使う: mvm.exe の代わりに起動するもの (引数は | で区切る)。verify は、
    # 固定した mvm.exe ではないとして不成立にする。
    [string]$ApplicationOverride = '',
    [string]$ApplicationArgumentsOverride = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$msys = 'C:\msys64\ucrt64'
$ucrt = Join-Path $msys 'bin'
$bin = Join-Path $repo 'build\ucrt64-release\bin'
$application = Join-Path $bin 'mvm.exe'
$harness = Join-Path $bin 'mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$ffprobe = Join-Path $ucrt 'ffprobe.exe'
$ffmpeg = Join-Path $ucrt 'ffmpeg.exe'
$root = Join-Path $repo $OutputDirectory
$fixtures = Join-Path $repo $FixtureDirectory
$freezePath = Join-Path $root 'freeze.json'
$referencePath = Join-Path $root 'reference.json'
$settingsKey = 'HKCU:\Software\mvm\mvm'
New-Item -ItemType Directory -Force $root | Out-Null
$projects = @{
    'v4'                       = (Join-Path $repo $V4ProjectPath)
    'short-math-only-eligible' = (Join-Path $fixtures 'short-math-only-eligible.mvm')
    'short-fallback-unknown'   = (Join-Path $fixtures 'short-fallback-unknown.mvm')
}

function Get-Sha([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash }

function Get-CacheFiles([string]$ProjectFile) {
    $cache = Join-Path (Split-Path -Parent $ProjectFile) 'cache'
    if (-not (Test-Path -LiteralPath $cache)) { return @() }
    # cache の jobs\ は、実行のたびに書き直される記録なので除く。
    @(Get-ChildItem $cache -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]jobs[\\/]' } | ForEach-Object { $_.FullName })
}

function Get-IdentityFiles {
    $files = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in (git -C $repo ls-files -- src apps tests CMakeLists.txt cmake)) { $files.Add((Join-Path $repo $relative)) }
    foreach ($relative in (git -C $repo ls-files --others --exclude-standard -- src apps tests)) { $files.Add((Join-Path $repo $relative)) }
    foreach ($relative in @('scripts/math-group-product-p36c2-accept.ps1', 'scripts/math-group-product-p36c2-selftest.ps1', 'scripts/math-group-product-p36b-verify.ps1',
            'scripts/math-group-product-p36a-verify.ps1', 'scripts/gui-environment-preflight.ps1', 'scripts/math-group-product-p36c2-drive.ps1', 'scripts/math-group-gui-smoke.ps1',
            'docs/math-group-product-p36c2-procedure.md')) { $files.Add((Join-Path $repo $relative)) }
    # 実行 file: 検収で人が操作する mvm.exe そのものと、参照を作る検証 harness。
    $files.Add($application)
    $files.Add($harness)
    # mvm.exe の隣に置かれる DLL (文字起こしの library など)。
    foreach ($file in (Get-ChildItem $bin -Filter *.dll)) { $files.Add($file.FullName) }
    # runtime: MSYS2 UCRT64 の DLL の全部、MLT の module、Qt の plugin と QML の plugin、ffprobe・ffmpeg。
    foreach ($file in (Get-ChildItem $ucrt -Filter *.dll)) { $files.Add($file.FullName) }
    foreach ($directory in @('lib\mlt', 'share\qt6\plugins', 'share\qt6\qml')) {
        foreach ($file in (Get-ChildItem (Join-Path $msys $directory) -Recurse -Filter *.dll)) { $files.Add($file.FullName) }
    }
    $files.Add($ffprobe); $files.Add($ffmpeg)
    # Project、数式の成果物、Project が絶対 path で参照する素材。
    foreach ($projectFile in $projects.Values) {
        $files.Add($projectFile)
        foreach ($file in (Get-CacheFiles $projectFile)) { $files.Add($file) }
        foreach ($match in [regex]::Matches((Get-Content -Raw $projectFile), '"([A-Za-z]:(?:\\|/)[^"]+)"')) {
            $media = $match.Groups[1].Value.Replace('\\', '\')
            if (Test-Path -LiteralPath $media -PathType Leaf) { $files.Add((Get-Item -LiteralPath $media).FullName) }
        }
    }
    @($files | Sort-Object -Unique)
}

function Get-SourceDiffSha {
    # git に file へ書かせて、その byte 列を hash する (PowerShell に文字列として読ませない)。
    $temporary = [IO.Path]::GetTempFileName()
    try {
        git -C $repo diff HEAD "--output=$temporary" -- src apps tests scripts CMakeLists.txt cmake
        if ($LASTEXITCODE -ne 0) { throw 'git diff に失敗しました' }
        Get-Sha $temporary
    } finally { Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue }
}

if ($Stage -eq 'freeze') {
    if (Test-Path -LiteralPath $freezePath) { throw "freeze.json が既にあります: $freezePath" }
    $hashes = [ordered]@{}
    foreach ($file in (Get-IdentityFiles)) { $hashes[$file] = Get-Sha $file }
    git -C $repo diff HEAD "--output=$(Join-Path $root 'freeze-git-diff.patch')" -- src apps tests scripts CMakeLists.txt cmake
    [ordered]@{
        created = (Get-Date -Format 'o'); git_head = (git -C $repo rev-parse HEAD); git_status = @(git -C $repo status --porcelain)
        source_diff_sha256 = (Get-SourceDiffSha)
        application = $application; application_sha256 = $hashes[$application]
        ffmpeg_version = (& $ffmpeg -hide_banner -version | Select-Object -First 1)
        melt_version = (& (Join-Path $ucrt 'melt.exe') --version 2>&1 | Select-Object -First 1)
        sha256 = $hashes
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $freezePath -Encoding utf8
    Write-Host "freeze: $($hashes.Count) file → $freezePath"
    Write-Host "mvm.exe: $($hashes[$application])"
    exit 0
}

if (-not (Test-Path -LiteralPath $freezePath)) { throw "freeze.json がありません: $freezePath" }
$frozen = Get-Content -Raw $freezePath | ConvertFrom-Json -AsHashtable

function Test-Frozen([string]$When) {
    $changed = @($frozen.sha256.Keys | Where-Object { -not (Test-Path -LiteralPath $_) -or (Get-Sha $_) -ne $frozen.sha256[$_] })
    $added = @(Get-IdentityFiles | Where-Object { -not $frozen.sha256.ContainsKey($_) })
    $diffSame = (Get-SourceDiffSha) -eq $frozen.source_diff_sha256
    $headSame = (git -C $repo rev-parse HEAD) -eq $frozen.git_head
    foreach ($file in $changed) { Write-Host "変わった: $file" }
    foreach ($file in $added) { Write-Host "増えた: $file" }
    $ok = $changed.Count -eq 0 -and $added.Count -eq 0 -and $diffSame -and $headSame
    Write-Host ("freeze.json との照合 ({0}): {1} ({2} file)" -f $When, $(if ($ok) { '一致' } else { '不一致' }), $frozen.sha256.Count)
    [pscustomobject]@{ when = $When; match = $ok; files = $frozen.sha256.Count; changed = $changed; added = $added; source_diff_same = $diffSame; head_same = $headSame }
}

function Get-SettingsSnapshot {
    $snapshot = [ordered]@{}
    if (-not (Test-Path $settingsKey)) { return $snapshot }
    foreach ($key in @(Get-Item $settingsKey) + @(Get-ChildItem $settingsKey -Recurse)) {
        foreach ($name in $key.GetValueNames()) { $snapshot["$($key.Name)\$name"] = "$($key.GetValue($name))" }
    }
    $snapshot
}

function Get-MediaLeft([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) { return @() }
    @(Get-ChildItem $Directory -File | Where-Object { $_.Name -match '\.(mvmtmp|tmp|part)$' } | ForEach-Object { $_.FullName })
}

# mvm.exe が stderr へ出す行 (apps/mvm/mvm_controller_export.cpp)。
function ConvertFrom-ExportLine([string]$Line) {
    if ($Line -match '^書き出しを始めます: (?<path>.+) \(数式の高速化: (?<selection>選択あり|選択なし)\)\s*$') {
        return [pscustomobject]@{ kind = 'start'; output = $Matches.path; selected = $Matches.selection -eq '選択あり' }
    }
    if ($Line -match "^書き出しの結果: (?<result>成功|失敗|取消) frame=(?<frames>-?\d+) error='(?<error>.*)' \| 数式の高速化: (?<fields>.+)$") {
        $row = [ordered]@{ kind = 'result'; result = $Matches.result; frames = [long]$Matches.frames; error = $Matches.error }
        foreach ($pair in ($Matches.fields -split ' ')) {
            $at = $pair.IndexOf('=')
            if ($at -gt 0) { $row[$pair.Substring(0, $at)] = $pair.Substring($at + 1) }
        }
        return [pscustomobject]$row
    }
    $null
}

$before = Test-Frozen '前'
if (-not $before.match) { throw 'freeze.json と一致しません' }
$env:PATH = "$ucrt;$latex;$env:PATH"

if ($Stage -eq 'reference') {
    if (Test-Path -LiteralPath $referencePath) { throw "reference.json が既にあります: $referencePath" }
    $run = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-reference')
    New-Item -ItemType Directory -Force $run | Out-Null
    $env:MVM_TEST_FIXED_WINDOW = '1'
    $rows = [ordered]@{}
    $failures = 0
    foreach ($name in @('short-math-only-eligible', 'short-fallback-unknown')) {
        foreach ($mode in @('on', 'off')) {
            $out = Join-Path $run "$name-$mode"
            New-Item -ItemType Directory -Force $out | Out-Null
            # fixture opened-project は「開いた Project をそのまま使う」指定である。
            & $harness --project $projects[$name] --manim $manim --out $out --ffprobe $ffprobe --fixture opened-project `
                --product-math-group $mode --product-pure 1 2> (Join-Path $out 'harness.log') | Out-Null
            $code = $LASTEXITCODE
            $reportPath = Join-Path $out 'opened-project-product-pure.json'
            $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
            $ok = $code -eq 0 -and $report -and $report.export_success -eq $true -and $report.request.export_observer_supplied -eq $false
            if (-not $ok) { ++$failures }
            $rows["$name/$mode"] = [ordered]@{ exit_code = $code; output_sha256 = $report.output_sha256; frames = $report.export_frame_count
                decision = $report.report.decision; reason = $report.report.reason; graph = $report.report.graph
                grouped_clips = $report.report.grouped_clips; composed_frames = $report.report.composed_frames
                wall_seconds = $report.end_to_end_wall_seconds; ok = [bool]$ok
            }
            Write-Host ("{0,-26} {1,-3} exit={2} 判定={3} graph={4} frame={5} {6:n1} s SHA={7}" -f $name, $mode, $code, $report.report.decision,
                $report.report.graph, $report.export_frame_count, $report.end_to_end_wall_seconds, "$($report.output_sha256)".Substring(0, 16))
        }
        if ($rows["$name/on"].output_sha256 -ne $rows["$name/off"].output_sha256) { ++$failures; Write-Host "${name}: on と off の出力が違います" }
    }
    $expected = $rows['short-math-only-eligible/on'].graph -eq 'math-group' -and $rows['short-fallback-unknown/on'].graph -eq 'conventional' -and
    $rows['short-fallback-unknown/on'].decision -eq 'UNKNOWN'
    if (-not $expected) { ++$failures }
    $historical = @($HistoricalOutputs | ForEach-Object { [ordered]@{ file = $_; sha256 = (Get-Sha (Join-Path $repo $_)); bytes = (Get-Item (Join-Path $repo $_)).Length } })
    $v4Sha = @($historical | ForEach-Object { $_.sha256 } | Sort-Object -Unique)
    if ($v4Sha.Count -ne 1) { ++$failures; Write-Host '過去の全長の出力の SHA-256 が揃っていません' }
    $after = Test-Frozen '後'
    if (-not $after.match) { ++$failures }
    if ($failures -eq 0) {
        [ordered]@{ created = (Get-Date -Format 'o'); run = (Split-Path -Leaf $run); fixtures = $rows
            v4 = [ordered]@{ sha256 = $v4Sha[0]; frames = $TotalFrames; historical = $historical }
        } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $referencePath -Encoding utf8
    }
    Write-Host "不成立: $failures"
    if ($failures -gt 0) { exit 1 }
    exit 0
}

if ($Stage -eq 'session') {
    if (-not (Test-Path -LiteralPath $referencePath)) { throw "reference.json がありません (先に -Stage reference)" }
    $busy = @(Get-CimInstance Win32_Process -Filter "Name='ctest.exe' OR Name='mvm_math_score_export_parity.exe' OR Name='ninja.exe' OR Name='mvm.exe'")
    if ($busy.Count -gt 0) { throw "同時に動かさない process があります: $(@($busy | ForEach-Object { $_.Name }) -join ', ')" }
    $run = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-session-$Project" + $(if ($Label) { "-$Label" } else { '' }))
    New-Item -ItemType Directory -Force (Join-Path $run 'screens') | Out-Null
    $environment = 'SKIPPED'
    if (-not $SkipEnvironmentPreflight) {
        pwsh (Join-Path $PSScriptRoot 'gui-environment-preflight.ps1') -OutputFile (Join-Path $run 'gui-environment-before.json') *> (Join-Path $run 'preflight-before.log')
        $environment = if ($LASTEXITCODE -eq 0) { 'VALID' } else { 'ENVIRONMENT_INVALID' }
        if ($environment -ne 'VALID' -and -not $Drive) {
            [ordered]@{ stage = 'session'; project = $Project; environment_before = $environment } | ConvertTo-Json |
                Set-Content -LiteralPath (Join-Path $run 'session.json') -Encoding utf8
            Write-Host 'ENVIRONMENT_INVALID: monitor・session を確かめてください。起動しません。'
            exit 2
        }
    }
    if (-not ('Mvm.Accept.LineCapture' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Mvm.Accept {
// 子 process の出力を pipe から 1 行ずつ受け取り、受け取った時刻 (この script の単調な時計) を付ける。
// file には、受け取ったそのときに書く (後から読む file を経由しない)。
public sealed class LineCapture {
    private readonly Stopwatch clock;
    private readonly StreamWriter log;
    private readonly object gate = new object();
    public readonly ConcurrentQueue<Tuple<double, string>> Lines = new ConcurrentQueue<Tuple<double, string>>();
    public LineCapture(Stopwatch clock, string path) {
        this.clock = clock;
        log = new StreamWriter(path, false, new UTF8Encoding(false));
        log.AutoFlush = true;
    }
    public void AttachError(Process process) { process.ErrorDataReceived += OnData; }
    public void AttachOutput(Process process) { process.OutputDataReceived += OnData; }
    private void OnData(object sender, DataReceivedEventArgs e) {
        if (e.Data == null) return;
        double seconds = clock.Elapsed.TotalSeconds;
        lock (gate) { log.WriteLine(e.Data); }
        Lines.Enqueue(Tuple.Create(seconds, e.Data));
    }
    public void Close() { lock (gate) { log.Dispose(); } }
}

public static class Desktop {
    [StructLayout(LayoutKind.Sequential)] private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct LastInput { public uint Size; public uint Time; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] private static extern bool GetLastInputInfo(ref LastInput info);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static uint ForegroundProcess() {
        uint processId;
        GetWindowThreadProcessId(GetForegroundWindow(), out processId);
        return processId;
    }
    // 最後の keyboard・mouse の入力の時刻 (tick)。値が変われば、その間に入力があった。
    public static uint LastInputTick() {
        LastInput info = new LastInput();
        info.Size = 8;
        GetLastInputInfo(ref info);
        return info.Time;
    }
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
    [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr item);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr item);
    // window の内容を bitmap (HBITMAP) に描かせる。上に他の window が重なっていても、その window の内容を
    // 描く (PW_RENDERFULLCONTENT)。最小化されていれば、または描けなければ IntPtr.Zero。
    // 呼び出し側が PNG に保存し、DeleteObject で捨てる。
    public static IntPtr Capture(IntPtr window) {
        if (window == IntPtr.Zero || IsIconic(window)) return IntPtr.Zero;
        Rect rect;
        if (!GetWindowRect(window, out rect)) return IntPtr.Zero;
        int width = rect.Right - rect.Left, height = rect.Bottom - rect.Top;
        if (width < 100 || height < 100) return IntPtr.Zero;
        IntPtr screen = GetDC(IntPtr.Zero);
        IntPtr memory = CreateCompatibleDC(screen);
        IntPtr bitmap = CreateCompatibleBitmap(screen, width, height);
        IntPtr previous = SelectObject(memory, bitmap);
        bool printed = PrintWindow(window, memory, 2);
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(IntPtr.Zero, screen);
        if (!printed) { DeleteObject(bitmap); return IntPtr.Zero; }
        return bitmap;
    }
}
}
'@
    }
    Add-Type -AssemblyName System.Drawing
    [Mvm.Accept.Desktop]::SetProcessDPIAware() | Out-Null
    $settingsBefore = Get-SettingsSnapshot
    $projectFile = $projects[$Project]
    $log = Join-Path $run 'mvm-stderr.log'
    $processor = (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average
    $launchFile = if ($ApplicationOverride) { $ApplicationOverride } else { $application }
    $launchArguments = if ($ApplicationOverride) { @($ApplicationArgumentsOverride -split '\|' | Where-Object { $_ }) }
    else { @('--project', $projectFile, '--manim-executable', $manim) }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $info = [System.Diagnostics.ProcessStartInfo]::new($launchFile)
    foreach ($argument in $launchArguments) { $info.ArgumentList.Add($argument) }
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    $info.RedirectStandardOutput = $true
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $repo
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    $errorCapture = [Mvm.Accept.LineCapture]::new($clock, $log)
    $outputCapture = [Mvm.Accept.LineCapture]::new($clock, (Join-Path $run 'mvm-stdout.log'))
    $errorCapture.AttachError($process)
    $outputCapture.AttachOutput($process)
    [void]$process.Start()
    $process.BeginErrorReadLine()
    $process.BeginOutputReadLine()
    $launchedPath = $null
    for ($attempt = 0; $attempt -lt 50 -and -not $launchedPath; ++$attempt) {
        try { $process.Refresh(); $launchedPath = $process.MainModule.FileName } catch { Start-Sleep -Milliseconds 100 }
    }
    if (-not $launchedPath) { throw '起動した process の実行 file を確かめられません' }
    Write-Host "起動しました: $launchedPath (PID $($process.Id))"
    Write-Host "Project: $projectFile"
    $driver = $null
    $driveResult = Join-Path $run 'drive.json'
    if ($Drive) {
        $scenario = @{ 'short-fallback-unknown' = 'A'; 'short-math-only-eligible' = 'B'; 'v4' = 'C' }[$Project]
        $driverArguments = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'math-group-product-p36c2-drive.ps1'), '-ProcessId', "$($process.Id)",
            '-Scenario', $scenario, '-ManualOut', (Join-Path $root 'manual-out'), '-ResultPath', $driveResult)
        $driver = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $driverArguments -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $run 'drive.log') -RedirectStandardError (Join-Path $run 'drive-error.log')
        Write-Host "操作は driver が行います (場面 $scenario、PID $($driver.Id))。この PC は使ってかまいません (mouse と keyboard は使いません)。"
    } else {
        Write-Host 'この window は閉じないでください。mvm の window を閉じると、記録を書いて終わります。'
        Write-Host '書き出しの間は、keyboard と mouse に触らないでください (触ると記録に残り、全長の書き出しの検収は不成立になります)。'
    }

    function Get-LoadedModules {
        $rows = @()
        try { $process.Refresh(); $modules = @($process.Modules) } catch { return $rows }
        foreach ($module in $modules) {
            $path = $module.FileName
            if ($path -like "$env:SystemRoot\*") { continue }
            $sha = Get-Sha $path
            # repository と MSYS2 の外から入ってくる DLL (他の software が全 process へ差し込む hook など) は、
            # 固定の対象にできない。別に記録する。
            $foreign = -not ($path -like "$repo\*" -or $path -like 'C:\msys64\*')
            $rows += [pscustomobject]@{ path = $path; sha256 = $sha; frozen = $frozen.sha256.ContainsKey($path); foreign = $foreign
                matches_freeze = ($frozen.sha256.ContainsKey($path) -and $frozen.sha256[$path] -eq $sha)
            }
        }
        $rows
    }

    $events = @(); $exports = @(); $current = $null; $moduleSets = @(); $watchdogFired = $false
    $nextShot = 0.0; $shots = 0; $modulesAtStart = $false; $lastInputTick = 0
    while ($true) {
        $exited = $process.HasExited
        if ($exited) { $process.WaitForExit() } # 出力の非同期の読み取りが終わるまで待つ
        $item = $null
        while ($errorCapture.Lines.TryDequeue([ref]$item)) {
            # 時刻は、行を pipe から受け取ったときのもの (この loop が処理したときではない)。
            $now = $item.Item1
            $line = $item.Item2
            $parsed = ConvertFrom-ExportLine $line
            if ($null -eq $parsed) { continue }
            $events += [pscustomobject]@{ received_seconds = $now; line = $line }
            if ($parsed.kind -eq 'start') {
                $current = [ordered]@{ output = $parsed.output; selected = $parsed.selected; start_seconds = $now; start_wall = (Get-Date -Format 'o')
                    quiet = [ordered]@{ samples = 0; input_changes = 0; foreground_other = 0 }
                }
                $lastInputTick = [Mvm.Accept.Desktop]::LastInputTick()
                Write-Host ("[{0,7:n1} s] 書き出しの開始: {1} (選択 {2})" -f $now, $parsed.output, $parsed.selected)
            } elseif ($current) {
                $current.finish_seconds = $now
                $current.elapsed_seconds = $now - $current.start_seconds
                $current.record = $parsed
                $directory = Split-Path -Parent $current.output
                $current.final_file_exists = Test-Path -LiteralPath $current.output
                $current.temporary_file_exists = Test-Path -LiteralPath ($current.output + '.mvmtmp')
                $current.media_left = @(Get-MediaLeft $directory)
                $exports += [pscustomobject]$current
                Write-Host ("[{0,7:n1} s] 書き出しの結果: {1}、{2:n1} 秒、outcome={3} graph={4} 合成={5} 失敗={6}、書き出し中の入力 {7} 回・他の window が前面 {8}/{9}" -f $now,
                    $parsed.result, $current.elapsed_seconds, $parsed.outcome, $parsed.graph, $parsed.composed, $parsed.errors,
                    $current.quiet.input_changes, $current.quiet.foreground_other, $current.quiet.samples)
                $moduleSets += [pscustomobject]@{ when = "export-$($exports.Count)"; modules = @(Get-LoadedModules) }
                $current = $null
            }
        }
        if ($exited) { break }
        $now = $clock.Elapsed.TotalSeconds
        if ($current) {
            # 書き出しの間の、利用者の入力と前面の window (記録するだけで、操作はしない)。
            ++$current.quiet.samples
            $tick = [Mvm.Accept.Desktop]::LastInputTick()
            if ($tick -ne $lastInputTick) { ++$current.quiet.input_changes; $lastInputTick = $tick }
            if ([Mvm.Accept.Desktop]::ForegroundProcess() -ne $process.Id) { ++$current.quiet.foreground_other }
        }
        if (-not $modulesAtStart -and $now -ge 8) {
            $modulesAtStart = $true
            $moduleSets += [pscustomobject]@{ when = 'startup'; modules = @(Get-LoadedModules) }
        }
        if ($current -and -not $watchdogFired -and $now - $current.start_seconds -gt $WatchdogSeconds) {
            $watchdogFired = $true
            Write-Host ("[{0,7:n1} s] watchdog: 書き出しが {1} 秒を超えました (記録するだけで、止めません)" -f $now, $WatchdogSeconds)
        }
        # mvm の window の内容を保存する (操作はしない)。書き出しの間は間隔を空ける。
        if ($ScreenshotSeconds -gt 0 -and $now -ge $nextShot) {
            $nextShot = $now + $(if ($current -and $now - $current.start_seconds -gt 20) { 10 * $ScreenshotSeconds } else { $ScreenshotSeconds })
            try {
                $process.Refresh()
                $bitmap = [Mvm.Accept.Desktop]::Capture($process.MainWindowHandle)
                if ($bitmap -ne [IntPtr]::Zero) {
                    try {
                        $image = [System.Drawing.Image]::FromHbitmap($bitmap)
                        $image.Save((Join-Path $run ('screens\{0:d5}-{1:d6}.png' -f $shots, [int]($now * 10))), [System.Drawing.Imaging.ImageFormat]::Png)
                        $image.Dispose()
                        ++$shots
                    } finally { [void][Mvm.Accept.Desktop]::DeleteObject($bitmap) }
                }
            } catch { Write-Verbose "$_" }
        }
        Start-Sleep -Milliseconds 40
    }
    $errorCapture.Close()
    $outputCapture.Close()
    $driveRecord = $null
    if ($driver) {
        if (-not $driver.WaitForExit(60000)) { try { $driver.Kill() } catch { Write-Verbose "$_" } }
        $driveRecord = if (Test-Path -LiteralPath $driveResult) { Get-Content -Raw $driveResult | ConvertFrom-Json } else { [ordered]@{ status = 'no-result'; all_ok = $false } }
    }
    $exitCode = $process.ExitCode
    $settingsAfter = Get-SettingsSnapshot
    $settingsChanged = @(@($settingsBefore.Keys) + @($settingsAfter.Keys) | Sort-Object -Unique | Where-Object { $settingsBefore[$_] -ne $settingsAfter[$_] })
    $allModules = @($moduleSets | ForEach-Object { $_.modules } | Sort-Object path -Unique)
    $unexpected = @($allModules | Where-Object { -not $_.matches_freeze -and -not $_.foreign })
    $foreignModules = @($allModules | Where-Object { $_.foreign })
    $after = Test-Frozen '後'
    [ordered]@{
        stage = 'session'; project = $Project; project_file = $projectFile; label = $Label; started = (Get-Date).AddSeconds(-$clock.Elapsed.TotalSeconds).ToString('o')
        capture = 'pipe'; application_override = [bool]$ApplicationOverride; driven = [bool]$Drive; drive = $driveRecord
        environment_before = $environment; processor_load_percent_at_launch = $processor
        application = $launchedPath; application_sha256 = (Get-Sha $launchedPath)
        application_matches_freeze = ($launchedPath -eq $frozen.application -and (Get-Sha $launchedPath) -eq $frozen.application_sha256)
        process_id = $process.Id; exit_code = $exitCode; session_seconds = $clock.Elapsed.TotalSeconds
        watchdog_seconds = $WatchdogSeconds; watchdog_fired = $watchdogFired; budget_seconds = $BudgetSeconds
        exports = $exports; unfinished_export = $current; events = $events
        loaded_module_count = $allModules.Count; loaded_modules_not_matching_freeze = $unexpected
        foreign_modules = $foreignModules; loaded_modules = $allModules
        settings_changed_keys = $settingsChanged; settings_before = $settingsBefore; settings_after = $settingsAfter
        screenshots = $shots; identity_before = $before; identity_after = $after
    } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $run 'session.json') -Encoding utf8
    Write-Host ("終了: 書き出し {0} 回、読み込まれた DLL {1} (freeze と違うもの {2}、外から差し込まれたもの {5})、設定の変化 {3} 件、画面 {4} 枚" -f $exports.Count,
        $allModules.Count, $unexpected.Count, $settingsChanged.Count, $shots, $foreignModules.Count)
    Write-Host "記録: $run"
    Write-Host "次 (この行をそのまま実行する): pwsh scripts/math-group-product-p36c2-accept.ps1 -Stage verify -OutputDirectory $OutputDirectory -Session `"$(Split-Path -Leaf $run)`""
    exit 0
}

# --- verify ---
$run = Join-Path $root $Session
$sessionPath = Join-Path $run 'session.json'
if (-not $Session -or -not (Test-Path -LiteralPath $sessionPath)) { throw "session の記録がありません: $sessionPath" }
$record = Get-Content -Raw $sessionPath | ConvertFrom-Json
# script の自己試験が作った記録 (人が操作していない)。検収の結果として扱わない。
$synthetic = @($record.PSObject.Properties.Name) -contains 'synthetic_selftest' -and [bool]$record.synthetic_selftest
foreach ($required in @('project', 'exports', 'application_matches_freeze', 'loaded_modules_not_matching_freeze', 'loaded_module_count',
        'environment_before', 'identity_before', 'identity_after')) {
    if (@($record.PSObject.Properties.Name) -notcontains $required) { throw "session の記録に項目がありません: $required" }
}
$reference = Get-Content -Raw $referencePath | ConvertFrom-Json

function Get-MathGroupSettingFinding($Record) {
    $names = @($Record.PSObject.Properties.Name)
    if ($names -notcontains 'settings_before' -or $names -notcontains 'settings_after') {
        return [pscustomobject]@{ ok = $false; reason = '設定の前後の写しが記録にありません'; math_group_keys = @(); changed_keys = @(); other_export_keys = @() }
    }
    $beforeMap = @{}; $afterMap = @{}
    if ($Record.settings_before) { foreach ($property in $Record.settings_before.PSObject.Properties) { $beforeMap[$property.Name] = "$($property.Value)" } }
    if ($Record.settings_after) { foreach ($property in $Record.settings_after.PSObject.Properties) { $afterMap[$property.Name] = "$($property.Value)" } }
    $pattern = '(?i)math[\s_\-]?group|accelerat|高速化'
    $changed = @(@($beforeMap.Keys) + @($afterMap.Keys) | Sort-Object -Unique | Where-Object { $beforeMap[$_] -ne $afterMap[$_] })
    $mathGroupKeys = @(@($afterMap.Keys) + $changed | Sort-Object -Unique | Where-Object { $_ -match $pattern })
    $otherExport = @($afterMap.Keys | Where-Object { $_ -match '(?i)export' -and $_ -notmatch $pattern } | Sort-Object)
    [pscustomobject]@{ ok = ($mathGroupKeys.Count -eq 0); reason = ''; math_group_keys = $mathGroupKeys; changed_keys = $changed
        other_export_keys = $otherExport
    }
}

function Get-X264([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $text = [Text.Encoding]::ASCII.GetString($bytes, 0, [math]::Min($bytes.Length, 4MB))
    if ($text -match 'x264 - core[^\x00]*') { $Matches[0] } else { $null }
}

function Test-Output([string]$Mp4, [int]$Frames, [string]$Name) {
    try { Test-OutputUnchecked $Mp4 $Frames $Name }
    catch { [ordered]@{ valid = $false; error = "出力を調べられません: $($_.Exception.Message)"; x264_settings = $null } }
}

function Test-OutputUnchecked([string]$Mp4, [int]$Frames, [string]$Name) {
    $stream = (& $ffprobe -v error -select_streams v:0 -count_frames -show_entries `
            'stream=codec_name,profile,level,pix_fmt,width,height,r_frame_rate,avg_frame_rate,time_base,nb_frames,nb_read_frames,duration,start_time' `
            -of json $Mp4 | Out-String | ConvertFrom-Json).streams[0]
    $packets = @(& $ffprobe -v error -select_streams v:0 -show_entries packet=pts,dts -of csv=p=0 $Mp4 | Where-Object { $_ } | ForEach-Object {
            $columns = $_ -split ','; [pscustomobject]@{ pts = [long]$columns[0]; dts = [long]$columns[1] } })
    $framePts = @(& $ffprobe -v error -select_streams v:0 -show_entries frame=pts -of csv=p=0 $Mp4 | Where-Object { $_ } | ForEach-Object { [long]($_ -replace ',', '') })
    $base = $stream.time_base -split '/'; $rate = $stream.r_frame_rate -split '/'
    $step = ([long]$base[1] * [long]$rate[1]) / ([long]$base[0] * [long]$rate[0])
    $ordered = $framePts.Count -eq $Frames
    for ($index = 0; $index -lt $framePts.Count; ++$index) { if ($framePts[$index] -ne $index * $step) { $ordered = $false; break } }
    $packetSet = @($packets | ForEach-Object { $_.pts } | Sort-Object -Unique)
    $cover = $packetSet.Count -eq $Frames -and $packets.Count -eq $Frames -and $packetSet[0] -eq 0 -and $packetSet[-1] -eq ($Frames - 1) * $step
    $dtsOk = @($packets | Where-Object { $_.dts -gt $_.pts }).Count -eq 0
    for ($index = 1; $index -lt $packets.Count; ++$index) { if ($packets[$index].dts -le $packets[$index - 1].dts) { $dtsOk = $false; break } }
    $decodeLog = Join-Path $run "ffmpeg-decode-$Name.log"
    & $ffmpeg -hide_banner -nostdin -v error -xerror -stats -i $Mp4 -map 0:v:0 -f null - 2> $decodeLog
    $decodeExit = $LASTEXITCODE
    $decodeText = Get-Content -Raw $decodeLog
    $decoded = if ($decodeText -match '(?s).*frame=\s*(\d+)') { [int]$Matches[1] } else { -1 }
    $decodeErrors = @((Get-Content $decodeLog) | Where-Object { $_ -and $_ -notmatch 'frame=\s*\d+' }).Count
    $valid = $stream.codec_name -eq 'h264' -and $stream.profile -eq 'High' -and $stream.pix_fmt -eq 'yuv420p' -and $stream.width -eq 1920 -and
    $stream.height -eq 1080 -and $stream.r_frame_rate -eq '60/1' -and $stream.avg_frame_rate -eq '60/1' -and [int]$stream.nb_frames -eq $Frames -and
    [int]$stream.nb_read_frames -eq $Frames -and $ordered -and $cover -and $dtsOk -and $decodeExit -eq 0 -and $decoded -eq $Frames -and
    $decodeErrors -eq 0 -and [math]::Abs([double]$stream.duration - $Frames / 60.0) -lt 1e-3 -and [double]$stream.start_time -eq 0
    [ordered]@{ valid = [bool]$valid; stream = $stream; frame_pts_count = $framePts.Count; frames_in_presentation_order = $ordered
        packet_count = $packets.Count; packets_cover_all_frames = $cover; dts_valid = $dtsOk; decode_exit = $decodeExit
        decoded_frames = $decoded; decode_error_lines = $decodeErrors; x264_settings = (Get-X264 $Mp4)
    }
}

$rows = @()
$index = 0
foreach ($export in @($record.exports)) {
    ++$index
    $entry = $export.record
    $row = [ordered]@{ index = $index; output = $export.output; selected = $export.selected; result = $entry.result; elapsed_seconds = $export.elapsed_seconds
        outcome = $entry.outcome; decision = $entry.decision; reason = $entry.reason; graph = $entry.graph; grouped_clips = [long]$entry.grouped_clips
        affine = [long]$entry.affine; composed = [long]$entry.composed; callbacks = [long]$entry.callbacks; errors = [long]$entry.errors
        raster_bytes = [long]$entry.raster_bytes; live_transitions = [long]$entry.live_transitions; active_callbacks = [long]$entry.active_callbacks
        final_file_exists_now = (Test-Path -LiteralPath $export.output); temporary_file_exists_now = (Test-Path -LiteralPath ($export.output + '.mvmtmp'))
        final_file_existed_at_result = $export.final_file_exists; temporary_file_existed_at_result = $export.temporary_file_exists
        sha256 = $null; bytes = 0; check = $null; sha256_matches_reference = $false; x264_settings_same_as_reference = $false
        within_budget = $false; frames_match_reference = $false
        quiet_recorded = $false; quiet_samples = 0; input_changes = -1; foreground_other = -1
    }
    if (@($export.PSObject.Properties.Name) -contains 'quiet' -and $export.quiet) {
        $row.quiet_recorded = $true
        $row.quiet_samples = [long]$export.quiet.samples
        $row.input_changes = [long]$export.quiet.input_changes
        $row.foreground_other = [long]$export.quiet.foreground_other
    }
    if ($entry.result -eq '成功' -and $row.final_file_exists_now) {
        $row.sha256 = Get-Sha $export.output
        $row.bytes = (Get-Item -LiteralPath $export.output).Length
        if ($record.project -eq 'v4') {
            $row.check = Test-Output $export.output $TotalFrames "export-$index"
            $row.sha256_matches_reference = $row.sha256 -eq $reference.v4.sha256
            $row.x264_settings_same_as_reference = [bool]($row.check.x264_settings -and
                $row.check.x264_settings -eq (Get-X264 (Join-Path $repo $HistoricalOutputs[0])))
            $row.within_budget = $export.elapsed_seconds -lt $BudgetSeconds
        } else {
            $wanted = $reference.fixtures."$($record.project)/on"
            $row.sha256_matches_reference = $row.sha256 -eq $wanted.output_sha256
            $row.frames_match_reference = [long]$entry.frames -eq [long]$wanted.frames
        }
    }
    # 成功でない書き出しは、最終 file も一時 file も残さない (既にあった最終 file を置き換えない場合を除く)。
    $row.clean = if ($entry.result -eq '成功') { $row.final_file_exists_now -and -not $row.temporary_file_exists_now }
    else { -not $row.temporary_file_exists_now -and -not $export.temporary_file_exists -and @($export.media_left).Count -eq 0 }
    $rows += [pscustomobject]$row
}
$verdicts = [ordered]@{}
$verdicts.application_is_frozen_executable = [bool]$record.application_matches_freeze
$verdicts.runtime_modules_match_freeze = @($record.loaded_modules_not_matching_freeze).Count -eq 0 -and $record.loaded_module_count -gt 0
$verdicts.environment_valid = $record.environment_before -eq 'VALID'
$verdicts.identity_frozen_during_session = [bool]$record.identity_before.match -and [bool]$record.identity_after.match
# 数式の高速化の選択が、アプリの設定に保存されていないこと。書き出しに関する他の設定 (既にあるもの、
# 今後足されるもの) は、この判定の対象にしない。名前で見るもの: 数式 group・高速化を指す key。
# 加えて、session の間に増えた・変わった key を全部記録し、その中に該当する名前が無いことを見る。
$settings = Get-MathGroupSettingFinding $record
$verdicts.math_group_selection_not_persisted = [bool]$settings.ok
$verdicts.exports_clean = @($rows | Where-Object { -not $_.clean }).Count -eq 0 -and $rows.Count -gt 0
if ($record.project -eq 'v4') {
    # 検収の対象: 高速化を選んで成功した書き出し (1 回目のもの)。
    $target = @($rows | Where-Object { $_.selected -and $_.result -eq '成功' }) | Select-Object -First 1
    $verdicts.full_length_completed = [bool]$target
    if ($target) {
        $verdicts.math_group_selected = $target.outcome -eq 'used' -and $target.decision -eq 'ELIGIBLE' -and $target.graph -eq 'math-group' -and
        $target.grouped_clips -gt 0 -and $target.composed -gt 0
        $verdicts.no_runtime_errors = $target.errors -eq 0
        $verdicts.within_budget = [bool]$target.within_budget
        $verdicts.output_valid = [bool]($target.check -and $target.check.valid)
        $verdicts.encoder_configuration = [bool]$target.x264_settings_same_as_reference
        $verdicts.sha256_matches_reference = [bool]$target.sha256_matches_reference
        $verdicts.teardown = $target.live_transitions -eq 0 -and $target.active_callbacks -eq 0
        # 書き出しの間に、keyboard・mouse の入力が無く、mvm の window が前面のままだったこと。
        $verdicts.quiet_during_export = [bool]$target.quiet_recorded -and $target.input_changes -eq 0 -and
        $target.foreground_other -eq 0 -and $target.quiet_samples -gt 0
    }
} else {
    $successes = @($rows | Where-Object { $_.result -eq '成功' })
    $verdicts.short_outputs_match_reference = $successes.Count -gt 0 -and
    @($successes | Where-Object { -not $_.sha256_matches_reference -or -not $_.frames_match_reference }).Count -eq 0
}
$after = Test-Frozen '後'
$verdicts.identity_frozen_at_verify = [bool]$after.match
# driver が操作した session: 操作の各段 (人の checklist の代わり) が全部成立したこと。
# 環境の検査と、書き出しの間の利用者の操作は、判定ではなく参考として残す (driver は画面の表示も mouse・
# keyboard も使わない。利用者が PC を使っていれば書き出しは遅くなる方向で、時間の基準は別に判定する)。
$driven = @($record.PSObject.Properties.Name) -contains 'driven' -and [bool]$record.driven
$information = [ordered]@{}
if ($driven) {
    $verdicts.automated_gui_operations = [bool]($record.drive -and $record.drive.all_ok)
    foreach ($key in @('environment_valid', 'quiet_during_export')) {
        if ($verdicts.Contains($key)) { $information[$key] = $verdicts[$key]; $verdicts.Remove($key) }
    }
}
$allOk = @($verdicts.Values | Where-Object { -not $_ }).Count -eq 0
[ordered]@{ stage = 'verify'; session = $Session; project = $record.project; synthetic_selftest = $synthetic
    human_acceptance = (-not $synthetic -and -not $driven); driven = $driven; all_ok = $allOk; verdicts = $verdicts
    information = $information; drive_steps = $(if ($driven -and $record.drive) { $record.drive.steps } else { @() }); settings = $settings; exports = $rows
    reference = $(if ($record.project -eq 'v4') { $reference.v4 } else { $reference.fixtures })
    note = 'この検査は、符号化された MP4 と session の記録を見る。encoder の直前の RGBA は比べていない。人が行った操作と目で見た確認は、operator-checklist.md と screens に残す。'
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $run 'verify.json') -Encoding utf8
foreach ($row in $rows) {
    Write-Host ("書き出し {0}: 選択={1} 結果={2} {3:n1} 秒 outcome={4} graph={5} まとめた={6} affine={7} 合成={8} 失敗={9} 後始末={10}" -f $row.index, $row.selected,
        $row.result, $row.elapsed_seconds, $row.outcome, $row.graph, $row.grouped_clips, $row.affine, $row.composed, $row.errors, $row.clean)
}
foreach ($key in $verdicts.Keys) { Write-Host ("  {0,-36} {1}" -f $key, $verdicts[$key]) }
if ($synthetic) { Write-Host 'SELFTEST: script の自己試験の記録です。人による検収ではありません。' }
Write-Host "結果: $(Join-Path $run 'verify.json')"
if (-not $allOk) { exit 1 }
