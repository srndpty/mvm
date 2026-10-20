# mvm_wer_launch の、打ち切りの前の dump の契約 (docs/crash-evidence.md)。
#
# CTest は TIMEOUT を過ぎた試験を何も残さずに止める。起動器は MVM_TEST_TIMEOUT_SECONDS の少し前に
# 終わらない子の minidump を MVM_CRASH_STAGING へ書き、stderr に場所を書く。ここでは
#   - 止まった probe の dump を打ち切りの前に書き、子は止めない (打ち切りは CTest に任せる)
#   - 解析器 (scripts/minidump-stack.py) が、止まった関数の連鎖 (probe_hang <- main) を出す
#   - すぐ終わる試験・TIMEOUT を知らない起動では dump を書かない
#   - 試験の記録 (scripts/lib/test-run-record.ps1) が、その行を試験の dump として読む
#   - CMake が試験の TIMEOUT を起動器へ渡している (製品の UI 試験で確かめる)
# を確かめる。WER は使わないので、CI でも動く。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Launcher,
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Addr2line,
    [Parameter(Mandatory)][string]$Analyzer,
    [Parameter(Mandatory)][string]$RecordLibrary,
    [Parameter(Mandatory)][string]$CTest,
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $RecordLibrary

$out = Join-Path $OutputDir (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force -Path $out | Out-Null
$probeName = [IO.Path]::GetFileName($Probe)
$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}

