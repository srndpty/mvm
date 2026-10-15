<#
.SYNOPSIS
    P3-4: 数式 group の診断用の経路で、v4 の Project 全体 (6840 frame) を書き出して検収する。

.DESCRIPTION
    製品の書き出しの経路・timeout・画素 oracle は変えない。数式 group は診断用の入力
    (--math-group auto) でだけ動く。

    段階 (-Stage):
      freeze    検収に使う source・binary・runtime・Project・script の SHA-256 を freeze.json へ固定する。
                以降の段階は、実行の前に freeze.json と照合し、1 つでも違えば実行しない。
      preflight 事前の判定の対照 (証明が欠けている・誤っている・V1 の隙間)。
      full      Project 全体を、判定つきの数式 group (mode 3) で 1 回書き出す。
                経過時間は Stopwatch (単調な時計) で測り、有限の外部 watchdog で打ち切る。
                600 秒を超えたら、後で完走しても「製品の性能要件は不成立」と記録する。
      baseline  製品の経路 (数式 group なし) で、全 frame の encoder 直前の RGBA の digest を採る。
                1 process では 60 分を超えるので、区間に分けて別々の process で実行する。
      compare   full の全 frame の digest を baseline と照合し、出力の MP4 を独立に検査する。
      safety    取消・実行時の拒否・外部 watchdog による打ち切りの後に、出力 file が残らないこと。

    source を書き換える処理は含まない。build もしない。

.EXAMPLE
    pwsh scripts/math-export-p34-verify.ps1 -Stage freeze
    pwsh scripts/math-export-p34-verify.ps1 -Stage full
