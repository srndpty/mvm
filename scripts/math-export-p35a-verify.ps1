<#
.SYNOPSIS
    P3-5A: 画素の証拠を強める (SHA-256 と、RGBA の byte どうしの比較) と、MLT の color producer の
    race が Windows の書き出しに現れるかの確認。

.DESCRIPTION
    P3-4 の全長の比較は、frame ごとの 64 bit の digest だった。ここでは、harness の改訂
    (--strong-digest、--dump-frames) で、次を足す。製品の経路の全長の baseline は取り直さない。

    段階 (-Stage):
      strong   判定つきの数式 group (mode 3) で全長を 1 回書き出し、全 frame の SHA-256 を採る。
               決めておいた frame は RGBA をそのまま file へ書く。
               製品の経路 (数式 group なし) は、P3-4 の区間の境目・先頭・末尾・代表の位置の
               短い区間だけを書き出し、同じものを採る。
               比べるもの: (1) 区間の全 frame の SHA-256 (candidate の全長 対 baseline の区間)、
               (2) dump した frame の全 byte、(3) baseline の区間の 64 bit の digest と P3-4 の baseline
               (別の位置から書き出し始めた同じ frame)、(4) candidate の全 frame の 64 bit の digest と
               P3-4 の baseline。
      color    V1 が隙間だけの fixture を、同じ process で 製品の経路 → 数式 group → 製品の経路 の順に
               書き出すことをくり返す (MLT の color producer の race が画素に現れるかを見る)。

    実行の前に、-OutputDirectory の freeze.json (scripts/math-export-p34-verify.ps1 -Stage freeze で
    作る) と照合する。source を書き換えない。build もしない。
