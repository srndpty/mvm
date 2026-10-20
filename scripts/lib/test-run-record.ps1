# CTest の実行 1 回分の記録を、crash の archive (scripts/lib/crash-archive.ps1) の runs/ に残す
# (docs/crash-evidence.md)。build/ を消しても残る。
#
# 残すもの: ctest の全出力、Testing/Temporary の LastTest.log・LastTestsFailed.log、junit、試験ごとの
# 名前と終了の分類、source・実行ファイル・runtime の identity、GUI の preflight の結果、落ちた試験の dump
# (起動器 mvm_wer_launch が MVM_CRASH_STAGING へ複製したもの、および run の間に CrashDumps に現れたもの)。
#
# 開始の前に既存の dump を保全する。保全に失敗したら例外にし、試験を始めない (WER の回転で未保全の
# dump を消さないため)。archive の lock は操作ごとに取り、run の間は持たない (試験の中からも archive を
# 使うため)。
#
# 使い方 (scripts/test.ps1):
#   $record = Start-MvmTestRunRecord -Name release -RepoRoot $repo -BinDirectory $bin
#   $args += Get-MvmRecordedCTestArguments -Record $record -Label normal
#   ... ctest ...
#   Complete-MvmRecordedCTestGroup -Record $record -Label normal -OutputLines $lines -ExitCode $code -BuildDirectory $build
#   Complete-MvmTestRunRecord -Record $record

. (Join-Path $PSScriptRoot 'crash-archive.ps1')

function Use-MvmCrashArchive {
    param([string]$Root, [Parameter(Mandatory)][scriptblock]$Action)
    $archive = Open-MvmCrashArchive -Root $Root
    try { & $Action $archive } finally { Close-MvmCrashArchive $archive }
}

function Get-MvmBinaryManifest {
    param([string[]]$BinDirectory)
    $files = foreach ($dir in $BinDirectory) {
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -Recurse | Where-Object { $_.Extension -in '.exe', '.dll' } | Sort-Object FullName)) {
            [ordered]@{ path = $f.FullName; size = $f.Length; last_write = $f.LastWriteTime.ToString('o'); sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash }
        }
    }
    return , @($files)
}

function Get-MvmTestRunIdentity {
    param([Parameter(Mandatory)][string]$RepoRoot, [string]$Ucrt64 = 'C:\msys64\ucrt64')
    $diff = (& git -C $RepoRoot diff HEAD --binary | Out-String)
    $pacman = Join-Path (Split-Path $Ucrt64) 'usr\bin\pacman.exe'
    [ordered]@{
        recorded = (Get-Date).ToString('o')
        git_head = (& git -C $RepoRoot rev-parse HEAD)
        git_status = @(& git -C $RepoRoot status --porcelain)
        git_diff_sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($diff)))
        packages = $(if (Test-Path -LiteralPath $pacman) { @(& $pacman -Q | Where-Object { $_ -like 'mingw-w64-ucrt-x86_64-*' }) } else { @() })
        gpu = @(Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) driver=$($_.DriverVersion)" })
        os = (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "$($_.Caption) $($_.Version)" })
        computer = $env:COMPUTERNAME
    }
}

function Write-MvmRecordJson {
    param([Parameter(Mandatory)][AllowNull()][AllowEmptyCollection()]$Value, [Parameter(Mandatory)][string]$Path)
    if (Test-Path -LiteralPath $Path) { throw "記録が既にあります (上書きしません): $Path" }
    # pipeline で渡すと、空の配列は何も書かれず、要素 1 つの配列は配列でなくなる。
    ConvertTo-Json -InputObject $Value -Depth 8 | Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}

function Start-MvmTestRunRecord {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$RepoRoot,
        [string[]]$BinDirectory = @(),
        [string]$CrashDumpDirectory = (Join-Path $env:LOCALAPPDATA 'CrashDumps'),
        [string]$ArchiveRoot
    )
    $run = Use-MvmCrashArchive -Root $ArchiveRoot -Action {
        param($a)
        Save-MvmCrashDumps -Archive $a -Directory $CrashDumpDirectory -Context "試験の run ($Name) の前" | Out-Null
        New-MvmCrashArchiveRun -Archive $a -Name $Name
    }
    $staging = Join-Path $run 'staging'
    New-Item -ItemType Directory -Path $staging | Out-Null
    Write-MvmRecordJson (Get-MvmTestRunIdentity -RepoRoot $RepoRoot) (Join-Path $run 'identity.json')
    $record = [pscustomobject]@{
        Name = $Name; Run = $run; Staging = $staging; ArchiveRoot = $ArchiveRoot; CrashDumps = $CrashDumpDirectory
        Started = (Get-Date).ToString('o'); PreviousStaging = $env:MVM_CRASH_STAGING
        Groups = [Collections.Generic.List[object]]::new(); Preflights = [Collections.Generic.List[object]]::new()
        Manifests = [Collections.Generic.List[string]]::new(); GroupStarted = @{}
    }
    foreach ($dir in $BinDirectory) { Add-MvmTestRunBinaryManifest -Record $record -BinDirectory $dir }
    $env:MVM_CRASH_STAGING = $staging
    Write-Host "試験の記録: $run"
    return $record
}

