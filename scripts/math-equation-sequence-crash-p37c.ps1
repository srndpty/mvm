<#
.SYNOPSIS
    P3-7C: math_equation_sequence_product_ui の過去の SegFault を、dump を採れる形で事前に決めた回数だけ
    実行し、1 回ごとに分類する。

.DESCRIPTION
    CTest と MSYS の shell は SEM_NOGPFAULTERRORBOX を子へ受け継がせるので、CTest の下で落ちても
    WER は呼ばれず、dump も Application の event log も残らない (2026-10-15 に probe で実測。
    docs/math-equation-sequence-crash-p37c.md)。そこで、CTest が登録している command と環境を
    `ctest --show-only=json-v1` から取り、scripts/lib/wer-launch.c の起動器で error mode を 0 にして
    直接起動する。debugger は付けない (heap と時間を変えない)。WER の LocalDumps は機械に既にある
    既定の設定 (%LOCALAPPDATA%\CrashDumps) を使い、registry は変えない。

    回数と内容は実行の前に固定する (結果を見て増減しない)。build はしない。
      Control       本物の ctest で 1 回 (CTest の経路での合否。dump は採れない)
      QuietFull     起動器で最後まで 5 回 (CTest と同じ TIMEOUT 300 秒)
      QuietWindow   起動器で起動直後の 10 秒だけを 60 回 (生き延びたら job ごと止める。合格ではない)
      StressWindow  QuietWindow と同じ 60 回を、disk 全体の走査 (元の失敗の時に動いていた負荷) の下で
      StressFull    QuietFull と同じ 3 回を、同じ負荷の下で
    負荷は disk の metadata の走査だけで、GPU の試験を並行に動かさない。

    各段の前後に scripts/gui-environment-preflight.ps1 を実行し、段の間は display-power lease を持つ。
    段の後の preflight が VALID でなければ、その段の run は GUI_ENVIRONMENT_INVALID として数え、
    合格と混ぜない。試験の binary と主要な DLL の SHA-256 を前後で比べ、変わっていたら全体を無効にする。

    既存の出力先は上書きしない。再現しなくても「解決」とは扱わない。

.EXAMPLE
    pwsh scripts/math-equation-sequence-crash-p37c.ps1
    pwsh scripts/math-equation-sequence-crash-p37c.ps1 -Stages QuietWindow
