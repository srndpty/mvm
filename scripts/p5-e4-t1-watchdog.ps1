[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ParentWorktree,
    [Parameter(Mandatory)][string]$ExpectedDiagnosticSha,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$PriorDumpPath,
    [ValidateRange(1,30)][int]$Repetitions = 10,
    [ValidateRange(90,900)][int]$WatchdogSeconds = 180,
    [string]$SourceA,
    [string]$SourceB
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'p5-e4-t1-classifier.ps1')
$parent = (Resolve-Path -LiteralPath $ParentWorktree).Path
$priorDump = (Resolve-Path -LiteralPath $PriorDumpPath).Path
if (-not $SourceA) { $SourceA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4' }
if (-not $SourceB) { $SourceB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4' }
$executable = Join-Path $parent 'build\ucrt64-release\bin\mvm_p3_av_sync_spike.exe'
$checker = Join-Path $parent 'scripts\check-p3-c2-contract.ps1'
foreach ($path in @($executable,$checker,$SourceA,$SourceB,$priorDump)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "ATTR-Q3-T1必須fileがありません: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "ATTR-Q3-T1 artifactを上書きしません: $OutputDirectory"
}
$actualSha = (& git -C $parent rev-parse HEAD).Trim()
$expectedSha = (& git -C $parent rev-parse $ExpectedDiagnosticSha).Trim()
$parentStatus = @(& git -C $parent status --porcelain)
if ($actualSha -ne $expectedSha -or $parentStatus.Count -ne 0) {
    throw 'ATTR-Q3-T1はparent clean exact diagnostic SHAでだけ実行します'
}

function Read-Sidecar([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $entries = [System.Collections.Generic.List[object]]::new()
    $expectedSequence = 0
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $entry = $line | ConvertFrom-Json
        if ($entry.schema -ne 'mvm-p5-e4-t1-liveness-1' -or -not $entry.event) {
            throw "ATTR-Q3-T1 sidecar entryが不正です: $Path"
        }
        if ($entry.sequence -ne $expectedSequence) {
            throw "ATTR-Q3-T1 sidecar sequenceが不連続です: $Path"
        }
        ++$expectedSequence
        $entries.Add($entry)
    }
    return @($entries)
}

function Get-TargetTree([int]$RootPid) {
    $all = @(Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,Name,CommandLine)
    $ids = [System.Collections.Generic.List[int]]::new()
    $ids.Add($RootPid)
    for ($index = 0; $index -lt $ids.Count; ++$index) {
        foreach ($child in $all | Where-Object {$_.ParentProcessId -eq $ids[$index]}) {
            if (-not $ids.Contains([int]$child.ProcessId)) { $ids.Add([int]$child.ProcessId) }
        }
    }
    return @($all | Where-Object {$ids.Contains([int]$_.ProcessId)})
}

New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$priorAnalysis = [ordered]@{
    schema='mvm-p5-e4-t1-prior-dump-analysis-1'
    dump_path=$priorDump
    dump_sha256=(Get-FileHash -LiteralPath $priorDump -Algorithm SHA256).Hash.ToLowerInvariant()
    debugger='C:\msys64\ucrt64\bin\gdb.exe'
    command='gdb -q -batch -ex "info threads" -ex "thread apply all bt" <exe> <dump>'
    blocking_frame_determined=$false
    result='Windows full dumpをGDBがcore dumpとして認識しない (file format not recognized)'
}
$priorAnalysis | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'prior-dump-analysis.json') -Encoding utf8

$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$records = [System.Collections.Generic.List[object]]::new()
$hangFound = $false
$classification = $null
$referencePath = ''

foreach ($index in 1..$Repetitions) {
    $runDirectory = Join-Path $OutputDirectory "run-$index"
    New-Item -ItemType Directory -Path $runDirectory | Out-Null
    $metrics = Join-Path $runDirectory 'metrics.json'
    $journal = Join-Path $runDirectory 'stage-journal.jsonl'
    $sidecar = Join-Path $runDirectory 'liveness-sidecar.jsonl'
    $stdout = Join-Path $runDirectory 'stdout.txt'
    $stderr = Join-Path $runDirectory 'stderr.txt'
    $arguments = @('--source-a',$SourceA,'--source-b',$SourceB,'--metrics',$metrics,
        '--mode','playback','--duration-seconds','60','--warmup-seconds','5',
        '--seek-count','1000','--seed','20260808','--display-timeout-ms','3000',
        '--formal-contract-c2','--shutdown-stage-journal',$journal,
        '--liveness-sidecar',$sidecar)
    Write-Host "ATTR-Q3-T1 parent formal playback $index/$Repetitions を開始します" -ForegroundColor Cyan
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $started = Get-Date
    $completed = $process.WaitForExit($WatchdogSeconds * 1000)
    $watchdog = $null
    if (-not $completed) {
        $hangFound = $true
        $tree = @(Get-TargetTree $process.Id)
        $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
        $threads = @()
        if ($live) {
            $threads = @($live.Threads | ForEach-Object {
                [ordered]@{id=$_.Id;thread_state="$($_.ThreadState)"
                    wait_reason=$(try{"$($_.WaitReason)"}catch{$null})
                    total_processor_time_ms=$(try{$_.TotalProcessorTime.TotalMilliseconds}catch{$null})}
            })
        }
        $threadPath = Join-Path $runDirectory 'thread-snapshot.json'
        $threads | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $threadPath -Encoding utf8
        $entries = @(Read-Sidecar $sidecar)
        $lastWriteAge = if (Test-Path -LiteralPath $sidecar) {
            [math]::Round(((Get-Date).ToUniversalTime() -
                (Get-Item -LiteralPath $sidecar).LastWriteTimeUtc).TotalSeconds,3)
        } else {[double]::PositiveInfinity}
        $classification = Get-P5E4T1HangClassification -Entries $entries `
            -LastWriteAgeSeconds $lastWriteAge
        $watchdog = [ordered]@{
            schema='mvm-p5-e4-t1-watchdog-timeout-1'
            timeout_seconds=$WatchdogSeconds
            process_id=$process.Id
            process_start_local=$started.ToString('o')
            elapsed_seconds=[math]::Round(((Get-Date)-$started).TotalSeconds,3)
            target_process_tree=$tree
            sidecar_entry_count=$entries.Count
            sidecar_last_write_age_seconds=$lastWriteAge
            last_sidecar_entry=$(if ($entries.Count -gt 0) {$entries[-1]} else {$null})
            classification=$classification
            thread_snapshot_path=$threadPath
        }
        $watchdog | ConvertTo-Json -Depth 10 |
            Set-Content -LiteralPath (Join-Path $runDirectory 'watchdog-timeout.json') -Encoding utf8
        foreach ($target in @($tree | Where-Object {$_.ProcessId -ne $process.Id})) {
            Stop-Process -Id $target.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
    }
    $exitCode = if ($completed) {$process.ExitCode} else {$null}
    $contractExit = -1
    if ($completed -and (Test-Path -LiteralPath $metrics)) {
        $checkerArguments = @('-NoProfile','-File',$checker,'-Json',$metrics,'-Mode','playback',
            '-ProcessExitCode',$exitCode)
        if ($referencePath) { $checkerArguments += @('-ReferenceJson',$referencePath) }
        & pwsh @checkerArguments
        $contractExit = $LASTEXITCODE
        if ($exitCode -eq 0 -and $contractExit -eq 0 -and -not $referencePath) {
            $referencePath = $metrics
        }
    }
    $entries = @(Read-Sidecar $sidecar)
    $records.Add([pscustomobject]@{
        run=$index;completed=$completed;process_exit_code=$exitCode;contract_exit_code=$contractExit
        metrics_path=$metrics;journal_path=$journal;sidecar_path=$sidecar
        sidecar_entry_count=$entries.Count
        last_sidecar_entry=$(if ($entries.Count -gt 0) {$entries[-1]} else {$null})
        watchdog=$watchdog
    })
    if ($hangFound) { break }
}

$attributionFound = $hangFound -and $null -ne $classification
$summary = [ordered]@{
    schema='mvm-p5-e4-t1-summary-1'
    authority='DIAGNOSTIC_ONLY'
    formal_pass_authority=$false
    formal_verdict='NOT_RUN'
    objective='parent formal-playback phase/audio-clock liveness attribution'
    expected_diagnostic_sha=$expectedSha
    actual_diagnostic_sha=$actualSha
    executable_path=$executable
    executable_sha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_a_sha256=(Get-FileHash -LiteralPath $SourceA -Algorithm SHA256).Hash.ToLowerInvariant()
    fixture_b_sha256=(Get-FileHash -LiteralPath $SourceB -Algorithm SHA256).Hash.ToLowerInvariant()
    watchdog_script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
    classifier_script_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot `
        'p5-e4-t1-classifier.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
    prior_dump_analysis=$priorAnalysis
    exact_formal_arguments=[ordered]@{mode='playback';warmup_seconds=5;measurement_seconds=60
        seed=20260808;seek_count=1000;display_timeout_ms=3000;contract='P3-C-2'}
    heartbeat_interval_ms=5000
    watchdog_seconds=$WatchdogSeconds
    requested_repetitions=$Repetitions
    completed_runs=$records.Count
    hang_found=$hangFound
    attribution_found=$attributionFound
    classification=$classification
    runs=@($records)
}
$summaryPath = Join-Path $OutputDirectory 'summary.json'
$summary | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
$rootPath = (Resolve-Path $OutputDirectory).Path
$manifest = Get-ChildItem -LiteralPath $rootPath -Recurse -File |
    Where-Object {$_.FullName -ne $manifestPath} | Sort-Object FullName | ForEach-Object {
        $relative = [IO.Path]::GetRelativePath($rootPath,$_.FullName).Replace('\','/')
        "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
    }
$manifest | Set-Content -LiteralPath $manifestPath -Encoding utf8
if (-not $hangFound) { exit 3 }
if (-not $attributionFound) { exit 4 }
exit 0
