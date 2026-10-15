<#
.SYNOPSIS
    P2-A: 書き出しの 1 frame の時間が、keyframe の数・layer の数・Project の長さのどれで決まるかを測る。

.DESCRIPTION
    事前に固定した行列を、行ごとに -Runs 回ずつ mvm_math_score_export_parity で実行する。
    行は実行前に決めてあり、結果を見て増減しない。各 run は上限時間 (-CapSeconds) で打ち切ってよい
    (1 frame あたりの時間は、打ち切った run でも合成できた frame から求まる)。

    keyframe の数だけを変える行は --keyframe-stride を使う。前後と同じ値の keyframe を残すだけなので、
    評価される値と画素は変わらない (summary が digest で確かめる)。
    「history」の行は、同じ 60 frame を、区間だけに切った Project と、Project 全体の graph のまま
    (--range) の両方で書き出す。

    結果は -OutputDirectory の下に run ごとの directory を作る。既存の directory は上書きしない。
#>
[CmdletBinding()]
param(
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-export-p2a-20261012',
    [string]$Label = 'matrix',
    [int]$Runs = 2,
    [int]$CapSeconds = 40,
    [string[]]$Only = @()
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
    if (-not (Test-Path -LiteralPath $required)) { throw "必要なファイルがありません: $required" }
}

# 固定した行列。name = 行の名前、fixture = harness の fixture、args = 追加の引数。
$rows = @(
    # 1 layer で keyframe の数だけを変える (値と画素は同じ)。
    @{ name = 'k-one-480-min'; fixture = 'p2-one-480'; args = @() }
    @{ name = 'k-one-480-stride8'; fixture = 'p2-one-480'; args = @('--keyframe-stride', 8) }
    @{ name = 'k-one-480-stride2'; fixture = 'p2-one-480'; args = @('--keyframe-stride', 2) }
    @{ name = 'k-one-480-all'; fixture = 'p2-one-480'; args = @('--keyframe-stride', 1) }
    @{ name = 'k-one-1920-min'; fixture = 'p2-one-1920'; args = @() }
    @{ name = 'k-one-1920-all'; fixture = 'p2-one-1920'; args = @('--keyframe-stride', 1) }
    # layer の数を変える。1 layer の keyframe は 480 に固定 (all) と、最小 (min)。
    @{ name = 'l-4-480-all'; fixture = 'p2-layers-4-480'; args = @('--keyframe-stride', 1) }
    @{ name = 'l-16-480-all'; fixture = 'math-16-480f'; args = @('--keyframe-stride', 1) }
    @{ name = 'l-48-480-all'; fixture = 'p2-layers-48-480'; args = @('--keyframe-stride', 1) }
    @{ name = 'l-4-480-min'; fixture = 'p2-layers-4-480'; args = @() }
    @{ name = 'l-16-480-min'; fixture = 'math-16-480f'; args = @() }
    @{ name = 'l-48-480-min'; fixture = 'p2-layers-48-480'; args = @() }
    # Project の長さだけを変える (16 layer、keyframe は最小)。480 frame は l-16-480-min と同じ条件。
    @{ name = 'd-16-120-min'; fixture = 'math-16'; args = @() }
    @{ name = 'd-16-1920-min'; fixture = 'math-16-1920f'; args = @() }
)
# history: 同じ 60 frame を、区間だけの Project と、Project 全体の graph で。
foreach ($window in @(@('early', 1200), @('middle', 3400), @('late', 6500))) {
    $name = $window[0]
    $range = "$($window[1]):60"
    $rows += @{ name = "h-$name-cut-full-frame"; fixture = "p2-window-$name"; args = @() }
    $rows += @{ name = "h-$name-cut-anchored"; fixture = "p2-window-$name"; args = @('--staging', 'anchored-box') }
    $rows += @{ name = "h-$name-whole-full-frame"; fixture = 'v4-full'; args = @('--range', $range) }
    $rows += @{ name = "h-$name-whole-anchored"; fixture = 'v4-full'; args = @('--range', $range, '--staging', 'anchored-box') }
    # render worker 1 本: lock 待ちが合成時間に混ざらないので、文字列化の割合をそのまま読める。
    $rows += @{ name = "h-$name-whole-full-frame-1thread"; fixture = 'v4-full'; args = @('--range', $range, '--render-threads', 1) }
}
if ($Only.Count -gt 0) { $rows = @($rows | Where-Object { $Only -contains $_.name }) }
if ($rows.Count -eq 0) { throw '実行する行がありません' }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path (Join-Path $repo $OutputDirectory) "$stamp-$Label"
if (Test-Path -LiteralPath $runDirectory) { throw "出力先が既にあります: $runDirectory" }
New-Item -ItemType Directory -Force $runDirectory | Out-Null