#>
[CmdletBinding()]
param(
    [ValidateSet('Control', 'QuietFull', 'QuietWindow', 'StressWindow', 'StressFull')]
    [string[]]$Stages = @('Control', 'QuietFull', 'QuietWindow', 'StressWindow', 'StressFull'),
    [string]$OutputDirectory = 'build/math-equation-sequence-crash-p37c-20261015',
    # 0 より大きければ、harness の試走として各段を最大この回数にし、出力を別の場所 (-trial) へ書く。
    # 試走の run は cohort の回数に数えない。
    [int]$Trial = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64'
$build = Join-Path $repo 'build\ucrt64-release'
$testName = 'math_equation_sequence_product_ui'
$plan = [ordered]@{ Control = 1; QuietFull = 5; QuietWindow = 60; StressWindow = 60; StressFull = 3 }
$windowMs = 10000
$timeoutMs = 300000
$crashDumps = Join-Path $env:LOCALAPPDATA 'CrashDumps'
$crashCodes = @{
    '0xC0000005' = 'ACCESS_VIOLATION'; '0xC0000374' = 'HEAP_CORRUPTION'; '0xC0000409' = 'FAST_FAIL'
    '0xC000001D' = 'ILLEGAL_INSTRUCTION'; '0xC00000FD' = 'STACK_OVERFLOW'; '0x80000003' = 'BREAKPOINT'
    '0xC0000602' = 'FAIL_FAST'; '0x40000015' = 'ABORT'
}

$baseOutput = $OutputDirectory
if ($Trial -gt 0) {
    foreach ($k in @($plan.Keys)) { $plan[$k] = [math]::Min($plan[$k], $Trial) }
    $OutputDirectory += '-trial'
}
$out = Join-Path (Join-Path $repo $OutputDirectory) (Get-Date -Format 'yyyyMMdd-HHmmss')
if (Test-Path -LiteralPath $out) { throw "出力先が既にあります: $out" }
New-Item -ItemType Directory -Force $out | Out-Null
$env:PATH = "$ucrt\bin;$env:PATH"
. (Join-Path $PSScriptRoot 'lib\test-display-lease.ps1')

if (-not ('Mvm.Tests.P37cIdle' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Mvm.Tests {
    public static class P37cIdle {
        [StructLayout(LayoutKind.Sequential)] struct LastInput { public uint Size; public uint Time; }
        [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LastInput info);
        public static uint LastInputTick() { var i = new LastInput(); i.Size = 8; GetLastInputInfo(ref i); return i.Time; }
    }
}
'@
}

function Write-Json($Value, [string]$Name) {
    $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out $Name) -Encoding utf8
}

# --- 起動器 (repo の source から、この run の中に build する) ---
$tools = Join-Path $out 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
$launcherSource = Join-Path $PSScriptRoot 'lib\wer-launch.c'
$launcher = Join-Path $tools 'wer-launch.exe'
& "$ucrt\bin\gcc.exe" -O2 -municode -Wall -o $launcher $launcherSource
if ($LASTEXITCODE -ne 0) { throw '起動器を build できません' }

# --- CTest が登録している command と環境 ---
$inventory = & "$ucrt\bin\ctest.exe" --test-dir $build --show-only=json-v1 -R "^$testName`$" | Out-String
Set-Content -LiteralPath (Join-Path $out 'ctest-show-only.json') -Value $inventory -Encoding utf8
$tests = @(($inventory | ConvertFrom-Json).tests)
if ($tests.Count -ne 1) { throw "CTest に $testName が 1 件だけ登録されていません ($($tests.Count) 件)" }
$registered = $tests[0]
$command = @($registered.command)
$testEnv = [ordered]@{}
$props = @{}
foreach ($p in @($registered.properties)) { $props[$p.name] = $p.value }
foreach ($kv in @($props['ENVIRONMENT'] | Where-Object { $_ })) {
    $k, $v = $kv -split '=', 2; $testEnv[$k] = $v
}
foreach ($kv in @($props['ENVIRONMENT_MODIFICATION'] | Where-Object { $_ })) {
    $k, $rest = $kv -split '=', 2
    $op, $v = $rest -split ':', 2
    if ($op -ne 'set') { throw "解釈しない ENVIRONMENT_MODIFICATION です (fail-closed): $kv" }
    $testEnv[$k] = $v
}
if ([int]$props['TIMEOUT'] * 1000 -ne $timeoutMs) { throw "CTest の TIMEOUT が想定 (300) と違います: $($props['TIMEOUT'])" }
$exe = $command[0]

# --- identity ---
$watched = @($exe) + @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Qml.dll', 'Qt6Quick.dll', 'Qt6QuickControls2.dll',
    'libmlt-7.dll', 'avcodec-62.dll', 'avformat-62.dll', 'avutil-60.dll', 'swscale-9.dll',
    'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll' | ForEach-Object { Join-Path "$ucrt\bin" $_ })
function Get-BinaryHashes { $h = [ordered]@{}; foreach ($f in $watched) { $h[$f] = (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash }; $h }
$pacman = 'C:\msys64\usr\bin\pacman.exe'
$packages = @('qt6-base', 'qt6-declarative', 'mlt', 'ffmpeg', 'gcc') | ForEach-Object {
    (& $pacman -Q "mingw-w64-ucrt-x86_64-$_" 2>&1 | Out-String).Trim()
}
$gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) driver=$($_.DriverVersion) date=$($_.DriverDate)" })
$identity = [ordered]@{
    started = (Get-Date -Format o)
    git_head = (git -C $repo rev-parse HEAD)
    git_status = @(git -C $repo status --porcelain)
    command = $command
    environment = $testEnv
    launcher_source_sha256 = (Get-FileHash -Algorithm SHA256 $launcherSource).Hash
    launcher_sha256 = (Get-FileHash -Algorithm SHA256 $launcher).Hash
    exe_last_write = (Get-Item $exe).LastWriteTime.ToString('o')
    binaries = Get-BinaryHashes
    packages = $packages
    gpu = $gpu
    os = (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "$($_.Caption) $($_.Version)" })
    plan = $plan; window_ms = $windowMs; timeout_ms = $timeoutMs; stages = $Stages
}
Write-Json $identity 'identity.json'