#>
param(
    [Parameter(Mandatory)][ValidateSet('freeze', 'preflight', 'full', 'baseline', 'compare', 'safety')][string]$Stage,
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-export-p34-20261013',
    [string]$Label = '',
    [int]$TotalFrames = 6840,
    [int]$BudgetSeconds = 600,      # 製品の要件 (変えない。判定に使うだけ)
    [int]$WatchdogSeconds = 1500,   # 外部 watchdog (これを超えたら process を止める)
    [int]$ChunkFrames = 570,        # baseline の 1 process あたりの frame 数
    [int]$Parallel = 3,             # baseline を同時に動かす process の数
    [string]$FullRun = '',          # compare: full の run の directory 名 (省略時は最新)
    [string]$BaselineRun = '',      # compare: baseline の run の directory 名 (省略時は最新)
    [int]$TrialFrames = 0           # full: 0 以外なら先頭のこの frame 数だけ (script の動作確認用。検収には使わない)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$bin = Join-Path $repo 'build\ucrt64-release\bin'
$harness = Join-Path $bin 'mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
$ffprobe = Join-Path $ucrt 'ffprobe.exe'
$ffmpeg = Join-Path $ucrt 'ffmpeg.exe'
$procdump = (Get-Command procdump -ErrorAction SilentlyContinue)?.Source
$root = Join-Path $repo $OutputDirectory
$freezePath = Join-Path $root 'freeze.json'
$runLabel = $Label
foreach ($required in @($harness, $manim, $latex, $project, $ffprobe, $ffmpeg)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}
New-Item -ItemType Directory -Force $root | Out-Null
$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'

function Get-Sha([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash }

function Get-ProjectCacheFiles([string]$Directory) {
    $cache = Join-Path $Directory 'cache'
    @(Get-ChildItem $cache -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]jobs[\\/]' } | ForEach-Object { $_.FullName })
}

# baseline を同時に動かすための Project の写し (Project は 1 process しか開けない)。
# Project の file と cache を写し、SHA-256 が元と同じであることを確かめてから使う。
function Get-ProjectCopy([int]$Slot) {
    if ($Slot -eq 0) { return $project }
    $source = Split-Path -Parent $project
    $target = Join-Path $root "project-copy-$Slot"
    if (-not (Test-Path -LiteralPath $target)) {
        New-Item -ItemType Directory -Force $target | Out-Null
        Copy-Item -LiteralPath $project -Destination $target
        foreach ($file in (Get-ProjectCacheFiles $source)) {
            $destination = Join-Path $target ([IO.Path]::GetRelativePath($source, $file))
            New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
            Copy-Item -LiteralPath $file -Destination $destination
        }
    }
    foreach ($file in (@($project) + (Get-ProjectCacheFiles $source))) {
        $copy = Join-Path $target ([IO.Path]::GetRelativePath($source, $file))
        if (-not (Test-Path -LiteralPath $copy) -or (Get-Sha $copy) -ne (Get-Sha $file)) { throw "Project の写しが元と違います: $copy" }
    }
    Join-Path $target (Split-Path -Leaf $project)
}

# 検収の authority になる file の一覧 (path → SHA-256)。
function Get-AuthorityHashes {
    $files = [System.Collections.Generic.List[string]]::new()
    # source: 追跡している src・tests・CMake と、この phase の script・文書以外の変更。
    foreach ($relative in (git -C $repo ls-files -- src tests CMakeLists.txt cmake)) { $files.Add((Join-Path $repo $relative)) }
    foreach ($relative in (git -C $repo ls-files --others --exclude-standard -- src tests)) { $files.Add((Join-Path $repo $relative)) }
    foreach ($relative in @('scripts/math-export-p34-verify.ps1', 'scripts/math-transition-p33d-sanitizers.sh')) {
        $files.Add((Join-Path $repo $relative))
    }
    # binary。
    foreach ($name in @('mvm_math_score_export_parity.exe', 'mvm_test_math_group_compose.exe', 'mvm_test_math_group_preflight.exe')) {
        $files.Add((Join-Path $bin $name))
    }
    # runtime: MLT (本体と module)、Qt、FFmpeg の library、x264、ffprobe・ffmpeg。
    foreach ($name in @('libmlt-7.dll', 'libmlt++-7.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'avcodec-62.dll', 'avformat-62.dll',
            'avutil-60.dll', 'swscale-9.dll', 'swresample-6.dll', 'libx264-165.dll', 'ffprobe.exe', 'ffmpeg.exe')) {
        $files.Add((Join-Path $ucrt $name))
    }
    foreach ($module in (Get-ChildItem 'C:\msys64\ucrt64\lib\mlt' -Filter *.dll)) { $files.Add($module.FullName) }
    # Project、数式の画像の cache (stage の元になる成果物)、Project が絶対 path で参照する素材。
    # cache の jobs\ は、実行のたびに書き直される記録 (tool の version の出力など) なので除く。
    $files.Add($project)
    foreach ($file in (Get-ProjectCacheFiles (Split-Path -Parent $project))) { $files.Add($file) }
    foreach ($match in [regex]::Matches((Get-Content -Raw $project), '"([A-Za-z]:(?:\\|/)[^"]+)"')) {
        $media = $match.Groups[1].Value.Replace('\\', '\') # JSON の escape を戻す
        if (Test-Path -LiteralPath $media -PathType Leaf) { $files.Add((Get-Item -LiteralPath $media).FullName) }
    }
    $hashes = [ordered]@{}
    foreach ($file in ($files | Sort-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
        $hashes[$file] = Get-Sha $file
    }
    $hashes
}

function Assert-Frozen {
    if (-not (Test-Path -LiteralPath $freezePath)) { throw "freeze.json がありません。先に -Stage freeze を実行します。" }
    $frozen = Get-Content -Raw $freezePath | ConvertFrom-Json -AsHashtable
    $current = Get-AuthorityHashes
    $differences = @()
    foreach ($key in $frozen.sha256.Keys) {
        if (-not $current.Contains($key)) { $differences += "無くなった: $key" }
        elseif ($current[$key] -ne $frozen.sha256[$key]) { $differences += "変わった: $key" }
    }
    foreach ($key in $current.Keys) { if (-not $frozen.sha256.ContainsKey($key)) { $differences += "増えた: $key" } }
    if ($differences.Count -gt 0) {
        $differences | ForEach-Object { Write-Host $_ }
        throw "freeze.json と一致しません ($($differences.Count) 件)。検収の authority が変わっています。"
    }
    Write-Host "freeze.json と一致: $($current.Count) file"
}

function New-RunDirectory([string]$Name) {
    $suffix = if ($runLabel) { "$Name-$runLabel" } else { $Name }
    $directory = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$suffix")
    if (Test-Path -LiteralPath $directory) { throw "出力先が既にあります: $directory" }
    New-Item -ItemType Directory -Force $directory | Out-Null
    $directory
}

function Get-Field($object, [string]$name) {
    if ($null -ne $object -and $object.PSObject.Properties[$name]) { return $object.$name }
    return $null
}

function Get-HarnessArguments([string]$Out, [string]$Fixture, [string[]]$Extra, [int]$Cap = 900, [string]$ProjectFile = $project) {
    @('--project', $ProjectFile, '--manim', $manim, '--out', $Out, '--ffprobe', $ffprobe, '--fixture', $Fixture,
        '--cap-seconds', "$Cap") + $Extra
}

function Read-Report([string]$Out, [string]$Fixture) {
    $path = Join-Path $Out "$Fixture-report.json"
    if (Test-Path -LiteralPath $path) { Get-Content -Raw $path | ConvertFrom-Json } else { $null }
}

# harness を前面で 1 回実行する (短い fixture 用)。
function Invoke-Harness([string]$RunDirectory, [string]$Fixture, [string]$Name, [string[]]$Extra) {
    $out = Join-Path $RunDirectory $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = Get-HarnessArguments $out $Fixture $Extra
    & $harness @arguments 2> (Join-Path $out 'harness.log') | Out-Null
    [pscustomobject]@{ name = $Name; fixture = $Fixture; exit_code = $LASTEXITCODE; report = (Read-Report $out $Fixture); out = $out }
}

# harness を別 process で起動し、外部の時計で見張る。$Limit 秒を超えたら止める。
# 1 秒ごとに CPU 時間・working set・commit・thread 数を samples.csv へ書く。
function Invoke-Watched([string]$Out, [string[]]$Arguments, [int]$Limit, [string]$FinalFile, [switch]$Dump) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $harness -ArgumentList $Arguments -NoNewWindow -PassThru `
        -RedirectStandardError (Join-Path $Out 'harness.log') -RedirectStandardOutput (Join-Path $Out 'harness.out')
    $null = $process.Handle # 終了コードを後で読むために握っておく
    $dumper = $null
    if ($Dump -and $procdump) {
        # 起動した後で attach する (debugger の下で起動すると debug heap になり、時間が変わる)。
        $dumpDirectory = Join-Path $Out 'dumps'
        New-Item -ItemType Directory -Force $dumpDirectory | Out-Null
        $dumper = Start-Process -FilePath $procdump -ArgumentList @('-accepteula', '-e', '-ma', "$($process.Id)", $dumpDirectory) `
            -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $Out 'procdump.log') -RedirectStandardError (Join-Path $Out 'procdump.err')
    }
    $samples = [System.Collections.Generic.List[string]]::new()
    $samples.Add('elapsed_s,cpu_s,working_set_mb,peak_working_set_mb,private_mb,peak_paged_mb,threads,final_file')
    $killed = $false
    $finalSeenAt = $null
    $modules = $null
    $peakWorkingSet = 0L; $peakPrivate = 0L
    $nextSample = 0.0
    while (-not $process.HasExited) {
        $elapsed = $clock.Elapsed.TotalSeconds
        if ($FinalFile -and $null -eq $finalSeenAt -and (Test-Path -LiteralPath $FinalFile)) { $finalSeenAt = $elapsed }
        if ($elapsed -ge $nextSample) {
            $nextSample = [math]::Floor($elapsed) + 1
            try {
                $process.Refresh()
                $peakWorkingSet = [math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
                $peakPrivate = [math]::Max($peakPrivate, $process.PrivateMemorySize64)
                $samples.Add(('{0:f2},{1:f2},{2:f1},{3:f1},{4:f1},{5:f1},{6},{7}' -f $elapsed, $process.TotalProcessorTime.TotalSeconds,
                        ($process.WorkingSet64 / 1MB), ($process.PeakWorkingSet64 / 1MB), ($process.PrivateMemorySize64 / 1MB),
                        ($process.PeakPagedMemorySize64 / 1MB), $process.Threads.Count, [int]($null -ne $finalSeenAt)))
                # 実際に load された module (render が始まった後に 1 回)。
                if ($null -eq $modules -and $elapsed -ge 20) {
                    $modules = @($process.Modules | ForEach-Object { $_.FileName } | Sort-Object -Unique)
                }
            } catch { Write-Verbose "$_" }
        }
        if ($elapsed -ge $Limit) {
            $killed = $true
            try { $process.Kill($true) } catch { Write-Verbose "$_" }
            break
        }
        Start-Sleep -Milliseconds 100
    }
    $process.WaitForExit()
    $wall = $clock.Elapsed.TotalSeconds
    if ($FinalFile -and $null -eq $finalSeenAt -and (Test-Path -LiteralPath $FinalFile)) { $finalSeenAt = $wall }
    if ($dumper) { try { $dumper.WaitForExit(15000) | Out-Null; if (-not $dumper.HasExited) { $dumper.Kill() } } catch { Write-Verbose "$_" } }
    $samples | Set-Content -LiteralPath (Join-Path $Out 'samples.csv') -Encoding utf8
    $moduleHashes = [ordered]@{}
    foreach ($module in @($modules)) { if ($module -and (Test-Path -LiteralPath $module)) { $moduleHashes[$module] = Get-Sha $module } }
    $moduleHashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Out 'runtime-modules.json') -Encoding utf8
    $dumps = if ($Dump -and $procdump) { @(Get-ChildItem (Join-Path $Out 'dumps') -Filter *.dmp -ErrorAction SilentlyContinue).Count } else { 0 }
    [pscustomobject]@{
        exit_code = if ($killed) { $null } else { $process.ExitCode }; killed_by_watchdog = $killed
        process_wall_seconds = $wall; final_file_seen_at_seconds = $finalSeenAt
        external_peak_working_set_mb = $peakWorkingSet / 1MB; external_peak_private_mb = $peakPrivate / 1MB
        runtime_modules = $moduleHashes.Count; dumps = $dumps
    }
}

function Read-Digests([string]$Path, [int]$Offset) {
    # 列: frame、呼ばれた回数、画素の digest、alpha の最小値。
    $map = @{}
    foreach ($line in [IO.File]::ReadLines($Path)) {
        $columns = $line -split "`t"
        if ($columns.Count -lt 4) { continue }
        $map[[int]$columns[0] + $Offset] = @($columns[2], [int]$columns[3], [int]$columns[1])
    }
    $map
}

$mathGroup = @('--math-group', 'auto', '--math-group-rule', '1')
$failures = 0

switch ($Stage) {
    'freeze' {
        if (Test-Path -LiteralPath $freezePath) { throw "freeze.json は既にあります (上書きしない): $freezePath" }
        $hashes = Get-AuthorityHashes
        $status = @(git -C $repo status --porcelain)
        $diff = (git -C $repo diff HEAD) -join "`n"
        $freeze = [ordered]@{
            frozen_at = (Get-Date -Format o)
            git_head = (git -C $repo rev-parse HEAD)
            git_status = $status
            git_diff_sha256 = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($diff))).Replace('-', '')
            machine = [ordered]@{
                cpu = (Get-CimInstance Win32_Processor).Name; logical_processors = [Environment]::ProcessorCount
                memory_gb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
                os = (Get-CimInstance Win32_OperatingSystem).Version
            }
            ffmpeg_version = (& $ffmpeg -hide_banner -version | Select-Object -First 1)
            files = $hashes.Count
            sha256 = $hashes
        }
        $freeze | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $freezePath -Encoding utf8
        (git -C $repo diff HEAD) | Set-Content -LiteralPath (Join-Path $root 'freeze-git-diff.patch') -Encoding utf8
        Write-Host "固定: $($hashes.Count) file → $freezePath"
        Write-Host "harness: $($hashes[$harness])"
    }

    'preflight' {
        Assert-Frozen
        $run = New-RunDirectory 'preflight'
        $auto = @('--compare', 'math-group-auto', '--math-group-rule', '1')
        # fixture、追加の引数、期待する判定、理由、graph、期待する終了 (実行の前に固定)。
        # exact = candidate の全 frame が、同じ process で先に書き出した製品の経路と一致する。
        # rejected = 判定は ELIGIBLE だが、実行時の下地の検査が拒否して書き出しが失敗する。
        $cases = @(
            @{ name = 'gap-only'; fixture = 'base-gap'; extra = @(); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; alpha = 255 },
            @{ name = 'half-clip-half-gap'; fixture = 'base-half-gap'; extra = @(); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; alpha = 255 },
            @{ name = 'still-opaque'; fixture = 'cmp-layers-16'; extra = @(); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; alpha = 255 },
            @{ name = 'still-alpha-1'; fixture = 'alpha-1'; extra = @(); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; alpha = 1 },
            @{ name = 'still-alpha-all-nonzero'; fixture = 'alpha-all-nonzero'; extra = @(); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'exact'; alpha = 1 },
            @{ name = 'evidence-missing'; fixture = 'cmp-layers-16'; extra = @('--base-alpha-proof', 'none'); decision = 'UNKNOWN'; reason = 'base-alpha-unproven'; graph = 'conventional'; expect = 'exact'; alpha = $null },
            @{ name = 'evidence-out-of-range'; fixture = 'cmp-layers-16'; extra = @('--base-alpha-proof', 'forge:999'); decision = 'UNKNOWN'; reason = 'base-alpha-unproven'; graph = 'conventional'; expect = 'exact'; alpha = $null },
            @{ name = 'evidence-negative'; fixture = 'cmp-layers-16'; extra = @('--base-alpha-proof', 'forge:-7'); decision = 'UNKNOWN'; reason = 'base-alpha-unproven'; graph = 'conventional'; expect = 'exact'; alpha = $null },
            @{ name = 'evidence-on-transformed-base'; fixture = 'base-transformed'; extra = @('--base-alpha-proof', 'forge:256'); decision = 'UNKNOWN'; reason = 'base-clip-transformed'; graph = 'conventional'; expect = 'exact'; alpha = $null },
            @{ name = 'alpha-zero-proven'; fixture = 'alpha-0'; extra = @(); decision = 'INELIGIBLE'; reason = 'base-alpha-zero'; graph = 'conventional'; expect = 'exact'; alpha = $null },
            @{ name = 'evidence-false-opaque-on-alpha-zero'; fixture = 'alpha-0'; extra = @('--base-alpha-proof', 'forge:256'); decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group'; expect = 'rejected'; alpha = $null }
        )
        $rows = @()
        foreach ($case in $cases) {
            $result = Invoke-Harness $run $case.fixture $case.name ($auto + $case.extra)
            $compare = Get-Field $result.report 'compare'
            $group = Get-Field $compare 'candidate_group'
            $destination = Get-Field (Get-Field $result.report 'math_group_candidate_destination') 'full_frame'
            $row = [ordered]@{
                name = $case.name; fixture = $case.fixture; extra = ($case.extra -join ' '); exit_code = $result.exit_code
                verdict = Get-Field $compare 'verdict'; decision = Get-Field $group 'decision'; decision_reason = Get-Field $group 'decision_reason'
                graph = Get-Field $group 'graph'; base_alpha_proven = Get-Field $group 'base_alpha_proven'
                frames_exact = Get-Field $compare 'frames_exact'; expected_frames = Get-Field $result.report 'expected_frames'
                frames_uncompared = Get-Field $compare 'frames_uncompared'; grouped_clips = Get-Field $group 'grouped_clips'
                errors = Get-Field $group 'errors'; candidate_error = Get-Field $compare 'candidate_error'
                destination_minimum_alpha = Get-Field $destination 'minimum'
                live_after_export = Get-Field $group 'live_after_export'; active_after_export = Get-Field $group 'active_after_export'
                raster_live_bytes_after_export = Get-Field $compare 'candidate_raster_live_bytes_after_export'
                expect = $case.expect
            }
            $common = $row.decision -eq $case.decision -and $row.decision_reason -eq $case.reason -and $row.graph -eq $case.graph -and
            $row.live_after_export -eq 0 -and $row.active_after_export -eq 0 -and $row.raster_live_bytes_after_export -eq 0
            if ($case.expect -eq 'exact') {
                $ok = $common -and $row.exit_code -eq 0 -and $row.verdict -eq 'EXACT' -and $row.frames_exact -eq $row.expected_frames -and
                $row.frames_uncompared -eq 0 -and $row.errors -eq 0 -and
                (($case.graph -eq 'math-group' -and $row.grouped_clips -gt 0) -or ($case.graph -eq 'conventional' -and $row.grouped_clips -eq 0)) -and
                ($null -eq $case.alpha -or $row.destination_minimum_alpha -ge [math]::Min($case.alpha, 254))
            } else {
                $ok = $common -and $row.exit_code -eq 15 -and $row.verdict -eq 'COMPARE_INVALID' -and $row.errors -gt 0 -and
                "$($row.candidate_error)" -match '合成器が拒否'
            }
            $row.as_expected = $ok
            $rows += [pscustomobject]$row
            if (-not $ok) { ++$failures }
            Write-Host ("{0,-38} exit={1} {2,-16} 判定={3} ({4}) graph={5} 一致={6}/{7} 下地の alpha の最小={8} 期待どおり={9}" -f $case.name,
                $row.exit_code, $row.verdict, $row.decision, $row.decision_reason, $row.graph, $row.frames_exact, $row.expected_frames,
                $row.destination_minimum_alpha, $ok)
        }
        $rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
        Write-Host "結果: $run"
    }

    'full' {
        Assert-Frozen
        $run = New-RunDirectory 'full'
        $out = Join-Path $run 'candidate'
        $final = Join-Path $out 'v4-full.mp4'
        # 区間を縮めない (--range なし)。worker の数・encoder・muxer・timeout は製品の値のまま。
        $fullExtra = if ($TrialFrames -gt 0) { $mathGroup + @('--range', "0:$TrialFrames") } else { $mathGroup }
        $arguments = Get-HarnessArguments $out 'v4-full' $fullExtra $WatchdogSeconds
        $arguments -join ' ' | Set-Content -LiteralPath (Join-Path $run 'command.txt') -Encoding utf8
        $watch = Invoke-Watched $out $arguments $WatchdogSeconds $final -Dump
        $report = Read-Report $out 'v4-full'
        $main = Get-Field $report 'math_group_main'
        $elapsed = Get-Field $report 'export_call_wall_seconds'
        $completed = $TrialFrames -eq 0 -and (-not $watch.killed_by_watchdog) -and $watch.exit_code -eq 0 -and (Get-Field $report 'export_success') -eq $true
        # 600 秒の判定: 書き出しの関数の呼び出しから戻りまで (process の中の単調な時計)。外部の時計で見た
        # 「process の起動から最終 file が現れるまで」は、それより長い上限になる。
        $withinBudget = $completed -and $null -ne $elapsed -and $elapsed -le $BudgetSeconds
        $summary = [ordered]@{
            stage = 'full'; trial_frames = $TrialFrames; budget_seconds = $BudgetSeconds; watchdog_seconds = $WatchdogSeconds
            completed = $completed; within_budget = $withinBudget
            export_call_wall_seconds = $elapsed
            process_wall_seconds = $watch.process_wall_seconds
            final_file_seen_at_seconds_from_process_start = $watch.final_file_seen_at_seconds
            killed_by_watchdog = $watch.killed_by_watchdog; exit_code = $watch.exit_code; dumps = $watch.dumps
            harness_verdict = Get-Field $report 'verdict'; harness_failures = Get-Field $report 'failures'
            harness_watchdog = Get-Field $report 'watchdog'
            export_error = Get-Field $report 'export_error'
            request_timeout_ms = Get-Field $report 'request_timeout_ms'; request_timeout_is_product = Get-Field $report 'request_timeout_is_product'
            render_threads_override = Get-Field $report 'render_threads_override'
            expected_frames = Get-Field $report 'expected_frames'; export_frame_count = Get-Field $report 'export_frame_count'
            encoder_submitted = Get-Field $report 'main_encoder_submitted'; encoded_packets = Get-Field $report 'main_encoded_packets'
            muxed_packets = Get-Field $report 'main_muxed_packets'
            pre_yuv_observed_frames = Get-Field $report 'pre_yuv_observed_frames'; pre_yuv_missing_count = Get-Field $report 'pre_yuv_missing_count'
            pre_yuv_lowest_alpha = Get-Field $report 'pre_yuv_lowest_alpha'
            final_file_exists = Get-Field $report 'final_file_exists'; temporary_file_exists = Get-Field $report 'temporary_file_exists'
            output_sha256 = Get-Field $report 'output_sha256'
            app_stages = Get-Field $report 'app_stages'; mlt_stages = Get-Field $report 'mlt_stages'
            main_render_seconds = Get-Field $report 'main_render_seconds'; cpu_seconds = Get-Field $report 'cpu_seconds'
            process_peak_working_set_mb = Get-Field $report 'process_peak_working_set_mb'
            external_peak_working_set_mb = $watch.external_peak_working_set_mb; external_peak_private_mb = $watch.external_peak_private_mb
            decoded = Get-Field $report 'decoded'
            math_group = [ordered]@{
                decision = Get-Field $main 'decision'; decision_reason = Get-Field $main 'decision_reason'; graph = Get-Field $main 'graph'
                base_alpha_proven = Get-Field $main 'base_alpha_proven'; base_clips_checked = Get-Field $main 'base_clips_checked'
                transitions = Get-Field $main 'transitions'; grouped_clips = Get-Field $main 'grouped_clips'
                affine_transitions = Get-Field $main 'affine_transitions'; callbacks = Get-Field $main 'callbacks'
                composed_frames = Get-Field $main 'composed_frames'; composed_layer_frames = Get-Field $main 'composed_layer_frames'
                errors = Get-Field $main 'errors'; error = Get-Field $main 'error'
                prepare_ms = Get-Field $main 'prepare_ms'; raster_count = Get-Field $main 'raster_count'; raster_bytes = Get-Field $main 'raster_bytes'
                raster_shared = Get-Field $main 'raster_shared'; raster_conflicts = Get-Field $main 'raster_conflicts'
                raster_live_bytes_after_prepare = Get-Field $main 'raster_live_bytes_after_prepare'
                raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
                live_after_export = Get-Field $main 'live_after_export'; active_after_export = Get-Field $main 'active_after_export'
                compose_ms_per_frame = Get-Field $main 'compose_ms_per_frame'; copy_ms_per_frame = Get-Field $main 'copy_ms_per_frame'
                callback_ms_per_frame = Get-Field $main 'callback_ms_per_frame'; callback_seconds_total = Get-Field $main 'callback_seconds_total'
                working_set_before_prepare_mb = Get-Field $main 'working_set_before_prepare_mb'
                working_set_after_prepare_mb = Get-Field $main 'working_set_after_prepare_mb'
                working_set_after_export_mb = Get-Field $main 'working_set_after_export_mb'
                destination = Get-Field (Get-Field $main 'destination') 'full_frame'
            }
        }
        $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
        Write-Host ("完走={0} 600 秒以内={1} 書き出しの経過={2:n1} s process={3:n1} s 最終 file が現れた時刻={4:n1} s exit={5} watchdog={6} dump={7}" -f `
                $completed, $withinBudget, $elapsed, $watch.process_wall_seconds, $watch.final_file_seen_at_seconds, $watch.exit_code,
            $watch.killed_by_watchdog, $watch.dumps)
        Write-Host ("判定={0} graph={1} encoder へ渡した frame={2} mux={3} RGBA を観測した frame={4}" -f $summary.math_group.decision,
            $summary.math_group.graph, $summary.encoder_submitted, $summary.muxed_packets, $summary.pre_yuv_observed_frames)
        Write-Host "結果: $run"
        if (-not $completed -or -not $withinBudget) { ++$failures }
    }

    'baseline' {
        Assert-Frozen
        $run = New-RunDirectory 'baseline'
        $chunks = @()
        for ($start = 0; $start -lt $TotalFrames; $start += $ChunkFrames) {
            $chunks += [pscustomobject]@{ start = $start; count = [math]::Min($ChunkFrames, $TotalFrames - $start) }
        }
        $jobs = @()
        $rows = @()
        $pending = [System.Collections.Queue]::new($chunks)
        $projects = @(0..($Parallel - 1) | ForEach-Object { Get-ProjectCopy $_ })
        while ($pending.Count -gt 0 -or $jobs.Count -gt 0) {
            while ($pending.Count -gt 0 -and $jobs.Count -lt $Parallel) {
                $chunk = $pending.Dequeue()
                $slot = @(0..($Parallel - 1) | Where-Object { $_ -notin @($jobs | ForEach-Object { $_.slot }) })[0]
                $out = Join-Path $run ("chunk-{0:d4}" -f $chunk.start)
                New-Item -ItemType Directory -Force $out | Out-Null
                # 製品の経路 (数式 group なし)。timeout だけは、区間が 600 秒で終わらないので上書きする
                # (harness が「製品の条件ではない」と report に残す)。画素には関わらない。
                $arguments = Get-HarnessArguments $out 'v4-full' @('--range', "$($chunk.start):$($chunk.count)", '--request-timeout-ms', '3000000') 3000 $projects[$slot]
                $process = Start-Process -FilePath $harness -ArgumentList $arguments -NoNewWindow -PassThru `
                    -RedirectStandardError (Join-Path $out 'harness.log') -RedirectStandardOutput (Join-Path $out 'harness.out')
                $null = $process.Handle
                $jobs += [pscustomobject]@{ slot = $slot; chunk = $chunk; out = $out; process = $process; clock = [System.Diagnostics.Stopwatch]::StartNew() }
                Write-Host ("開始: frame {0}〜{1}" -f $chunk.start, ($chunk.start + $chunk.count - 1))
            }
            Start-Sleep -Seconds 2
            $still = @()
            foreach ($job in $jobs) {
                if (-not $job.process.HasExited -and $job.clock.Elapsed.TotalSeconds -gt 3300) { try { $job.process.Kill($true) } catch { Write-Verbose "$_" } }
                if (-not $job.process.HasExited) { $still += $job; continue }
                $job.process.WaitForExit()
                $report = Read-Report $job.out 'v4-full'
                $digests = Join-Path $job.out 'v4-full-pre-yuv.tsv'
                $lines = if (Test-Path -LiteralPath $digests) { @([IO.File]::ReadLines($digests)).Count } else { 0 }
                $ok = $job.process.ExitCode -eq 0 -and (Get-Field $report 'export_success') -eq $true -and $lines -eq $job.chunk.count -and
                (Get-Field $report 'pre_yuv_missing_count') -eq 0
                $rows += [pscustomobject]@{ start = $job.chunk.start; count = $job.chunk.count; exit_code = $job.process.ExitCode
                    verdict = Get-Field $report 'verdict'; digest_lines = $lines; wall_seconds = $job.clock.Elapsed.TotalSeconds
                    render_seconds = Get-Field $report 'main_render_seconds'; affine_transitions = Get-Field $report 'overlay_transitions'
                    request_timeout_is_product = Get-Field $report 'request_timeout_is_product'; project_slot = $job.slot; ok = $ok
                }
                if (-not $ok) { ++$failures }
                Write-Host ("終了: frame {0}〜 exit={1} digest={2}/{3} {4:n0} s ok={5}" -f $job.chunk.start, $job.process.ExitCode, $lines,
                    $job.chunk.count, $job.clock.Elapsed.TotalSeconds, $ok)
            }
            $jobs = $still
        }
        ($rows | Sort-Object start) | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
        Write-Host "結果: $run"
    }

    'compare' {
        Assert-Frozen
        $pick = { param($name, $suffix) if ($name) { Join-Path $root $name } else {
                (Get-ChildItem $root -Directory | Where-Object { $_.Name -match "-$suffix(-|$)" } | Sort-Object Name | Select-Object -Last 1).FullName } }
        $fullDirectory = & $pick $FullRun 'full'
        $baselineDirectory = & $pick $BaselineRun 'baseline'
        if (-not $fullDirectory -or -not $baselineDirectory) { throw 'full または baseline の run がありません' }
        $run = New-RunDirectory 'compare'
        $mp4 = Join-Path $fullDirectory 'candidate\v4-full.mp4'
        $candidate = Read-Digests (Join-Path $fullDirectory 'candidate\v4-full-pre-yuv.tsv') 0
        $baseline = @{}
        $overlap = 0
        foreach ($chunk in (Get-ChildItem $baselineDirectory -Directory -Filter 'chunk-*' | Sort-Object Name)) {
            $offset = [int]($chunk.Name -replace 'chunk-', '')
            $path = Join-Path $chunk.FullName 'v4-full-pre-yuv.tsv'
            if (-not (Test-Path -LiteralPath $path)) { continue }
            $part = Read-Digests $path $offset
            foreach ($frame in $part.Keys) { if ($baseline.ContainsKey($frame)) { ++$overlap }; $baseline[$frame] = $part[$frame] }
        }
        $exact = 0; $different = @(); $missingCandidate = @(); $missingBaseline = @()
        $lowestCandidate = 255; $lowestBaseline = 255
        for ($frame = 0; $frame -lt $TotalFrames; ++$frame) {
            $c = $candidate[$frame]; $b = $baseline[$frame]
            if ($null -eq $c) { $missingCandidate += $frame }
            if ($null -eq $b) { $missingBaseline += $frame }
            if ($null -eq $c -or $null -eq $b) { continue }
            $lowestCandidate = [math]::Min($lowestCandidate, $c[1]); $lowestBaseline = [math]::Min($lowestBaseline, $b[1])
            if ($c[0] -eq $b[0] -and $c[1] -eq $b[1]) { ++$exact } else { $different += $frame }
        }
        $extraCandidate = @($candidate.Keys | Where-Object { $_ -lt 0 -or $_ -ge $TotalFrames })

        # --- 出力の MP4 の独立な検査 (harness の集計を使わない) ---
        $stream = (& $ffprobe -v error -select_streams v:0 -count_frames -show_entries `
                'stream=codec_name,profile,pix_fmt,width,height,r_frame_rate,avg_frame_rate,time_base,nb_frames,nb_read_frames,duration,start_time' `
                -of json $mp4 | Out-String | ConvertFrom-Json).streams[0]
        $allStreams = (& $ffprobe -v error -show_entries 'stream=index,codec_type,codec_name:format=format_name,duration,size' -of json $mp4 | Out-String | ConvertFrom-Json)
        # packet (mux した順) と frame (復号して表示する順) の時刻。
        $packetPts = @(& $ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 $mp4 | ForEach-Object { [long]($_ -replace ',', '') })
        $framePts = @(& $ffprobe -v error -select_streams v:0 -show_entries frame=pts -of csv=p=0 $mp4 | ForEach-Object { [long]($_ -replace ',', '') })
        $base = $stream.time_base -split '/'
        $rate = $stream.r_frame_rate -split '/'
        $step = ([long]$base[1] * [long]$rate[1]) / ([long]$base[0] * [long]$rate[0])
        $framesOrdered = $true
        for ($index = 0; $index -lt $framePts.Count; ++$index) { if ($framePts[$index] -ne $index * $step) { $framesOrdered = $false; break } }
        $packetSet = @($packetPts | Sort-Object -Unique)
        $packetsCover = $packetSet.Count -eq $TotalFrames -and $packetPts.Count -eq $TotalFrames -and $packetSet[0] -eq 0 -and
        $packetSet[-1] -eq ($TotalFrames - 1) * $step
        # 全 frame を復号する (error があれば失敗)。
        $decodeLog = Join-Path $run 'ffmpeg-decode.log'
        & $ffmpeg -hide_banner -nostdin -v error -xerror -stats -i $mp4 -map 0:v:0 -f null - 2> $decodeLog
        $decodeExit = $LASTEXITCODE
        $decodeText = Get-Content -Raw $decodeLog
        $decodedFrames = if ($decodeText -match '(?s).*frame=\s*(\d+)') { [int]$Matches[1] } else { -1 }
        $decodeErrors = @((Get-Content $decodeLog) | Where-Object { $_ -and $_ -notmatch '^frame=' -and $_ -notmatch 'frame=\s*\d+ fps' }).Count
        # x264 が stream へ書いた設定。
        $bytes = [IO.File]::ReadAllBytes($mp4)
        $text = [Text.Encoding]::ASCII.GetString($bytes, 0, [math]::Min($bytes.Length, 4MB))
        $x264 = if ($text -match 'x264 - core[^\x00]*') { $Matches[0] } else { $null }
        $bytes = $null

        $outputOk = $stream.codec_name -eq 'h264' -and $stream.width -eq 1920 -and $stream.height -eq 1080 -and
        $stream.r_frame_rate -eq '60/1' -and [int]$stream.nb_frames -eq $TotalFrames -and [int]$stream.nb_read_frames -eq $TotalFrames -and
        $framePts.Count -eq $TotalFrames -and $framesOrdered -and $packetsCover -and $decodeExit -eq 0 -and
        $decodedFrames -eq $TotalFrames -and $decodeErrors -eq 0 -and
        [math]::Abs([double]$stream.duration - $TotalFrames / 60.0) -lt 1e-3
        $parityOk = $exact -eq $TotalFrames -and $different.Count -eq 0 -and $missingCandidate.Count -eq 0 -and
        $missingBaseline.Count -eq 0 -and $extraCandidate.Count -eq 0
        $summary = [ordered]@{
            stage = 'compare'; full_run = (Split-Path -Leaf $fullDirectory); baseline_run = (Split-Path -Leaf $baselineDirectory)
            total_frames = $TotalFrames
            parity = [ordered]@{
                complete_and_exact = $parityOk; frames_exact = $exact; frames_different = $different.Count
                different_first = @($different | Select-Object -First 64)
                candidate_missing = $missingCandidate.Count; candidate_missing_first = @($missingCandidate | Select-Object -First 64)
                baseline_missing = $missingBaseline.Count; baseline_missing_first = @($missingBaseline | Select-Object -First 64)
                candidate_frames_outside_range = $extraCandidate.Count; baseline_chunk_overlap = $overlap
                candidate_lowest_alpha = $lowestCandidate; baseline_lowest_alpha = $lowestBaseline
            }
            output = [ordered]@{
                valid = $outputOk; file = $mp4; sha256 = (Get-Sha $mp4); bytes = (Get-Item $mp4).Length
                stream = $stream; container = $allStreams
                frame_pts_count = $framePts.Count; frame_pts_step = $step; frames_in_presentation_order = $framesOrdered
                packet_count = $packetPts.Count; packet_pts_unique = $packetSet.Count; packets_cover_all_frames = $packetsCover
                decode_exit = $decodeExit; decoded_frames = $decodedFrames; decode_error_lines = $decodeErrors
                x264_settings = $x264
            }
        }
        $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
        $different | Set-Content -LiteralPath (Join-Path $run 'different-frames.txt') -Encoding utf8
        Write-Host ("画素: 一致 {0}/{1}、不一致 {2}、candidate に無い {3}、baseline に無い {4}" -f $exact, $TotalFrames, $different.Count,
            $missingCandidate.Count, $missingBaseline.Count)
        Write-Host ("出力: {0} {1}x{2} {3} fps、frame {4} (読めた {5}、復号 {6}、exit {7})、時刻の順 {8}、packet {9}" -f $stream.codec_name,
            $stream.width, $stream.height, $stream.r_frame_rate, $stream.nb_frames, $stream.nb_read_frames, $decodedFrames, $decodeExit,
            $framesOrdered, $packetPts.Count)
        Write-Host "結果: $run"
        if (-not $parityOk -or -not $outputOk) { ++$failures }
    }

    'safety' {
        Assert-Frozen
        $run = New-RunDirectory 'safety'
        $rows = @()
        # Project 全体の graph で: 途中で取消、実行時の拒否 (下地に alpha 0 を作る故障)。
        $cases = @(
            @{ name = 'full-cancel-mid-render'; fixture = 'v4-full'; extra = @('--inject', 'cancel', '--cancel-after', '600'); exit = 11 },
            @{ name = 'full-runtime-rejection'; fixture = 'v4-full'; extra = @('--math-group-fault', '4'); exit = 6 },
            @{ name = 'false-evidence-rejected'; fixture = 'alpha-0'; extra = @('--base-alpha-proof', 'forge:256'); exit = 6 }
        )
        foreach ($case in $cases) {
            $out = Join-Path $run $case.name
            $arguments = Get-HarnessArguments $out $case.fixture ($mathGroup + $case.extra) 900
            $watch = Invoke-Watched $out $arguments 900 '' -Dump
            $report = Read-Report $out $case.fixture
            $main = Get-Field $report 'math_group_main'
            $left = @(Get-ChildItem $out -File | Where-Object { $_.Extension -in @('.mp4', '.tmp', '.part', '.mkv') -or $_.Name -match '\.mp4\.' })
            $ok = $watch.exit_code -eq $case.exit -and (Get-Field $report 'export_success') -eq $false -and
            (Get-Field $report 'final_file_exists') -eq $false -and (Get-Field $report 'temporary_file_exists') -eq $false -and
            $left.Count -eq 0 -and (Get-Field $main 'graph') -eq 'math-group' -and (Get-Field $main 'live_after_export') -eq 0 -and
            (Get-Field $main 'active_after_export') -eq 0 -and (Get-Field $main 'raster_live_bytes_after_export') -eq 0 -and $watch.dumps -eq 0
            $rows += [pscustomobject]@{ name = $case.name; exit_code = $watch.exit_code; export_error = Get-Field $report 'export_error'
                decision = Get-Field $main 'decision'; graph = Get-Field $main 'graph'; composed_frames = Get-Field $main 'composed_frames'
                final_file_exists = Get-Field $report 'final_file_exists'; temporary_file_exists = Get-Field $report 'temporary_file_exists'
                media_files_left = @($left | ForEach-Object { $_.Name }); live_after_export = Get-Field $main 'live_after_export'
                active_after_export = Get-Field $main 'active_after_export'
                raster_live_bytes_after_export = Get-Field $main 'raster_live_bytes_after_export'
                process_wall_seconds = $watch.process_wall_seconds; dumps = $watch.dumps; as_expected = $ok
            }
            if (-not $ok) { ++$failures }
            Write-Host ("{0,-28} exit={1} error='{2}' 最終 file={3} 一時 file={4} 残った file={5} 期待どおり={6}" -f $case.name, $watch.exit_code,
                (Get-Field $report 'export_error'), (Get-Field $report 'final_file_exists'), (Get-Field $report 'temporary_file_exists'), $left.Count, $ok)
        }
        # 外部 watchdog による打ち切り: process を外から止めるので、後始末は走らない。
        # 最終 file が無いこと (未完成の file を最終の名前で残さないこと) を確かめ、残った file は記録する。
        $out = Join-Path $run 'external-watchdog-kill'
        $arguments = Get-HarnessArguments $out 'v4-full' $mathGroup 900
        $watch = Invoke-Watched $out $arguments 45 (Join-Path $out 'v4-full.mp4')
        $left = @(Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match '\.(mp4|mkv|tmp|part)' })
        $ok = $watch.killed_by_watchdog -and -not (Test-Path -LiteralPath (Join-Path $out 'v4-full.mp4'))
        $rows += [pscustomobject]@{ name = 'external-watchdog-kill'; killed_by_watchdog = $watch.killed_by_watchdog
            process_wall_seconds = $watch.process_wall_seconds; final_file_exists = (Test-Path -LiteralPath (Join-Path $out 'v4-full.mp4'))
            media_files_left = @($left | ForEach-Object { "$($_.Name) ($($_.Length) bytes)" }); as_expected = $ok
        }
        if (-not $ok) { ++$failures }
        Write-Host ("external-watchdog-kill       打ち切り={0} ({1:n1} s) 最終 file={2} 残った file={3}" -f $watch.killed_by_watchdog,
            $watch.process_wall_seconds, (Test-Path -LiteralPath (Join-Path $out 'v4-full.mp4')), ($left | ForEach-Object { $_.Name }) -join ', ')
        $rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
        Write-Host "結果: $run"
    }
}

Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