# 実行ファイルの SHA-256 の一覧を記録する (preset ごとに build の後で呼ぶ)。
function Add-MvmTestRunBinaryManifest {
    param([Parameter(Mandatory)]$Record, [Parameter(Mandatory)][string]$BinDirectory)
    $name = 'binaries-{0}.json' -f (Split-Path (Split-Path $BinDirectory) -Leaf)
    Write-MvmRecordJson (Get-MvmBinaryManifest $BinDirectory) (Join-Path $Record.Run $name)
    $Record.Manifests.Add($name)
}

function Get-MvmRecordedCTestArguments {
    param([Parameter(Mandatory)]$Record, [Parameter(Mandatory)][string]$Label)
    if ($Label -notmatch '^[A-Za-z0-9._-]+$') { throw "記録の label に使えない文字があります: $Label" }
    # これより前に書かれた Testing/Temporary の file は、前の実行の残りとして写さない。
    $Record.GroupStarted[$Label] = [DateTime]::UtcNow
    return @('--output-junit', (Join-Path $Record.Run "$Label.junit.xml"))
}

# junit の failure の message (ctest が付ける) から終了の分類を決める。
function Get-MvmCTestClassification {
    param([string]$Status, [string]$Message)
    if ($Status -eq 'run') { return 'PASS' }
    if ($Status -in 'notrun', 'disabled') { return 'NOT_RUN' }
    switch -Regex ($Message) {
        '^Timeout$' { return 'TIMEOUT' }
        '^Failed$' { return 'FAILED' }
        '^(SEGFAULT|ILLEGAL|NUMERICAL|INTERRUPT|OTHER|Exception.*|Exit code 0x[0-9a-fA-F]+.*)$' { return 'NATIVE_CRASH' }
        default { return 'UNKNOWN' }
    }
}

function Read-MvmCTestJunit {
    param([Parameter(Mandatory)][string]$Path)
    [xml]$xml = Get-Content -Raw -LiteralPath $Path -Encoding utf8
    foreach ($case in @($xml.SelectNodes('//testcase'))) {
        $failure = $case.SelectSingleNode('failure')
        $message = if ($failure) { $failure.GetAttribute('message') } else { '' }
        $outNode = $case.SelectSingleNode('system-out')
        $output = if ($outNode) { $outNode.InnerText } else { '' }
        $staged = @([regex]::Matches($output, 'wer_launch: 例外 (0x[0-9A-F]{8}) で終了。dump: (\S+) \(複製: ([^)]+)\)') |
            ForEach-Object { [ordered]@{ code = $_.Groups[1].Value; original = $_.Groups[2].Value; staged = $_.Groups[3].Value } })
        # 打ち切りの前に止まっていた試験の dump (起動器が書く。code は hang)。
        $staged += @([regex]::Matches($output, 'wer_launch: \d+ ms で終わっていません。打ち切りの前の dump: ([^\r\n]+)') |
            ForEach-Object { [ordered]@{ code = 'hang'; original = $null; staged = $_.Groups[1].Value.Trim() } })
        $missing = @([regex]::Matches($output, 'wer_launch: (例外 0x[0-9A-F]{8} で終了。(dump がありません|dump の複製に失敗|LOCALAPPDATA が無く)|\d+ ms で終わっていません。(MVM_CRASH_STAGING が無い|dump を作れません|dump の書き込みに失敗))[^\r\n]*') | ForEach-Object { $_.Value })
        [ordered]@{
            name = $case.GetAttribute('name'); status = $case.GetAttribute('status'); time = $case.GetAttribute('time')
            failure = $message; classification = (Get-MvmCTestClassification $case.GetAttribute('status') $message)
            launcher_dumps = $staged; launcher_problems = $missing
        }
    }
}