#>
param(
    [Parameter(Mandatory)][ValidateSet('strong', 'color')][string]$Stage,
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-export-p35a-20261013',
    [string]$P34Baseline = 'build/math-export-p34-20261013/20261013-011914-baseline',
    [int]$TotalFrames = 6840,
    [int]$ChunkFrames = 570,
    [int]$ColorRepeats = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$harness = Join-Path $repo 'build\ucrt64-release\bin\mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
$root = Join-Path $repo $OutputDirectory
$freezePath = Join-Path $root 'freeze.json'
if (-not (Test-Path -LiteralPath $freezePath)) { throw "freeze.json がありません: $freezePath" }
$frozen = Get-Content -Raw $freezePath | ConvertFrom-Json -AsHashtable
$changed = @($frozen.sha256.Keys | Where-Object {
        -not (Test-Path -LiteralPath $_) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash -ne $frozen.sha256[$_] })
if ($changed.Count -gt 0) { $changed | ForEach-Object { Write-Host "変わった: $_" }; throw 'freeze.json と一致しません' }
Write-Host "freeze.json と一致: $($frozen.sha256.Count) file"
$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$run = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Stage")
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null
$failures = 0

function Invoke-Harness([string]$Name, [string]$Fixture, [string[]]$Extra) {
    $out = Join-Path $run $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = @('--project', $project, '--manim', $manim, '--out', $out, '--ffprobe', (Join-Path $ucrt 'ffprobe.exe'),
        '--fixture', $Fixture, '--cap-seconds', '1500') + $Extra
    & $harness @arguments 2> (Join-Path $out 'harness.log') | Out-Null
    $reportPath = Join-Path $out "$Fixture-report.json"
    [pscustomobject]@{ out = $out; exit_code = $LASTEXITCODE
        report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
    }
}

# 列: frame、呼ばれた回数、64 bit の digest、alpha の最小値、SHA-256 (あれば)。
function Read-Digests([string]$Path, [int]$Offset) {
    $map = @{}
    foreach ($line in [IO.File]::ReadLines($Path)) {
        $columns = $line -split "`t"
        if ($columns.Count -lt 4) { continue }
        $map[[int]$columns[0] + $Offset] = [pscustomobject]@{ digest = $columns[2]; alpha = [int]$columns[3]
            sha256 = if ($columns.Count -ge 5) { $columns[4] } else { $null }
        }
    }
    $map
}

if ($Stage -eq 'strong') {
    # 比べる区間 (実行の前に固定): 先頭、P3-4 の区間の境目 (前 3 frame と後 3 frame)、既存の代表 frame、
    # P3-3D の全体 graph の区間の先頭、末尾。dump するのは、各区間の 3 番目と 4 番目の frame。
    $starts = @(0) + @(1..([math]::Floor(($TotalFrames - 1) / $ChunkFrames)) | ForEach-Object { $_ * $ChunkFrames - 3 }) +
    @(598, 1200, 3400, 5280, 5368, 6500, 6718, ($TotalFrames - 6))
    $windows = @($starts | Sort-Object -Unique | ForEach-Object { [pscustomobject]@{ start = $_; count = 6 } })
    $dumpFrames = @($windows | ForEach-Object { $_.start + 2; $_.start + 3 })
    $dumpArgument = ($dumpFrames -join ',')

    # P3-4 の baseline (64 bit の digest。区間ごとの file)。
    $p34 = @{}
    foreach ($chunk in (Get-ChildItem (Join-Path $repo $P34Baseline) -Directory -Filter 'chunk-*')) {
        $part = Read-Digests (Join-Path $chunk.FullName 'v4-full-pre-yuv.tsv') ([int]($chunk.Name -replace 'chunk-', ''))
        foreach ($frame in $part.Keys) { $p34[$frame] = $part[$frame] }
    }
    if ($p34.Count -ne $TotalFrames) { throw "P3-4 の baseline の frame 数が違います: $($p34.Count)" }

    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $candidate = Invoke-Harness 'candidate-full' 'v4-full' @('--math-group', 'auto', '--math-group-rule', '1', '--strong-digest', '1',
        '--dump-frames', $dumpArgument)
    $main = $candidate.report.math_group_main
    $candidateDigests = Read-Digests (Join-Path $candidate.out 'v4-full-pre-yuv.tsv') 0
    Write-Host ("candidate: exit={0} 判定={1} graph={2} frame={3} 書き出し {4:n1} s (SHA-256 と dump の負荷を含む)" -f $candidate.exit_code,
        $main.decision, $main.graph, $candidateDigests.Count, $candidate.report.export_call_wall_seconds)
    $candidateOk = $candidate.exit_code -eq 0 -and $main.decision -eq 'ELIGIBLE' -and $main.graph -eq 'math-group' -and
    $candidateDigests.Count -eq $TotalFrames -and $main.errors -eq 0
    if (-not $candidateOk) { ++$failures }

    # (4) candidate の全 frame の 64 bit の digest 対 P3-4 の baseline。
    $fullDigestExact = 0
    for ($frame = 0; $frame -lt $TotalFrames; ++$frame) {
        $c = $candidateDigests[$frame]
        if ($null -ne $c -and $c.digest -eq $p34[$frame].digest -and $c.alpha -eq $p34[$frame].alpha) { ++$fullDigestExact }
    }
    $distinctSha = @($candidateDigests.Values | ForEach-Object { $_.sha256 } | Sort-Object -Unique).Count
    $distinctDigest = @($candidateDigests.Values | ForEach-Object { $_.digest } | Sort-Object -Unique).Count
    # 64 bit の digest が同じ frame は SHA-256 も同じか (digest の衝突が、この出力の中に無いこと)。
    $byDigest = @{}
    $collisions = 0
    foreach ($value in $candidateDigests.Values) {
        if ($byDigest.ContainsKey($value.digest)) { if ($byDigest[$value.digest] -ne $value.sha256) { ++$collisions } }
        else { $byDigest[$value.digest] = $value.sha256 }
    }

    $rows = @()
    $shaCompared = 0; $shaExact = 0; $segmentCompared = 0; $segmentExact = 0; $bytesCompared = 0; $bytesExact = 0; $dumpSelfOk = 0
    foreach ($window in $windows) {
        $name = 'baseline-{0:d4}' -f $window.start
        $baseline = Invoke-Harness $name 'v4-full' @('--range', "$($window.start):$($window.count)", '--strong-digest', '1',
            '--dump-frames', $dumpArgument, '--request-timeout-ms', '3000000')
        $digests = if (Test-Path -LiteralPath (Join-Path $baseline.out 'v4-full-pre-yuv.tsv')) {
            Read-Digests (Join-Path $baseline.out 'v4-full-pre-yuv.tsv') $window.start
        } else { @{} }
        $windowSha = 0; $windowSegment = 0; $windowBytes = 0; $windowDumps = 0
        for ($frame = $window.start; $frame -lt $window.start + $window.count; ++$frame) {
            $b = $digests[$frame]; $c = $candidateDigests[$frame]
            ++$shaCompared; ++$segmentCompared
            if ($null -ne $b -and $null -ne $c -and $b.sha256 -and $b.sha256 -eq $c.sha256) { ++$shaExact; ++$windowSha }
            if ($null -ne $b -and $b.digest -eq $p34[$frame].digest -and $b.alpha -eq $p34[$frame].alpha) { ++$segmentExact; ++$windowSegment }
        }
        foreach ($frame in ($dumpFrames | Where-Object { $_ -ge $window.start -and $_ -lt $window.start + $window.count })) {
            $candidateDump = Join-Path $candidate.out "v4-full-frame-$frame.rgba"
            $baselineDump = Join-Path $baseline.out "v4-full-frame-$frame.rgba"
            ++$bytesCompared; ++$windowDumps
            if (-not (Test-Path -LiteralPath $candidateDump) -or -not (Test-Path -LiteralPath $baselineDump)) { continue }
            # 全 byte を比べる (hash を介さない)。
            & cmd /c "fc /b `"$candidateDump`" `"$baselineDump`" > nul"
            $same = $LASTEXITCODE -eq 0 -and (Get-Item -LiteralPath $candidateDump).Length -eq 1920 * 1080 * 4 -and
            (Get-Item -LiteralPath $baselineDump).Length -eq 1920 * 1080 * 4
            if ($same) { ++$bytesExact; ++$windowBytes }
            # dump した byte の SHA-256 が、digest の file の値と同じか (dump と hash が同じ画素を見ている)。
            $fileSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidateDump).Hash.ToLowerInvariant()
            if ($fileSha -eq $candidateDigests[$frame].sha256 -and $fileSha -eq $digests[$frame].sha256) { ++$dumpSelfOk }
            Remove-Item -LiteralPath $baselineDump # 比べた後は消す (1 枚 8.3 MB)。candidate の側は最後に消す
        }
        $rows += [pscustomobject]@{ start = $window.start; count = $window.count; exit_code = $baseline.exit_code
            overlay_transitions = ${baseline}.report?.overlay_transitions; sha256_exact = $windowSha; p34_digest_exact = $windowSegment
            dumped = $windowDumps; bytes_exact = $windowBytes
        }
        Write-Host ("baseline {0,4}〜{1,4}: exit={2} SHA-256 一致 {3}/{4}、P3-4 の digest と一致 {5}/{4}、byte 一致 {6}/{7}" -f $window.start,
            ($window.start + $window.count - 1), $baseline.exit_code, $windowSha, $window.count, $windowSegment, $windowBytes, $windowDumps)
        if ($baseline.exit_code -ne 0 -or $windowSha -ne $window.count -or $windowSegment -ne $window.count -or $windowBytes -ne $windowDumps) { ++$failures }
    }
    Get-ChildItem $candidate.out -Filter '*.rgba' | Remove-Item
    if ($fullDigestExact -ne $TotalFrames -or $collisions -ne 0 -or $dumpSelfOk -ne $bytesCompared) { ++$failures }
    $summary = [ordered]@{
        stage = 'strong'; total_frames = $TotalFrames; wall_seconds = $clock.Elapsed.TotalSeconds
        candidate = [ordered]@{ ok = $candidateOk; exit_code = $candidate.exit_code; decision = $main.decision; graph = $main.graph
            export_call_wall_seconds = $candidate.report.export_call_wall_seconds; output_sha256 = $candidate.report.output_sha256
            frames = $candidateDigests.Count; distinct_sha256 = $distinctSha; distinct_digest64 = $distinctDigest
            digest64_same_but_sha256_differs = $collisions
        }
        full_length_digest64_vs_p34_baseline = [ordered]@{ compared = $TotalFrames; exact = $fullDigestExact }
        windows = $rows
        window_sha256_candidate_vs_baseline = [ordered]@{ compared = $shaCompared; exact = $shaExact }
        window_baseline_digest64_vs_p34_baseline = [ordered]@{ compared = $segmentCompared; exact = $segmentExact }
        raw_rgba_bytes_candidate_vs_baseline = [ordered]@{ compared = $bytesCompared; exact = $bytesExact; frames = $dumpFrames
            dump_sha256_matches_recorded = $dumpSelfOk
        }
    }
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
    Write-Host ("全長の 64 bit digest 対 P3-4: {0}/{1}。区間の SHA-256: {2}/{3}。区間の baseline 対 P3-4: {4}/{5}。byte: {6}/{7}。digest が同じで SHA-256 が違う: {8}" -f `
            $fullDigestExact, $TotalFrames, $shaExact, $shaCompared, $segmentExact, $segmentCompared, $bytesExact, $bytesCompared, $collisions)
}

if ($Stage -eq 'color') {
    $rows = @()
    foreach ($fixture in @('base-gap', 'base-half-gap')) {
        for ($index = 1; $index -le $ColorRepeats; ++$index) {
            $result = Invoke-Harness "$fixture-$index" $fixture @('--compare', 'math-group-auto', '--math-group-rule', '1')
            $compare = $result.report?.compare
            $ok = $result.exit_code -eq 0 -and $compare.verdict -eq 'EXACT' -and $compare.frames_exact -eq $result.report.expected_frames -and
            $compare.baseline_repeat_mismatches -eq 0 -and $compare.candidate_group.errors -eq 0
            $rows += [pscustomobject]@{ fixture = $fixture; index = $index; exit_code = $result.exit_code; verdict = ${compare}?.verdict
                frames_exact = ${compare}?.frames_exact; baseline_repeat_mismatches = ${compare}?.baseline_repeat_mismatches
                group_errors = ${compare}?.candidate_group?.errors; candidate_error = ${compare}?.candidate_error; ok = $ok
            }
            if (-not $ok) { ++$failures }
            Write-Host ("{0} #{1}: exit={2} {3} 一致={4} baseline のくり返しの不一致={5} ok={6}" -f $fixture, $index, $result.exit_code,
                ${compare}?.verdict, ${compare}?.frames_exact, ${compare}?.baseline_repeat_mismatches, $ok)
        }
    }
    $rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
}

Write-Host "結果: $run"
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