$identity = [ordered]@{
    git_head = (& $git -C $repo rev-parse HEAD).Trim()
    git_status = @(& $git -C $repo status --short)
    runs_per_row = $Runs
    cap_seconds = $CapSeconds
    rows = @($rows | ForEach-Object { [ordered]@{ name = $_.name; fixture = $_.fixture; args = ($_.args -join ' ') } })
    sha256 = [ordered]@{}
}
$hashed = @($harness, (Join-Path $ucrt 'libmlt-7.dll'), 'C:\msys64\ucrt64\lib\mlt\libmltplus.dll',
    'C:\msys64\ucrt64\lib\mlt\libmltqt6.dll', 'C:\msys64\ucrt64\lib\mlt\libmltavformat.dll',
    (Join-Path $ucrt 'avcodec-62.dll'), (Join-Path $ucrt 'libx264-165.dll'), $project,
    (Join-Path $repo 'src/media/mlt/mvm_mlt_export.c'), (Join-Path $repo 'src/media/mlt/mvm_mlt_export.h'),
    (Join-Path $repo 'src/app/timeline_export.cpp'), (Join-Path $repo 'tests/harness/math_score_export_parity.cpp'),
    (Join-Path $repo 'tests/harness/mlt_property_probe.h'), (Join-Path $repo 'scripts/math-export-p2a-matrix.ps1'))
foreach ($file in $hashed) {
    if (-not (Test-Path -LiteralPath $file)) { throw "file がありません: $file" }
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
}
$identity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runDirectory 'identity.json') -Encoding utf8

$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$outcomes = @()
foreach ($run in 1..$Runs) {
    foreach ($row in $rows) {
        $caseDirectory = Join-Path $runDirectory "$($row.name)/run$run"
        New-Item -ItemType Directory -Force $caseDirectory | Out-Null
        $arguments = @('--project', $project, '--manim', $manim, '--out', $caseDirectory, '--ffprobe', $ffprobe,
            '--gdb', $gdb, '--fixture', $row.fixture, '--cap-seconds', $CapSeconds, '--stall-seconds', 45,
            '--stack-samples', 0) + $row.args
        & $harness @arguments 2> (Join-Path $caseDirectory 'harness.log') | Out-Null
        $code = $LASTEXITCODE
        $outcomes += [pscustomobject]@{ row = $row.name; run = $run; exit_code = $code }
        Write-Host ("{0,-36} run{1} exit={2}" -f $row.name, $run, $code)
    }
}
$outcomes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'outcomes.json') -Encoding utf8
# 0 = 完走して照合成立、9 = 上限時間で打ち切り (進行中)。それ以外は計測として使えない。
$unusable = @($outcomes | Where-Object { $_.exit_code -notin 0, 9 })
Write-Host "結果: $runDirectory"
Write-Host ("計測に使える run: {0}/{1}" -f ($outcomes.Count - $unusable.Count), $outcomes.Count)
if ($unusable.Count -gt 0) { exit 1 }
exit 0