function Complete-MvmRecordedCTestGroup {
    param(
        [Parameter(Mandatory)]$Record,
        [Parameter(Mandatory)][string]$Label,
        [string[]]$OutputLines = @(),
        [Parameter(Mandatory)][int]$ExitCode,
        [Parameter(Mandatory)][string]$BuildDirectory,
        [string]$Description = ''
    )
    $log = Join-Path $Record.Run "$Label.ctest.log"
    if (Test-Path -LiteralPath $log) { throw "記録が既にあります (上書きしません): $log" }
    [IO.File]::WriteAllLines($log, $OutputLines, [Text.UTF8Encoding]::new($false))
    $copied = @()
    $stale = @()
    $started = $Record.GroupStarted[$Label]
    foreach ($f in 'LastTest.log', 'LastTestsFailed.log') {
        $src = Join-Path $BuildDirectory "Testing\Temporary\$f"
        if ((Test-Path -LiteralPath $src) -and $started -and (Get-Item -LiteralPath $src).LastWriteTimeUtc -lt $started.AddSeconds(-2)) {
            # ctest は失敗が無いと LastTestsFailed.log を書かないので、前の実行の file が残っている。
            $stale += $f
        } elseif (Test-Path -LiteralPath $src) {
            $dst = Join-Path $Record.Run "$Label.$f"
            [IO.File]::Copy($src, $dst, $false)
            $copied += $f
        }
    }
    $junit = Join-Path $Record.Run "$Label.junit.xml"
    $tests = @(if (Test-Path -LiteralPath $junit) { Read-MvmCTestJunit $junit })
    Write-MvmRecordJson $tests (Join-Path $Record.Run "$Label.tests.json")
    $counts = [ordered]@{}
    foreach ($g in ($tests | Group-Object { $_.classification })) { $counts[$g.Name] = $g.Count }
    $group = [ordered]@{
        label = $Label; description = $Description; exit_code = $ExitCode; build_directory = $BuildDirectory
        junit = (Test-Path -LiteralPath $junit)
        copied = $copied; stale_skipped = $stale; tests = $tests.Count; counts = $counts
    }
    if (-not $group.junit) { Write-Host "注意: junit がありません ($Label)。試験ごとの分類は記録できません" -ForegroundColor Yellow }
    $Record.Groups.Add($group)
}

# GUI の preflight を記録する。結果で試験を止めはしない (記録だけ)。戻り値は VALID か。
function Invoke-MvmTestRunPreflight {
    param(
        [Parameter(Mandatory)]$Record,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][string]$Script,
        [string[]]$Arguments = @()
    )
    $file = Join-Path $Record.Run "preflight-$Label.json"
    $pwsh = (Get-Process -Id $PID).Path
    & $pwsh -NoProfile -File $Script -OutputFile $file @Arguments | Out-Null
    $code = $LASTEXITCODE
    $verdict = if ($code -eq 0) { 'VALID' } elseif ($code -eq 2) { 'ENVIRONMENT_INVALID' } else { "PREFLIGHT_FAILED ($code)" }
    $Record.Preflights.Add([ordered]@{ label = $Label; exit_code = $code; verdict = $verdict; file = (Split-Path $file -Leaf) })
    Write-Host "GUI の preflight ($Label): $verdict"
    return $code -eq 0
}

function Complete-MvmTestRunRecord {
    param([Parameter(Mandatory)]$Record)
    $env:MVM_CRASH_STAGING = $Record.PreviousStaging
    $dumps = Use-MvmCrashArchive -Root $Record.ArchiveRoot -Action {
        param($a)
        Save-MvmCrashDumps -Archive $a -Directory @($Record.Staging, $Record.CrashDumps) -Context "試験の run ($($Record.Name))"
    }
    $bySource = @{}
    foreach ($d in $dumps) { $bySource[$d.original_path] = $d }
    $crashes = foreach ($g in $Record.Groups) {
        $tests = Get-Content -Raw -LiteralPath (Join-Path $Record.Run "$($g.label).tests.json") | ConvertFrom-Json
        foreach ($t in @($tests | Where-Object { $_.classification -eq 'NATIVE_CRASH' -or @($_.launcher_dumps).Count -gt 0 })) {
            [ordered]@{
                group = $g.label; test = $t.name; classification = $t.classification; failure = $t.failure
                dumps = @(foreach ($d in @($t.launcher_dumps)) {
                        $hit = $bySource[[IO.Path]::GetFullPath($d.staged)]
                        [ordered]@{ code = $d.code; staged = $d.staged; sha256 = $(if ($hit) { $hit.sha256 } else { $null }) }
                    })
                launcher_problems = @($t.launcher_problems)
            }
        }
    }
    $usage = Use-MvmCrashArchive -Root $Record.ArchiveRoot -Action { param($a) Get-MvmCrashArchiveUsage $a }
    $summary = [ordered]@{
        name = $Record.Name; started = $Record.Started; finished = (Get-Date).ToString('o')
        manifests = @($Record.Manifests); preflights = @($Record.Preflights); groups = @($Record.Groups)
        crashes = @($crashes)
        # この run の間に新しく保全した dump (staging の複製と、CrashDumps に現れたもの)。
        new_dumps = @($dumps | Where-Object { $_.status -ne 'already_indexed' })
        archive_usage = $usage
    }
    $path = Join-Path $Record.Run 'summary.json'
    Write-MvmRecordJson $summary $path
    Write-Host ("試験の記録を保存: {0} (crash {1} 件、archive {2:N0} / {3:N0} byte)" -f $Record.Run, @($crashes).Count, $usage.bytes, $usage.max_bytes)
    return $path
}
