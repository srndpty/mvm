<#
.SYNOPSIS
    解の公式 Project の H.264 書き出しを、固定の fixture 行列で診断・照合する。

.DESCRIPTION
    mvm_math_score_export_parity を fixture ごとに 1 process ずつ起動し、終了コードを事前に
    決めた期待と照合する。進行は出力 file の大きさではなく、harness が記録する各境界の通過回数で
    見る。上限時間まで進み続けた書き出し (CAPPED) と、進行が止まった書き出し (STALLED) を分ける。

    結果は -OutputDirectory の下に run ごとの directory を作って残す。既存の directory は
    上書きしない (過去の FAIL を消さない)。

.PARAMETER Fixtures
    実行する fixture。既定は全部。

.PARAMETER Expect
    fixture ごとの期待 (pass / capped-or-pass / any)。既定は全 fixture が pass。
    修正前の記録を取るときは -Expect any を使い、終了コードをそのまま残す。

.PARAMETER NegativeControls
    math-1 に対する 4 種の negative control (被覆の欠落・検査の拒否・期待値のずれ・取消) も回す。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/quadratic-export-diagnosis-20261011',
    [string]$Label = 'run',
    [string[]]$Fixtures = @('background-only', 'math-1', 'math-1-integer', 'math-16', 'math-48', 'math-16-480f',
        'math-16-1920f', 'v4-region', 'v4-full'),
    [ValidateSet('pass', 'any')][string]$Expect = 'pass',
    [int]$CapSeconds = 240,
    [int]$StallSeconds = 45,
    [int]$StackSamples = 2,
    # 0 は製品の timeout のまま。0 以外は製品の条件ではない (report に残る)。原因の特定後、製品の
    # timeout では終わらない長さの Project の全 frame を照合するためだけに使う。
    [int]$RequestTimeoutMs = 0,
    [switch]$NegativeControls
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$harness = Join-Path $repo 'build\ucrt64-release\bin\mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$project = Join-Path $repo $ProjectPath
$ffprobe = Join-Path $ucrt 'ffprobe.exe'
$gdb = Join-Path $ucrt 'gdb.exe'
$git = (Get-Command git -ErrorAction SilentlyContinue)?.Source
if (-not $git) { $git = 'C:\Program Files\Git\cmd\git.exe' }
foreach ($required in @($harness, $manim, $latex, $project, $ffprobe, $gdb, $git)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "診断に必要なファイルがありません: $required (harness は pwsh scripts/build.ps1 -Target mvm_math_score_export_parity)"
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path (Join-Path $repo $OutputDirectory) "$stamp-$Label"
if (Test-Path -LiteralPath $runDirectory) { throw "出力先が既にあります: $runDirectory" }
New-Item -ItemType Directory -Force $runDirectory | Out-Null

# source と runtime の識別。結果がどの binary のものかを後から確かめられるようにする。
$runtimeFiles = @($harness, (Join-Path $ucrt 'libmlt-7.dll'), (Join-Path $ucrt 'avcodec-62.dll'),
    (Join-Path $ucrt 'avformat-62.dll'), (Join-Path $ucrt 'avutil-60.dll'),
    (Join-Path $ucrt 'libx264-165.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll', 'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll',
    $project)
$sourceFiles = @('src/media/mlt/mvm_mlt_export.c', 'src/media/mlt/mvm_mlt_export.h',
    'src/app/timeline_export.cpp', 'src/app/timeline_export.h',
    'tests/harness/math_score_export_parity.cpp', 'tests/harness/mlt_rgba_oracle.h',
    'scripts/math_authoring/math_score.py', 'scripts/math-score-export-diagnose.ps1')
$identity = [ordered]@{
    git_head     = (& $git -C $repo rev-parse HEAD).Trim()
    git_branch   = (& $git -C $repo rev-parse --abbrev-ref HEAD).Trim()
    git_status   = @(& $git -C $repo status --short)
    source_sha256  = [ordered]@{}
    runtime_sha256 = [ordered]@{}
}
foreach ($file in $sourceFiles) {
    $identity.source_sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $repo $file)).Hash
}
foreach ($file in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath $file)) { throw "runtime の file がありません: $file" }
    $identity.runtime_sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'