# 起動器を CTest と同じ形 (--test) で起動する。終わるのを待たない。
function Start-Launcher([string]$Name, [string[]]$Arguments, [hashtable]$Environment) {
    $saved = @{}
    foreach ($k in $Environment.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Environment[$k]) }
    try {
        Start-Process -FilePath $Launcher -ArgumentList (@('--test') + $Arguments) -WorkingDirectory $out -NoNewWindow -PassThru `
            -RedirectStandardOutput (Join-Path $out "$Name.out") -RedirectStandardError (Join-Path $out "$Name.err")
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}
function Read-Err([string]$Name) { (Get-Content -Raw -Encoding utf8 -ErrorAction SilentlyContinue (Join-Path $out "$Name.err")) + '' }
# stderr に Pattern が現れるまで待つ (最大 TimeoutSeconds)。
function Wait-Err([string]$Name, [string]$Pattern, [int]$TimeoutSeconds = 20) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Read-Err $Name) -match $Pattern) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}
function Get-ChildProcessId([int]$ParentId) {
    @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $ParentId" | ForEach-Object ProcessId)
}

$staging = Join-Path $out 'staging'
New-Item -ItemType Directory -Path $staging | Out-Null

# 1. 止まった子: 打ち切り (3 秒) の前 (3 秒の 1/5 前 = 2400 ms) に dump を書き、子は止めない
$started = Get-Date
$p = Start-Launcher 'hang' @($Probe, 'hang') @{ MVM_TEST_TIMEOUT_SECONDS = '3'; MVM_CRASH_STAGING = $staging }
try {
    $written = Wait-Err 'hang' '打ち切りの前の dump: '
    $elapsed = ((Get-Date) - $started).TotalSeconds
    Check $written "打ち切りの前の dump の場所を stderr に書く ($(Read-Err 'hang'))"
    Check ($elapsed -ge 2.0) "打ち切りの少し前まで待つ ($([Math]::Round($elapsed, 1)) 秒)"
    Check ((Read-Err 'hang') -match 'wer_launch: 2400 ms で終わっていません') 'TIMEOUT の 1/5 前 (短い試験) に書く'
    Start-Sleep -Milliseconds 500
    $children = @(Get-ChildProcessId $p.Id)
    Check (-not $p.HasExited -and $children.Count -eq 1) 'dump の後も子を止めない (打ち切りは CTest に任せる)'
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
Start-Sleep -Milliseconds 1000
Check (@($children | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue }).Count -eq 0) 'CTest が起動器を止めると子も止まる'

$dumps = @(Get-ChildItem -LiteralPath $staging -Filter "$probeName.*.hang.*.dmp")
Check ($dumps.Count -eq 1) "staging に <exe>.<pid>.hang.<tick>.dmp を 1 つ書く ($($dumps.Name -join ', '))"
if ($dumps.Count -eq 1) {
    Check ((Read-Err 'hang') -match [regex]::Escape($dumps[0].FullName)) 'stderr の場所と実際の file が一致する'
    $json = Join-Path $out 'hang-stack.json'
    $text = & $Python -I $Analyzer $dumps[0].FullName --addr2line $Addr2line --json $json
    Set-Content -LiteralPath (Join-Path $out 'hang-stack.txt') -Value $text -Encoding utf8
    Check ($LASTEXITCODE -eq 0) '解析器が打ち切りの前の dump を読める'
    $report = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
    Check (-not $report.PSObject.Properties['exception'] -or -not $report.exception) '例外の無い dump として読む'
    $chains = foreach ($t in $report.threads) {
        , @($t.unwind | ForEach-Object { if ($_.symbol) { ($_.symbol -split ' ')[0] } else { '' } })
    }
    $hung = @($chains | Where-Object { $i = [Array]::IndexOf($_, 'probe_hang'); $i -ge 0 -and $i + 1 -lt $_.Count -and $_[$i + 1] -eq 'main' })
    Check ($hung.Count -eq 1) "止まった thread の stack に probe_hang <- main を出す ($(@($chains | ForEach-Object { $_[0..([Math]::Min(4, $_.Count) - 1)] -join ' <- ' }) -join ' | '))"
}

# 2. すぐ終わる子・TIMEOUT を知らない起動・staging の無い起動
$p = Start-Launcher 'quick' @($Probe, 'sleep', '200') @{ MVM_TEST_TIMEOUT_SECONDS = '3'; MVM_CRASH_STAGING = $staging }
$p.WaitForExit()
Check ($p.ExitCode -eq 0 -and (Read-Err 'quick') -notmatch 'wer_launch') 'すぐ終わる試験では何も書かない'
$p = Start-Launcher 'no-timeout' @($Probe, 'sleep', '3000') @{ MVM_TEST_TIMEOUT_SECONDS = $null; MVM_CRASH_STAGING = $staging }
$p.WaitForExit()
Check ($p.ExitCode -eq 0 -and (Read-Err 'no-timeout') -notmatch 'wer_launch') 'TIMEOUT を知らなければ dump を書かない'
$p = Start-Launcher 'no-staging' @($Probe, 'hang') @{ MVM_TEST_TIMEOUT_SECONDS = '2'; MVM_CRASH_STAGING = $null }
try {
    Check (Wait-Err 'no-staging' 'MVM_CRASH_STAGING が無いので dump を取りません') 'staging が無ければ、dump を取らないことを書く (成功を装わない)'
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
Check (@(Get-ChildItem -LiteralPath $staging -Filter '*.dmp').Count -eq 1) '2 の起動は staging に dump を足さない'

# 3. 試験の記録が、打ち切りの前の dump を試験の dump として読む
$junit = Join-Path $out 'hang.junit.xml'
$escaped = [Security.SecurityElement]::Escape((Read-Err 'hang') + "`nwer_launch: 1000 ms で終わっていません。dump を作れません (5): X:\no\such.dmp`n")
Set-Content -LiteralPath $junit -Encoding utf8 -Value @"
<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="fixture" tests="1" failures="1">
<testcase name="hung_test" status="fail" time="60"><failure message="Timeout"/><system-out>$escaped</system-out></testcase>
</testsuite>
"@
$case = @(Read-MvmCTestJunit $junit)[0]
Check ($case.classification -eq 'TIMEOUT') '打ち切りは TIMEOUT に分類する'
Check (@($case.launcher_dumps).Count -eq 1 -and $case.launcher_dumps[0].code -eq 'hang' -and
    $dumps.Count -eq 1 -and $case.launcher_dumps[0].staged -eq $dumps[0].FullName) '打ち切りの前の dump を launcher_dumps (code hang) に記録する'
Check (@($case.launcher_problems).Count -eq 1 -and $case.launcher_problems[0] -match 'dump を作れません') 'dump を取れなかったことを launcher_problems に記録する'

# 4. CMake が TIMEOUT を起動器へ渡している (TIMEOUT 60 の製品の UI 試験)
$tests = (& $CTest --test-dir $BuildDir --show-only=json-v1 | Out-String | ConvertFrom-Json).tests
$inspector = @($tests | Where-Object name -eq 'math_inspector_product_ui')
$modifications = if ($inspector.Count -eq 1) {
    @($inspector[0].properties | Where-Object name -eq 'ENVIRONMENT_MODIFICATION' | ForEach-Object { $_.value })
} else { @() }
Check ($modifications -contains 'MVM_TEST_TIMEOUT_SECONDS=set:60') "試験の TIMEOUT を MVM_TEST_TIMEOUT_SECONDS で渡す ($($modifications -join ', '))"
Check ($inspector.Count -eq 1 -and [IO.Path]::GetFileNameWithoutExtension($inspector[0].command[0]) -eq 'mvm_wer_launch') 'その試験は起動器を通る'

if ($failures -gt 0) { Write-Host "$failures 件失敗"; exit 1 }
Write-Host '全件通過'
