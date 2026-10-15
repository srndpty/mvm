# mvm_wer_launch (CTest の TEST_LAUNCHER) と scripts/minidump-stack.py の契約 (docs/crash-evidence.md)。
#
# CTest の下 (子に SEM_NOGPFAULTERRORBOX が渡る) で動かすので、「CTest の下でも dump が残る」ことを
# そのまま確かめる。probe を実際に落とすと WER が %LOCALAPPDATA%\CrashDumps に dump を書き、既定の
# 10 件を超えると最古を消す。そこで最初に実際の crash の archive へ既存の dump を保全し、失敗したら
# probe を起動しない。終わったら probe が作った dump を CrashDumps から出し、CrashDumps を元に戻す。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Launcher,
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$Addr2line,
    [Parameter(Mandatory)][string]$Analyzer,
    [Parameter(Mandatory)][string]$ArchiveLibrary,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $ArchiveLibrary

$out = Join-Path $OutputDir (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force -Path $out | Out-Null
$crashDumps = Join-Path $env:LOCALAPPDATA 'CrashDumps'
$probeName = [IO.Path]::GetFileName($Probe)
$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}
function Hex([int]$Code) { '0x{0:X8}' -f $Code }

# 起動器を CTest と同じ形 (--test) で動かす。標準出力と標準エラーは file に受ける。
function Invoke-Launcher([string]$Name, [string[]]$Arguments, [hashtable]$Environment = @{}, [string]$WorkingDirectory = $out) {
    $saved = @{}
    foreach ($k in $Environment.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Environment[$k]) }
    try {
        $p = Start-Process -FilePath $Launcher -ArgumentList (@('--test') + $Arguments) -WorkingDirectory $WorkingDirectory `
            -RedirectStandardOutput (Join-Path $out "$Name.out") -RedirectStandardError (Join-Path $out "$Name.err") -NoNewWindow -PassThru -Wait
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    [pscustomobject]@{
        Code = $p.ExitCode
        Out = (Get-Content -Raw -Encoding utf8 (Join-Path $out "$Name.out")) + ''
        Err = (Get-Content -Raw -Encoding utf8 (Join-Path $out "$Name.err")) + ''
    }
}

# probe の dump を、次の crash の前に CrashDumps から出す。CrashDumps の件数を「試験の前 + 1」以下に
# 保ち、WER の回転 (既定 10 件) を起こさない。
function Move-ProbeDump([string]$Name) {
    $path = Join-Path $crashDumps $Name
    for ($i = 0; $i -lt 100 -and -not (Test-Path -LiteralPath $path); ++$i) { Start-Sleep -Milliseconds 100 }
    if (Test-Path -LiteralPath $path) { Move-Item -LiteralPath $path -Destination $out; $script:probeDumps.Add($Name) }
}

# 1. 既存の dump の保全 (失敗したら probe を落とさずに終わる)
$before = @(Get-ChildItem -LiteralPath $crashDumps -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object Name)
$archive = Open-MvmCrashArchive
try {
    Invoke-MvmAfterCrashArchive -Archive $archive -Directory $crashDumps -Context 'crash_capture_contract の前' -Action {
        param($saved)
        Write-Host "既存の dump を保全: $($saved.Count) 件"
    }
} finally { Close-MvmCrashArchive $archive }

$script:probeDumps = [Collections.Generic.List[string]]::new()
try {
    # 2. 正常終了・終了コード・入出力・作業 directory・環境の受け渡し
    $r = Invoke-Launcher 'echo' @($Probe, 'echo') @{ MVM_CRASH_PROBE_ENV = 'passed-through' }
    Check ($r.Code -eq 0) '正常終了は 0 を返す'
    Check ($r.Out -match 'probe-stdout' -and $r.Err -match 'probe-stderr') '標準出力と標準エラーを受け継ぐ'
    Check ($r.Out -match [regex]::Escape("cwd=$out")) '作業 directory を受け継ぐ'
    Check ($r.Out -match 'env=passed-through') '環境を受け継ぐ'
    $r = Invoke-Launcher 'exit' @($Probe, 'exit', '2A')
    Check ($r.Code -eq 42) "終了コードをそのまま返す ($($r.Code))"
    $r = Invoke-Launcher 'exit-status' @($Probe, 'exit', 'C0000374')
    Check ((Hex $r.Code) -eq '0xC0000374') 'NTSTATUS の終了コードもそのまま返す (CTest の表示が変わらない)'

    # 3. 起動器自身の失敗
    $r = Invoke-Launcher 'missing' @((Join-Path $out 'no-such-test.exe'))
    Check ((Hex $r.Code) -eq '0xE0C0FF02' -and $r.Err -match 'CreateProcess に失敗') '起動できなければ 0xE0C0FF02 と理由を返す'

    # 4. access violation: dump が残り、staging へ複製され、unwind が既知の連鎖を出す
    $staging = Join-Path $out 'staging'
    New-Item -ItemType Directory -Path $staging | Out-Null
    $r = Invoke-Launcher 'crash' @($Probe, 'crash') @{ MVM_CRASH_STAGING = $staging }
    Check ((Hex $r.Code) -eq '0xC0000005') "access violation の終了コードを返す ($(Hex $r.Code))"
    Check ($r.Err -match 'wer_launch: 例外 0xC0000005 で終了。dump: .+ \(複製: ') '例外と dump の場所を標準エラーに書く'
    $staged = @(Get-ChildItem -LiteralPath $staging -Filter "$probeName.*.dmp")
    Check ($staged.Count -eq 1) 'dump を staging へ複製する'
    if ($staged.Count -eq 1) {
        Move-ProbeDump "$probeName.$($staged[0].Name.Split('.')[2]).dmp"
        $json = Join-Path $out 'crash-stack.json'
        $text = & $Python -I $Analyzer $staged[0].FullName --addr2line $Addr2line --json $json
        Set-Content -LiteralPath (Join-Path $out 'crash-stack.txt') -Value $text -Encoding utf8
        Check ($LASTEXITCODE -eq 0) '解析器が dump を読める'
        $report = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json
        Check ($report.exception.code -eq '0xc0000005' -and $report.exception.parameters[1] -eq '0x10') '例外の code と書き込み先 (0x10) を読む'
        $fault = @($report.threads | Where-Object faulting)[0]
        $names = @($fault.unwind | ForEach-Object { if ($_.symbol) { ($_.symbol -split ' ')[0] } else { '' } })
        $expected = @('probe_leaf', 'probe_alloca', 'probe_compare', 'qsort+*', 'probe_big_frame', 'main')
        $match = $names.Count -ge $expected.Count
        for ($i = 0; $match -and $i -lt $expected.Count; ++$i) { $match = $names[$i] -like $expected[$i] }
        Check $match "unwind が既知の呼び出しの連鎖を順に出す ($($names[0..([Math]::Min(6, $names.Count) - 1)] -join ' <- '))"
        Check (@($fault.unwind | Where-Object { $_.kind -ne 'unwind' }).Count -eq 0) 'unwind の frame に scan の値を混ぜない'
        Check ($fault.unwind_stopped -match 'rip = 0') 'unwind が thread の始まりまで届く'
        Check (@($fault.scan | Where-Object { $_.kind -eq 'scan' }).Count -gt 0) 'heuristic の走査は別に scan として出す'
        Check (@($report.modules | Where-Object { $_.path -like "*$probeName" -and $_.image_matches }).Count -eq 1) 'module の identity (TimeDateStamp・大きさ) を disk の image と照合する'
    }

    # 5. abort (fast fail) も dump を残す
    $r = Invoke-Launcher 'abort' @($Probe, 'abort') @{ MVM_CRASH_STAGING = $staging }
    Check ((Hex $r.Code) -eq '0xC0000409') "abort は 0xC0000409 を返す ($(Hex $r.Code))"
    $abortDump = @(Get-ChildItem -LiteralPath $staging -Filter "$probeName.*.dmp" | Where-Object { $staged.Name -notcontains $_.Name })
    Check ($abortDump.Count -eq 1) 'abort の dump も staging へ複製する'
    if ($abortDump.Count -eq 1) { Move-ProbeDump ("$probeName.{0}.dmp" -f $abortDump[0].Name.Split('.')[2]) }

    # 6. staging が無くても dump の場所を書く (成功を装わない)
    $r = Invoke-Launcher 'crash-no-staging' @($Probe, 'crash') @{ MVM_CRASH_STAGING = $null }
    Check ($r.Err -match 'MVM_CRASH_STAGING が無いので複製しません') 'staging が無ければ、複製しないことを書く'
    if ($r.Err -match 'CrashDumps\\(\S+\.dmp)') { Move-ProbeDump $Matches[1] }

    # 7. 起動器が止められたら (CTest の TIMEOUT)、子と孫も止まる
    $p = Start-Process -FilePath $Launcher -ArgumentList @('--test', $Probe, 'spawn-sleep', '60000') -RedirectStandardOutput (Join-Path $out 'spawn.out') -NoNewWindow -PassThru
    $grandchild = $null
    for ($i = 0; $i -lt 100 -and -not $grandchild; ++$i) {
        Start-Sleep -Milliseconds 100
        if ((Get-Content -Raw -ErrorAction SilentlyContinue (Join-Path $out 'spawn.out')) -match 'grandchild (\d+)') { $grandchild = [int]$Matches[1] }
    }
    $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($p.Id)" | ForEach-Object ProcessId)
    Stop-Process -Id $p.Id -Force
    Start-Sleep -Milliseconds 1500
    Check ($null -ne $grandchild -and $children.Count -eq 1) '子と孫が動いている'
    Check (-not (Get-Process -Id $children[0] -ErrorAction SilentlyContinue) -and -not (Get-Process -Id $grandchild -ErrorAction SilentlyContinue)) '起動器を止めると子と孫も止まる (Job Object)'

    # 8. cohort mode: timeout と window の打ち切り
    $json = & $Launcher --timeout-ms 500 --window-ms 0 --error-mode wer --stdout (Join-Path $out 't.out') --stderr (Join-Path $out 't.err') --cwd $out -- $Probe sleep 10000 | ConvertFrom-Json
    Check ($json.outcome -eq 'timeout') 'cohort mode の timeout'
    $json = & $Launcher --timeout-ms 5000 --window-ms 300 --error-mode wer --stdout (Join-Path $out 'w.out') --stderr (Join-Path $out 'w.err') --cwd $out -- $Probe sleep 10000 | ConvertFrom-Json
    Check ($json.outcome -eq 'window_survived') 'cohort mode の window の打ち切り'
    $json = & $Launcher --timeout-ms 5000 --window-ms 0 --error-mode wer --stdout (Join-Path $out 'w.out') --stderr (Join-Path $out 'w2.err') --cwd $out -- $Probe sleep 1 2>$null
    Check ($LASTEXITCODE -eq 2) 'cohort mode は既存の出力 file を上書きしない'
} finally {
    # 途中で失敗しても、probe の dump を CrashDumps に残さない。
    foreach ($d in @(Get-ChildItem -LiteralPath $crashDumps -Filter "$probeName.*.dmp" -ErrorAction SilentlyContinue)) {
        Move-Item -LiteralPath $d.FullName -Destination $out
    }
}
# 9. 利用者の dump: 試験の前の dump は、CrashDumps に残っているか archive にある (失われていない)。
#    試験の前が 9 件以下なら、WER の回転は起きないので、CrashDumps は完全に元どおりのはず。
$after = @(Get-ChildItem -LiteralPath $crashDumps -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object Name)
$archived = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path (Get-MvmCrashArchiveRoot) 'index.jsonl')) {
    if ($line) { $r = $line | ConvertFrom-Json; if ($r.kind -eq 'dump') { $archived[$r.original_path] = $true } }
}
$lost = @($before | Where-Object { $after -notcontains $_ -and -not $archived[(Join-Path $crashDumps $_)] })
Check ($lost.Count -eq 0) "試験の前の dump を失っていない (失われたもの: $($lost -join ', '))"
if ($before.Count -le 9) {
    Check ((($before | Sort-Object) -join '|') -eq (($after | Sort-Object) -join '|')) 'CrashDumps を試験の前と同じ状態に戻す'
} else {
    Write-Host "注意: 試験の前に CrashDumps が $($before.Count) 件あり、WER の回転が起こりうる (archive に保全済み)"
}
Check ($probeDumps.Count -eq 3) "probe の dump 3 件を把握している ($($probeDumps.Count))"

if ($failures -gt 0) { Write-Host "$failures 件失敗"; exit 1 }
Write-Host '全件通過'
