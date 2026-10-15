# scripts/lib/test-run-record.ps1 の契約。小さな fixture (tests/fixtures/test-run-record) の CTest を、
# 一時 directory の archive に記録させ、出力・LastTest.log・試験ごとの分類・dump・identity・preflight が
# 残ることを確かめる。全 CTest は使わない。
#
# fixture の crash は WER に dump を書かせる (%LOCALAPPDATA%\CrashDumps)。既定の 10 件の回転で利用者の
# dump を消さないよう、先に実際の archive へ保全し (失敗したら fixture を動かさない)、最後に probe の
# dump を CrashDumps から出す。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$OutputDir,
    [Parameter(Mandatory)][string]$CMake,
    [Parameter(Mandatory)][string]$CTest,
    [Parameter(Mandatory)][string]$Ninja,
    [Parameter(Mandatory)][string]$Launcher,
    [Parameter(Mandatory)][string]$Probe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}
function Throws([scriptblock]$Block, [string]$Pattern) {
    try { & $Block; return $false } catch { return $_.Exception.Message -match $Pattern }
}

$out = Join-Path $OutputDir (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force -Path $out | Out-Null
$temp = Join-Path ([IO.Path]::GetTempPath()) "mvm-run-record-test-$PID-$([Environment]::TickCount64)"
$archiveRoot = Join-Path $temp 'archive'
$fakeDumps = Join-Path $temp 'CrashDumps'
New-Item -ItemType Directory -Force -Path $fakeDumps | Out-Null
[IO.File]::WriteAllText((Join-Path $fakeDumps 'old.exe.1.dmp'), 'old-dump')
$realDumps = Join-Path $env:LOCALAPPDATA 'CrashDumps'
$probeName = [IO.Path]::GetFileName($Probe)
$repo = (Resolve-Path (Join-Path (Split-Path $Library) '..\..')).Path
$savedStaging = $env:MVM_CRASH_STAGING

try {
    # 1. archive に失敗したら試験を始めない
    $notDirectory = Join-Path $temp 'archive-is-a-file'
    Set-Content -LiteralPath $notDirectory -Value 'x'
    Check (Throws { Start-MvmTestRunRecord -Name 'x' -RepoRoot $repo -ArchiveRoot $notDirectory -CrashDumpDirectory $fakeDumps } 'directory ではありません') '保全に失敗したら記録を始めない (例外)'
    Check ($env:MVM_CRASH_STAGING -eq $savedStaging) '失敗した開始は MVM_CRASH_STAGING を変えない'

    # 2. fixture の configure
    $build = Join-Path $out 'fixture'
    & $CMake -S $Fixture -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" "-DMVM_FIXTURE_LAUNCHER=$Launcher" "-DMVM_FIXTURE_PROBE=$Probe" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'fixture の configure に失敗しました' }
    $bin = Join-Path $temp 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Copy-Item -LiteralPath $Probe -Destination $bin

    # 3. 記録しながら fixture の CTest を実行する (実際の CrashDumps を先に保全する)
    $real = Open-MvmCrashArchive
    try {
        Invoke-MvmAfterCrashArchive -Archive $real -Directory $realDumps -Context 'test_run_record_contract の前' -Action { }
    } finally { Close-MvmCrashArchive $real }
    $record = Start-MvmTestRunRecord -Name 'contract' -RepoRoot $repo -BinDirectory $bin -ArchiveRoot $archiveRoot -CrashDumpDirectory $fakeDumps
    Check ($env:MVM_CRASH_STAGING -eq $record.Staging) '実行中は MVM_CRASH_STAGING が run の staging を指す'
    $arguments = @('--test-dir', $build) + (Get-MvmRecordedCTestArguments -Record $record -Label 'fixture')
    $lines = [Collections.Generic.List[string]]::new()
    & $CTest @arguments 2>&1 | ForEach-Object { $lines.Add("$_") }
    $code = $LASTEXITCODE
    Complete-MvmRecordedCTestGroup -Record $record -Label 'fixture' -OutputLines $lines -ExitCode $code -BuildDirectory $build
    Check (Throws { Complete-MvmRecordedCTestGroup -Record $record -Label 'fixture' -OutputLines $lines -ExitCode $code -BuildDirectory $build } '上書きしません') '同じ label の記録を上書きしない'

    # 前の実行の Testing/Temporary の file は写さない (ctest は失敗が無いと LastTestsFailed.log を書かない)
    Start-Sleep -Seconds 3
    Get-MvmRecordedCTestArguments -Record $record -Label 'stale' | Out-Null
    Complete-MvmRecordedCTestGroup -Record $record -Label 'stale' -ExitCode 0 -BuildDirectory $build
    $staleGroup = $record.Groups[1]
    Check (-not (Test-Path -LiteralPath (Join-Path $record.Run 'stale.LastTestsFailed.log')) -and @($staleGroup.stale_skipped) -contains 'LastTestsFailed.log') '前の実行の LastTestsFailed.log を写さず、写さなかったことを記録する'

    # 4. preflight は記録だけ (VALID と INVALID)
    $valid = Join-Path $temp 'preflight-valid.ps1'
    $invalid = Join-Path $temp 'preflight-invalid.ps1'
    Set-Content -LiteralPath $valid -Value 'param([string]$OutputFile) ''{"verdict":"VALID"}'' | Set-Content $OutputFile; exit 0'
    Set-Content -LiteralPath $invalid -Value 'param([string]$OutputFile) ''{"verdict":"INVALID"}'' | Set-Content $OutputFile; exit 2'
    Check (Invoke-MvmTestRunPreflight -Record $record -Label 'before' -Script $valid) 'VALID の preflight を記録する'
    Check (-not (Invoke-MvmTestRunPreflight -Record $record -Label 'after' -Script $invalid)) 'INVALID の preflight を記録する'

    $summaryPath = Complete-MvmTestRunRecord -Record $record
    Check ($env:MVM_CRASH_STAGING -eq $savedStaging) '終了で MVM_CRASH_STAGING を戻す'

    # 5. 記録の中身
    $run = $record.Run
    Check ($run.StartsWith((Join-Path $archiveRoot 'runs'))) '記録は archive の runs/ にある'
    $log = Get-Content -Raw -LiteralPath (Join-Path $run 'fixture.ctest.log')
    Check ($log -match 'fixture_crash .+\*\*\*Exception: SegFault' -and $log -match 'tests passed') 'ctest の全出力を残す'
    Check ((Get-Content -Raw -LiteralPath (Join-Path $run 'fixture.LastTest.log')) -match 'fixture_pass') 'LastTest.log を残す'
    Check (Test-Path -LiteralPath (Join-Path $run 'fixture.LastTestsFailed.log')) 'LastTestsFailed.log を残す'
    $tests = @(Get-Content -Raw -LiteralPath (Join-Path $run 'fixture.tests.json') | ConvertFrom-Json)
    $class = @{}
    foreach ($t in $tests) { $class[$t.name] = $t.classification }
    Check ($class['fixture_pass'] -eq 'PASS' -and $class['fixture_fail'] -eq 'FAILED' -and $class['fixture_crash'] -eq 'NATIVE_CRASH' -and $class['fixture_timeout'] -eq 'TIMEOUT') "試験ごとの分類 ($(($class.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', '))"
    $summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
    $crash = @($summary.crashes | Where-Object { $_.test -eq 'fixture_crash' })
    Check ($crash.Count -eq 1 -and @($summary.crashes).Count -eq 1) 'crash した試験を 1 件だけ挙げる'
    $sha = if ($crash.Count -eq 1 -and @($crash[0].dumps).Count -eq 1) { $crash[0].dumps[0].sha256 } else { $null }
    Check ($null -ne $sha -and (Test-Path -LiteralPath (Join-Path $archiveRoot "objects\$sha.dmp"))) 'crash した試験の dump を SHA-256 で archive に保全し、試験と結び付ける'
    Check (@($summary.preflights).Count -eq 2 -and $summary.preflights[1].verdict -eq 'ENVIRONMENT_INVALID') 'preflight の結果を summary に残す'
    Check ($summary.groups[0].exit_code -ne 0 -and $summary.groups[0].tests -eq 4 -and @($summary.groups[0].copied).Count -eq 2) 'group の終了コードと件数、その実行の LastTest.log・LastTestsFailed.log を残す'
    $identity = Get-Content -Raw -LiteralPath (Join-Path $run 'identity.json') | ConvertFrom-Json
    Check ($identity.git_head -match '^[0-9a-f]{40}$' -and $identity.git_diff_sha256 -match '^[0-9A-F]{64}$' -and @($identity.packages).Count -gt 0) 'source と runtime の identity を残す'
    $manifest = @(Get-Content -Raw -LiteralPath (Join-Path $run $summary.manifests[0]) | ConvertFrom-Json)
    Check ($manifest.Count -eq 1 -and $manifest[0].sha256 -eq (Get-FileHash -Algorithm SHA256 -LiteralPath $Probe).Hash) '実行ファイルの SHA-256 を残す'
    $index = @(Get-Content -LiteralPath (Join-Path $archiveRoot 'index.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    Check (@($index | Where-Object { $_.kind -eq 'dump' -and $_.original_name -eq 'old.exe.1.dmp' }).Count -eq 1) '開始の前に既存の dump を保全する'

    # 6. 2 回目の run は別の directory (上書きしない)
    Start-Sleep -Seconds 1
    $second = Start-MvmTestRunRecord -Name 'contract' -RepoRoot $repo -ArchiveRoot $archiveRoot -CrashDumpDirectory $fakeDumps
    Check ($second.Run -ne $run) '2 回目の run は別の directory'
    Complete-MvmTestRunRecord -Record $second | Out-Null
} finally {
    $env:MVM_CRASH_STAGING = $savedStaging
    foreach ($d in @(Get-ChildItem -LiteralPath $realDumps -Filter "$probeName.*.dmp" -ErrorAction SilentlyContinue)) {
        Move-Item -LiteralPath $d.FullName -Destination $out
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) { Write-Host "$failures 件失敗"; exit 1 }
Write-Host '全件通過'