# 期待する終了コード。0 = PASS、8 = 照合不成立、6 = 書き出し失敗、11 = 指示した取消。
$cases = @()
foreach ($fixture in $Fixtures) {
    $cases += [pscustomobject]@{ name = $fixture; fixture = $fixture; inject = 'none'; expected = 0 }
}
if ($NegativeControls) {
    $cases += [pscustomobject]@{ name = 'negative-drop-frame'; fixture = 'math-1'; inject = 'drop-frame'; expected = 8 }
    $cases += [pscustomobject]@{ name = 'negative-oracle-shift'; fixture = 'math-1'; inject = 'oracle-shift'; expected = 8 }
    $cases += [pscustomobject]@{ name = 'negative-reject-frame'; fixture = 'math-1'; inject = 'reject-frame'; expected = 6 }
    $cases += [pscustomobject]@{ name = 'negative-cancel'; fixture = 'math-1'; inject = 'cancel'; expected = 11 }
}
if ($cases.Count -eq 0) { throw '実行する fixture がありません' }

$results = @()
foreach ($case in $cases) {
    $caseDirectory = Join-Path $runDirectory $case.name
    New-Item -ItemType Directory -Force $caseDirectory | Out-Null
    $log = Join-Path $caseDirectory 'harness.log'
    $arguments = @('--project', $project, '--manim', $manim, '--out', $caseDirectory,
        '--ffprobe', $ffprobe, '--gdb', $gdb, '--fixture', $case.fixture,
        '--cap-seconds', $CapSeconds, '--stall-seconds', $StallSeconds,
        '--stack-samples', $StackSamples, '--inject', $case.inject)
    if ($RequestTimeoutMs -gt 0) { $arguments += @('--request-timeout-ms', $RequestTimeoutMs) }
    $started = Get-Date
    & $harness @arguments 2> $log | Out-Null
    $code = $LASTEXITCODE
    $reportPath = Join-Path $caseDirectory "$($case.fixture)-report.json"
    $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw -LiteralPath $reportPath | ConvertFrom-Json } else { $null }
    $matched = if ($Expect -eq 'any' -and $case.inject -eq 'none') { $true } else { $code -eq $case.expected }
    $rendered = if ($report) { ($report.points | Where-Object point -eq 'image_return').count } else { $null }
    $results += [pscustomobject]@{
        case       = $case.name
        exit_code  = $code
        expected   = if ($Expect -eq 'any' -and $case.inject -eq 'none') { 'any' } else { $case.expected }
        matched    = $matched
        verdict    = if ($report) { $report.verdict } else { 'NO_REPORT' }
        frames     = if ($report) { $report.expected_frames } else { $null }
        composited = $rendered
        wall_s     = if ($report) { [math]::Round($report.wall_seconds, 1) } else { $null }
        cpu_s      = if ($report) { [math]::Round($report.cpu_seconds, 1) } else { $null }
        seconds    = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    }
    Write-Host ("{0,-24} exit={1,-3} {2,-26} 合成済み={3}/{4} wall={5}s cpu={6}s" -f $case.name, $code,
        $results[-1].verdict, $rendered, $results[-1].frames, $results[-1].wall_s, $results[-1].cpu_s)
    Get-Content -LiteralPath $log -Tail 6 | ForEach-Object { Write-Host "    $_" }
}

$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $runDirectory 'summary.json') -Encoding utf8
$failed = @($results | Where-Object { -not $_.matched })
Write-Host "結果: $runDirectory"
Write-Host ("照合: {0}/{1} が期待どおり" -f ($results.Count - $failed.Count), $results.Count)
if ($failed.Count -gt 0) { exit 1 }
exit 0