# --- 環境の前提 ---
function Invoke-Preflight([string]$Name) {
    $file = Join-Path $out "preflight-$Name.json"
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'gui-environment-preflight.ps1') -OutputFile $file | Out-Null
    return $LASTEXITCODE -eq 0
}

# --- 負荷 (disk 全体の metadata の走査) ---
function Start-DiskScan {
    $script = 'while ($true) { Get-ChildItem C:\ -Recurse -Force -ErrorAction SilentlyContinue | Out-Null }'
    Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', $script) -WindowStyle Hidden -PassThru
}

function Find-NewDump([int]$ProcessId) {
    $name = [IO.Path]::GetFileName($exe) + ".$ProcessId.dmp"
    $deadline = (Get-Date).AddSeconds(30)
    do {
        $path = Join-Path $crashDumps $name
        if (Test-Path -LiteralPath $path) {
            # WER が書き終えるまで待つ (大きさが変わらなくなるまで)。
            $last = -1
            while ((Get-Item -LiteralPath $path).Length -ne $last) { $last = (Get-Item -LiteralPath $path).Length; Start-Sleep -Milliseconds 500 }
            return $path
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    return $null
}

function Get-Phase([string]$Directory, [datetime]$Start) {
    if (-not $Directory -or -not (Test-Path -LiteralPath $Directory)) { return [ordered]@{ reached = 'BEFORE_EVIDENCE_DIRECTORY'; files = @() } }
    $files = @(Get-ChildItem -LiteralPath $Directory -File | Sort-Object LastWriteTime | ForEach-Object {
        [ordered]@{ name = $_.Name; at_s = [math]::Round(($_.LastWriteTime - $Start).TotalSeconds, 2) } })
    $names = @($files | ForEach-Object { $_.name })
    $reached = if ($names -contains 'results.json') { 'RESULTS_WRITTEN' }
    elseif (@($names | Where-Object { $_ -like '*.png' }).Count -gt 0) { "QUADRATIC_UI ($(@($names | Where-Object { $_ -like '*.png' }).Count) png)" }
    elseif ($names -contains 'quadratic.mvm') { 'QUADRATIC_OPEN (window 作成前後, png 0)' }
    elseif ($names -contains 'recovery-guard.mvm') { 'RECOVERY_GUARD' }
    else { 'EVIDENCE_DIRECTORY_EMPTY' }
    [ordered]@{ reached = $reached; files = $files }
}

function Get-Classification($Result, [string]$StdErr, [bool]$WindowArm) {
    $code = $Result.exit_code
    if ($Result.outcome -eq 'window_survived') { return 'SURVIVED_WINDOW' }
    if ($Result.outcome -eq 'timeout') { return 'TIMEOUT_OR_HANG' }
    if ($crashCodes.ContainsKey($code)) { return "NATIVE_CRASH ($($crashCodes[$code]))" }
    if ($code -eq '0x00000000') { return $(if ($WindowArm) { 'PASS_BEFORE_WINDOW_END' } else { 'PASS' }) }
    if ($code -eq '0x00000001') { return 'ASSERTION_OR_TEST_FAILURE' }
    if ($code -eq '0x00000004') {
        if ($StdErr -match '利用者|操作|前面') { return 'EXTERNAL_INTERFERENCE (試験が自分で検出)' }
        return 'HARNESS_PROTOCOL_INVALID'
    }
    if ($code -in '0x00000002', '0x00000003') { return 'HARNESS_PROTOCOL_INVALID' }
    return 'UNKNOWN'
}

function Invoke-Run([string]$Stage, [int]$Index, [bool]$WindowArm) {
    $runDir = Join-Path $out ("{0}\{1:D3}" -f $Stage, $Index)
    New-Item -ItemType Directory -Force $runDir | Out-Null
    # CTest の command をそのまま使う (証拠の親も同じ)。親を長い path へ替えると、数式の作業 directory が
    # 作れず試験が落ちた (試走 20261015-202740、harness 側の不具合)。この run の directory は、--scratch が
    # 作る UUID の子を前後の差で特定する。
    $arguments = @($command[1..($command.Count - 1)])
    $at = [array]::IndexOf($arguments, '--equation-sequence-ui')
    $evidenceParent = $arguments[$at + 1]
    $existing = @(Get-ChildItem -LiteralPath $evidenceParent -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    # CTest の環境は起動の間だけ当てる (この script の PATH などを壊さない)。
    $saved = @{}
    foreach ($k in $testEnv.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); Set-Item -Path "env:$k" -Value $testEnv[$k] }
    $inputBefore = [Mvm.Tests.P37cIdle]::LastInputTick()
    $start = Get-Date
    try {
        $line = & $launcher --timeout-ms $timeoutMs --window-ms $(if ($WindowArm) { $windowMs } else { 0 }) --error-mode wer `
            --stdout (Join-Path $runDir 'stdout.txt') --stderr (Join-Path $runDir 'stderr.txt') --cwd (Split-Path $exe) -- $exe @arguments
        $launcherCode = $LASTEXITCODE
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    if ($launcherCode -ne 0) { throw "起動器が失敗しました ($Stage $Index)" }
    $result = $line | ConvertFrom-Json
    $userInput = [Mvm.Tests.P37cIdle]::LastInputTick() -ne $inputBefore
    $stderr = Get-Content -Raw -LiteralPath (Join-Path $runDir 'stderr.txt') -Encoding utf8
    $class = Get-Classification $result $stderr $WindowArm
    $dump = $null
    if ($class -like 'NATIVE_CRASH*') {
        $found = Find-NewDump $result.pid
        if ($found) {
            $copy = Join-Path $runDir ([IO.Path]::GetFileName($found))
            Copy-Item -LiteralPath $found $copy
            $dump = [ordered]@{ original = $found; copy = $copy; sha256 = (Get-FileHash -Algorithm SHA256 $copy).Hash }
        }
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $start } -ErrorAction SilentlyContinue |
            Where-Object { $_.Message -match [regex]::Escape([IO.Path]::GetFileName($exe)) } | ForEach-Object { $_.Message })
        $events | Set-Content -LiteralPath (Join-Path $runDir 'application-events.txt') -Encoding utf8
    }
    $scratch = @(Get-ChildItem -LiteralPath $evidenceParent -Directory | Where-Object { $existing -notcontains $_.Name })
    $evidence = if ($scratch.Count -eq 1) { $scratch[0].FullName } else { $null }
    $phase = Get-Phase $(if ($scratch.Count -eq 1) { $scratch[0].FullName } else { $null }) $start
    $row = [ordered]@{
        stage = $Stage; index = $Index; started = $start.ToString('o'); pid = $result.pid
        outcome = $result.outcome; exit_code = $result.exit_code; elapsed_ms = $result.elapsed_ms
        classification = $class; dump = $dump; user_input_during_run = $userInput
        evidence = $evidence; phase = $phase.reached; stderr_tail = (($stderr -split "`n" | Where-Object { $_ } | Select-Object -Last 3) -join ' | ')
    }
    $row | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDir 'run.json') -Encoding utf8
    $phase | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDir 'phase.json') -Encoding utf8
    Write-Host ("{0} {1,3} {2,-16} {3} {4,8:N0} ms  {5}  input={6}" -f $Stage, $Index, $result.outcome, $result.exit_code, $result.elapsed_ms, $class, $userInput)
    return [pscustomobject]$row
}

function Invoke-Control {
    $log = Join-Path $out 'Control\ctest.log'
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    $start = Get-Date
    & "$ucrt\bin\ctest.exe" --test-dir $build -R "^$testName`$" --output-on-failure *> $log
    $code = $LASTEXITCODE
    $text = Get-Content -Raw -LiteralPath $log
    $line = ([regex]"Test\s+#\d+: $testName \.+\s*(.+)").Match($text).Groups[1].Value.Trim()
    $class = if ($code -eq 0) { 'PASS' } elseif ($line -match 'Exception') { "NATIVE_CRASH (ctest: $line, dump なし)" } elseif ($line -match 'Timeout') { 'TIMEOUT_OR_HANG' } else { "FAILED (ctest: $line)" }
    Write-Host "Control   1 ctest exit=$code $line  $class"
    [pscustomobject][ordered]@{ stage = 'Control'; index = 1; started = $start.ToString('o'); pid = $null; outcome = 'ctest'
        exit_code = $code; elapsed_ms = $null; classification = $class; dump = $null; user_input_during_run = $null; phase = $null; stderr_tail = $line }
}

# --- 既存の dump の保全 ---
# WER の LocalDumps は既定で 10 件を超えると最古の dump を消す (2026-10-15 に probe で 1 件失った)。
# crash しうる run の前に、まだ保全していない dump を hash 付きで複製する (元は残す)。
$historical = Join-Path (Join-Path $repo $baseOutput) 'historical-crashdumps'
New-Item -ItemType Directory -Force $historical | Out-Null
foreach ($d in @(Get-ChildItem -LiteralPath $crashDumps -Filter *.dmp -ErrorAction SilentlyContinue)) {
    $copy = Join-Path $historical $d.Name
    if (-not (Test-Path -LiteralPath $copy)) {
        Copy-Item -LiteralPath $d.FullName $copy
        Add-Content -LiteralPath (Join-Path $historical 'added.txt') -Encoding utf8 -Value ("{0} {1} {2}" -f (Get-Date -Format o), $d.Name, (Get-FileHash -Algorithm SHA256 $copy).Hash)
    }
}

# --- 実行 ---
$rows = [Collections.Generic.List[object]]::new()
$stageValidity = [ordered]@{}
$lease = Start-MvmTestDisplayLease
try {
    foreach ($stage in $Stages) {
        $before = Invoke-Preflight "$stage-before"
        if (-not $before) { Write-Host "${stage}: 開始前の preflight が VALID でないので実行しません"; $stageValidity[$stage] = 'NOT_RUN_ENVIRONMENT_INVALID'; continue }
        $scan = $null
        if ($stage -like 'Stress*') { $scan = Start-DiskScan; Start-Sleep -Seconds 5 }
        $stageRows = @()
        try {
            for ($i = 1; $i -le $plan[$stage]; ++$i) {
                $stageRows += $(if ($stage -eq 'Control') { Invoke-Control } else { Invoke-Run $stage $i ($stage -like '*Window') })
                $lease.AssertValid()
            }
        } finally {
            if ($scan) { Stop-Process -Id $scan.Id -Force -ErrorAction SilentlyContinue }
        }
        $after = Invoke-Preflight "$stage-after"
        $stageValidity[$stage] = if ($after) { 'VALID' } else { 'GUI_ENVIRONMENT_INVALID' }
        foreach ($r in $stageRows) {
            if (-not $after -and $r.classification -notlike 'NATIVE_CRASH*') { $r.classification = "GUI_ENVIRONMENT_INVALID (元: $($r.classification))" }
            $rows.Add($r)
        }
    }
} finally {
    $lease.Dispose()
}

$afterHashes = Get-BinaryHashes
$identityStable = ($afterHashes | ConvertTo-Json) -eq ($identity.binaries | ConvertTo-Json)
$counts = [ordered]@{}
foreach ($g in ($rows | Group-Object stage)) {
    $counts[$g.Name] = [ordered]@{}
    foreach ($c in ($g.Group | Group-Object classification)) { $counts[$g.Name][$c.Name] = $c.Count }
}
$summary = [ordered]@{
    finished = (Get-Date -Format o); identity_stable = $identityStable; stage_environment = $stageValidity
    counts = $counts; crashes = @($rows | Where-Object { $_.classification -like 'NATIVE_CRASH*' })
    runs = $rows
}
Write-Json $summary 'summary.json'
Write-Host "identity_stable=$identityStable"
$counts | ConvertTo-Json -Depth 4 | Write-Host
if (-not $identityStable) { Write-Host 'binary が実行中に変わりました。この cohort は無効です。'; exit 3 }
